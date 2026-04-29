#ifndef OLDINV_H
#define OLDINV_H


#include "f3fs.h"

struct oldinv_item {
	struct list_head list;
	block_t blkaddr;
};

struct oldinv_worker {
	struct task_struct *thread;
	wait_queue_head_t wq;

	spinlock_t qlock;
	struct list_head q;

	/*
	 * inflight: queue 안에 아직 worker가 가져가지 않은 oldinv item 수
	 * running: worker가 queue에서 빼서 실제 invalidation 중인 item 수
	 */
	atomic_t inflight;
	atomic_t running;

	struct f3fs_sb_info *sbi;
	int bucket;
};

struct oldinv_ctx {
	struct oldinv_worker *w;
	unsigned int nbuckets;
};

int oldinv_init(struct f3fs_sb_info *sbi);
void oldinv_exit(struct f3fs_sb_info *sbi);

void oldinv_enqueue(struct f3fs_sb_info *sbi, block_t old_blkaddr);
void oldinv_flush_all(struct f3fs_sb_info *sbi);
bool oldinv_has_pending(struct f3fs_sb_info *sbi);

#endif
