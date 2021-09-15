#include <linux/fs.h>
#include <asm/segment.h>
//#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/lz4.h>
#include "overlay_vbd.h"

struct file *file_open(const char *path, int flags, int rights) 
{
    struct file *fp = NULL;
    fp = filp_open(path, O_RDONLY, 0);
    if (IS_ERR(fp)) {
         printk("Cannot open the file %ld\n", PTR_ERR(fp));
         return NULL;
    }
    printk("Opened the file %s", path);
    return fp;
}

void file_close(struct file *file)
{
    filp_close(file, NULL);
}

size_t file_read(struct file *file, void *buf, size_t count, loff_t *pos)  
{
    unsigned int ret = kernel_read(file, buf, count, pos);
    if (!ret) {
       printk("reading data failed at %d", pos);
    }
    return ret;
}  


size_t get_file_size(const char* path) {
   struct kstat *stat;
   size_t size;
   mm_segment_t fs;

   fs = get_fs();
        set_fs(KERNEL_DS);
        
        stat =(struct kstat *) kmalloc(sizeof(struct kstat), GFP_KERNEL);
        if (!stat)
                return ERR_PTR(-ENOMEM);

        vfs_stat(path, stat);
        size = stat->size;
   return size;
   set_fs(fs);
   kfree(stat);
}

size_t get_range ( struct ovbd_device *odev, size_t idx) {

         // return BASE  + idx % (page_size) * local_minimum + sum(delta - local_minimum)
     off_t part_idx = idx / DEFAULT_PART_SIZE;
     off_t inner_idx = idx & (DEFAULT_PART_SIZE - 1);
     uint16_t local_min = odev->partial_offset[part_idx] & ((1 << DEFAULT_LSHIFT) - 1);
     uint64_t part_offset = odev->partial_offset[part_idx] >> DEFAULT_LSHIFT;
     off_t ret = part_offset + odev->deltas[idx] + (inner_idx)*local_min;
     printk("get_range : %d , %d", idx, ret);
     return ret;

}

int get_blocks_length( struct ovbd_device* odev, size_t begin, size_t end) {
    printk("begin: %d, end %d", begin, end);
    return (get_range( odev, end) - get_range(odev, begin));
}

bool decompress_to( struct ovbd_device *odev, void* dst, loff_t start, loff_t length,  loff_t *dlen) {
   printk ("get decrompess length [%d - %d]", start, start + length );
   loff_t begin, range;
   range = get_range(odev, start);
   begin = start + ZF_SPACE;

//range = end - start; 
   
   //end = begin + length;
   
   if (odev->path == NULL) {
	   printk("device not initiated yet");
	   return false;
   } else {
	   printk("Using file (%s) as backend", odev->path);
   }

   unsigned char *src_buf; 
   src_buf = kmalloc(range,  GFP_KERNEL);
   memset(src_buf, 0, range);

   struct file* fp = file_open( odev->path, 0, 644);
   if (!fp) {
	   printk("Canot open zfile %s\n", odev->path);
	   return false;
   }
  
   printk("src_buf length %d, begin %d", length, begin);
   
   size_t ret = file_read(fp, src_buf, range, &begin);
   if (ret !=  range ) {
	   printk( "Did read enough data, something may be wrong %d", ret);
	   return false;
   }
   printk("loaded %d src data at offset [%d - %d]", ret, start, start + range); 

   ret = LZ4_decompress_safe(src_buf, (unsigned char *)dst, range, *dlen);
   *dlen = ret;
   printk("Decompressed [%d]", ret);

   kfree(src_buf);
   
   return true;
}

bool test_decompress(struct ovbd_device* odev, size_t partial_size, size_t deltas_size) {
  size_t i, j;
  loff_t start_offset, stop_offset;
  printk ( "begining decompress");
  for (i = 0; i< partial_size; i ++) {
	  for (j =0 ; j< deltas_size; j++) {
                  if (j == 0) {
                          start_offset = 0;
                  } else {
                          start_offset = get_range(odev, j-1 );
                  }
                  stop_offset = get_range(odev, j);
	   printk(" get test range [%d -  %d]", start_offset, stop_offset);
	   size_t src_blk_size = odev->block_size;
	   unsigned char *src_buf;
	   unsigned char *dst_buf;

	   src_buf = kmalloc(stop_offset - start_offset, GFP_KERNEL);
	   dst_buf = kmalloc(MAX_READ_SIZE, GFP_KERNEL);
           struct file* fp = file_open( "/test.c", 0, 644);
           size_t ret = file_read(fp, src_buf, stop_offset - start_offset, &start_offset);
	   printk("loaded %d src data at offset [%d]", ret, &start_offset);

	   LZ4_decompress_safe(src_buf, (unsigned char *)dst_buf, odev->block_size, MAX_READ_SIZE);
	   printk("Decompressed [%s]", dst_buf);

	   kfree(src_buf);
	   kfree(dst_buf);
	}
  }
  return true;
}

bool build_jump_table(struct ovbd_device* odev, uint32_t *jt_saved, struct zfile_ht* pht) {
  size_t i;
  int part_size = DEFAULT_PART_SIZE;
  off_t offset_begin = pht->opt.dict_size + ZF_SPACE;
  size_t n = pht->index_size ;
  uint16_t local_min;
  off_t raw_offset = offset_begin;
  uint16_t lshift = DEFAULT_LSHIFT;
  uint16_t last_delta;
//  printk("offset_begin %d", offset_begin);
  
  local_min = 0;
  odev->partial_offset[0] = (raw_offset << lshift) + local_min;
  odev->deltas[0] = 0;
 // printk("partial_offset %d", odev->partial_offset[0]);

  uint16_t partial_size = 1;
  uint16_t deltas_size = 1;


    for (i = 1; i < (size_t) n + 1 ; ++i) {
          //PRINT_INFO(" jump_table[%d]:  %d", i, (uint32_t) (jt_saved[i-1]));
 //         odev->jump_table[i] = odev->jump_table[i-1] + jt_saved[i - 1];
//	  printk("Load jump_table ...  [%d, %d]", odev->jump_table[i-1], odev->jump_table[i]);

          last_delta = 0;
          if (( i % part_size) == 0 ) {
                  local_min = 1<<16 - 1;
                  printk(" local_min  %d", local_min);
		  size_t j ;
                  for (j = i; j < min( (size_t)(n + 1), (size_t)(i + part_size) ); j ++ )
                          local_min = min( (uint16_t)jt_saved[j - 1], (uint16_t)local_min);
                  odev->partial_offset[i % part_size]  = (raw_offset << lshift) + local_min;
                  partial_size++;
                  odev->deltas[deltas_size++] = 0;
                  last_delta = 0;

                  continue;
          }
          odev->deltas[deltas_size++] = odev->deltas[i-1] + jt_saved[i-1] - local_min;
          last_delta = last_delta + odev->deltas[i-1] - local_min;

//          printk("delta %d, iterated %i", odev->deltas[i], i);

  }
  
  //test_decompress(odev, partial_size, deltas_size);

  return true;

}

bool open_zfile(struct ovbd_device* odev , const char* path, bool ownership) {
   unsigned int ret, i;
   struct zfile_ht* zht;
   unsigned char* header_tail; 
   uint32_t* jt_saved;
   size_t jt_size = 0;
   loff_t pos = 0;
   odev->initialized = false;

   struct file* fp = file_open( path, 0, 644);
   if (!fp) {
	   printk("Canot open zfile %s\n", path);
	   return false;
   }

   header_tail = kmalloc(HT_SPACE, GFP_KERNEL);
   memset(header_tail, 0, HT_SPACE);
   ret = file_read(fp, header_tail, ZF_SPACE, &pos);
   zht = (struct zfile_ht*) header_tail;
   
   if ( ret < (ssize_t) ZF_SPACE) {
	   printk("failed to load header \n");
   } 

   odev->compressed_fp = fp;
   odev->path = kmalloc( strlen(path), GFP_KERNEL);
   memset(odev->path, 0, strlen(path));
   strncpy( odev->path, path, strlen(path));
   printk("opened zfile as %d", fp);
   

   size_t file_size = get_file_size(path);
   loff_t tailer_offset = file_size - ZF_SPACE;
   ret = file_read(fp, header_tail, ZF_SPACE, &tailer_offset);
   odev->block_size = zht->opt.block_size;

   jt_size = ((uint64_t)zht->index_size) * sizeof(uint32_t) ;
   printk ("get index_size %d, index_offset %d", jt_size, zht->index_offset);

   jt_saved = kmalloc(jt_size, GFP_KERNEL);
   memset(jt_saved, 0, jt_size);
   memset(odev->partial_offset, 0, 200);
   memset(odev->deltas, 0, (1<<16)-1);

   loff_t index_offset = zht->index_offset;
   ret = file_read(fp, jt_saved, jt_size, &index_offset);
   for (i =0 ; i < 4; i++) 
	   printk("jt_saved[%d] = %d", i, jt_saved[i]);

   odev->jump_table = kmalloc(jt_size, GFP_KERNEL);
   memset(odev->jump_table, 0, jt_size);

   build_jump_table(odev, jt_saved, zht);

   //load_lsmt(odev, fp, file_size, ownership);
   
   odev->initialized = true;
   kfree(header_tail);
   return true;
}



