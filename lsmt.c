#include <linux/fs.h>
#include <asm/segment.h>
//#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/lz4.h>
#include "overlay_vbd.h"


static uint64_t segment_end(const void /* const struct segment */ *m) {
  const struct segment *s = (const struct segment *)m;
  return s->offset + s->length;
}


void forward_offset_to(void *m, uint64_t x, int8_t type) {
  struct segment *s = (struct segment *)m;
  ASSERT(x >= s->offset);
  uint64_t delta = x - s->offset;
  s->offset = x;
  s->length -= delta;
  if (type == TYPE_SEGMENT_MAPPING) {
    struct segment_mapping *tmp = (struct segment_mapping *)m;
    if (!tmp->zeroed) {
      tmp->moffset += delta;
    }
  }
}

void backward_end_to(void *m, uint64_t x) {
  struct segment *s = (struct segment *)m;
  if (x <= s->offset) {
    printk("%lu > %lu is FALSE", x, s->offset);
  }

  s->length = x - s->offset;
}

static void trim_edge(void *m, const struct segment *bound_segment,
                      uint8_t type) {
  if (((struct segment *)m)->offset < bound_segment->offset) {
    forward_offset_to(m, bound_segment->offset, type);
  }
  if (segment_end(m) > segment_end(bound_segment)) {
    backward_end_to(m, segment_end(bound_segment));
  }
}

const struct segment_mapping *
ro_index_lower_bound(const struct lsmt_ro_index *index, uint64_t offset) {
  const struct segment_mapping *l = index->pbegin;
  const struct segment_mapping *r = index->pend - 1;
  int ret = -1;
  while (l <= r) {
    int m = ((l - index->pbegin) + (r - index->pbegin)) >> 1;
    const struct segment_mapping *cmp = index->pbegin + m;
    if (offset >= segment_end(cmp)) {
      ret = m;
      l = index->pbegin + (m + 1);
    } else {
      r = index->pbegin + (m - 1);
    }
  }
  const struct segment_mapping *pret = index->pbegin + (ret + 1);
  if (pret >= index->pend) {
    return index->pend;
  } else {
    return pret;
  }
}

int ro_index_lookup(const struct lsmt_ro_index *index,
                    const struct segment *query_segment,
                    struct segment_mapping *ret_mappings, size_t n) {
  if (query_segment->length == 0)
    return 0;
  const struct segment_mapping *lb =
      ro_index_lower_bound(index, query_segment->offset);
  int cnt = 0;
  const struct segment_mapping *it = lb;
  for (; it != index->pend; it++) {
    if (it->offset >= segment_end(query_segment))
      break;
    ret_mappings[cnt++] = *it;
    if (cnt == n)
      break;
  }
  if (cnt == 0)
    return 0;
  trim_edge(&ret_mappings[0], query_segment, TYPE_SEGMENT_MAPPING);
  if (cnt > 1) {
    trim_edge(&ret_mappings[cnt - 1], query_segment, TYPE_SEGMENT_MAPPING);
  }
  return cnt;
}

size_t ro_index_size(const struct lsmt_ro_index *index) {
  return index->pend - index->pbegin;
}

struct lsmt_ro_index *
build_memory_index(const struct segment_mapping *pmappings, size_t n,
                    uint64_t moffset_begin, uint64_t moffset_end, bool copy) {
  struct lsmt_ro_index *ret = NULL;
    int index_size = sizeof(struct lsmt_ro_index);
    if (copy) {
      index_size += sizeof(struct lsmt_ro_index) * n;
    }
    ret = (struct lsmt_ro_index *)kmalloc(index_size, GFP_KERNEL);
    if (!ret) {
      return NULL;
    }
    if (!copy) {
      ret->pbegin = pmappings;
      ret->pend = pmappings + n;
    } else {
      memcpy(ret->mapping, pmappings, n * sizeof(struct segment_mapping));
      ret->pbegin = ret->mapping;
      ret->pend = ret->mapping + n;
    }
  return NULL;
};

bool read_index(struct ovbd_device* odev, size_t index_offset, size_t index_size) {
   struct segment_mapping *indexes;
   size_t ret, i;
   size_t dst_size;
   dst_size = sizeof(struct segment_mapping) * index_size;
   printk("dst size %u", sizeof(struct segment_mapping));
   printk("reading index %u, decompress_size %u", index_size, dst_size);
   indexes = decompress_by_addr(odev, index_offset, dst_size);
  
   if (indexes == NULL) {
	   printk("Can not decompress address %x", index_offset);
	   return false;
   }

   for (i = 0; i < index_size; i++) {
      	   
      printk("offset[%u] = %lu, length %u ", i, indexes[i].offset, indexes[i].length);
      printk("moffset[%u] = %lu, zeroed = %u, tag = %u", i, indexes[i].moffset, indexes[i].zeroed, indexes[i].tag);
   }

   kfree(indexes);
   return true;
}

bool load_lsmt(struct ovbd_device* odev , struct file* fp, size_t decompressed_size, bool ownership) {
   size_t ret, i;
   struct lsmt_ht* pht;
   struct lsmt_ro_index *pi = NULL;
   struct segment_mapping *p = NULL;
   struct segment_mapping *it = NULL;
   unsigned char* buffer; 
   loff_t pos = ZF_SPACE;
   loff_t tailer_jp = 0;
   loff_t length = 0;
   loff_t remain = 0;

   printk("load_lsmt decompressed_size %u", decompressed_size);
   buffer = kmalloc(HT_SPACE, GFP_KERNEL);
   memset(buffer, 0, HT_SPACE);

   ret = decompress_one_page(odev, buffer, 0 );
   if (!ret) {
	   printk("error loading header %u", ret);
	   return false;
   }

   //ret = kernel_read(fp, buffer, HT_SPACE, &pos);
   pht = (struct lsmt_ht*) buffer;
   
   printk("after read header: size = %u, flag = %u, index_size = %u, index_offset = %u, virtual_size = %u",
		   pht->size, pht->flags, pht->index_size, pht->index_offset, pht->virtual_size);

   if ( decompressed_size % HT_SPACE) {
       remain = decompressed_size % HT_SPACE;
       printk("load_lsmt: decompressed_size = %lu, remain = %lu", decompressed_size, remain);
       tailer_jp = odev->jt_size - 3;
   } else {
       remain = 0;
       tailer_jp = odev->jt_size - 2;
   }

   printk("tailer_jp = %u", tailer_jp);
   
/*   printk("last offset is %u", odev->jump_table[odev->jt_size - 1]);

   for (i = 0; i < odev->jt_size ; i++) {
	   printk ( "jump_table[%u] = %u ", i,  odev->jump_table[i]);
//	   decompress_to(odev, buffer, HT_SPACE*i, HT_SPACE, (loff_t*) &ret);
   }
*/
   ret = decompress_by_jp(odev, buffer, tailer_jp);
   if (!ret) {
	   printk("decompress tailer failed");
	   return false;
   }
   
   pht = (struct lsmt_ht*) (buffer + remain );
   //uint64_t *tmp = (uint64_t *) (buffer + remain );
   //uint32_t *tmp2 = (uint32_t *) (buffer + remain );

   //printk("offset + remain %lu = %lu", remain, tmp );
   printk("after read tailer: size = %lu, flag = %u, index_size = %u, index_offset = %lu, virtual_size = %lu",
		   pht->size, pht->flags, pht->index_size, pht->index_offset, pht->virtual_size);

   read_index(odev, pht->index_offset, pht->index_size);
   /*
   for (i = 0; i < 16; i++) {
	printk("get uint64_t buffer[%u] = %lu", i, *( (uint64_t*) (tmp + i)) );
   }
   printk("now 32 bit ");
   printk("..................................................................");
   for (i = 0; i < (1536 / 4); i++) {
	printk("get uint32_t buffer[%u] = %u", i, *( (uint32_t*) (buffer + i*4)) );
   }
   */

  /* size_t file_size = filelen;
   loff_t tailer_offset = file_size - HT_SPACE - ZF_SPACE;
   loff_t index_offset = pht->index_offset;
   printk("load_lsmt: index_offset %u", index_offset);
   ret = kernel_read(fp, buffer, HT_SPACE, &tailer_offset);
   */
   if ( ret < HT_SPACE) {
      printk("loading file failed");
      return false;
   }

   kfree(buffer);
   return true;
}

