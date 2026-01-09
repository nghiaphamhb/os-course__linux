#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <linux/fs.h>        // file_system_type, super_block, inode, register_filesystem...
#include <linux/pagemap.h>   // d_make_root
#include <linux/stat.h>      // S_IFDIR
#include <linux/errno.h>     // -ENOMEM
#include <linux/mount.h>     // nop_mnt_idmap
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/dirent.h>

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

// Forward declarations
static int vtfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data);
static void vtfs_kill_sb(struct super_block *sb);
static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino);

static struct dentry *vtfs_lookup(struct inode *parent_inode,
                                  struct dentry *child_dentry,
                                  unsigned int flag) {
  (void)flag;

  ino_t ino = parent_inode->i_ino;
  const char *name = child_dentry->d_name.name;

  // Root directory contains: test.txt (101) and dir (200)
  if (ino == 1000 && strcmp(name, "test.txt") == 0) {
    struct inode *inode = vtfs_get_inode(parent_inode->i_sb,
                                         parent_inode,
                                         S_IFREG | 0777,
                                         101);
    if (!inode)
      return ERR_PTR(-ENOMEM);

    d_add(child_dentry, inode);
    return NULL;
  }

  if (ino == 1000 && strcmp(name, "dir") == 0) {
    struct inode *inode = vtfs_get_inode(parent_inode->i_sb,
                                         parent_inode,
                                         S_IFDIR | 0777,
                                         200);
    if (!inode)
      return ERR_PTR(-ENOMEM);

    d_add(child_dentry, inode);
    return NULL;
  }

  return NULL;
}

static int vtfs_iterate_shared(struct file *filp, struct dir_context *ctx) {
  struct dentry *dentry = filp->f_path.dentry;
  struct inode  *inode  = dentry->d_inode;
  ino_t ino = inode->i_ino;

  if (!dir_emit_dots(filp, ctx))
    return 0;

  // Root directory: show test.txt + dir
  if (ino == 1000) {
    if (ctx->pos == 2) {
      if (!dir_emit(ctx, "test.txt", strlen("test.txt"), 101, DT_REG))
        return 0;
      ctx->pos++;
    }
    if (ctx->pos == 3) {
      if (!dir_emit(ctx, "dir", strlen("dir"), 200, DT_DIR))
        return 0;
      ctx->pos++;
    }
    return 0;
  }

  // /dir (ino 200): for now it's empty (only "." and "..")
  if (ino == 200) {
    return 0;
  }

  // Any other directories: empty
  return 0;
}

static const struct file_operations vtfs_dir_ops = {
  .owner = THIS_MODULE,
  .iterate_shared = vtfs_iterate_shared,
};

static const struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
};

static const struct inode_operations vtfs_file_inode_ops = {
  // empty for now
};

static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino) {
  struct inode *inode = new_inode(sb);
  if (inode != NULL) {
    inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
    inode->i_op = &vtfs_inode_ops;

    if (S_ISDIR(mode)) {
      inode->i_op  = &vtfs_inode_ops;
      inode->i_fop = &vtfs_dir_ops;
    } else {
      inode->i_op  = &vtfs_file_inode_ops;
    }
  }

  inode->i_ino = i_ino;
  return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  (void)data;
  (void)silent;

  struct inode *inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 1000);

  sb->s_root = d_make_root(inode);
  if (sb->s_root == NULL) {
    return -ENOMEM;
  }

  LOG("vtfs_fill_super: root created\n");
  return 0;
}

static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data) {
  (void)token;

  struct dentry *ret = mount_nodev(fs_type, flags, data, vtfs_fill_super);
  if (ret == NULL) {
    LOG("Can't mount file system\n");
  } else {
    LOG("Mounted successfully\n");
  }
  return ret;
}

static void vtfs_kill_sb(struct super_block *sb) {
  (void)sb;
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
