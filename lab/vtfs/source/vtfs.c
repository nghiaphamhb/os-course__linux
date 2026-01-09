#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

static int __init vtfs_init(void) {
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>        // file_system_type, super_block, inode, register_filesystem...
#include <linux/pagemap.h>   // d_make_root
#include <linux/stat.h>      // S_IFDIR
#include <linux/errno.h>     // -ENOMEM
#include <linux/mount.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)


static int vtfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data);
static void vtfs_kill_sb(struct super_block *sb);

static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino) {
  struct inode *inode = new_inode(sb);
  if (inode != NULL) {
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
  }

  inode->i_ino = i_ino;
  return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR, 1000);

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }

  LOG("vtfs_fill_super: root created\n");
  return 0;
}

static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data) {
  struct dentry *ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    LOG("Can't mount file system\n");
  } else {
    LOG("Mounted successfully\n");
  }
  return ret;
}

static void vtfs_kill_sb(struct super_block *sb) {
  LOG("vtfs super block is destroyed. Unmount successfully.\n");
}

static struct file_system_type vtfs_fs_type = {
  .name = "vtfs",
  .mount = vtfs_mount,
  .kill_sb = vtfs_kill_sb,
};

static int __init vtfs_init(void) {
  int ret = register_filesystem(&vtfs_fs_type);
  if (ret != 0) {
    LOG("register_filesystem failed: %d\n", ret);
    return ret;
  }
  LOG("VTFS joined the kernel\n");
  return 0;
}

static void __exit vtfs_exit(void) {
  int ret = unregister_filesystem(&vtfs_fs_type);
  if (ret != 0) {
    LOG("unregister_filesystem failed: %d\n", ret);
  }
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
