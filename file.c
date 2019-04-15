#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include "simple.h"

const struct file_operations simplefs_file_operations = {
	.llseek         = generic_file_llseek,
	.read_iter      = generic_file_read_iter,
	.write_iter     = generic_file_write_iter,
	.mmap           = generic_file_mmap,
	.fsync          = generic_file_fsync,
	.splice_read    = generic_file_splice_read,
};

static int simplefs_get_block(struct inode *inode, sector_t block,
		struct buffer_head *bh_result, int create)
{
	unsigned long phys;
	struct super_block *sb = inode->i_sb;
	struct simplefs_inode_info *sinfo = simplefs_i(inode);

	phys = sinfo->data_block_number + block;
	map_bh(bh_result, sb, phys);

	return 0;
}

static int simplefs_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, simplefs_get_block, wbc);
}

static int simplefs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, simplefs_get_block);
}

static void simplefs_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size)
		truncate_pagecache(inode, inode->i_size);
}

static int simplefs_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, unsigned flags,
		struct page **pagep, void **fsdata)
{
	int ret;
	struct inode *inode = mapping->host;
	struct simplefs_inode_info *sinfo = simplefs_i(inode);

	ret = block_write_begin(mapping, pos, len, flags, pagep,
			simplefs_get_block);
	if (unlikely(ret))
		simplefs_write_failed(mapping, pos + len);
	sinfo->file_size += len;

	return ret;
}

static sector_t simplefs_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, simplefs_get_block);
}

const struct address_space_operations simplefs_aops = {
	.readpage       = simplefs_readpage,
	.writepage      = simplefs_writepage,
	.write_begin    = simplefs_write_begin,
	.write_end      = generic_write_end,
	.bmap           = simplefs_bmap,
};

const struct inode_operations simplefs_file_inops;
