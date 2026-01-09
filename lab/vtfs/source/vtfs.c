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
#include <linux/slab.h>     // kmalloc/kfree
#include <linux/mutex.h>    // mutex
#include <linux/string.h>   // strcmp/strlen

#define MODULE_NAME "vtfs"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

// RAM backend
enum vtfs_node_type {
  VTFS_DIR = 1,
  VTFS_FILE = 2,
};

struct vtfs_node {
  enum vtfs_node_type type;
  ino_t ino;
  umode_t mode;

  char name[64];

  // For directories: linked list of children
  struct vtfs_node *parent;
  struct vtfs_node *children; // head
  struct vtfs_node *next;     // sibling link

  // For files: data buffer in RAM (not used yet)
  char *data;
  size_t size;
};

static DEFINE_MUTEX(vtfs_tree_lock);

static ino_t vtfs_next_ino = 300;
static ino_t vtfs_alloc_ino(void) {
  return vtfs_next_ino++;
}

static struct vtfs_node *vtfs_root_node = NULL;

static struct vtfs_node *vtfs_node_new(const char *name,
                                       enum vtfs_node_type type,
                                       ino_t ino,
                                       umode_t mode) {
  struct vtfs_node *n = kmalloc(sizeof(*n), GFP_KERNEL);
  if (!n) return NULL;

  memset(n, 0, sizeof(*n));
  n->type = type;
  n->ino = ino;
  n->mode = mode;

  // Safe copy
  strscpy(n->name, name, sizeof(n->name));
  return n;
}

static void vtfs_node_add_child(struct vtfs_node *dir, struct vtfs_node *child) {
  child->parent = dir;
  child->next = dir->children;
  dir->children = child;
}

static struct vtfs_node *vtfs_node_find_child(struct vtfs_node *dir,
                                              const char *name) {
  struct vtfs_node *it;
  for (it = dir->children; it != NULL; it = it->next) {
    if (strcmp(it->name, name) == 0)
      return it;
  }
  return NULL;
}

static int vtfs_node_remove_child(struct vtfs_node *dir, const char *name) {
  struct vtfs_node *prev = NULL;
  struct vtfs_node *it = dir->children;

  while (it) {
    if (strcmp(it->name, name) == 0) {
      if (prev) prev->next = it->next;
      else dir->children = it->next;

      // Free file data if present
      if (it->data) kfree(it->data);
      kfree(it);
      return 0;
    }
    prev = it;
    it = it->next;
  }
  return -ENOENT;
}

static void vtfs_node_free_tree(struct vtfs_node *n) {
  struct vtfs_node *child, *next;
  if (!n) return;

  child = n->children;
  while (child) {
    next = child->next;
    vtfs_node_free_tree(child);
    child = next;
  }

  if (n->data) kfree(n->data);
  kfree(n);
}

static int vtfs_backend_init(void) {
  // root (not stored as a named entry)
  vtfs_root_node = vtfs_node_new("", VTFS_DIR, 1000, S_IFDIR | 0777);
  if (!vtfs_root_node) return -ENOMEM;

  // Create /dir by default (like previous parts)
  struct vtfs_node *dir = vtfs_node_new("dir", VTFS_DIR, 200, S_IFDIR | 0777);
  if (!dir) {
    vtfs_node_free_tree(vtfs_root_node);
    vtfs_root_node = NULL;
    return -ENOMEM;
  }
  vtfs_node_add_child(vtfs_root_node, dir);

  return 0;
}

static void vtfs_backend_destroy(void) {
  vtfs_node_free_tree(vtfs_root_node);
  vtfs_root_node = NULL;
}

// Forward declarations
static int vtfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data);
static void vtfs_kill_sb(struct super_block *sb);
static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino,
                                    struct vtfs_node *node);
static struct dentry *vtfs_lookup(struct inode *parent_inode,
                                  struct dentry *child_dentry,
                                  unsigned int flag) {
  (void)flag;

  struct vtfs_node *parent = (struct vtfs_node *)parent_inode->i_private;
  const char *name = child_dentry->d_name.name;
  struct vtfs_node *child;
  struct inode *inode;

  if (!parent || parent->type != VTFS_DIR)
    return NULL;

  mutex_lock(&vtfs_tree_lock);
  child = vtfs_node_find_child(parent, name);
  if (!child) {
    mutex_unlock(&vtfs_tree_lock);
    return NULL; // not found
  }

  inode = vtfs_get_inode(parent_inode->i_sb, parent_inode,
                         child->mode, child->ino, child);
  mutex_unlock(&vtfs_tree_lock);

  if (!inode)
    return ERR_PTR(-ENOMEM);

  d_add(child_dentry, inode);
  return NULL;
}

static int vtfs_create(struct mnt_idmap *idmap,
                       struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode,
                       bool excl) {
  (void)idmap;
  (void)mode;
  (void)excl;

  struct vtfs_node *parent = (struct vtfs_node *)parent_inode->i_private;
  const char *name = child_dentry->d_name.name;
  struct vtfs_node *child;
  struct inode *inode;

  if (!parent || parent->type != VTFS_DIR)
    return -ENOTDIR;

  mutex_lock(&vtfs_tree_lock);

  if (vtfs_node_find_child(parent, name)) {
    mutex_unlock(&vtfs_tree_lock);
    return -EEXIST;
  }

  child = vtfs_node_new(name, VTFS_FILE, vtfs_alloc_ino(), S_IFREG | 0777);
  if (!child) {
    mutex_unlock(&vtfs_tree_lock);
    return -ENOMEM;
  }

  vtfs_node_add_child(parent, child);

  inode = vtfs_get_inode(parent_inode->i_sb, parent_inode,
                         child->mode, child->ino, child);
  mutex_unlock(&vtfs_tree_lock);

  if (!inode)
    return -ENOMEM;

  d_add(child_dentry, inode);
  return 0;
}

static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
  struct vtfs_node *parent = (struct vtfs_node *)parent_inode->i_private;
  const char *name = child_dentry->d_name.name;
  int rc;

  if (!parent || parent->type != VTFS_DIR)
    return -ENOTDIR;

  mutex_lock(&vtfs_tree_lock);
  rc = vtfs_node_remove_child(parent, name);
  mutex_unlock(&vtfs_tree_lock);

  return rc;
}

static int vtfs_iterate_shared(struct file *filp, struct dir_context *ctx) {
  struct dentry *dentry = filp->f_path.dentry;
  struct inode *inode = dentry->d_inode;

  struct vtfs_node *dirnode = (struct vtfs_node *)inode->i_private;
  struct vtfs_node *it;
  long idx;

  if (!dirnode || dirnode->type != VTFS_DIR)
    return 0;

  if (!dir_emit_dots(filp, ctx))
    return 0;

  /*
    ctx->pos:
      0,1 are used by dots
      starting from 2 -> our children list
    We'll map child index = ctx->pos - 2
  */
  mutex_lock(&vtfs_tree_lock);

  idx = (long)ctx->pos - 2;
  it = dirnode->children;

  while (it && idx > 0) {
    it = it->next;
    idx--;
  }

  while (it) {
    unsigned char ftype = (it->type == VTFS_DIR) ? DT_DIR : DT_REG;

    if (!dir_emit(ctx, it->name, strlen(it->name), it->ino, ftype))
      break;

    ctx->pos++;      // advance position per emitted entry
    it = it->next;
  }

  mutex_unlock(&vtfs_tree_lock);
  return 0;
}

static const struct file_operations vtfs_dir_ops = {
  .owner = THIS_MODULE,
  .iterate_shared = vtfs_iterate_shared,
};

static const struct inode_operations vtfs_inode_ops = {
  .lookup = vtfs_lookup,
  .create = vtfs_create,
  .unlink = vtfs_unlink,
};

static const struct inode_operations vtfs_file_inode_ops = {
  // empty for now
};

static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino,
                                    struct vtfs_node *node) {
  struct inode *inode = new_inode(sb);
  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);

  if (S_ISDIR(mode)) {
    inode->i_op  = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;
  } else {
    inode->i_op  = &vtfs_file_inode_ops;
  }

  inode->i_ino = i_ino;
  inode->i_private = node;   // bind inode to RAM node
  return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  (void)data;
  (void)silent;

  struct inode *inode;

  mutex_lock(&vtfs_tree_lock);
  inode = vtfs_get_inode(sb, NULL, S_IFDIR | 0777, 1000, vtfs_root_node);
  mutex_unlock(&vtfs_tree_lock);

  if (!inode)
    return -ENOMEM;

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
  int rc = vtfs_backend_init();
  if (rc != 0) {
    LOG("vtfs_backend_init failed: %d\n", rc);
    return rc;
  }

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
  vtfs_backend_destroy();

  if (ret != 0) {
    LOG("unregister_filesystem failed: %d\n", ret);
  }
  LOG("VTFS left the kernel\n");
}

module_init(vtfs_init);
module_exit(vtfs_exit);
