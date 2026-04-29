#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
//#include <linux/fs.h>
#include <linux/f3fs_fs.h>

#include "f3fs.h"
#include "segment.h"
#include "oldinv.h"

#define OLDINV_MAX_BATCH 256
#define OLDINV_HIGH_WATERMARK 4096

static struct kmem_cache *oldinv_slab;
static DEFINE_MUTEX(oldinv_slab_lock);
static atomic_t oldinv_slab_users = ATOMIC_INIT(0);

int segno_to_sentry_cpu(struct f3fs_sb_info *sbi, unsigned int segno);
void update_segment_mtime(struct f3fs_sb_info *sbi, block_t blkaddr,
						unsigned long long old_mtime);
void update_sit_entry(struct f3fs_sb_info *sbi, block_t blkaddr, int del);
void locate_dirty_segment(struct f3fs_sb_info *sbi, unsigned int segno);

static int oldinv_slab_get(void)
{
        int ret = 0;

        mutex_lock(&oldinv_slab_lock);

        if (!oldinv_slab) {
                oldinv_slab = kmem_cache_create("oldinv_item",
                                sizeof(struct oldinv_item),
                                0, SLAB_RECLAIM_ACCOUNT, NULL);
                if (!oldinv_slab)
                        ret = -ENOMEM;
        }

        if (!ret)
                atomic_inc(&oldinv_slab_users);

        mutex_unlock(&oldinv_slab_lock);
        return ret;
}

static void oldinv_slab_put(void)
{
        mutex_lock(&oldinv_slab_lock);

        if (atomic_dec_and_test(&oldinv_slab_users)) {
                if (oldinv_slab) {
                        kmem_cache_destroy(oldinv_slab);
                        oldinv_slab = NULL;
                }
        }

        mutex_unlock(&oldinv_slab_lock);
}

static void oldinv_wait_quiescent(struct oldinv_worker *wk)
{
        wait_event(wk->wq,
                atomic_read(&wk->inflight) == 0 &&
                atomic_read(&wk->running) == 0);
}

KTDEC(locate_dirty_segment);
static bool try_invalidate_one_locked_internal(struct f3fs_sb_info *sbi, block_t blk)
{
        ktime_t stopwatch[2];
        unsigned int seg = GET_SEGNO(sbi, blk);
        unsigned int off;
        struct seg_entry *se;
        bool was_valid;

        if (seg == NULL_SEGNO)
                return false;

        se = get_seg_entry(sbi, seg);
        off = GET_BLKOFF_FROM_SEG0(sbi, blk);

        was_valid = f3fs_test_bit(off, (void *)se->cur_valid_map);
        if (!was_valid)
                return false;

        update_segment_mtime(sbi, blk, 0);
        update_sit_entry(sbi, blk, -1);

        ktget(&stopwatch[0]);
        locate_dirty_segment(sbi, seg);
        ktget(&stopwatch[1]);
        ktput(stopwatch, locate_dirty_segment);

        return true;
}

//static bool try_invalidate_one_locked_internal(struct f3fs_sb_info *sbi,
//                                               block_t blk)
//{
//        ktime_t stopwatch[2];
//        unsigned int seg = GET_SEGNO(sbi, blk);
//        unsigned int off;
//        struct seg_entry *se;
//
//        if (seg == NULL_SEGNO)
//                return false;
//
//        se = get_seg_entry(sbi, seg);
//        off = GET_BLKOFF_FROM_SEG0(sbi, blk);
//
//        /* 이미 invalid이면 아무 것도 하지 않음 */
//        if (!f3fs_test_bit(off, (void *)se->cur_valid_map))
//                return false;
//
//        update_segment_mtime(sbi, blk, 0);
//        update_sit_entry(sbi, blk, -1);
//
//        ktget(&stopwatch[0]);
//        locate_dirty_segment(sbi, seg);
//        ktget(&stopwatch[1]);
//        ktput(stopwatch, locate_dirty_segment);
//
//        return true;
//}

//static bool try_invalidate_one_locked_internal(struct f3fs_sb_info *sbi, block_t blk)
//{
//	ktime_t stopwatch[2];
//	unsigned int seg = GET_SEGNO(sbi, blk);
////	unsigned int off;
////	struct seg_entry *se;
////	bool was_valid;
//
//	if (seg == NULL_SEGNO)
//		return false;
//
////	se = get_seg_entry(sbi, seg);
////	off = f3fs_blks_in_seg(sbi) ? 
////		(blk - START_BLOCK(sbi, seg)) : GET_BLKOFF_FROM_SEG0(sbi, blk);
////	
////	was_valid = test_bit_le(off, (void *)se->cur_valid_map);
////	if (!was_valid)
////		return false;
//
//	update_segment_mtime(sbi, blk, 0);
//	update_sit_entry(sbi, blk, -1);
//	ktget(&stopwatch[0]);
//	locate_dirty_segment(sbi, seg);
//	ktget(&stopwatch[1]);
//	ktput(stopwatch, locate_dirty_segment);
//	return 0;
//}

KTDEF(try_invalidate_one_locked);
static bool try_invalidate_one_locked(struct f3fs_sb_info *sbi, block_t blk)
{
	ktime_t stopwatch[2];
	bool tmp;
	ktget(&stopwatch[0]);
	tmp = try_invalidate_one_locked_internal(sbi, blk);
	ktget(&stopwatch[1]);
	ktput(stopwatch, try_invalidate_one_locked);

	return tmp;
}


KTDEC(sentry_lock);
static int oldinv_thread(void *data)
{
	ktime_t stopwatch[2];
	struct oldinv_worker *wk = data;
	struct f3fs_sb_info *sbi = wk->sbi;
	struct sit_info *sit_i = SIT_I(sbi);
	struct rw_semaphore *se_lock;

	se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, wk->bucket);

	for (;;) {
		wait_event_interruptible(wk->wq, 
				!list_empty(&wk->q) || kthread_should_stop());

		spin_lock(&wk->qlock);
		if (kthread_should_stop() && list_empty(&wk->q)) {
			spin_unlock(&wk->qlock);
			break;
		}

		while (!list_empty(&wk->q)) {
			struct oldinv_item *it = 
				list_first_entry(&wk->q, struct oldinv_item, list);
			
			list_del(&it->list);
			atomic_dec(&wk->inflight);
			atomic_inc(&wk->running);
			spin_unlock(&wk->qlock);

			ktget(&stopwatch[0]);
			down_write(se_lock);
			ktget(&stopwatch[1]);
			ktput(stopwatch, sentry_lock);
			try_invalidate_one_locked(sbi, it->blkaddr);
			up_write(se_lock);

			kmem_cache_free(oldinv_slab, it);

			atomic_dec(&wk->running);
			spin_lock(&wk->qlock);
		}
		spin_unlock(&wk->qlock);

		if (atomic_read(&wk->inflight) == 0 &&
			atomic_read(&wk->running) == 0) {
			wake_up(&wk->wq);
		}	

	}

	return 0;
}

bool oldinv_has_pending(struct f3fs_sb_info *sbi)
{
	struct oldinv_ctx *ctx = sbi->oldinv;
	unsigned int i;

	if (!ctx)
		return false;

	for (i = 0; i < ctx->nbuckets; i++) {
		struct oldinv_worker *wk = &ctx->w[i];

		if (atomic_read(&wk->inflight) ||
		    atomic_read(&wk->running))
			return true;
	}

	return false;
}

void oldinv_enqueue(struct f3fs_sb_info *sbi, block_t old_blkaddr)
{
	ktime_t stopwatch[2];
	unsigned int seg = GET_SEGNO(sbi, old_blkaddr);
	int sentry_cpu;
	struct oldinv_worker *wk = NULL;
	struct oldinv_item *it;
	struct sit_info *sit_i;
	struct rw_semaphore *se_lock;
	bool force_sync;

	if (seg == NULL_SEGNO)
		return;

	sentry_cpu = segno_to_sentry_cpu(sbi, seg);
	sit_i = SIT_I(sbi);
	se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);

	/*
	 * oldinv context가 없거나 bucket index가 이상하면 async queue 사용 금지.
	 * 이 경우는 반드시 sync invalidation으로 처리한다.
	 */
	if (!sbi->oldinv ||
	    sentry_cpu < 0 ||
	    sentry_cpu >= sbi->oldinv->nbuckets) {
		force_sync = true;
	} else {
		wk = &sbi->oldinv->w[sentry_cpu];

		/*
		 * DATA old invalidation도 다음 상황에서는 async로 미루면 안 된다.
		 *
		 * 1) checkpoint drain 중
		 * 2) NODE segment
		 * 3) free section이 부족한 critical path
		 * 4) oldinv queue backlog가 이미 큰 경우
		 */
		force_sync = atomic_read(&sbi->oldinv_cp_mode) ||
			     IS_NODESEG(get_seg_entry(sbi, seg)->type) ||
			     has_not_enough_free_secs(sbi, 0, 0) ||
			     atomic_read(&wk->inflight) >= OLDINV_HIGH_WATERMARK;
	}

	if (force_sync) {
		ktget(&stopwatch[0]);
		down_write(se_lock);
		ktget(&stopwatch[1]);
		ktput(stopwatch, sentry_lock);

		try_invalidate_one_locked(sbi, old_blkaddr);
		up_write(se_lock);
		return;
	}

	it = kmem_cache_zalloc(oldinv_slab, GFP_ATOMIC);
	if (!it) {
		down_write(se_lock);
		try_invalidate_one_locked(sbi, old_blkaddr);
		up_write(se_lock);
		return;
	}

	it->blkaddr = old_blkaddr;

	spin_lock(&wk->qlock);
	list_add_tail(&it->list, &wk->q);
	atomic_inc(&wk->inflight);
	spin_unlock(&wk->qlock);

	wake_up(&wk->wq);
}

//void oldinv_enqueue(struct f3fs_sb_info *sbi, block_t old_blkaddr)
//{
//        ktime_t stopwatch[2];
//        unsigned int seg = GET_SEGNO(sbi, old_blkaddr);
//        int sentry_cpu;
//        struct oldinv_worker *wk;
//        struct oldinv_item *it;
//        struct sit_info *sit_i;
//        struct rw_semaphore *se_lock;
//        bool force_sync;
//
//        if (seg == NULL_SEGNO)
//                return;
//
//        sentry_cpu = segno_to_sentry_cpu(sbi, seg);
//        sit_i = SIT_I(sbi);
//        se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
//
//        /*
//         * 1) checkpoint drain 중에는 전부 동기 처리
//         * 2) NODE old invalidate는 평소에도 동기 처리
//         */
//        force_sync = atomic_read(&sbi->oldinv_cp_mode) ||
//                     IS_NODESEG(get_seg_entry(sbi, seg)->type);
//
//        if (force_sync) {
//                ktget(&stopwatch[0]);
//                down_write(se_lock);
//                ktget(&stopwatch[1]);
//                ktput(stopwatch, sentry_lock);
//
//                try_invalidate_one_locked(sbi, old_blkaddr);
//                up_write(se_lock);
//                return;
//        }
//
//        wk = &sbi->oldinv->w[sentry_cpu];
//
//        it = kmem_cache_zalloc(oldinv_slab, GFP_ATOMIC);
//        if (!it) {
//                down_write(se_lock);
//                try_invalidate_one_locked(sbi, old_blkaddr);
//                up_write(se_lock);
//                return;
//        }
//
//        it->blkaddr = old_blkaddr;
//
//        spin_lock(&wk->qlock);
//        list_add_tail(&it->list, &wk->q);
//        atomic_inc(&wk->inflight);
//        spin_unlock(&wk->qlock);
//
//        wake_up(&wk->wq);
//}

//void oldinv_enqueue(struct f3fs_sb_info *sbi, block_t old_blkaddr)
//{
//	ktime_t stopwatch[2];
//	unsigned int seg = GET_SEGNO(sbi, old_blkaddr);
//	int sentry_cpu;
//	struct oldinv_worker *wk;
//	struct oldinv_item *it;
//
//	if (seg == NULL_SEGNO)
//		return;
//
//	if (atomic_read(&sbi->oldinv_cp_mode)) {
//		printk(KERN_ERR "[%s] if 1\n", __func__);
//	        struct sit_info *sit_i = SIT_I(sbi);
//		int sentry_cpu = segno_to_sentry_cpu(sbi, seg);
//		struct rw_semaphore *se_lock = per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
//		ktget(&stopwatch[0]);
//		down_write(se_lock);
//		ktget(&stopwatch[1]);
//		ktput(stopwatch, sentry_lock);
//
//		try_invalidate_one_locked(sbi, old_blkaddr);
//		up_write(se_lock);
//		return;	
//	}
//
//	sentry_cpu = segno_to_sentry_cpu(sbi, seg);
//	wk = &sbi->oldinv->w[sentry_cpu];
//
//	it = kmem_cache_zalloc(oldinv_slab, GFP_ATOMIC);
//	if (!it) {
//		printk(KERN_ALERT "[%s] if 2\n", __func__);
//		struct sit_info *sit_i = SIT_I(sbi);
//		struct rw_semaphore *se_lock =
//			per_cpu_ptr(sit_i->percore_sentry_lock, sentry_cpu);
//
//		down_write(se_lock);
//		try_invalidate_one_locked(sbi, old_blkaddr);
//		up_write(se_lock);
//		return;
//	}
//
//	it->blkaddr = old_blkaddr;
//
//	spin_lock(&wk->qlock);
//	list_add_tail(&it->list, &wk->q);
//	atomic_inc(&wk->inflight);
//	spin_unlock(&wk->qlock);
//	wake_up(&wk->wq);
//}

void oldinv_flush_all(struct f3fs_sb_info *sbi)
{
        int i;
        struct oldinv_ctx *ctx = sbi->oldinv;

        if (!ctx)
                return;

        atomic_set(&sbi->oldinv_cp_mode, 1);
        smp_mb();

        for (i = 0; i < ctx->nbuckets; i++) {
                struct oldinv_worker *wk = &ctx->w[i];
                LIST_HEAD(local);
                struct oldinv_item *it, *tmp;
                int n = 0;

                spin_lock(&wk->qlock);
                if (!list_empty(&wk->q)) {
                        list_splice_init(&wk->q, &local);
                        list_for_each_entry(it, &local, list)
                                n++;
                        atomic_sub(n, &wk->inflight);
                }
                spin_unlock(&wk->qlock);

                if (!list_empty(&local)) {
                        struct sit_info *sit_i = SIT_I(sbi);
                        struct rw_semaphore *se_lock =
                                per_cpu_ptr(sit_i->percore_sentry_lock, i);

                        down_write(se_lock);
                        list_for_each_entry_safe(it, tmp, &local, list) {
                                list_del(&it->list);
                                try_invalidate_one_locked(sbi, it->blkaddr);
                                kmem_cache_free(oldinv_slab, it);
                        }
                        up_write(se_lock);
                }

                /*
                 * local로 빼서 처리한 것 외에,
                 * 이미 worker가 dequeue해서 돌고 있던 running 작업까지 모두 끝날 때까지 대기
                 */
                oldinv_wait_quiescent(wk);
        }

        smp_mb();
        atomic_set(&sbi->oldinv_cp_mode, 0);
}

//void oldinv_flush_all(struct f3fs_sb_info *sbi)
//{
//	atomic_set(&sbi->oldinv_cp_mode, 1);
//
//	int i;
//	struct oldinv_ctx *ctx = sbi->oldinv;
//
//	/* 각 버킷에 대해: 큐 스플라이스 + 동기 처리 */
//	for (i = 0; i < ctx->nbuckets; i++) {
//		struct oldinv_worker *wk = &ctx->w[i];
//		LIST_HEAD(local);
//
//		/* 큐를 통째로 가져오고 inflight를 한 번에 감소 */
//		spin_lock(&wk->qlock);
//		if (!list_empty(&wk->q)) {
//			struct oldinv_item *it;
//			int n = 0;
//			list_splice_init(&wk->q, &local);
//			/* inflight 감소 수를 세려면 리스트 한 바퀴 돌거나,
//			wk->inflight를 0으로 만들고 running만 고려해도 됨 */
//			list_for_each_entry(it, &local, list) n++;
//			atomic_sub(n, &wk->inflight);
//		}
//		spin_unlock(&wk->qlock);
//
//		if (!list_empty(&local)) {
//			/* 이 버킷 락 하나만 잡고 배치로 싹 처리 */
//			struct sit_info *sit_i = SIT_I(sbi);
//			struct rw_semaphore *se_lock =
//						per_cpu_ptr(sit_i->percore_sentry_lock, i);
//			struct oldinv_item *it, *tmp;
//
//			down_write(se_lock);
//			list_for_each_entry_safe(it, tmp, &local, list) {
//				list_del(&it->list);
//				try_invalidate_one_locked(sbi, it->blkaddr);
//				kmem_cache_free(oldinv_slab, it);
//			}
//			up_write(se_lock);
//		}
//	}
//
//	atomic_set(&sbi->oldinv_cp_mode, 0);
//}


int oldinv_init(struct f3fs_sb_info *sbi)
{
        unsigned int i, nb = num_online_cpus();
        struct oldinv_ctx *ctx;
        int err;

        err = oldinv_slab_get();
        if (err)
                return err;

        ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
        if (!ctx) {
                oldinv_slab_put();
                return -ENOMEM;
        }

        ctx->nbuckets = nb;
        ctx->w = kcalloc(nb, sizeof(*ctx->w), GFP_KERNEL);
        if (!ctx->w) {
                kfree(ctx);
                oldinv_slab_put();
                return -ENOMEM;
        }

        for (i = 0; i < nb; i++) {
                struct oldinv_worker *wk = &ctx->w[i];
                wk->sbi = sbi;
                wk->bucket = i;
                init_waitqueue_head(&wk->wq);
                spin_lock_init(&wk->qlock);
                INIT_LIST_HEAD(&wk->q);
                atomic_set(&wk->inflight, 0);
                atomic_set(&wk->running, 0);

                wk->thread = kthread_create(oldinv_thread, wk, "oldinv/%u", i);
                if (IS_ERR(wk->thread)) {
                        err = PTR_ERR(wk->thread);
                        while (i--)
                                kthread_stop(ctx->w[i].thread);
                        kfree(ctx->w);
                        kfree(ctx);
                        oldinv_slab_put();
                        return err;
                }

                kthread_bind(wk->thread, i);
                wake_up_process(wk->thread);
        }

        sbi->oldinv = ctx;
        atomic_set(&sbi->oldinv_cp_mode, 0);
        return 0;
}

//int oldinv_init(struct f3fs_sb_info *sbi)
//{
//	printk("[%s]: start\n", __func__);
//	unsigned int i, nb = num_online_cpus();
//	struct oldinv_ctx *ctx;
//
//	if (!oldinv_slab) {
//		oldinv_slab = kmem_cache_create("oldinv_item", sizeof(struct oldinv_item),
//						0, SLAB_RECLAIM_ACCOUNT, NULL);
//		if (!oldinv_slab)
//			return -ENOMEM;
//	}
//
//	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
//	if (!ctx)
//		return -ENOMEM;
//
//	ctx->nbuckets = nb;
//	ctx->w = kcalloc(nb, sizeof(*ctx->w), GFP_KERNEL);
//	if (!ctx->w) {
//		kfree(ctx);
//		return -ENOMEM;
//	}
//
//	for (i = 0; i < nb; i++) {
//		struct oldinv_worker *wk = &ctx->w[i];
//		wk->sbi = sbi;
//		wk->bucket = i;
//		init_waitqueue_head(&wk->wq);
//		spin_lock_init(&wk->qlock);
//		INIT_LIST_HEAD(&wk->q);
//		atomic_set(&wk->inflight, 0);
//		atomic_set(&wk->running, 0);
//
//		wk->thread = kthread_create(oldinv_thread, wk, "oldinv/%u", i);
//		if (IS_ERR(wk->thread)) {
//			int err = PTR_ERR(wk->thread);
//
//			while (i--) {
//				kthread_stop(ctx->w[i].thread);
//			}
//			kfree(ctx->w);
//			kfree(ctx);
//			return err;
//		}
//
//		kthread_bind(wk->thread, i);
//		wake_up_process(wk->thread);
//	}
//
//	sbi->oldinv = ctx;
//	return 0;
//}

void oldinv_exit(struct f3fs_sb_info *sbi)
{
        unsigned int i;
        struct oldinv_ctx *ctx = sbi->oldinv;

        if (!ctx)
                return;

        oldinv_flush_all(sbi);

        for (i = 0; i < ctx->nbuckets; i++) {
                struct oldinv_worker *wk = &ctx->w[i];

                kthread_stop(wk->thread);

                spin_lock(&wk->qlock);
                while (!list_empty(&wk->q)) {
                        struct oldinv_item *it =
                                list_first_entry(&wk->q, struct oldinv_item, list);
                        list_del(&it->list);
                        kmem_cache_free(oldinv_slab, it);
                }
                spin_unlock(&wk->qlock);
        }

        kfree(ctx->w);
        kfree(ctx);
        sbi->oldinv = NULL;

        atomic_set(&sbi->oldinv_cp_mode, 0);
        oldinv_slab_put();
}

//void oldinv_exit(struct f3fs_sb_info *sbi)
//{
//	printk("[%s]: start\n", __func__);
//	unsigned int i;
//	struct oldinv_ctx *ctx = sbi->oldinv;
//	if (!ctx) return;
//
//	for (i = 0; i < ctx->nbuckets; i++) {
//		struct oldinv_worker *wk = &ctx->w[i];
//		kthread_stop(wk->thread);
//		wake_up(&wk->wq);
//
//		spin_lock(&wk->qlock);
//		BUG_ON(!list_empty(&wk->q));
////		while (!list_empty(&wk->q)) {
////			struct oldinv_item *it = 
////				list_first_entry(&wk->q, struct oldinv_item, list);
////			list_del(&it->list);
////			kmem_cache_free(oldinv_slab, it);
////		}
//		spin_unlock(&wk->qlock);
//	}
//	kfree(ctx->w);
//	kfree(ctx);
//	sbi->oldinv = NULL;
//
//	if (oldinv_slab)
//		kmem_cache_destroy(oldinv_slab);
//}








