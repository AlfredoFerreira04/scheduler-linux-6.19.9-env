#ifndef __EDF_CBS_RQ_H_
#define __EDF_CBS_RQ_H_

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>

struct edf_cbs_rq {
	struct rb_root tasks_tree;	// edf priority sorted tree
	struct task_struct *task;	// cached highest priority task
	raw_spinlock_t lock;		// rq spinlock
};

void init_edf_cbs_rq(struct edf_cbs_rq *rq);

#endif
