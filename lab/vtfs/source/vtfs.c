#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/fs.h>        // file_system_type, super_block, inode, register_filesystem...
#include <linux/pagemap.h>   // d_make_root
#include <linux/stat.h>      // S_IFDIR
#include <linux/errno.h>     // -ENOMEM
#include <linux/mount.h>     // nop_mnt_idmap
#include <linux/uaccess.h>
#include <linux/slab.h>     // kmalloc/kfree
#include <linux/string.h>   // strcmp/strlen
#include <linux/fcntl.h>    // O_TRUNC
#include "http.h"
#include <linux/base64.h>
#include <linux/unaligned.h>   // get_unaligned_le64

MODULE_LICENSE("GPL");
MODULE_AUTHOR("secs-dev");
MODULE_DESCRIPTION("A simple FS kernel module");

#define MODULE_NAME "vtfs"
#define VTFS_MAGIC 0x56544653  // "VTFS"
#define VTFS_TOKEN "TODO"
#define LOG(fmt, ...) pr_info("[" MODULE_NAME "]: " fmt, ##__VA_ARGS__)

struct vtfs_remote_info {
  ino_t ino;
  int type;
  umode_t mode;
  unsigned int nlink;
};

// Forward declarations
static void vtfs_evict_inode(struct inode *inode);
static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino,
                                    struct vtfs_remote_info *ri);
static int vtfs_fill_super(struct super_block *sb, void *data, int silent);
static struct dentry *vtfs_mount(struct file_system_type *fs_type, int flags,
                                 const char *token, void *data);
static void vtfs_kill_sb(struct super_block *sb);

// Remote API wrappers
static int vtfs_api_list(ino_t parent, char *out, size_t out_sz) {
  char parent_s[32];
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);

  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "list", out, out_sz - 1, 1,
                             "parent", parent_s);
}

static int vtfs_api_lookup(ino_t parent, const char *name,
                           char *out, size_t out_sz) {
  char parent_s[32];
  char enc_name[256];

  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  encode(name, enc_name);

  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "lookup", out, out_sz - 1, 2,
                             "parent", parent_s,
                             "name", enc_name);
}

static int vtfs_api_create(ino_t parent, const char *name, umode_t mode,
                           char *out, size_t out_sz) {
  char parent_s[32], mode_s[32], enc_name[256];
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  snprintf(mode_s, sizeof(mode_s), "%u", (unsigned int)mode);
  encode(name, enc_name);
  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "create", out, out_sz - 1, 3,
                             "parent", parent_s,
                             "name", enc_name,
                             "mode", mode_s);
}

static int vtfs_api_mkdir(ino_t parent, const char *name, umode_t mode,
                          char *out, size_t out_sz) {
  char parent_s[32], mode_s[32], enc_name[256];
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  snprintf(mode_s, sizeof(mode_s), "%u", (unsigned int)mode);
  encode(name, enc_name);
  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "mkdir", out, out_sz - 1, 3,
                             "parent", parent_s,
                             "name", enc_name,
                             "mode", mode_s);
}

static int vtfs_api_rmdir(ino_t parent, const char *name) {
  char parent_s[32], enc_name[256], resp[64];
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  encode(name, enc_name);
  memset(resp, 0, sizeof(resp));
  return (int)vtfs_http_call(VTFS_TOKEN, "rmdir", resp, sizeof(resp) - 1, 2,
                             "parent", parent_s,
                             "name", enc_name);
}

static int vtfs_api_unlink(ino_t parent, const char *name) {
  char parent_s[32], enc_name[256], resp[64];
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  encode(name, enc_name);
  memset(resp, 0, sizeof(resp));
  return (int)vtfs_http_call(VTFS_TOKEN, "unlink", resp, sizeof(resp)-1, 2,
                             "parent", parent_s,
                             "name", enc_name);
}

static int vtfs_api_read(ino_t ino, loff_t off, size_t len, char *out, size_t out_sz) {
  char ino_s[32], off_s[32], len_s[32];
  snprintf(ino_s, sizeof(ino_s), "%lu", (unsigned long)ino);
  snprintf(off_s, sizeof(off_s), "%lld", (long long)off);
  snprintf(len_s, sizeof(len_s), "%lu", (unsigned long)len);
  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "read", out, out_sz, 3,
                             "ino", ino_s, "off", off_s, "len", len_s);
}

static int vtfs_api_truncate(ino_t ino) {
  char ino_s[32], resp[64];
  snprintf(ino_s, sizeof(ino_s), "%lu", (unsigned long)ino);
  memset(resp, 0, sizeof(resp));
  return (int)vtfs_http_call(VTFS_TOKEN, "truncate", resp, sizeof(resp) - 1, 1,
                             "ino", ino_s);
}

static int vtfs_api_write_b64url(ino_t ino, loff_t off, const char *b64url) {
  char ino_s[32], off_s[32], resp[64];
  snprintf(ino_s, sizeof(ino_s), "%lu", (unsigned long)ino);
  snprintf(off_s, sizeof(off_s), "%lld", (long long)off);
  memset(resp, 0, sizeof(resp));
  return (int)vtfs_http_call(VTFS_TOKEN, "write", resp, sizeof(resp) - 1, 3,
                             "ino", ino_s, "off", off_s, "data", b64url);
}

static int vtfs_api_link(ino_t old_ino, ino_t parent, const char *name,
                         char *out, size_t out_sz) {
  char old_s[32], parent_s[32], enc_name[256];

  snprintf(old_s, sizeof(old_s), "%lu", (unsigned long)old_ino);
  snprintf(parent_s, sizeof(parent_s), "%lu", (unsigned long)parent);
  encode(name, enc_name);

  memset(out, 0, out_sz);
  return (int)vtfs_http_call(VTFS_TOKEN, "link", out, out_sz - 1, 3,
                             "old_ino", old_s,
                             "parent", parent_s,
                             "name", enc_name);
}

// Helpers 
static int vtfs_srvcode_to_errno(int rc) {
  if (rc == 0) return 0;
  if (rc > 0 && rc < 4096) return -rc; // treat as errno
  return -EIO;
}

static int vtfs_parse_info_line(const char *buf,
                                unsigned long *ino,
                                unsigned int *type,
                                unsigned int *mode,
                                unsigned int *nlink) {
  if (sscanf(buf, "%lu %u %u %u", ino, type, mode, nlink) != 4)
    return -EINVAL;
  return 0;
}

static int vtfs_b64url_encode(const u8 *in, size_t inlen, char **outp)
{
  // base64 output length = 4 * ceil(n/3)
  // plus 1 for '\0'
  size_t b64len = ((inlen + 2) / 3) * 4;
  size_t need = b64len + 1;

  // base64_encode takes (int len) => check overflow safely without INT_MAX
  int ilen = (int)inlen;
  if ((size_t)ilen != inlen)
    return -EOVERFLOW;

  char *b64 = kmalloc(need, GFP_KERNEL);
  if (!b64)
    return -ENOMEM;

  // Correct call order for kernel:
  // base64_encode(src, len, dst)
  int n = base64_encode(in, ilen, b64);
  if (n < 0) {
    kfree(b64);
    return -EIO;
  }

  // Ensure NUL-terminated (base64_encode may not do it)
  if ((size_t)n >= need) {
    kfree(b64);
    return -EIO;
  }
  b64[n] = '\0';

  // Convert to base64url: '+'->'-', '/'->'_' and strip '='
  for (int i = 0; i < n; i++) {
    if (b64[i] == '+') b64[i] = '-';
    else if (b64[i] == '/') b64[i] = '_';
  }
  while (n > 0 && b64[n - 1] == '=') {
    b64[n - 1] = '\0';
    n--;
  }

  *outp = b64; // caller must kfree()
  return 0;
}

// Super ops + inode lifecycle
static void vtfs_evict_inode(struct inode *inode) {
  struct vtfs_remote_info *ri = inode->i_private;
  truncate_inode_pages_final(&inode->i_data);
  clear_inode(inode);
  kfree(ri);
}

static const struct super_operations vtfs_super_ops = {
  .statfs      = simple_statfs,
  .drop_inode  = generic_delete_inode,
  .evict_inode = vtfs_evict_inode,
};

// Logic FS 
static struct dentry *vtfs_lookup(struct inode *parent_inode,
                                  struct dentry *child_dentry,
                                  unsigned int flag) {
  (void)flag;

  struct vtfs_remote_info *pri =
      (struct vtfs_remote_info *)parent_inode->i_private;

  const char *name = child_dentry->d_name.name;
  char buf[512];

  unsigned long ino;
  unsigned int type;
  unsigned int mode;
  unsigned int nlink;

  struct inode *inode;
  struct vtfs_remote_info *ri;

  if (!pri || pri->type != 1)
    return NULL;

  int rc = vtfs_api_lookup(pri->ino, name, buf, sizeof(buf));
  if (rc == 2) return NULL;          // not found
  if (rc != 0) return ERR_PTR(-EIO); // server/network error (tối thiểu)


  if (sscanf(buf, "%lu %u %u %u", &ino, &type, &mode, &nlink) != 4)
    return NULL;

  ri = kmalloc(sizeof(*ri), GFP_KERNEL);
  if (!ri)
    return ERR_PTR(-ENOMEM);

  ri->ino = (ino_t)ino;
  ri->type = (int)type;
  ri->mode = (umode_t)mode;
  ri->nlink = nlink;

  inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, ri->mode, ri->ino, ri);
  if (!inode) { kfree(ri); return ERR_PTR(-ENOMEM); }
  d_add(child_dentry, inode);

  return NULL;
}

static int vtfs_iterate_shared(struct file *filp, struct dir_context *ctx) {
  struct inode *inode = file_inode(filp);
  ino_t parent_ino = inode->i_ino;

  if (!dir_emit_dots(filp, ctx))
    return 0;

  size_t resp_sz = 4096;
  char *resp = kmalloc(resp_sz, GFP_KERNEL);
  if (!resp)
    return -ENOMEM;

  int code = vtfs_api_list(parent_ino, resp, resp_sz);
  if (code != 0) {
    kfree(resp);
    return 0;
  }

  long want_skip = (ctx->pos >= 2) ? (long)(ctx->pos - 2) : 0;
  long idx = 0;

  char *p = resp;
  char *line;
  while ((line = strsep(&p, "\n")) != NULL) {
    if (*line == '\0')
      continue;

    if (idx < want_skip) {
      idx++;
      continue;
    }

    char *lineptr = line;
    char *name  = strsep(&lineptr, "\t");
    char *ino_s = strsep(&lineptr, "\t");
    char *type_s= strsep(&lineptr, "\t");
    if (!name || !ino_s || !type_s) continue;

    unsigned long child_ino = 0;
    unsigned long child_type = 0;
    if (kstrtoul(ino_s, 10, &child_ino) != 0)
      continue;
    if (kstrtoul(type_s, 10, &child_type) != 0)
      continue;

    unsigned char ftype = (child_type == 1) ? DT_DIR : DT_REG;

    if (!dir_emit(ctx, name, strlen(name), (ino_t)child_ino, ftype))
      break;

    ctx->pos++;
    idx++;
  }

  kfree(resp);
  return 0;
}

static int vtfs_create(struct mnt_idmap *idmap,
                       struct inode *parent_inode,
                       struct dentry *child_dentry,
                       umode_t mode,
                       bool excl) {
  (void)idmap;
  (void)excl;

  struct vtfs_remote_info *pri = parent_inode->i_private;
  const char *name = child_dentry->d_name.name;

  char buf[512];
  unsigned long ino;
  unsigned int type, m, nlink;

  struct vtfs_remote_info *ri;
  struct inode *inode;

  if (!pri || pri->type != 1) return -ENOTDIR;

  // force file type + perms (server cũng ok nếu bạn gửi đầy đủ)
  umode_t req_mode = (S_IFREG | (mode & 0777));
  if ((mode & 0777) == 0) req_mode = (S_IFREG | 0777);

  int rc = vtfs_api_create(pri->ino, name, req_mode, buf, sizeof(buf));
  if (rc != 0) return vtfs_srvcode_to_errno(rc);

  if (vtfs_parse_info_line(buf, &ino, &type, &m, &nlink) != 0) return -EIO;

  ri = kmalloc(sizeof(*ri), GFP_KERNEL);
  if (!ri) return -ENOMEM;

  ri->ino = (ino_t)ino;
  ri->type = (int)type;
  ri->mode = (umode_t)m;
  ri->nlink = nlink;

  inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, ri->mode, ri->ino, ri);
  if (!inode) { kfree(ri); return -ENOMEM; }

  d_add(child_dentry, inode);
  return 0;
}

static int vtfs_mkdir(struct mnt_idmap *idmap,
                      struct inode *parent_inode,
                      struct dentry *child_dentry,
                      umode_t mode) {
  (void)idmap;

  struct vtfs_remote_info *pri = parent_inode->i_private;
  const char *name = child_dentry->d_name.name;

  char buf[512];
  unsigned long ino;
  unsigned int type, m, nlink;

  struct vtfs_remote_info *ri;
  struct inode *inode;

  if (!pri || pri->type != 1) return -ENOTDIR;

  umode_t req_mode = (S_IFDIR | (mode & 0777));
  if ((mode & 0777) == 0) req_mode = (S_IFDIR | 0777);

  int rc = vtfs_api_mkdir(pri->ino, name, req_mode, buf, sizeof(buf));
  if (rc != 0) return vtfs_srvcode_to_errno(rc);

  if (vtfs_parse_info_line(buf, &ino, &type, &m, &nlink) != 0) return -EIO;

  ri = kmalloc(sizeof(*ri), GFP_KERNEL);
  if (!ri) return -ENOMEM;

  ri->ino = (ino_t)ino;
  ri->type = (int)type;
  ri->mode = (umode_t)m;
  ri->nlink = nlink;

  inode = vtfs_get_inode(parent_inode->i_sb, parent_inode, ri->mode, ri->ino, ri);
  if (!inode) { kfree(ri); return -ENOMEM; }

  d_add(child_dentry, inode);
  return 0;
}

static int vtfs_unlink(struct inode *parent_inode, struct dentry *child_dentry) {
  struct vtfs_remote_info *pri = parent_inode->i_private;
  const char *name = child_dentry->d_name.name;

  if (!pri || pri->type != 1) return -ENOTDIR;

  int rc = vtfs_api_unlink(pri->ino, name);
  if (rc != 0) return vtfs_srvcode_to_errno(rc);

  return 0;
}

static int vtfs_rmdir(struct inode *parent_inode, struct dentry *child_dentry) {
  struct vtfs_remote_info *pri = parent_inode->i_private;
  const char *name = child_dentry->d_name.name;

  if (!pri || pri->type != 1) return -ENOTDIR;

  int rc = vtfs_api_rmdir(pri->ino, name);
  if (rc != 0) return vtfs_srvcode_to_errno(rc);

  return 0;
}

static int vtfs_link(struct dentry *old_dentry,
                     struct inode *parent_dir,
                     struct dentry *new_dentry) {
  struct inode *old_inode = d_inode(old_dentry);
  struct vtfs_remote_info *old_ri;
  struct vtfs_remote_info *pri;
  const char *newname;
  char buf[256];

  unsigned long ino;
  unsigned int type, mode, nlink;

  struct vtfs_remote_info *ri2;
  struct inode *inode2;

  if (!old_inode)
    return -ENOENT;

  old_ri = (struct vtfs_remote_info *)old_inode->i_private;
  pri = (struct vtfs_remote_info *)parent_dir->i_private;
  newname = new_dentry->d_name.name;

  if (!old_ri || !pri)
    return -EIO;

  // server only supports hardlink for regular files
  if (old_ri->type != 2)
    return -EPERM; // or -EISDIR

  if (pri->type != 1)
    return -ENOTDIR;

  int rc = vtfs_api_link(old_ri->ino, pri->ino, newname, buf, sizeof(buf));
  if (rc != 0)
    return vtfs_srvcode_to_errno(rc);

  if (vtfs_parse_info_line(buf, &ino, &type, &mode, &nlink) != 0)
    return -EIO;

  // create new remote_info for the new dentry's inode
  ri2 = kmalloc(sizeof(*ri2), GFP_KERNEL);
  if (!ri2)
    return -ENOMEM;

  ri2->ino = (ino_t)ino;
  ri2->type = (int)type;
  ri2->mode = (umode_t)mode;
  ri2->nlink = nlink;

  inode2 = vtfs_get_inode(parent_dir->i_sb, parent_dir, ri2->mode, ri2->ino, ri2);
  if (!inode2) {
    kfree(ri2);
    return -ENOMEM;
  }

  // update link count in old inode too (cache correctness)
  set_nlink(old_inode, nlink);

  d_add(new_dentry, inode2);
  return 0;
}

static int vtfs_open(struct inode *inode, struct file *filp) {
  struct vtfs_remote_info *ri = inode->i_private;
  if (!ri || ri->type != 2) return -EINVAL;

  if (filp->f_flags & O_TRUNC) {
    int rc = vtfs_api_truncate(ri->ino);
    if (rc != 0) return vtfs_srvcode_to_errno(rc);
  }
  return 0;
}

static ssize_t vtfs_read(struct file *filp, char __user *buffer, size_t len, loff_t *offset) {
  struct vtfs_remote_info *ri = file_inode(filp)->i_private;
  if (!ri || ri->type != 2) return -EINVAL;
  if (len == 0) return 0;

  size_t cap = min_t(size_t, len, 4096);
  size_t resp_sz = 8 + cap; // 8 bytes length + data
  char *resp = kmalloc(resp_sz, GFP_KERNEL);
  if (!resp) return -ENOMEM;

  int rc = vtfs_api_read(ri->ino, *offset, cap, resp, resp_sz);
  if (rc != 0) { kfree(resp); return vtfs_srvcode_to_errno(rc); }

  u64 n = get_unaligned_le64(resp);   // read little-endian safely
  if (n > cap) { kfree(resp); return -EIO; }

  if (n > 0 && copy_to_user(buffer, resp + 8, (size_t)n)) {
    kfree(resp);
    return -EFAULT;
  }

  *offset += (loff_t)n;
  kfree(resp);
  return (ssize_t)n;
}

static ssize_t vtfs_write(struct file *filp, const char __user *buffer,
                          size_t len, loff_t *offset)
{
  struct inode *inode = file_inode(filp);
  struct vtfs_remote_info *ri = inode->i_private;

  if (!ri || ri->type != 2) return -EINVAL;   // must be FILE
  if (!offset || *offset < 0) return -EINVAL;
  if (len == 0) return 0;

  size_t done = 0;

  // Keep small to avoid very long URL
  const size_t CHUNK = 512;

  while (done < len) {
    size_t chunk = min_t(size_t, len - done, CHUNK);

    u8 *tmp = kmalloc(chunk, GFP_KERNEL);
    if (!tmp) return done ? (ssize_t)done : -ENOMEM;

    if (copy_from_user(tmp, buffer + done, chunk)) {
      kfree(tmp);
      return done ? (ssize_t)done : -EFAULT;
    }

    char *b64url = NULL;
    int erc = vtfs_b64url_encode(tmp, chunk, &b64url);
    kfree(tmp);
    if (erc != 0) return done ? (ssize_t)done : erc;

    int rc = vtfs_api_write_b64url(ri->ino, *offset, b64url);
    kfree(b64url);

    if (rc != 0) {
      int kerr = vtfs_srvcode_to_errno(rc);
      return done ? (ssize_t)done : kerr;
    }

    *offset += (loff_t)chunk;
    done += chunk;
  }

  return (ssize_t)done;
}

// ops tables
static const struct file_operations vtfs_dir_ops = {
  .owner = THIS_MODULE,
  .iterate_shared = vtfs_iterate_shared,
};

static const struct file_operations vtfs_file_ops = {
  .owner  = THIS_MODULE,
  .open   = vtfs_open,
  .read   = vtfs_read,
  .write  = vtfs_write,
  .llseek = generic_file_llseek,
};

static const struct inode_operations vtfs_inode_ops = {
  .lookup  = vtfs_lookup,
  .create  = vtfs_create,
  .unlink  = vtfs_unlink,
  .mkdir   = vtfs_mkdir,
  .rmdir   = vtfs_rmdir,
  .link   = vtfs_link,
};

static const struct inode_operations vtfs_file_inode_ops = {
  // empty for now
};

// builders 
static struct inode *vtfs_get_inode(struct super_block *sb,
                                    const struct inode *dir,
                                    umode_t mode, int i_ino,
                                    struct vtfs_remote_info *ri) {
  struct inode *inode = new_inode(sb);
  if (!inode)
    return NULL;

  inode_init_owner(&nop_mnt_idmap, inode, dir, mode);

  if (S_ISDIR(mode)) {
    inode->i_op  = &vtfs_inode_ops;
    inode->i_fop = &vtfs_dir_ops;
  } else {
    inode->i_op  = &vtfs_file_inode_ops;
    inode->i_fop = &vtfs_file_ops;
  }

  inode->i_ino = i_ino;
  inode->i_private = ri;
  return inode;
}

static int vtfs_fill_super(struct super_block *sb, void *data, int silent) {
  (void)data; (void)silent;

  sb->s_magic = VTFS_MAGIC;
  sb->s_op = &vtfs_super_ops;

  struct vtfs_remote_info *ri = kmalloc(sizeof(*ri), GFP_KERNEL);
  if (!ri) return -ENOMEM;

  ri->ino = 1000;
  ri->type = 1; // DIR
  ri->mode = S_IFDIR | 0777;
  ri->nlink = 1;

  struct inode *inode = vtfs_get_inode(sb, NULL, ri->mode, ri->ino, ri);
  if (!inode) { kfree(ri); return -ENOMEM; }

  sb->s_root = d_make_root(inode);
  if (!sb->s_root) return -ENOMEM;

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
  kill_litter_super(sb);
  LOG("vtfs super block is destroyed. Unmount successfully.\n");
}

// file system type
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