#ifndef __SIMPLE_H__
#define __SIMPLE_H__

#define SIMPLEFS_MAGIC 0x10032013
#define SIMPLEFS_DEFAULT_BLOCK_SIZE 4096
#define SIMPLEFS_FILENAME_MAXLEN 24
#define SIMPLEFS_START_INO 10
#define SIMPLEFS_ROOTDIR_INO 1
/**
 * Reserver inodes for super block, inodestore
 * and datablock
 */
#define SIMPLEFS_RESERVED_INODES 3

#ifdef SIMPLEFS_DEBUG
#define sfs_trace(fmt, ...) {                       \
	printk(KERN_ERR "[simplefs] %s +%d:" fmt,       \
	       __FILE__, __LINE__, ##__VA_ARGS__);      \
}
#define sfs_debug(level, fmt, ...) {                \
	printk(level "[simplefs]:" fmt, ##__VA_ARGS__); \
}
#else
#define sfs_trace(fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#define sfs_debug(level, fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

/* Hard-coded inode number for the root directory */
#define SIMPLEFS_ROOTDIR_INODE_NUMBER		1

#define SIMPLEFS_LAST_INODE_NUMBER		64

/* The disk block where super block is stored */
#define SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER	0

/* The disk block where the inodes are stored */
#define SIMPLEFS_INODESTORE_BLOCK_NUMBER	1

/* The disk block where the name+inode_number pairs of the
 * contents of the root directory are stored */
#define SIMPLEFS_START_DATABLOCK_NUMBER		2
#define SIMPLEFS_END_DATABLOCK_NUMBER		66

/* The name+inode_number pair for each file in a directory.
 * This gets stored as the data for a directory */
struct simplefs_dir_record {
	uint64_t inode_no;
	char filename[SIMPLEFS_FILENAME_MAXLEN];
};

#define SIMPLEFS_VDIR 2
#define SIMPLEFS_VREG 1

struct simplefs_inode {
	mode_t mode;
	uint16_t i_nlink;
	uint64_t inode_no;
	uint64_t data_block_number;

	union {
		uint64_t file_size;
		uint64_t dir_children_count;
	};
};

struct simplefs_inode_info {
	uint64_t data_block_number;
	union {
		uint64_t file_size;
		uint64_t dir_children_count;
	};
	struct inode vfs_inode;
};

#define SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED 64
/* min (
		SIMPLEFS_DEFAULT_BLOCK_SIZE / sizeof(struct simplefs_inode),
		sizeof(uint64_t) //The free_blocks tracker in the sb
 	); */

/* FIXME: Move the struct to its own file and not expose the members
 * Always access using the simplefs_sb_* functions and
 * do not access the members directly */
struct simplefs_super_block {
	uint64_t version;
	uint64_t magic;
	uint64_t block_size;

	/* FIXME: This should be moved to the inode store and not part of the sb */
	uint64_t inodes_count;

	//uint64_t free_blocks;
	int64_t imap;
	int64_t dmap;

	char padding[SIMPLEFS_DEFAULT_BLOCK_SIZE - (6 * sizeof(uint64_t))];
};

struct simplefs_sb_info {
	struct buffer_head *sbh;
	struct simplefs_super_block *sb;
	struct mutex simplefs_lock;
};

static inline struct simplefs_inode_info *simplefs_i(struct inode *inode)
{
	return container_of(inode, struct simplefs_inode_info, vfs_inode);
}

static inline struct simplefs_sb_info *simplefs_sb(struct super_block *sb)
{
	return sb->s_fs_info;
}

#define SIMPLEFS_INODES_PER_BLOCK ((SIMPLEFS_DEFAULT_BLOCK_SIZE)/(sizeof(struct simplefs_inode)))
#define simplefs_test_and_clear_bit(nr, addr) \
        __test_and_clear_bit((nr), (unsigned long *)(addr))

/* inode.c */
extern struct inode *simplefs_iget(struct super_block *sb, unsigned long ino);
extern void simplefs_dump_imap(const char *, struct super_block *);

/* file.c */
extern const struct inode_operations simplefs_file_inops;
extern const struct file_operations simplefs_file_operations;
extern const struct address_space_operations simplefs_aops;

/* dir.c */
extern const struct inode_operations simplefs_dir_inops;
extern const struct file_operations simplefs_dir_operations;

extern const struct inode_operations simplefs_symlink_inops;

#endif
