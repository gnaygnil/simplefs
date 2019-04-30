#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/writeback.h>
#include <linux/statfs.h>

#include "simple.h"

struct inode *simplefs_iget(struct super_block *sb, unsigned long ino)
{
	struct simplefs_inode *sinode;
	struct simplefs_inode_info *sinfo;
	struct inode *inode;
	struct buffer_head *bh;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	if ((ino < SIMPLEFS_ROOTDIR_INODE_NUMBER) || (ino > SIMPLEFS_LAST_INODE_NUMBER)) {
		printk(KERN_ERR "Bad inode number %s:%08lx\n", inode->i_sb->s_id, ino);
		goto out;
	}

	bh = sb_bread(inode->i_sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	if (!bh) {
		printk(KERN_ERR "Unable to read inode %s:%08lx\n", inode->i_sb->s_id, ino);
		goto out;
	}

	sinode = (struct simplefs_inode *)bh->b_data + ino - SIMPLEFS_ROOTDIR_INODE_NUMBER;

	inode->i_mode = sinode->mode;
	sinfo = simplefs_i(inode);
	sinfo->data_block_number = sinode->data_block_number;
	if (S_ISDIR(inode->i_mode)) {
		sinfo->dir_children_count = inode->i_blocks = sinode->dir_children_count;
		inode->i_size = sinfo->dir_children_count * sizeof(struct simplefs_dir_record);
		inode->i_op = &simplefs_dir_inops;
		inode->i_fop = &simplefs_dir_operations;
	} else if (S_ISREG(inode->i_mode)) {
		sinfo->file_size = inode->i_size = sinode->file_size;
		inode->i_op = &simplefs_file_inops;
		inode->i_fop = &simplefs_file_operations;
	} else if (S_ISLNK(inode->i_mode)) {
		inode->i_op = &simplefs_symlink_inops;
		inode_nohighmem(inode);
	}
	if (IS_DAX(inode))
		inode->i_mapping->a_ops = &simplefs_dax_aops;
	else
		inode->i_mapping->a_ops = &simplefs_aops;

	set_nlink(inode, sinode->i_nlink);
	inode->i_atime = inode->i_mtime = inode->i_ctime = current_time(inode);

	brelse(bh);
	unlock_new_inode(inode);
	return inode;

out:
	iget_failed(inode);
	return ERR_PTR(-EIO);
}

static struct simplefs_inode *find_inode(struct super_block *sb, unsigned long ino, struct buffer_head **p)
{
	if ((ino < SIMPLEFS_ROOTDIR_INODE_NUMBER) || (ino > SIMPLEFS_LAST_INODE_NUMBER)) {
		printk("Bad inode number %s:%lu\n", sb->s_id, ino);
		return ERR_PTR(-EIO);
	}

	*p = sb_bread(sb, SIMPLEFS_INODESTORE_BLOCK_NUMBER);
	if (!*p) {
		printk("Unable to read inode %s:%lu\n", sb->s_id, ino);
		return ERR_PTR(-EIO);
	}

	return (struct simplefs_inode *)(*p)->b_data +  ino - SIMPLEFS_ROOTDIR_INODE_NUMBER;
}

static int simplefs_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	struct simplefs_sb_info *sbinfo = simplefs_sb(inode->i_sb);
	unsigned long ino = inode->i_ino;
	struct simplefs_inode *sinode;
	struct simplefs_inode_info *sinfo = simplefs_i(inode);
	struct buffer_head *bh;
	int err = 0;

	sinode = find_inode(inode->i_sb, ino, &bh);
	if (IS_ERR(sinode))
		return PTR_ERR(sinode);

	mutex_lock(&sbinfo->simplefs_lock);

	sinode->inode_no = ino;
	sinode->mode = inode->i_mode;
	sinode->i_nlink = inode->i_nlink;
	sinode->data_block_number = sinfo->data_block_number;

	if (S_ISDIR(inode->i_mode)) {
		sinode->dir_children_count = sinfo->dir_children_count;
	} else {
		sinode->file_size = sinfo->file_size;
	}

	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	mutex_unlock(&sbinfo->simplefs_lock);
	return err;
}

static void simplefs_evict_inode(struct inode *inode)
{
	struct simplefs_inode *sinode;
	struct buffer_head *bh;
	struct super_block *s = inode->i_sb;
	struct simplefs_sb_info *sbinfo = simplefs_sb(s);

	truncate_inode_pages_final(&inode->i_data);
	invalidate_inode_buffers(inode);
	clear_inode(inode);

	if (inode->i_nlink)
		return;

	sinode = find_inode(s, inode->i_ino, &bh);
	if (IS_ERR(sinode))
		return;

	mutex_lock(&sbinfo->simplefs_lock);

	memset(sinode, 0, sizeof(struct simplefs_inode));
	mark_buffer_dirty(bh);
	sync_dirty_buffer(bh);
	brelse(bh);
	mutex_unlock(&sbinfo->simplefs_lock);
}

static void simplefs_put_super(struct super_block *sb)
{
	struct simplefs_sb_info *sbinfo = simplefs_sb(sb);

	if (!sbinfo)
		return;
	mutex_destroy(&sbinfo->simplefs_lock);
	brelse(sbinfo->sbh);
	sb->s_fs_info = NULL;
	kfree(sbinfo);
}

static int simplefs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *s = dentry->d_sb;
	u64 id = huge_encode_dev(s->s_bdev->bd_dev);
	buf->f_type = SIMPLEFS_MAGIC;
	buf->f_bsize = s->s_blocksize;
	buf->f_fsid.val[0] = (u32)id;
	buf->f_fsid.val[1] = (u32)(id >> 32);
	buf->f_namelen = SIMPLEFS_FILENAME_MAXLEN;
	return 0;
}

static struct kmem_cache *simplefs_inode_cachep;

static struct inode *simplefs_alloc_inode(struct super_block *sb)
{
	struct simplefs_inode_info *sinfo;
	sinfo = kmem_cache_alloc(simplefs_inode_cachep, GFP_KERNEL);
	if (!sinfo)
		return NULL;
	return &sinfo->vfs_inode;
}

static void simplefs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);
	kmem_cache_free(simplefs_inode_cachep, simplefs_i(inode));
}

static void simplefs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, simplefs_i_callback);
}

static void init_once(void *foo)
{
	struct simplefs_inode_info *sinfo = (struct simplefs_inode_info *)foo;
	inode_init_once(&sinfo->vfs_inode);
}

static struct super_operations simplefs_sops = {
	.alloc_inode	= simplefs_alloc_inode,
	.destroy_inode	= simplefs_destroy_inode,
	.write_inode	= simplefs_write_inode,
	.evict_inode	= simplefs_evict_inode,
	.put_super	= simplefs_put_super,
	.statfs		= simplefs_statfs,
};


static int simplefs_fill_super(struct super_block *s, void *data, int silent)
{
	struct buffer_head *sbh;
	struct simplefs_super_block *sb;
	struct inode *root_inode;
	struct simplefs_sb_info *sbi;
	int ret = -EINVAL;

	sbi = kzalloc(sizeof(struct simplefs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	mutex_init(&sbi->simplefs_lock);
	s->s_fs_info = sbi;

	sb_set_blocksize(s, SIMPLEFS_DEFAULT_BLOCK_SIZE);

	sbh = sb_bread(s, SIMPLEFS_SUPERBLOCK_BLOCK_NUMBER);
	if (!sbh)
		goto out;

	sb = (struct simplefs_super_block *)sbh->b_data;
	sbi->sb = sb;
	sbi->sbh = sbh;

	if (sb->magic != SIMPLEFS_MAGIC) {
		printk("simplefs: magicnumber mismatch.\n");
		goto out1;
	}
	s->s_magic = sb->magic;

	s->s_op = &simplefs_sops;
	root_inode = simplefs_iget(s, SIMPLEFS_ROOTDIR_INODE_NUMBER);
	if (IS_ERR(root_inode)) {
		ret = PTR_ERR(root_inode);
		goto out1;
	}

	s->s_root = d_make_root(root_inode);
	if (!s->s_root) {
		ret = -ENOMEM;
		goto out1;
	}

	mark_buffer_dirty(sbh);
	sync_dirty_buffer(sbh);
	return 0;

out1:
	brelse(sbh);
out:
	mutex_destroy(&sbi->simplefs_lock);
	s->s_fs_info = NULL;
	kfree(sbi);
	return ret;
}

static struct dentry *simplefs_mount(struct file_system_type *fs_type,
		int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, simplefs_fill_super);
}

static struct file_system_type simplefs_type = {
	.owner		= THIS_MODULE,
	.name		= "simplefs",
	.mount		= simplefs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_simplefs(void)
{
	int err = 0;

	simplefs_inode_cachep = kmem_cache_create("simplefs_inode_cache",
			sizeof(struct simplefs_inode_info),
			0, (SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD|SLAB_ACCOUNT),
			init_once);
	if (simplefs_inode_cachep == NULL)
		return -ENOMEM;

	err = register_filesystem(&simplefs_type);
	if (err)
		kmem_cache_destroy(simplefs_inode_cachep);

	return 0;
}

static void __exit exit_simplefs(void)
{
	unregister_filesystem(&simplefs_type);
	rcu_barrier();
	kmem_cache_destroy(simplefs_inode_cachep);
}

module_init(init_simplefs);
module_exit(exit_simplefs);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("LY");
