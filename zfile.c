#include <linux/fs.h>
#include <asm/segment.h>
//#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include "zfile.h"

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

bool open_zfile(const char* path, bool ownership) {
   printk ("Trying to open zfile %s", path);
   unsigned int ret;
   unsigned int i;
   unsigned char *header;
   struct file* fp = file_open( path, 0, 644);
   if (!fp) {
	   printk("Canot open zfile %s\n", path);
	   return false;
   }

   header = kmalloc(HT_SPACE, GFP_KERNEL);
   for ( i = 0 ; ; i++) {
	 loff_t pos = i * HT_SPACE;
	 size_t res = file_read(fp, header, HT_SPACE, &pos);
         if (res > 0) {
		 printk ("data [%s] ;\n", header);
	 } else {
		 printk ("error happened\n");
		 return false;
	 }
   }
   return true;
}
