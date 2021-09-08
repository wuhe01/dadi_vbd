#include <linux/init.h>
#include <linux/initrd.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/major.h>
#include <linux/blkdev.h>
#include <linux/bio.h>
#include <linux/highmem.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/backing-dev.h>

#include <linux/uaccess.h>

#define PAGE_SECTORS_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define PAGE_SECTORS		(1 << PAGE_SECTORS_SHIFT)

/*
 * Each block ramdisk device has a radix_tree ovbd_pages of pages that stores
 * the pages containing the block device's contents. A ovbd page's ->index is
 * its offset in PAGE_SIZE units. This is similar to, but in no way connected
 * with, the kernel's pagecache or buffer cache (which sit above our block
 * device).
 */
struct ovbd_device {
	int		ovbd_number;

	struct request_queue	*ovbd_queue;
	struct gendisk		*ovbd_disk;
	struct list_head	ovbd_list;

	/*
	 * Backing store of pages and lock to protect it. This is the contents
	 * of the block device.
	 */
	spinlock_t		ovbd_lock;
	struct radix_tree_root	ovbd_pages;
};

/*
 * Look up and return a ovbd's page for a given sector.
 */
static struct page *ovbd_lookup_page(struct ovbd_device *ovbd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;

	/*
	 * The page lifetime is protected by the fact that we have opened the
	 * device node -- ovbd pages will never be deleted under us, so we
	 * don't need any further locking or refcounting.
	 *
	 * This is strictly true for the radix-tree nodes as well (ie. we
	 * don't actually need the rcu_read_lock()), however that is not a
	 * documented feature of the radix-tree API so it is better to be
	 * safe here (we don't have total exclusion from radix tree updates
	 * here, only deletes).
	 */
	rcu_read_lock();
	idx = sector >> PAGE_SECTORS_SHIFT; /* sector to page index */
	page = radix_tree_lookup(&ovbd->ovbd_pages, idx);
	rcu_read_unlock();

	BUG_ON(page && page->index != idx);

	return page;
}

/*
 * Look up and return a ovbd's page for a given sector.
 * If one does not exist, allocate an empty page, and insert that. Then
 * return it.
 */
static struct page *ovbd_insert_page(struct ovbd_device *ovbd, sector_t sector)
{
	pgoff_t idx;
	struct page *page;
	gfp_t gfp_flags;

	page = ovbd_lookup_page(ovbd, sector);
	if (page)
		return page;

	/*
	 * Must use NOIO because we don't want to recurse back into the
	 * block or filesystem layers from page reclaim.
	 *
	 * Cannot support DAX and highmem, because our ->direct_access
	 * routine for DAX must return memory that is always addressable.
	 * If DAX was reworked to use pfns and kmap throughout, this
	 * restriction might be able to be lifted.
	 */
	gfp_flags = GFP_NOIO | __GFP_ZERO;
	page = alloc_page(gfp_flags);
	if (!page)
		return NULL;

	if (radix_tree_preload(GFP_NOIO)) {
		__free_page(page);
		return NULL;
	}

	spin_lock(&ovbd->ovbd_lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	page->index = idx;
	if (radix_tree_insert(&ovbd->ovbd_pages, idx, page)) {
		__free_page(page);
		page = radix_tree_lookup(&ovbd->ovbd_pages, idx);
		BUG_ON(!page);
		BUG_ON(page->index != idx);
	}
	spin_unlock(&ovbd->ovbd_lock);

	radix_tree_preload_end();

	return page;
}

/*
 * Free all backing store pages and radix tree. This must only be called when
 * there are no other users of the device.
 */
#define FREE_BATCH 16
static void ovbd_free_pages(struct ovbd_device *ovbd)
{
	unsigned long pos = 0;
	struct page *pages[FREE_BATCH];
	int nr_pages;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(&ovbd->ovbd_pages,
				(void **)pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			void *ret;

			BUG_ON(pages[i]->index < pos);
			pos = pages[i]->index;
			ret = radix_tree_delete(&ovbd->ovbd_pages, pos);
			BUG_ON(!ret || ret != pages[i]);
			__free_page(pages[i]);
		}

		pos++;

		/*
		 * This assumes radix_tree_gang_lookup always returns as
		 * many pages as possible. If the radix-tree code changes,
		 * so will this have to.
		 */
	} while (nr_pages == FREE_BATCH);
}

/*
 * copy_to_ovbd_setup must be called before copy_to_ovbd. It may sleep.
 */
static int copy_to_ovbd_setup(struct ovbd_device *ovbd, sector_t sector, size_t n)
{
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	if (!ovbd_insert_page(ovbd, sector))
		return -ENOSPC;
	if (copy < n) {
		sector += copy >> SECTOR_SHIFT;
		if (!ovbd_insert_page(ovbd, sector))
			return -ENOSPC;
	}
	return 0;
}

/*
 * Copy n bytes from src to the ovbd starting at sector. Does not sleep.
 */
static void copy_to_ovbd(struct ovbd_device *ovbd, const void *src,
			sector_t sector, size_t n)
{
	struct page *page;
	void *dst;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = ovbd_lookup_page(ovbd, sector);
	BUG_ON(!page);

	dst = kmap_atomic(page);
	memcpy(dst + offset, src, copy);
	kunmap_atomic(dst);

	if (copy < n) {
		src += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = ovbd_lookup_page(ovbd, sector);
		BUG_ON(!page);

		dst = kmap_atomic(page);
		memcpy(dst, src, copy);
		kunmap_atomic(dst);
	}
}

/*
 * Copy n bytes to dst from the ovbd starting at sector. Does not sleep.
 */
static void copy_from_ovbd(void *dst, struct ovbd_device *ovbd,
			sector_t sector, size_t n)
{
	struct page *page;
	void *src;
	unsigned int offset = (sector & (PAGE_SECTORS-1)) << SECTOR_SHIFT;
	size_t copy;

	copy = min_t(size_t, n, PAGE_SIZE - offset);
	page = ovbd_lookup_page(ovbd, sector);
	if (page) {
		src = kmap_atomic(page);
		memcpy(dst, src + offset, copy);
		kunmap_atomic(src);
	} else
		memset(dst, 0, copy);

	if (copy < n) {
		dst += copy;
		sector += copy >> SECTOR_SHIFT;
		copy = n - copy;
		page = ovbd_lookup_page(ovbd, sector);
		if (page) {
			src = kmap_atomic(page);
			memcpy(dst, src, copy);
			kunmap_atomic(src);
		} else
			memset(dst, 0, copy);
	}
}

/*
 * Process a single bvec of a bio.
 */
static int ovbd_do_bvec(struct ovbd_device *ovbd, struct page *page,
			unsigned int len, unsigned int off, bool is_write,
			sector_t sector)
{
	void *mem;
	int err = 0;

	if (is_write) {
		err = copy_to_ovbd_setup(ovbd, sector, len);
		if (err)
			goto out;
	}

	mem = kmap_atomic(page);
	if (!is_write) {
		copy_from_ovbd(mem + off, ovbd, sector, len);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		copy_to_ovbd(ovbd, mem + off, sector, len);
	}
	kunmap_atomic(mem);

out:
	return err;
}

static blk_qc_t ovbd_make_request(struct request_queue *q, struct bio *bio)
{
	struct ovbd_device *ovbd = bio->bi_disk->private_data;
	struct bio_vec bvec;
	sector_t sector;
	struct bvec_iter iter;

	sector = bio->bi_iter.bi_sector;
	if (bio_end_sector(bio) > get_capacity(bio->bi_disk))
		goto io_error;

	bio_for_each_segment(bvec, bio, iter) {
		unsigned int len = bvec.bv_len;
		int err;

		err = ovbd_do_bvec(ovbd, bvec.bv_page, len, bvec.bv_offset,
					op_is_write(bio_op(bio)), sector);
		if (err)
			goto io_error;
		sector += len >> SECTOR_SHIFT;
	}

	bio_endio(bio);
	return BLK_QC_T_NONE;
io_error:
	bio_io_error(bio);
	return BLK_QC_T_NONE;
}

static int ovbd_rw_page(struct block_device *bdev, sector_t sector,
		       struct page *page, bool is_write)
{
	struct ovbd_device *ovbd = bdev->bd_disk->private_data;
	int err;

	if (PageTransHuge(page))
		return -ENOTSUPP;
	err = ovbd_do_bvec(ovbd, page, PAGE_SIZE, 0, is_write, sector);
	page_endio(page, is_write, err);
	return err;
}

static const struct block_device_operations ovbd_fops = {
	.owner =		THIS_MODULE,
	.rw_page =		ovbd_rw_page,
};

/*
 * And now the modules code and kernel interface.
 */
static int rd_nr = CONFIG_BLK_DEV_RAM_COUNT;
module_param(rd_nr, int, S_IRUGO);
MODULE_PARM_DESC(rd_nr, "Maximum number of ovbd devices");

unsigned long rd_size = CONFIG_BLK_DEV_RAM_SIZE;
module_param(rd_size, ulong, S_IRUGO);
MODULE_PARM_DESC(rd_size, "Size of each RAM disk in kbytes.");

static int max_part = 1;
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Num Minors to reserve between devices");

MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(RAMDISK_MAJOR);
MODULE_ALIAS("ovbd");

#ifndef MODULE
/* Legacy boot options - nonmodular */
static int __init ramdisk_size(char *str)
{
	rd_size = simple_strtol(str, NULL, 0);
	return 1;
}
__setup("ramdisk_size=", ramdisk_size);
#endif

/*
 * The device scheme is derived from loop.c. Keep them in synch where possible
 * (should share code eventually).
 */
static LIST_HEAD(ovbd_devices);
static DEFINE_MUTEX(ovbd_devices_mutex);

#define OVERLAY_VBD_MAJOR  231
static struct ovbd_device *ovbd_alloc(int i)
{
	struct ovbd_device *ovbd;
	struct gendisk *disk;

	ovbd = kzalloc(sizeof(*ovbd), GFP_KERNEL);
	if (!ovbd)
		goto out;
	ovbd->ovbd_number		= i;
	spin_lock_init(&ovbd->ovbd_lock);
	INIT_RADIX_TREE(&ovbd->ovbd_pages, GFP_ATOMIC);

	ovbd->ovbd_queue = blk_alloc_queue(GFP_KERNEL);
	if (!ovbd->ovbd_queue)
		goto out_free_dev;

	blk_queue_make_request(ovbd->ovbd_queue, ovbd_make_request);
	blk_queue_max_hw_sectors(ovbd->ovbd_queue, 1024);

	/* This is so fdisk will align partitions on 4k, because of
	 * direct_access API needing 4k alignment, returning a PFN
	 * (This is only a problem on very small devices <= 4M,
	 *  otherwise fdisk will align on 1M. Regardless this call
	 *  is harmless)
	 */
	blk_queue_physical_block_size(ovbd->ovbd_queue, PAGE_SIZE);
	disk = ovbd->ovbd_disk = alloc_disk(max_part);
	if (!disk)
		goto out_free_queue;
	disk->major		= OVERLAY_VBD_MAJOR;
	disk->first_minor	= i * max_part;
	disk->fops		= &ovbd_fops;
	disk->private_data	= ovbd;
	disk->queue		= ovbd->ovbd_queue;
	disk->flags		= GENHD_FL_EXT_DEVT;
	sprintf(disk->disk_name, "ram%d", i);
	set_capacity(disk, rd_size * 2);
	disk->queue->backing_dev_info->capabilities |= BDI_CAP_SYNCHRONOUS_IO;

	return ovbd;

out_free_queue:
	blk_cleanup_queue(ovbd->ovbd_queue);
out_free_dev:
	kfree(ovbd);
out:
	return NULL;
}

static void ovbd_free(struct ovbd_device *ovbd)
{
	put_disk(ovbd->ovbd_disk);
	blk_cleanup_queue(ovbd->ovbd_queue);
	ovbd_free_pages(ovbd);
	kfree(ovbd);
}

static struct ovbd_device *ovbd_init_one(int i, bool *new)
{
	struct ovbd_device *ovbd;

	*new = false;
	list_for_each_entry(ovbd, &ovbd_devices, ovbd_list) {
		if (ovbd->ovbd_number == i)
			goto out;
	}

	ovbd = ovbd_alloc(i);
	if (ovbd) {
		add_disk(ovbd->ovbd_disk);
		list_add_tail(&ovbd->ovbd_list, &ovbd_devices);
	}
	*new = true;
out:
	return ovbd;
}

static void ovbd_del_one(struct ovbd_device *ovbd)
{
	list_del(&ovbd->ovbd_list);
	del_gendisk(ovbd->ovbd_disk);
	ovbd_free(ovbd);
}

static struct kobject *ovbd_probe(dev_t dev, int *part, void *data)
{
	struct ovbd_device *ovbd;
	struct kobject *kobj;
	bool new;

	mutex_lock(&ovbd_devices_mutex);
	ovbd = ovbd_init_one(MINOR(dev) / max_part, &new);
	kobj = ovbd ? get_disk_and_module(ovbd->ovbd_disk) : NULL;
	mutex_unlock(&ovbd_devices_mutex);

	if (new)
		*part = 0;

	return kobj;
}

static int __init ovbd_init(void)
{
	struct ovbd_device *ovbd, *next;
	int i;

	/*
	 * ovbd module now has a feature to instantiate underlying device
	 * structure on-demand, provided that there is an access dev node.
	 *
	 * (1) if rd_nr is specified, create that many upfront. else
	 *     it defaults to CONFIG_BLK_DEV_RAM_COUNT
	 * (2) User can further extend ovbd devices by create dev node themselves
	 *     and have kernel automatically instantiate actual device
	 *     on-demand. Example:
	 *		mknod /path/devnod_name b 1 X	# 1 is the rd major
	 *		fdisk -l /path/devnod_name
	 *	If (X / max_part) was not already created it will be created
	 *	dynamically.
	 */

	if (register_blkdev(RAMDISK_MAJOR, "ramdisk"))
		return -EIO;

	if (unlikely(!max_part))
		max_part = 1;

	for (i = 0; i < rd_nr; i++) {
		ovbd = ovbd_alloc(i);
		if (!ovbd)
			goto out_free;
		list_add_tail(&ovbd->ovbd_list, &ovbd_devices);
	}

	/* point of no return */

	list_for_each_entry(ovbd, &ovbd_devices, ovbd_list)
		add_disk(ovbd->ovbd_disk);

	blk_register_region(MKDEV(RAMDISK_MAJOR, 0), 1UL << MINORBITS,
				  THIS_MODULE, ovbd_probe, NULL, NULL);

	pr_info("ovbd: module loaded\n");
	return 0;

out_free:
	list_for_each_entry_safe(ovbd, next, &ovbd_devices, ovbd_list) {
		list_del(&ovbd->ovbd_list);
		ovbd_free(ovbd);
	}
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");

	pr_info("ovbd: module NOT loaded !!!\n");
	return -ENOMEM;
}

static void __exit ovbd_exit(void)
{
	struct ovbd_device *ovbd, *next;

	list_for_each_entry_safe(ovbd, next, &ovbd_devices, ovbd_list)
		ovbd_del_one(ovbd);

	blk_unregister_region(MKDEV(RAMDISK_MAJOR, 0), 1UL << MINORBITS);
	unregister_blkdev(RAMDISK_MAJOR, "ramdisk");

	pr_info("ovbd: module unloaded\n");
}

module_init(ovbd_init);
module_exit(ovbd_exit);

