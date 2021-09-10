#include <linux/fs.h>
#include <asm/segment.h>
//#include <asm/uaccess.h>
#include <linux/buffer_head.h>
#include <linux/module.h>
#include <linux/moduleparam.h>

MODULE_LICENSE("GPL");

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

unsigned int file_read(struct file *file, unsigned long long offset, unsigned char *data, unsigned int size) 
{
    unsigned int i;
    unsigned int ret = kernel_read(file, offset, data, size);
    if (ret != size) {
       printk("reading data failed at %d", offset);
    }
    return ret;
}   

struct zfile_ro_file* open_zfile(const char *path, bool ownership) {
   printk ("Trying to open zfile %s", path);
   unsigned int ret;
   unsigned char* header;
   struct file* fp = file_open( path, 0, 644);
   if (!fp) {
	   printk("Canot open zfile %s\n", path);
	   return NULL;
   }
  
   ret = file_read (fp, 0, header, 512);
   if (ret != 512) {
	   printk ("Error, reading file failed\n");
	   return NULL;
   }

}
