void sqfs_swapin_super_block(struct squashfs_super_block *s);
void sqfs_swapin_dir_index(struct squashfs_dir_index *s);
void sqfs_swapin_base_inode(struct squashfs_base_inode *s);
void sqfs_swapin_ipc_inode(struct squashfs_ipc_inode *s);
void sqfs_swapin_lipc_inode(struct squashfs_lipc_inode *s);
void sqfs_swapin_dev_inode(struct squashfs_dev_inode *s);
void sqfs_swapin_ldev_inode(struct squashfs_ldev_inode *s);
void sqfs_swapin_symlink_inode(struct squashfs_symlink_inode *s);
void sqfs_swapin_reg_inode(struct squashfs_reg_inode *s);
void sqfs_swapin_lreg_inode(struct squashfs_lreg_inode *s);
void sqfs_swapin_dir_inode(struct squashfs_dir_inode *s);
void sqfs_swapin_ldir_inode(struct squashfs_ldir_inode *s);
void sqfs_swapin_dir_entry(struct squashfs_dir_entry *s);
void sqfs_swapin_dir_header(struct squashfs_dir_header *s);
void sqfs_swapin_fragment_entry(struct squashfs_fragment_entry *s);
void sqfs_swapin_xattr_entry(struct squashfs_xattr_entry *s);
void sqfs_swapin_xattr_val(struct squashfs_xattr_val *s);
void sqfs_swapin_xattr_id(struct squashfs_xattr_id *s);
void sqfs_swapin_xattr_id_table(struct squashfs_xattr_id_table *s);
