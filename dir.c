#include <linux/time.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/sched.h>
#include "simple.h"

static int simplefs_add_entry(struct inode *dir, const struct qstr *child, int ino);
static struct buffer_head *simplefs_find_entry(struct inode *dir,
		const struct qstr *child,
		struct simplefs_dir_record **res_dir);
static int simplefs_delete_entry(struct buffer_head *bh, struct inode *dir,
		struct simplefs_dir_record *drecord);

static int simplefs_readdir(struct file *f, struct dir_context *ctx)
{
	struct inode *dir = file_inode(f);
	struct buffer_head *bh;
	struct simplefs_dir_record *drecord;
	struct simplefs_inode_info *sinfo = simplefs_i(dir);
	int i;

	if (ctx->pos & (sizeof(struct simplefs_dir_record) - 1)) {
		printk(KERN_ERR "Bad f_pos=%08lx for %s:%08lx\n",
				(unsigned long)ctx->pos,
				dir->i_sb->s_id, dir->i_ino);
		return -EINVAL;
	}

	bh = sb_bread(dir->i_sb, sinfo->data_block_number);
	if (!bh)
		return -EIO;

	while (ctx->pos < dir->i_size) {
		drecord = (struct simplefs_dir_record *)bh->b_data;
		for (i = 0; i < sinfo->dir_children_count; i++)
		{
			int size = strnlen(drecord->filename, SIMPLEFS_FILENAME_MAXLEN);
			dir_emit(ctx, drecord->filename, size, drecord->inode_no, DT_UNKNOWN);
			ctx->pos += sizeof(struct simplefs_dir_record);
			drecord++;
		}
	}
	brelse(bh);

	return 0;
}

const struct file_operations simplefs_dir_operations = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = simplefs_readdir,
	.fsync          = generic_file_fsync,
};

const struct inode_operations simplefs_symlink_inops = {
	.get_link       = page_get_link,
}; 

static int simplefs_create_inode(struct inode *dir, struct dentry *dentry, umode_t mode, const void *d)
{
	int err, i;
	struct inode *inode;
	struct super_block *s = dir->i_sb;
	struct simplefs_sb_info *sbinfo = simplefs_sb(s);
	struct simplefs_super_block *sb = sbinfo->sb;
	uint64_t data_block_number = 0;
	unsigned long ino = 0;
	const char *symname = d;

	inode = new_inode(s);
	if (!inode)
		return -ENOMEM;
	mutex_lock(&sbinfo->simplefs_lock);
	for (i = 0; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
	{
		if (sb->imap & (1 << i)) {
			ino = i + SIMPLEFS_ROOTDIR_INODE_NUMBER;
			break;
		}
	}
	if (!ino) {
		err = -ENOSPC;
		goto out;
	}
	sb->imap &= ~(1 << (ino - SIMPLEFS_ROOTDIR_INODE_NUMBER));
	sb->inodes_count++;
	for (i = 0; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
	{
		if (sb->dmap & (1 << i)) {
			data_block_number = i + SIMPLEFS_START_DATABLOCK_NUMBER;
			break;
		}
	}
	if (!data_block_number) {
		err = -ENOSPC;
		goto out;
	}
	sb->dmap &= ~(1 << (data_block_number - SIMPLEFS_START_DATABLOCK_NUMBER));
	mark_buffer_dirty(sbinfo->sbh);
	sync_dirty_buffer(sbinfo->sbh);

	inode_init_owner(inode, dir, mode);
	inode->i_mtime = inode->i_atime = inode->i_ctime = current_time(inode);
	inode->i_blocks = 0;
	inode->i_mapping->a_ops = &simplefs_aops;
	inode->i_ino = ino;
	inode->i_size = 0;
	simplefs_i(inode)->data_block_number = data_block_number;
	simplefs_i(inode)->dir_children_count = 0;

	if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &simplefs_dir_inops;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		inode->i_op = &simplefs_file_inops;
		inode->i_fop = &simplefs_file_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &simplefs_symlink_inops;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &simplefs_aops;
		err = page_symlink(inode, symname, strlen(symname) + 1);
		if (err) {
			inode_dec_link_count(inode);
			goto out;
		}
	}

	insert_inode_hash(inode);
	mark_inode_dirty(inode);

	err = simplefs_add_entry(dir, &dentry->d_name, inode->i_ino);
	if (err) {
		inode_dec_link_count(inode);
		goto out;
	}
	mutex_unlock(&sbinfo->simplefs_lock);
	d_instantiate(dentry, inode);

	return 0;
out:
	mutex_unlock(&sbinfo->simplefs_lock);
	iput(inode);
	return err;
}

static int simplefs_create(struct inode *dir, struct dentry *dentry, umode_t mode, bool excl)
{
	return simplefs_create_inode(dir, dentry, mode, NULL);
}

static struct dentry *simplefs_lookup(struct inode *dir, struct dentry *dentry,
		unsigned int flags)
{
	struct inode *inode = NULL;
	struct buffer_head *bh;
	struct simplefs_dir_record *drecord;
	struct simplefs_sb_info *sbinfo = simplefs_sb(dir->i_sb);

	mutex_lock(&sbinfo->simplefs_lock);
	if (dentry->d_name.len > SIMPLEFS_FILENAME_MAXLEN)
		return ERR_PTR(-ENAMETOOLONG);

	bh = simplefs_find_entry(dir, &dentry->d_name, &drecord);
	if (bh) {
		unsigned long ino = (unsigned long)(drecord->inode_no);
		brelse(bh);
		inode = simplefs_iget(dir->i_sb, ino);
	}
	mutex_unlock(&sbinfo->simplefs_lock);
	return d_splice_alias(inode, dentry);
}

static int simplefs_link(struct dentry *old, struct inode *dir, struct dentry *new)
{
	struct inode *inode = d_inode(old);
	struct simplefs_sb_info *sbinfo = simplefs_sb(inode->i_sb);
	int err;

	mutex_lock(&sbinfo->simplefs_lock);
	err = simplefs_add_entry(dir, &new->d_name, inode->i_ino);
	if (err) {
		mutex_unlock(&sbinfo->simplefs_lock);
		return err;
	}
	inc_nlink(inode);
	inode->i_ctime = current_time(inode);
	mark_inode_dirty(inode);
	ihold(inode);
	d_instantiate(new, inode);
	mutex_unlock(&sbinfo->simplefs_lock);
	return 0;
}

static int simplefs_unlink(struct inode *dir, struct dentry *dentry)
{
	int error = -ENOENT;
	struct inode *inode = d_inode(dentry);
	struct simplefs_inode_info *sinfo = simplefs_i(inode);
	uint64_t data_block_number = sinfo->data_block_number;
	struct buffer_head *bh;
	struct simplefs_dir_record *drecord;
	struct super_block *s = inode->i_sb;
	struct simplefs_sb_info *sbinfo = simplefs_sb(s);
	struct simplefs_super_block *sb = sbinfo->sb;

	mutex_lock(&sbinfo->simplefs_lock);

	if (inode->i_nlink == 1) {
		sb->inodes_count--;
		sb->imap |= (1 << (inode->i_ino - SIMPLEFS_ROOTDIR_INODE_NUMBER));
		sb->dmap |= (1 << (data_block_number - SIMPLEFS_START_DATABLOCK_NUMBER));
		mark_buffer_dirty(sbinfo->sbh);
		sync_dirty_buffer(sbinfo->sbh);
	}

	bh = simplefs_find_entry(dir, &dentry->d_name, &drecord);
	if (!bh || drecord->inode_no != inode->i_ino)
		goto out;

	if (!inode->i_nlink) {
		printk("unlinking non-existent file %s:%lu (nlink=%d)\n",
				inode->i_sb->s_id, inode->i_ino,
				inode->i_nlink);
		set_nlink(inode, 1);
	}

	error = simplefs_delete_entry(bh, dir, drecord);
	if (error)
		goto out;

	mark_buffer_dirty_inode(bh, dir);
	dir->i_ctime = dir->i_mtime = current_time(dir);
	mark_inode_dirty(dir);
	inode->i_ctime = dir->i_ctime;
	inode_dec_link_count(inode);
	error = 0;
out:
	brelse(bh);
	mutex_unlock(&sbinfo->simplefs_lock);
	return error;
}

static int simplefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	return simplefs_create_inode(dir, dentry, S_IFDIR | mode, NULL);
}

static int simplefs_rmdir(struct inode * dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct simplefs_inode_info *sinfo = simplefs_i(inode);
	int err = -ENOTEMPTY;

	if (!sinfo->dir_children_count)
		err = simplefs_unlink(dir, dentry);
	return err;
}

static int simplefs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
	return simplefs_create_inode(dir, dentry, S_IFLNK | S_IRWXUGO, symname);
}

const struct inode_operations simplefs_dir_inops = {
	.create                 = simplefs_create,
	.lookup                 = simplefs_lookup,
	.link                   = simplefs_link,
	.unlink                 = simplefs_unlink,
	.mkdir			= simplefs_mkdir,
	.rmdir			= simplefs_rmdir,
	.symlink		= simplefs_symlink,
};

static int simplefs_add_entry(struct inode *dir, const struct qstr *child, int ino)
{
	const unsigned char *name = child->name;
	int i, namelen = child->len, err = 0;
	struct buffer_head *ibh, *dbh;
	struct simplefs_dir_record *drecord;
	struct simplefs_inode_info *sinfo = simplefs_i(dir);
	struct simplefs_sb_info *sbinfo = simplefs_sb(dir->i_sb);
	struct simplefs_super_block *sb = sbinfo->sb;
	struct simplefs_inode *sinode;
	uint64_t data_block_number = sinfo->data_block_number;
	uint64_t dir_children_count = sinfo->dir_children_count;

	if (!namelen)
		return -ENOENT;
	if (namelen > SIMPLEFS_FILENAME_MAXLEN)
		return -ENAMETOOLONG;

	if (!data_block_number) {
		for (i = 0; i < SIMPLEFS_MAX_FILESYSTEM_OBJECTS_SUPPORTED; i++)
		{
			if (sb->dmap & (1 << i)) {
				data_block_number = i + SIMPLEFS_START_DATABLOCK_NUMBER;
				break;
			}
		}
		if (!data_block_number) {
			err = -ENOSPC;
			goto out;
		}
	}
	sb->dmap &= ~(1 << (data_block_number - SIMPLEFS_START_DATABLOCK_NUMBER));
	mark_buffer_dirty(sbinfo->sbh);
	sync_dirty_buffer(sbinfo->sbh);

	ibh = sb_bread(dir->i_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	if (!ibh) {
		err = -EIO;
		goto out;
	}
	sinode = (struct simplefs_inode *)(ibh->b_data);
	sinode += (dir->i_ino - SIMPLEFS_ROOTDIR_INODE_NUMBER);
	sinode->data_block_number = data_block_number;
	sinode->dir_children_count++;
	sinfo->dir_children_count = sinode->dir_children_count;
	dir->i_size = sinfo->dir_children_count * sizeof(struct simplefs_dir_record);
	mark_inode_dirty(dir);
	mark_buffer_dirty(ibh);
	sync_dirty_buffer(ibh);
	brelse(ibh);

	dbh = sb_bread(dir->i_sb, data_block_number);
	if (!dbh) {
		err = -EIO;
		goto out;
	}
	drecord = (struct simplefs_dir_record *)(dbh->b_data);

	drecord += dir_children_count;
	drecord->inode_no = ino;
	memcpy(drecord->filename, name, namelen);

	mark_buffer_dirty(dbh);
	sync_dirty_buffer(dbh);
	brelse(dbh);
out:
	return err;
}

static inline int simplefs_namecmp(int len, const unsigned char *name,
		const char *buffer)
{
	if ((len < SIMPLEFS_FILENAME_MAXLEN) && buffer[len])
		return 0;
	return !memcmp(name, buffer, len);
}

static struct buffer_head *simplefs_find_entry(struct inode *dir,
		const struct qstr *child,
		struct simplefs_dir_record **res_dir)
{
	struct buffer_head *bh = NULL;
	struct simplefs_dir_record *drecord;
	struct simplefs_inode_info *sinfo = simplefs_i(dir);
	const unsigned char *name = child->name;
	int namelen = child->len;
	int i;

	*res_dir = NULL;
	if (namelen > SIMPLEFS_FILENAME_MAXLEN)
		return NULL;

	bh = sb_bread(dir->i_sb, sinfo->data_block_number);
	drecord = (struct simplefs_dir_record *)(bh->b_data);

	for (i = 0; i < dir->i_size; i++)
	{
		if ((strlen(drecord->filename) == namelen) && !memcmp(drecord->filename, name, namelen)) {
			*res_dir = drecord;
			return bh;
		}
		drecord++;
	}

	brelse(bh);
	return NULL;
}

static int simplefs_delete_entry(struct buffer_head *bh, struct inode *dir,
		struct simplefs_dir_record *drecord)
{
	int size = 0;
	struct simplefs_inode_info *sinfo = simplefs_i(dir);
	struct simplefs_inode *sinode;
	struct buffer_head *ibh;
	struct simplefs_dir_record *start_drecord;

	start_drecord = (struct simplefs_dir_record *)(bh->b_data);
	size = (sinfo->dir_children_count - 1) * sizeof(struct simplefs_dir_record) - \
	       (drecord - start_drecord);
	memmove(drecord, drecord + 1, size);
	memset(start_drecord + sinfo->dir_children_count - 1, 0, sizeof(struct simplefs_dir_record));
	dir->i_ctime = dir->i_mtime = current_time(dir);
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);

	ibh = sb_bread(dir->i_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	if (!ibh)
		return -EIO;
	sinode = (struct simplefs_inode *)(ibh->b_data);
	sinode += (dir->i_ino - SIMPLEFS_ROOTDIR_INODE_NUMBER);
	sinode->dir_children_count--;
	sinfo->dir_children_count = sinode->dir_children_count;

	dir->i_size = sinfo->dir_children_count * sizeof(struct simplefs_dir_record);
	mark_inode_dirty(dir);
	mark_buffer_dirty(ibh);
	sync_dirty_buffer(ibh);
	brelse(ibh);

	return 0;
}
