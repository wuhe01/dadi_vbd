#include <linux/fs.h>
#include <asm/segment.h>
//#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/lz4.h>
#include "overlay_vbd.h"


bool load_lsmt(struct ovbd_device* odev , struct file* fp, size_t filelen, bool ownership) {
   unsigned int ret, i;
   struct lsmt_ht* pht;
   struct lsmt_ro_index *pi = NULL;
   struct segment_mapping *p = NULL;
   struct segment_mapping *it = NULL;
   unsigned char* header_tail; 
   loff_t pos = ZF_SPACE;

   header_tail = kmalloc(HT_SPACE, GFP_KERNEL);
   memset(header_tail, 0, HT_SPACE);
   ret = kernel_read(fp, header_tail, HT_SPACE, &pos);
   pht = (struct lsmt_ht*) header_tail;
   
   if ( ret < (ssize_t) HT_SPACE) {
       printk("failed to load header \n");
   } 

   size_t file_size = filelen;
   loff_t tailer_offset = file_size - HT_SPACE - ZF_SPACE;
   loff_t index_offset = pht->index_offset;
   printk("index_offset %d", index_offset);
   ret = kernel_read(fp, header_tail, HT_SPACE, &tailer_offset);
   
   kfree(header_tail);
   return true;
}
