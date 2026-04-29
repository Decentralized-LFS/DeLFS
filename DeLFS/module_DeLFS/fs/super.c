#include <linux/export.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/mount.h>
#include <linux/security.h>
#include <linux/writeback.h>		/* for the emergency remount stuff */
#include <linux/idr.h>
#include <linux/mutex.h>
#include <linux/backing-dev.h>
#include <linux/rculist_bl.h>
#include <linux/fscrypt.h>
#include <linux/fsnotify.h>
#include <linux/lockdep.h>
#include <linux/user_namespace.h>
#include <linux/fs_context.h>
#include <uapi/linux/mount.h>
#include "internal.h"










static int test_bdev_super(struct super_block *s, void *data)
{
	return !(s->s_iflags & SB_I_RETIRED) && s->s_dev == *(dev_t *)data;
}

static int set_bdev_super(struct super_block *s, void *data)
{
	s->s_dev = *(dev_t *)data;
	return 0;
}

static inline void __super_lock(struct super_block *sb, bool excl)
{
	if (excl)
		down_write(&sb->s_umount);
	else
		down_read(&sb->s_umount);
}

static inline void super_unlock(struct super_block *sb, bool excl)
{
	if (excl)
		up_write(&sb->s_umount);
	else
		up_read(&sb->s_umount);
}

static inline void __super_lock_excl(struct super_block *sb)
{
	__super_lock(sb, true);
}

static inline void super_unlock_excl(struct super_block *sb)
{
	super_unlock(sb, true);
}


struct dentry *my_mount_bdev(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data,
	int (*fill_super)(struct super_block *, void *, int))
{
	struct super_block *s;
	int error;
	dev_t dev;

	error = lookup_bdev(dev_name, &dev);
	if (error)
		return ERR_PTR(error);

	flags |= SB_NOSEC;
	s = sget(fs_type, test_bdev_super, set_bdev_super, flags, &dev);
	if (IS_ERR(s))
		return ERR_CAST(s);

	if (s->s_root) {
		if ((flags ^ s->s_flags) & SB_RDONLY) {
			deactivate_locked_super(s);
			return ERR_PTR(-EBUSY);
		}
	} else {
		/*
		 * We drop s_umount here because we need to open the bdev and
		 * bdev->open_mutex ranks above s_umount (blkdev_put() ->
		 * bdev_mark_dead()). It is safe because we have active sb
		 * reference and SB_BORN is not set yet.
		 */
		super_unlock_excl(s);
		error = setup_bdev_super(s, flags, NULL);
		__super_lock_excl(s);
		if (!error) {
			error = fill_super(s, data, flags & SB_SILENT ? 1 : 0);
		}
		if (error) {
			deactivate_locked_super(s);
			return ERR_PTR(error);
		}

		s->s_flags |= SB_ACTIVE;
	}

	return dget(s->s_root);
}

void super_wake(struct super_block *sb, unsigned int flag);
int my_sync_filesystem(struct super_block *sb);

void my_generic_shutdown_super(struct super_block *sb)
{
	const struct super_operations *sop = sb->s_op;

	if (sb->s_root) {
		shrink_dcache_for_umount(sb);
		//sync_filesystem(sb);
		my_sync_filesystem(sb);
		sb->s_flags &= ~SB_ACTIVE;

		cgroup_writeback_umount();

		/* Evict all inodes with zero refcount. */
		evict_inodes(sb);

		/*
		 * Clean up and evict any inodes that still have references due
		 * to fsnotify or the security policy.
		 */
		fsnotify_sb_delete(sb);
		security_sb_delete(sb);

		/*
		 * Now that all potentially-encrypted inodes have been evicted,
		 * the fscrypt keyring can be destroyed.
		 */
		fscrypt_destroy_keyring(sb);

		if (sb->s_dio_done_wq) {
			destroy_workqueue(sb->s_dio_done_wq);
			sb->s_dio_done_wq = NULL;
		}

		if (sop->put_super)
			sop->put_super(sb);

		if (CHECK_DATA_CORRUPTION(!list_empty(&sb->s_inodes),
				"VFS: Busy inodes after unmount of %s (%s)",
				sb->s_id, sb->s_type->name)) {
			/*
			 * Adding a proper bailout path here would be hard, but
			 * we can at least make it more likely that a later
			 * iput_final() or such crashes cleanly.
			 */
			struct inode *inode;

			spin_lock(&sb->s_inode_list_lock);
			list_for_each_entry(inode, &sb->s_inodes, i_sb_list) {
				inode->i_op = VFS_PTR_POISON;
				inode->i_sb = VFS_PTR_POISON;
				inode->i_mapping = VFS_PTR_POISON;
			}
			spin_unlock(&sb->s_inode_list_lock);
		}
	}
	/*
	 * Broadcast to everyone that grabbed a temporary reference to this
	 * superblock before we removed it from @fs_supers that the superblock
	 * is dying. Every walker of @fs_supers outside of sget{_fc}() will now
	 * discard this superblock and treat it as dead.
	 *
	 * We leave the superblock on @fs_supers so it can be found by
	 * sget{_fc}() until we passed sb->kill_sb().
	 */
	super_wake(sb, SB_DYING);
	super_unlock_excl(sb);
	if (sb->s_bdi != &noop_backing_dev_info) {
		if (sb->s_iflags & SB_I_PERSB_BDI)
			bdi_unregister(sb->s_bdi);
		bdi_put(sb->s_bdi);
		sb->s_bdi = &noop_backing_dev_info;
	}
}

void my_kill_block_super(struct super_block *sb)
{
	struct block_device *bdev = sb->s_bdev;

	//generic_shutdown_super(sb);
	my_generic_shutdown_super(sb);
	if (bdev) {
		sync_blockdev(bdev);
		blkdev_put(bdev, sb);
	}
}

