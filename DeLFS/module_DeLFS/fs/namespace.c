#include <linux/syscalls.h>
#include <linux/export.h>
#include <linux/capability.h>
#include <linux/mnt_namespace.h>
#include <linux/user_namespace.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/cred.h>
#include <linux/idr.h>
#include <linux/init.h>		/* init_rootfs */
#include <linux/fs_struct.h>	/* get_fs_root et.al. */
#include <linux/fsnotify.h>	/* fsnotify_vfsmount_delete */
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/proc_ns.h>
#include <linux/magic.h>
#include <linux/memblock.h>
#include <linux/proc_fs.h>
#include <linux/task_work.h>
#include <linux/sched/task.h>
#include <uapi/linux/mount.h>
#include <linux/fs_context.h>
#include <linux/shmem_fs.h>
#include <linux/mnt_idmapping.h>

#include "pnode.h"
#include "internal.h"






void super_wake(struct super_block *sb, unsigned int flag);


int my_vfs_get_tree(struct fs_context *fc)
{
	struct super_block *sb;
	int error;

	if (fc->root)
		return -EBUSY;

	/* Get the mountable root in fc->root, with a ref on the root and a ref
	 * on the superblock.
	 */
	error = fc->ops->get_tree(fc);
	if (error < 0) { //taehwan return here
		return error;
	}

	if (!fc->root) {
		pr_err("Filesystem %s get_tree() didn't set fc->root\n",
		       fc->fs_type->name);
		/* We don't know what the locking state of the superblock is -
		 * if there is a superblock.
		 */
		BUG();
	}

	sb = fc->root->d_sb;
	WARN_ON(!sb->s_bdi);

	/*
	 * super_wake() contains a memory barrier which also care of
	 * ordering for super_cache_count(). We place it before setting
	 * SB_BORN as the data dependency between the two functions is
	 * the superblock structure contents that we just set up, not
	 * the SB_BORN flag.
	 */
	super_wake(sb, SB_BORN);

	error = security_sb_set_mnt_opts(sb, fc->security, 0, NULL);
	if (unlikely(error)) {
		fc_drop_locked(fc);
		return error;
	}

	/*
	 * filesystems should never set s_maxbytes larger than MAX_LFS_FILESIZE
	 * but s_maxbytes was an unsigned long long for many releases. Throw
	 * this warning for a little while to try and catch filesystems that
	 * violate this rule.
	 */
	WARN((sb->s_maxbytes < 0), "%s set sb->s_maxbytes to "
		"negative value (%lld)\n", fc->fs_type->name, sb->s_maxbytes);

	return 0;
}

int do_new_mount_fc(struct fs_context *fc, struct path *mountpoint,
			   unsigned int mnt_flags);

static int my_do_new_mount(struct path *path, const char *fstype, int sb_flags,
			int mnt_flags, const char *name, void *data)
{
	struct file_system_type *type;
	struct fs_context *fc;
	const char *subtype = NULL;
	int err = 0;

	if (!fstype) {
		return -EINVAL;
	}

	type = get_fs_type(fstype);
	if (!type) {
		return -ENODEV;
	}

	if (type->fs_flags & FS_HAS_SUBTYPE) {
		subtype = strchr(fstype, '.');
		if (subtype) {
			subtype++;
			if (!*subtype) {
				put_filesystem(type);
				return -EINVAL;
			}
		}
	}

	fc = fs_context_for_mount(type, sb_flags);
	put_filesystem(type);
	if (IS_ERR(fc)) {
		return PTR_ERR(fc);
	}

	if (subtype) {
		err = vfs_parse_fs_string(fc, "subtype",
					  subtype, strlen(subtype));
	}
	if (!err && name) {
		err = vfs_parse_fs_string(fc, "source", name, strlen(name));
	}
	if (!err) {
		err = parse_monolithic_mount_data(fc, data);
	}
	if (!err && !mount_capable(fc)) {
		err = -EPERM;
	}
	if (!err) {
		err = my_vfs_get_tree(fc); //taehwan err here
	}
	if (!err) {
		err = do_new_mount_fc(fc, path, mnt_flags);
	}

	put_fs_context(fc);
	return err;
}


int security_sb_mount(const char *dev_name, const struct path *path,
		      const char *type, unsigned long flags, void *data);
void warn_mandlock(void);
int do_reconfigure_mnt(struct path *path, unsigned int mnt_flags);
int do_remount(struct path *path, int ms_flags, int sb_flags,
		      int mnt_flags, void *data);
int do_loopback(struct path *path, const char *old_name,
				int recurse);
int do_change_type(struct path *path, int ms_flags);
int do_move_mount_old(struct path *path, const char *old_name);

int my_path_mount(const char *dev_name, struct path *path,
		const char *type_page, unsigned long flags, void *data_page)
{
	unsigned int mnt_flags = 0, sb_flags;
	int ret;

	/* Discard magic */
	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

	/* Basic sanity checks */
	if (data_page)
		((char *)data_page)[PAGE_SIZE - 1] = 0;

	if (flags & MS_NOUSER)
		return -EINVAL;

	ret = security_sb_mount(dev_name, path, type_page, flags, data_page);
	if (ret)
		return ret;
	if (!may_mount())
		return -EPERM;
	if (flags & SB_MANDLOCK)
		warn_mandlock();

	/* Default to relatime unless overriden */
	if (!(flags & MS_NOATIME))
		mnt_flags |= MNT_RELATIME;

	/* Separate the per-mountpoint flags */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	if (flags & MS_NOATIME)
		mnt_flags |= MNT_NOATIME;
	if (flags & MS_NODIRATIME)
		mnt_flags |= MNT_NODIRATIME;
	if (flags & MS_STRICTATIME)
		mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
	if (flags & MS_RDONLY)
		mnt_flags |= MNT_READONLY;
	if (flags & MS_NOSYMFOLLOW)
		mnt_flags |= MNT_NOSYMFOLLOW;

	/* The default atime for remount is preservation */
	if ((flags & MS_REMOUNT) &&
	    ((flags & (MS_NOATIME | MS_NODIRATIME | MS_RELATIME |
		       MS_STRICTATIME)) == 0)) {
		mnt_flags &= ~MNT_ATIME_MASK;
		mnt_flags |= path->mnt->mnt_flags & MNT_ATIME_MASK;
	}

	sb_flags = flags & (SB_RDONLY |
			    SB_SYNCHRONOUS |
			    SB_MANDLOCK |
			    SB_DIRSYNC |
			    SB_SILENT |
			    SB_POSIXACL |
			    SB_LAZYTIME |
			    SB_I_VERSION);

	if ((flags & (MS_REMOUNT | MS_BIND)) == (MS_REMOUNT | MS_BIND))  {
		return do_reconfigure_mnt(path, mnt_flags);
	}
	if (flags & MS_REMOUNT) {
		return do_remount(path, flags, sb_flags, mnt_flags, data_page);
	}
	if (flags & MS_BIND) {
		return do_loopback(path, dev_name, flags & MS_REC);
	}
	if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE)) {
		return do_change_type(path, flags);
	}
	if (flags & MS_MOVE) {
		return do_move_mount_old(path, dev_name);
	}

	return my_do_new_mount(path, type_page, sb_flags, mnt_flags, dev_name,
			    data_page);
}


long my_do_mount(const char *dev_name, const char __user *dir_name,
		const char *type_page, unsigned long flags, void *data_page)
{
	struct path path;
	int ret;

	ret = user_path_at(AT_FDCWD, dir_name, LOOKUP_FOLLOW, &path);
	if (ret) {
		return ret;
	}
	ret = my_path_mount(dev_name, &path, type_page, flags, data_page);
	path_put(&path);
	return ret;
}

