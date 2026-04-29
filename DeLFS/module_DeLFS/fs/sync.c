#include <linux/blkdev.h>
#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/namei.h>
#include <linux/sched.h>
#include <linux/writeback.h>
#include <linux/syscalls.h>
#include <linux/linkage.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/backing-dev.h>
#include "internal.h"

#define VALID_FLAGS (SYNC_FILE_RANGE_WAIT_BEFORE|SYNC_FILE_RANGE_WRITE| \
			SYNC_FILE_RANGE_WAIT_AFTER)


void my_sync_inodes_sb(struct super_block *sb);

int my_sync_filesystem(struct super_block *sb)
{
	int ret = 0;

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/*
	 * No point in syncing out anything if the filesystem is read-only.
	 */
	if (sb_rdonly(sb))
		return 0;

	/*
	 * Do the filesystem syncing work.  For simple filesystems
	 * writeback_inodes_sb(sb) just dirties buffers with inodes so we have
	 * to submit I/O for these buffers via sync_blockdev().  This also
	 * speeds up the wait == 1 case since in that case write_inode()
	 * methods call sync_dirty_buffer() and thus effectively write one block
	 * at a time.
	 */
	writeback_inodes_sb(sb, WB_REASON_SYNC);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 0);
		if (ret) {
			return ret;
		}
	}
	ret = sync_blockdev_nowait(sb->s_bdev);
	if (ret)
		return ret;

	//sync_inodes_sb(sb);
	my_sync_inodes_sb(sb);
	if (sb->s_op->sync_fs) {
		ret = sb->s_op->sync_fs(sb, 1);
		if (ret) {
			return ret;
		}
	}
	return sync_blockdev(sb->s_bdev);
}

