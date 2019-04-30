#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/uio.h>
#include <linux/dax.h>
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

static int simplefs_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, simplefs_get_block);
}

static int simplefs_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, simplefs_get_block);
}

static int simplefs_readpages(struct file *file, struct address_space *mapping,
		struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, simplefs_get_block);
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

static ssize_t simplefs_direct_IO(struct kiocb *iocb, struct iov_iter *iter)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	size_t count = iov_iter_count(iter);
	loff_t offset = iocb->ki_pos;
	ssize_t ret;

	ret = blockdev_direct_IO(iocb, inode, iter, simplefs_get_block);
	if (ret < 0 && iov_iter_rw(iter) == WRITE)
		simplefs_write_failed(mapping, offset + count);
	return ret;
}

static int simplefs_dax_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return dax_writeback_mapping_range(mapping,
			mapping->host->i_sb->s_bdev, wbc);
}

const struct address_space_operations simplefs_aops = {
	.readpage		= simplefs_readpage,
	.readpages		= simplefs_readpages,
	.writepage		= simplefs_writepage,
	.writepages		= simplefs_writepages,
	.write_begin		= simplefs_write_begin,
	.write_end		= generic_write_end,
	.bmap			= simplefs_bmap,
	.direct_IO		= simplefs_direct_IO,
	.migratepage            = buffer_migrate_page,
	.is_partially_uptodate  = block_is_partially_uptodate,
	.error_remove_page      = generic_error_remove_page,
};

const struct address_space_operations simplefs_dax_aops = {
	.writepages             = simplefs_dax_writepages,
	.direct_IO              = noop_direct_IO,
	.set_page_dirty         = noop_set_page_dirty,
	.invalidatepage         = noop_invalidatepage,
};

const struct inode_operations simplefs_file_inops;
