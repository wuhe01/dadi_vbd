#ifndef __LSMT_RO_H__
#define __LSMT_RO_H__

#undef __KERNEL__
#ifndef HBDEBUG
#define HBDEBUG (1)
#endif

#include <linux/err.h>
#include <linux/printk.h>
#include <linux/uuid.h>

#define PRINT_INFO(fmt, ...)                                     \
	do { if ((HBDEBUG)) \
	printk(KERN_INFO fmt, ## __VA_ARGS__);} while (0)

#define PRINT_ERROR(fmt, ...)                                          \
	do { if ((HBDEBUG)) \
	printk(KERN_ERR fmt, ## __VA_ARGS__);} while (0)
#define ASSERT(exp)						\
	BUG_ON(exp)

#define ALIGNED_MEM(name, size, alignment)  \
        char __buf##name[(size) + (alignment)]; \
        char *name = (char *)(((uint64_t)(__buf##name + (alignment) - 1)) & \
                        ~((uint64_t)(alignment) - 1));

#define REVERSE_LIST(type, begin, back) { type *l = (begin); type *r = (back);\
        while (l<r){ type tmp = *l; *l = *r; *r = tmp; l++; r--; }} \

typedef uint16_t inttype;
static const inttype inttype_max = 1<<16 - 1;
static const int DEFAULT_PART_SIZE = 16; // 16 x 4k = 64k
static const int DEFAULT_LSHIFT = 16;    // save local minimum of each part.
static const uint32_t HT_SPACE = 4096;
static const uint32_t ZF_SPACE = 512;

const static size_t MAX_READ_SIZE     = 65536; // 64K
const static size_t BUF_SIZE = 512;
const static uint32_t NOI_WELL_KNOWN_PRIME = 100007;
const static uint32_t SPACE = 512;
static const uint32_t FLAG_SHIFT_HEADER = 0; // 1:header     0:trailer
static const uint32_t FLAG_SHIFT_TYPE = 1;   // 1:data file, 0:index file
static const uint32_t FLAG_SHIFT_SEALED = 2; // 1:YES,       0:NO

const static uint8_t MINI_LZO   = 0;
const static uint8_t LZ4        = 1;
const static uint8_t ZSTD       = 2;
const static uint32_t DEFAULT_BLOCK_SIZE = 4096;//8192;//32768;

struct compress_options {

  uint32_t block_size;
  uint8_t type;
  uint8_t level;
  uint8_t use_dict;
  uint8_t args;
  uint32_t dict_size;
  uint32_t unknown;
  uint8_t verify;
  uint8_t padding[];
} __attribute__ ((packed));

struct _UUID {
  uint32_t a;
  uint16_t b, c, d;
  uint8_t e[6];
} __attribute__((packed));

static uint64_t *MAGIC0 = (uint64_t *)"ZFile\0\1";

static struct _UUID MAGIC1 = {
    0x696a7574, 0x792e, 0x6679, 0x4140, {0x6c, 0x69, 0x62, 0x61, 0x62, 0x61}};

struct zfile_ht {
  uint64_t magic0;
  uuid_t magic1;
  // offset 24, 28
  uint32_t size;  //= sizeof(HeaderTrailer);
  uint32_t flags; //= 0;
  uint64_t padding;
  // offset 32, 40, 48
  uint64_t index_offset; // in bytes
  uint64_t index_size;   // 

  uint64_t raw_data_size;
  uint64_t reserved_0; // fix compatibility with HeaderTrailer in release_v1.0

  struct compress_options opt;
  uint8_t pad[4];
}  __packed; 


struct jump_table {
   uint64_t partial_offset; // 48 bits logical offset + 16 bits partial minimum
   uint16_t delta;
};

struct zfile_file {
        struct zfile_ro_index *m_index;
	bool is_header;
	bool is_sealed;
	bool is_data_file;
	struct zfile_ht m_ht;
	uint64_t m_vsize;
        bool m_ownership;       
        size_t m_files_count;
        size_t MAX_IO_SIZE;
        void* m_files[0];
} __attribute__((packed));

#define TYPE_SEGMENT         0
#define TYPE_SEGMENT_MAPPING 1
#define TYPE_FILDES          2
#define TYPE_LSMT_RO_INDEX   3

struct lsmt_ht {
  uint64_t magic0;
  struct _UUID magic1;
  // offset 24, 28
  uint32_t size;  //= sizeof(HeaderTrailer);
  uint32_t flags; //= 0;
  // offset 32, 40, 48
  uint64_t index_offset; // in bytes
  uint64_t index_size;   // # of SegmentMappings
  uint64_t virtual_size; // in bytes
} __attribute__((packed));


struct segment {                             /* 8 bytes */
        uint64_t offset : 50; // offset (0.5 PB if in sector)
        uint32_t length : 14; // length (8MB if in sector)
} /* __attribute__((packed)); */;

struct segment_mapping {                             /* 8 + 8 bytes */
        uint64_t offset : 50; // offset (0.5 PB if in sector)
        uint32_t length : 14;
        uint64_t moffset : 55; // mapped offset (2^64 B if in sector)
        uint32_t zeroed : 1;   // indicating a zero-filled segment
        uint8_t tag;
};

struct lsmt_ro_index {
        const struct segment_mapping *pbegin;
        const struct segment_mapping *pend;
        struct segment_mapping mapping[0];
};


/*
 * Each block ovbd device has a radix_tree ovbd_pages of pages that stores
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

	uint64_t partial_offset[ 200 ];
        uint16_t deltas[1<<16 - 1];
	uint16_t block_size;

	size_t *jump_table;
	struct file* compressed_fp;
 	unsigned char* path;
	bool initialized ;

};

// open a zfile layer
bool open_zfile( struct ovbd_device *odev, const char* file,  bool ownership);
bool decompress_to( struct ovbd_device *odev, void* dst, loff_t start, loff_t length, loff_t* len);
bool load_lsmt( struct ovbd_device *odev, struct file* file, size_t filelen, bool ownership);

/*struct file *file_open(const char *path, int flags, int rights)
void  file_close(struct file *file)
size_t get_file_size(struct file* path) ;
size_t file_read(struct file *file, void *buf, size_t count, loff_t *pos);
*/
#endif
