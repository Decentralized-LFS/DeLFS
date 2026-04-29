#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/kthread.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/backing-dev.h>
#include <linux/tracepoint.h>
#include <linux/device.h>
#include <linux/memcontrol.h>
#include "internal.h"
#include <linux/calclock.h>

/*
 * 4MB minimal write chunk size
 */
#define MIN_WRITEBACK_PAGES	(4096UL >> (PAGE_SHIFT - 10))

/*
 * Passed into wb_writeback(), essentially a subset of writeback_control
 */
struct wb_writeback_work {
	long nr_pages;
	struct super_block *sb;
	enum writeback_sync_modes sync_mode;
	unsigned int tagged_writepages:1;
	unsigned int for_kupdate:1;
	unsigned int range_cyclic:1;
	unsigned int for_background:1;
	unsigned int for_sync:1;	/* sync(2) WB_SYNC_ALL writeback */
	unsigned int auto_free:1;	/* free on completion */
	enum wb_reason reason;		/* why was writeback initiated? */

	struct list_head list;		/* pending work list */
	struct wb_completion *done;	/* set if the caller waits */
};









void queue_io(struct bdi_writeback *wb, struct wb_writeback_work *work,
		     unsigned long dirtied_before);
long writeback_sb_inodes(struct super_block *sb,
				struct bdi_writeback *wb,
				struct wb_writeback_work *work);
long __writeback_inodes_wb(struct bdi_writeback *wb,
				  struct wb_writeback_work *work);
inline struct inode *wb_inode(struct list_head *head);
void inode_sleep_on_writeback(struct inode *inode)
	__releases(inode->i_lock);


KTDEF(writeback_sb_inodes);
KTDEF(__writeback_inodes_wb);
KTDEF(inode_sleep_on_writeback);
static long df_wb_writeback(struct bdi_writeback *wb,
			 struct wb_writeback_work *work)
{
	ktime_t stopwatch[2];
	long nr_pages = work->nr_pages;
	unsigned long dirtied_before = jiffies;
	struct inode *inode;
	long progress;
	struct blk_plug plug;

	blk_start_plug(&plug);
	for (;;) {
		/*
		 * Stop writeback when nr_pages has been consumed
		 */
		if (work->nr_pages <= 0)
			break;

		/*
		 * Background writeout and kupdate-style writeback may
		 * run forever. Stop them if there is other work to do
		 * so that e.g. sync can proceed. They'll be restarted
		 * after the other works are all done.
		 */
		if ((work->for_background || work->for_kupdate) &&
		    !list_empty(&wb->work_list))
			break;

		/*
		 * For background writeout, stop when we are below the
		 * background dirty threshold
		 */
		if (work->for_background && !wb_over_bg_thresh(wb))
			break;


		spin_lock(&wb->list_lock);

		/*
		 * Kupdate and background works are special and we want to
		 * include all inodes that need writing. Livelock avoidance is
		 * handled by these works yielding to any other work so we are
		 * safe.
		 */
		if (work->for_kupdate) {
			dirtied_before = jiffies -
				msecs_to_jiffies(dirty_expire_interval * 10);
		} else if (work->for_background)
			dirtied_before = jiffies;

		//trace_writeback_start(wb, work);
		if (list_empty(&wb->b_io))
			queue_io(wb, work, dirtied_before);

		if (work->for_background && !wb_over_bg_thresh(wb)) {
			spin_unlock(&wb->list_lock);
			break;
		}

		if (work->sb) {
			ktget(&stopwatch[0]);
			progress = writeback_sb_inodes(work->sb, wb, work);
			ktget(&stopwatch[1]);
			ktput(stopwatch, writeback_sb_inodes);
		} else {
			ktget(&stopwatch[0]);
			progress = __writeback_inodes_wb(wb, work);
			ktget(&stopwatch[1]);
			ktput(stopwatch, __writeback_inodes_wb);
		}
		//trace_writeback_written(wb, work);

		/*
		 * Did we write something? Try for more
		 *
		 * Dirty inodes are moved to b_io for writeback in batches.
		 * The completion of the current batch does not necessarily
		 * mean the overall work is done. So we keep looping as long
		 * as made some progress on cleaning pages or inodes.
		 */
		if (progress) {
			spin_unlock(&wb->list_lock);
			continue;
		}

		/*
		 * No more inodes for IO, bail
		 */
		if (list_empty(&wb->b_more_io)) {
			spin_unlock(&wb->list_lock);
			break;
		}

		/*
		 * Nothing written. Wait for some inode to
		 * become available for writeback. Otherwise
		 * we'll just busyloop.
		 */
		//trace_writeback_wait(wb, work);
		inode = wb_inode(wb->b_more_io.prev);
		spin_lock(&inode->i_lock);
		spin_unlock(&wb->list_lock);
		/* This function drops i_lock... */
		ktget(&stopwatch[0]);
		inode_sleep_on_writeback(inode);
		ktget(&stopwatch[1]);
		ktput(stopwatch, inode_sleep_on_writeback);
	}
	blk_finish_plug(&plug);

	return nr_pages - work->nr_pages;
}


static long df_wb_check_background_flush(struct bdi_writeback *wb)
{
	if (wb_over_bg_thresh(wb)) {

		struct wb_writeback_work work = {
			.nr_pages	= LONG_MAX,
			.sync_mode	= WB_SYNC_NONE,
			.for_background	= 1,
			.range_cyclic	= 1,
			.reason		= WB_REASON_BACKGROUND,
		};

		//return wb_writeback(wb, &work);
		return df_wb_writeback(wb, &work);
	}

	return 0;
}

long wb_check_start_all(struct bdi_writeback *wb);
long wb_check_old_data_flush(struct bdi_writeback *wb);

KTDEF(wb_check_start_all);
KTDEF(wb_check_old_data_flush);
KTDEF(df_wb_check_background_flush);
long df_wb_do_writeback(struct bdi_writeback *wb)
{
	ktime_t stopwatch[2];
	long wrote = 0;

	if (!__atomic_fetch_add(&wb->nr_threads, 1, __ATOMIC_SEQ_CST))
		set_bit(WB_writeback_running, &wb->state);
	/*
	 * Check for a flush-everything request
	 */
	ktget(&stopwatch[0]);
	wrote += wb_check_start_all(wb);
	ktget(&stopwatch[1]);
	ktput(stopwatch, wb_check_start_all);

	/*
	 * Check for periodic writeback, kupdated() style
	 */
	ktget(&stopwatch[0]);
	wrote += wb_check_old_data_flush(wb);
	ktget(&stopwatch[1]);
	ktput(stopwatch, wb_check_old_data_flush);

	ktget(&stopwatch[0]);
	wrote += df_wb_check_background_flush(wb);
	ktget(&stopwatch[1]);
	ktput(stopwatch, df_wb_check_background_flush);
	if (!__atomic_sub_fetch(&wb->nr_threads, 1, __ATOMIC_SEQ_CST))
		clear_bit(WB_writeback_running, &wb->state);

	return wrote;
}

static void wait_sb_inodes(struct super_block *sb)
{
	printk("[%s]: start\n", __func__);
	LIST_HEAD(sync_list);

	/*
	 * We need to be protected against the filesystem going from
	 * r/o to r/w or vice versa.
	 */
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	printk("[%s] if 1\n", __func__);
	mutex_lock(&sb->s_sync_lock);
	printk("[%s] if 2\n", __func__);

	/*
	 * Splice the writeback list onto a temporary list to avoid waiting on
	 * inodes that have started writeback after this point.
	 *
	 * Use rcu_read_lock() to keep the inodes around until we have a
	 * reference. s_inode_wblist_lock protects sb->s_inodes_wb as well as
	 * the local list because inodes can be dropped from either by writeback
	 * completion.
	 */
	rcu_read_lock();
	printk("[%s] if 3\n", __func__);
	spin_lock_irq(&sb->s_inode_wblist_lock);
	printk("[%s] if 4\n", __func__);
	list_splice_init(&sb->s_inodes_wb, &sync_list);
	printk("[%s] if 5\n", __func__);

	/*
	 * Data integrity sync. Must wait for all pages under writeback, because
	 * there may have been pages dirtied before our sync call, but which had
	 * writeout started before we write it out.  In which case, the inode
	 * may not be on the dirty list, but we still have to wait for that
	 * writeout.
	 */
	while (!list_empty(&sync_list)) {
		printk("[%s] if 6\n", __func__);
		struct inode *inode = list_first_entry(&sync_list, struct inode,
						       i_wb_list);
		struct address_space *mapping = inode->i_mapping;

		/*
		 * Move each inode back to the wb list before we drop the lock
		 * to preserve consistency between i_wb_list and the mapping
		 * writeback tag. Writeback completion is responsible to remove
		 * the inode from either list once the writeback tag is cleared.
		 */
		list_move_tail(&inode->i_wb_list, &sb->s_inodes_wb);

		/*
		 * The mapping can appear untagged while still on-list since we
		 * do not have the mapping lock. Skip it here, wb completion
		 * will remove it.
		 */
		if (!mapping_tagged(mapping, PAGECACHE_TAG_WRITEBACK))
			continue;

		spin_unlock_irq(&sb->s_inode_wblist_lock);

		printk("[%s] if 7\n", __func__);
		spin_lock(&inode->i_lock);
		printk("[%s] if 8\n", __func__);
		if (inode->i_state & (I_FREEING|I_WILL_FREE|I_NEW)) {
			spin_unlock(&inode->i_lock);

			printk("[%s] if 9\n", __func__);
			spin_lock_irq(&sb->s_inode_wblist_lock);
			continue;
		}
		printk("[%s] if 10\n", __func__);
		__iget(inode);
		printk("[%s] if 11\n", __func__);
		spin_unlock(&inode->i_lock);
		rcu_read_unlock();

		/*
		 * We keep the error status of individual mapping so that
		 * applications can catch the writeback error using fsync(2).
		 * See filemap_fdatawait_keep_errors() for details.
		 */
		printk("[%s] if 12\n", __func__);
		filemap_fdatawait_keep_errors(mapping); //o
		printk("[%s] if 13\n", __func__);

		cond_resched();

		iput(inode);
		printk("[%s] if 14\n", __func__);

		rcu_read_lock();
		printk("[%s] if 15\n", __func__);
		spin_lock_irq(&sb->s_inode_wblist_lock);
		printk("[%s] if 16\n", __func__);
	}
	spin_unlock_irq(&sb->s_inode_wblist_lock);
	rcu_read_unlock();
	mutex_unlock(&sb->s_sync_lock);
}

void bdi_down_write_wb_switch_rwsem(struct backing_dev_info *bdi);
void bdi_split_work_to_wbs(struct backing_dev_info *bdi,
				  struct wb_writeback_work *base_work,
				  bool skip_if_busy);
void bdi_up_write_wb_switch_rwsem(struct backing_dev_info *bdi);

void my_sync_inodes_sb(struct super_block *sb)
{
	printk("[%s]: start\n", __func__);
	struct backing_dev_info *bdi = sb->s_bdi;
	DEFINE_WB_COMPLETION(done, bdi);
	struct wb_writeback_work work = {
		.sb		= sb,
		.sync_mode	= WB_SYNC_ALL,
		.nr_pages	= LONG_MAX,
		.range_cyclic	= 0,
		.done		= &done,
		.reason		= WB_REASON_SYNC,
		.for_sync	= 1,
	};

	/*
	 * Can't skip on !bdi_has_dirty() because we should wait for !dirty
	 * inodes under writeback and I_DIRTY_TIME inodes ignored by
	 * bdi_has_dirty() need to be written out too.
	 */
	if (bdi == &noop_backing_dev_info)
		return;
	WARN_ON(!rwsem_is_locked(&sb->s_umount));

	/* protect against inode wb switch, see inode_switch_wbs_work_fn() */
	printk("[%s] if 1\n", __func__);
	bdi_down_write_wb_switch_rwsem(bdi);
	printk("[%s] if 2\n", __func__);
	bdi_split_work_to_wbs(bdi, &work, false);
	printk("[%s] if 3\n", __func__);
	wb_wait_for_completion(&done);
	printk("[%s] if 4\n", __func__);
	bdi_up_write_wb_switch_rwsem(bdi);
	printk("[%s] if 5\n", __func__);

	wait_sb_inodes(sb);
	printk("[%s] if 6\n", __func__);
}

