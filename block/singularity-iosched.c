// SPDX-License-Identifier: GPL-2.0
/*
 * Singularity cgroup-aware latency I/O scheduler.
 *
 * Original author: The_Anomalist <sellgren11@gmail.om>
 *                  ("Singularity Performance Team")
 * Ported from the Singularity kernel (block/singularity-iosched.c,
 * commit e84d9543887b, 2026-03-01) with permission; behavior unchanged.
 */
#include <linux/bio.h>
#include <linux/bitmap.h>
#include <linux/blk-cgroup.h>
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/jiffies.h>

#define SING_NR_CLASSES          2
#define SING_SYNC_CLASS          0
#define SING_BG_CLASS            1
#define SING_MAX_BUCKETS         16
#define SING_DYN_FG_BATCH_MIN    8
#define SING_DYN_FG_BATCH_MAX    32
#define SING_BG_STARVE_MS        6
#define SING_SYNC_STARVE_MS      8
#define SING_EXPIRED_FG_BATCH    8

/*
 * Switching classes too frequently breaks up the request stream delivered to
 * fast flash storage.  Keep a substantial foreground run by default so that
 * sequential writes and parallel random I/O can keep the device queue full.
 */
static unsigned int sing_fg_batch = 24;
module_param_named(fg_batch, sing_fg_batch, uint, 0644);
MODULE_PARM_DESC(fg_batch, "Consecutive sync-class dispatch budget");

static bool sing_adaptive_batch = true;
module_param_named(adaptive_batch, sing_adaptive_batch, bool, 0644);
MODULE_PARM_DESC(adaptive_batch,
		 "Adapt sync dispatch budget to queue pressure and write ratio");

static unsigned int sing_bg_starve_ms = SING_BG_STARVE_MS;
module_param_named(bg_starve_ms, sing_bg_starve_ms, uint, 0644);
MODULE_PARM_DESC(bg_starve_ms,
		 "Async queue age threshold for priority dispatch");

static unsigned int sing_sync_starve_ms = SING_SYNC_STARVE_MS;
module_param_named(sync_starve_ms, sing_sync_starve_ms, uint, 0644);
MODULE_PARM_DESC(sync_starve_ms,
		 "Sync queue age threshold for oldest-first bucket selection");

struct sing_data {
	struct list_head q[SING_NR_CLASSES][SING_MAX_BUCKETS];
	unsigned long active[SING_NR_CLASSES][BITS_TO_LONGS(SING_MAX_BUCKETS)];
	unsigned int rr[SING_NR_CLASSES];
	unsigned int fg_budget;
	unsigned int queued[SING_NR_CLASSES];
};

static inline unsigned int sing_class(struct request *rq)
{
	return rq_is_sync(rq) ? SING_SYNC_CLASS : SING_BG_CLASS;
}

static inline unsigned int sing_bucket(struct request *rq)
{
	struct blkcg *blkcg;
	u64 serial = 0;

	if (rq->bio) {
		blkcg = bio_blkcg(rq->bio);
		if (blkcg)
			serial = blkcg->css.serial_nr;
	}

	return hash_64(serial, ilog2(SING_MAX_BUCKETS));
}

static inline struct list_head *sing_list(struct sing_data *sd,
					  struct request *rq)
{
	return &sd->q[sing_class(rq)][sing_bucket(rq)];
}

/* FIFO age is a scheduler wait threshold, not a completion deadline. */
static unsigned int sing_expired_bucket(struct sing_data *sd,
				       unsigned int class, unsigned int starve_ms)
{
	unsigned int i, oldest = SING_MAX_BUCKETS;
	unsigned long now = jiffies, oldest_time = 0;
	unsigned long starve = msecs_to_jiffies(max_t(unsigned int, 1, starve_ms));

	for (i = find_first_bit(sd->active[class], SING_MAX_BUCKETS);
	     i < SING_MAX_BUCKETS;
	     i = find_next_bit(sd->active[class], SING_MAX_BUCKETS, i + 1)) {
		struct request *rq;
		unsigned long queued_at;

		rq = list_first_entry(&sd->q[class][i],
				      struct request, queuelist);
		queued_at = (unsigned long)rq->fifo_time;
		if (time_before(now, queued_at + starve))
			continue;
		if (oldest == SING_MAX_BUCKETS ||
		    time_before(queued_at, oldest_time)) {
			oldest = i;
			oldest_time = queued_at;
		}
	}

	return oldest;
}

static inline unsigned int sing_dynamic_fg_batch(struct sing_data *sd)
{
	unsigned int sync_q = sd->queued[SING_SYNC_CLASS];
	unsigned int bg_q = sd->queued[SING_BG_CLASS];
	unsigned int total_q = sync_q + bg_q;
	unsigned int configured = READ_ONCE(sing_fg_batch);
	unsigned int batch;

	if (!READ_ONCE(sing_adaptive_batch))
		return clamp_t(unsigned int, configured, 1, SING_DYN_FG_BATCH_MAX);

	batch = clamp_t(unsigned int, configured,
			SING_DYN_FG_BATCH_MIN, SING_DYN_FG_BATCH_MAX);

	/*
	 * Keep the scheduler latency-first, but avoid long sync-only runs on
	 * mobile workloads where radio/tethering services can issue both sync
	 * metadata and async log/cache writes.  Sustained mixed queues should
	 * retain a useful foreground run so the storage queue stays deep while
	 * still allowing the async side to make progress.
	 */
	if (bg_q && sync_q) {
		if (bg_q >= sync_q || total_q > 16)
			batch = max_t(unsigned int, batch / 2,
				      SING_DYN_FG_BATCH_MIN);
		else
			batch = max_t(unsigned int, batch - 4,
				      SING_DYN_FG_BATCH_MIN);
	} else if (total_q < 8) {
		batch = max_t(unsigned int, batch - 4, SING_DYN_FG_BATCH_MIN);
	}

	return batch;
}

static void sing_remove_request(struct sing_data *sd, struct request *rq)
{
	unsigned int class = sing_class(rq);
	unsigned int bucket = sing_bucket(rq);

	if (list_empty(&rq->queuelist))
		return;

	list_del_init(&rq->queuelist);
	if (sd->queued[class])
		sd->queued[class]--;
	if (list_empty(&sd->q[class][bucket]))
		__clear_bit(bucket, sd->active[class]);
}

/* Keep the class and cgroup of a queued request stable across front merges. */
static int sing_allow_bio_merge(struct request_queue *q, struct request *rq,
				struct bio *bio)
{
	return rq->bio && rq_is_sync(rq) == op_is_sync(bio->bi_opf) &&
		bio_blkcg(rq->bio) == bio_blkcg(bio);
}

static int sing_allow_rq_merge(struct request_queue *q, struct request *rq,
			       struct request *next)
{
	return rq->bio && next->bio && sing_class(rq) == sing_class(next) &&
		bio_blkcg(rq->bio) == bio_blkcg(next->bio);
}

static void sing_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	struct sing_data *sd = q->elevator->elevator_data;

	/* Keep the older FIFO position when two queued peers merge. */
	if (!list_empty(&rq->queuelist) && !list_empty(&next->queuelist) &&
	    sing_class(rq) == sing_class(next) &&
	    sing_bucket(rq) == sing_bucket(next) &&
	    time_before((unsigned long)next->fifo_time,
			(unsigned long)rq->fifo_time)) {
		list_move(&rq->queuelist, &next->queuelist);
		rq->fifo_time = next->fifo_time;
	}
	sing_remove_request(sd, next);
}

static int sing_dispatch_bucket(struct request_queue *q, struct sing_data *sd,
				unsigned int class, unsigned int bucket)
{
	struct request *rq;

	if (bucket == SING_MAX_BUCKETS)
		return 0;
	rq = list_first_entry(&sd->q[class][bucket], struct request, queuelist);
	sing_remove_request(sd, rq);
	sd->rr[class] = (bucket + 1) & (SING_MAX_BUCKETS - 1);
	/* Preserve the scheduler's chosen order on the core dispatch list. */
	elv_dispatch_add_tail(q, rq);
	return 1;
}

static int sing_dispatch_class(struct request_queue *q, struct sing_data *sd,
			       unsigned int class)
{
	unsigned int b;

	/* Age protection within the sync class, including bucket collisions. */
	if (class == SING_SYNC_CLASS && sd->queued[class]) {
		b = sing_expired_bucket(sd, class, READ_ONCE(sing_sync_starve_ms));
		if (b != SING_MAX_BUCKETS)
			return sing_dispatch_bucket(q, sd, class, b);
	}

	/*
	 * Walking every cgroup bucket for every dispatched request is expensive
	 * on high-IOPS UFS devices, especially when a benchmark uses one cgroup.
	 * Track non-empty buckets instead, while retaining round-robin fairness.
	 */
	b = find_next_bit(sd->active[class], SING_MAX_BUCKETS, sd->rr[class]);
	if (b == SING_MAX_BUCKETS)
		b = find_first_bit(sd->active[class], SING_MAX_BUCKETS);
	return sing_dispatch_bucket(q, sd, class, b);
}

static int sing_dispatch(struct request_queue *q, int force)
{
	struct sing_data *sd = q->elevator->elevator_data;
	unsigned int fg_batch = sing_dynamic_fg_batch(sd);
	unsigned int expired_batch = min_t(unsigned int, fg_batch,
					 SING_EXPIRED_FG_BATCH);

	/*
	 * Expired async I/O shortens the sync run, but must not turn sustained
	 * mixed traffic into one sync followed by one async request forever.
	 * With the default policy it gets one slot after eight sync requests.
	 */
	if (sd->queued[SING_BG_CLASS] &&
	    (!sd->queued[SING_SYNC_CLASS] || sd->fg_budget >= expired_batch) &&
	    sing_dispatch_bucket(q, sd, SING_BG_CLASS,
				 sing_expired_bucket(sd, SING_BG_CLASS,
						     READ_ONCE(sing_bg_starve_ms)))) {
		sd->fg_budget = 0;
		return 1;
	}

	if (sd->fg_budget < fg_batch && sing_dispatch_class(q, sd, SING_SYNC_CLASS)) {
		sd->fg_budget++;
		return 1;
	}

	if (sing_dispatch_class(q, sd, SING_BG_CLASS)) {
		sd->fg_budget = 0;
		return 1;
	}

	if (sing_dispatch_class(q, sd, SING_SYNC_CLASS)) {
		sd->fg_budget = min(sd->fg_budget + 1, fg_batch);
		return 1;
	}

	return 0;
}

static void sing_add_request(struct request_queue *q, struct request *rq)
{
	struct sing_data *sd = q->elevator->elevator_data;
	unsigned int class = sing_class(rq);
	unsigned int bucket = sing_bucket(rq);
	struct list_head *head = &sd->q[class][bucket];
	bool was_empty = list_empty(head);

	/* A fresh burst must not inherit an exhausted budget from idle. */
	if (!sd->queued[SING_SYNC_CLASS] && !sd->queued[SING_BG_CLASS])
		sd->fg_budget = 0;

	rq->fifo_time = jiffies;
	list_add_tail(&rq->queuelist, head);
	sd->queued[class]++;
	if (was_empty)
		__set_bit(bucket, sd->active[class]);
}

static struct request *sing_former_request(struct request_queue *q,
					   struct request *rq)
{
	struct sing_data *sd = q->elevator->elevator_data;
	struct list_head *head = sing_list(sd, rq);
	struct request *prev;

	if (list_empty(&rq->queuelist) || rq->queuelist.prev == head)
		return NULL;
	prev = list_prev_entry(rq, queuelist);
	/* Neighbor merges bypass elevator_allow_rq_merge_fn in this core. */
	return sing_allow_rq_merge(q, prev, rq) ? prev : NULL;
}

static struct request *sing_latter_request(struct request_queue *q,
					   struct request *rq)
{
	struct sing_data *sd = q->elevator->elevator_data;
	struct list_head *head = sing_list(sd, rq);
	struct request *next;

	if (list_empty(&rq->queuelist) || rq->queuelist.next == head)
		return NULL;
	next = list_next_entry(rq, queuelist);
	return sing_allow_rq_merge(q, rq, next) ? next : NULL;
}

static int sing_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct sing_data *sd;
	struct elevator_queue *eq;
	unsigned int c, b;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	sd = kzalloc_node(sizeof(*sd), GFP_KERNEL, q->node);
	if (!sd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}

	for (c = 0; c < SING_NR_CLASSES; c++)
		for (b = 0; b < SING_MAX_BUCKETS; b++)
			INIT_LIST_HEAD(&sd->q[c][b]);

	eq->elevator_data = sd;

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void sing_exit_queue(struct elevator_queue *e)
{
	struct sing_data *sd = e->elevator_data;
	unsigned int c, b;

	for (c = 0; c < SING_NR_CLASSES; c++)
		for (b = 0; b < SING_MAX_BUCKETS; b++)
			BUG_ON(!list_empty(&sd->q[c][b]));

	kfree(sd);
}

static struct elevator_type elevator_singularity = {
	.ops.sq = {
		.elevator_allow_bio_merge_fn    = sing_allow_bio_merge,
		.elevator_allow_rq_merge_fn     = sing_allow_rq_merge,
		.elevator_merge_req_fn          = sing_merged_requests,
		.elevator_dispatch_fn           = sing_dispatch,
		.elevator_add_req_fn            = sing_add_request,
		.elevator_former_req_fn         = sing_former_request,
		.elevator_latter_req_fn         = sing_latter_request,
		.elevator_init_fn               = sing_init_queue,
		.elevator_exit_fn               = sing_exit_queue,
	},
	.elevator_name = "singularity",
	.elevator_owner = THIS_MODULE,
};

static int __init sing_init(void)
{
	return elv_register(&elevator_singularity);
}

static void __exit sing_exit(void)
{
	elv_unregister(&elevator_singularity);
}

module_init(sing_init);
module_exit(sing_exit);

MODULE_AUTHOR("Singularity Performance Team");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Cgroup-aware latency-first I/O scheduler");
