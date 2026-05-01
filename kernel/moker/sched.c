#include "../sched/sched.h"
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

static void enqueue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{
	struct rb_root *root = &rq->edf_cbs.tasks_tree;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	struct rb_node *first;
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;

	raw_spin_lock(&rq->edf_cbs.lock);

	if (!RB_EMPTY_NODE(&sched_entity->node)) {
		raw_spin_unlock(&rq->edf_cbs.lock);
		return;
	}

	while (*link) {
		struct sched_edf_cbs_entity *entry;

		entry = rb_entry(*link, struct sched_edf_cbs_entity, node);
		parent = *link;

		if (sched_entity->absDL < entry->absDL)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&sched_entity->node, parent, link);
	rb_insert_color(&sched_entity->node, root);

	first = rb_first(root);
	if (first) {
		struct sched_edf_cbs_entity *first_entity;

		first_entity = rb_entry(first, struct sched_edf_cbs_entity, node);
		rq->edf_cbs.task = container_of(first_entity, struct task_struct, edf_cbs);
	} else {
		rq->edf_cbs.task = NULL;
	}

	add_nr_running(rq, 1);

	raw_spin_unlock(&rq->edf_cbs.lock);

#ifdef CONFIG_MOKER_TRACING
	moker_trace(ENQUEUE_RQ, p, -1);
#endif
}

static bool dequeue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{
	struct rb_node *first;

	raw_spin_lock(&rq->edf_cbs.lock);

	if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
		rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
		RB_CLEAR_NODE(&p->edf_cbs.node);
		sub_nr_running(rq, 1);
	}

	first = rb_first(&rq->edf_cbs.tasks_tree);
	if (first) {
		struct sched_edf_cbs_entity *first_entity;

		first_entity = rb_entry(first, struct sched_edf_cbs_entity, node);
		rq->edf_cbs.task = container_of(first_entity, struct task_struct, edf_cbs);
	} else {
		rq->edf_cbs.task = NULL;
	}

	raw_spin_unlock(&rq->edf_cbs.lock);

#ifdef CONFIG_MOKER_TRACING
	moker_trace(DEQUEUE_RQ, p, -1);
#endif

	return true;
}

static void yield_task_edf_cbs(struct rq *rq)
{
	resched_curr(rq);
}

static bool yield_to_task_edf_cbs(struct rq *rq, struct task_struct *p)
{
	return false;
}

static void task_tick_edf_cbs(struct rq *rq, struct task_struct *p, int queued)
{
}

static void wakeup_preempt_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{
	struct task_struct *curr = rq->curr;

	if (curr == p)
		return;

	if (curr->policy != SCHED_EDF_CBS) {
		resched_curr(rq);
		return;
	}

	if (curr->edf_cbs.absDL > p->edf_cbs.absDL)
		resched_curr(rq);
}

static int balance_edf_cbs(struct rq *rq, struct task_struct *prev, struct rq_flags *rf)
{
	return 0;
}

static struct task_struct *pick_task_edf_cbs(struct rq *rq, struct rq_flags *rf)
{
	struct task_struct *task;

	raw_spin_lock(&rq->edf_cbs.lock);
	task = rq->edf_cbs.task;
	raw_spin_unlock(&rq->edf_cbs.lock);

	return task;
}

static struct task_struct *pick_next_task_edf_cbs(struct rq *rq,
						  struct task_struct *prev,
						  struct rq_flags *rf)
{
	struct task_struct *task = NULL;
	struct rb_node *first;

	raw_spin_lock(&rq->edf_cbs.lock);

	first = rb_first(&rq->edf_cbs.tasks_tree);
	if (first) {
		struct sched_edf_cbs_entity *picked_entity;

		picked_entity = rb_entry(first, struct sched_edf_cbs_entity, node);
		task = container_of(picked_entity, struct task_struct, edf_cbs);
		rq->edf_cbs.task = task;
	} else {
		rq->edf_cbs.task = NULL;
	}

	raw_spin_unlock(&rq->edf_cbs.lock);

	return task;
}

static void put_prev_task_edf_cbs(struct rq *rq,
				  struct task_struct *p,
				  struct task_struct *next)
{
}

static void set_next_task_edf_cbs(struct rq *rq,
				  struct task_struct *p,
				  bool first)
{
}

static int select_task_rq_edf_cbs(struct task_struct *p, int cpu, int flags)
{
	return cpu;
}

static void switching_from_edf_cbs(struct rq *this_rq, struct task_struct *task)
{
}

static void switching_to_edf_cbs(struct rq *this_rq, struct task_struct *task)
{
}

static void switched_from_edf_cbs(struct rq *this_rq, struct task_struct *task)
{
}

static void switched_to_edf_cbs(struct rq *this_rq, struct task_struct *task)
{
}

static void reweight_task_edf_cbs(struct rq *this_rq,
				  struct task_struct *task,
				  const struct load_weight *lw)
{
}

static void prio_changed_edf_cbs(struct rq *rq, struct task_struct *p, u64 oldprio)
{
}

static void update_curr_edf_cbs(struct rq *rq)
{
}

DEFINE_SCHED_CLASS(edf_cbs) = {
	.queue_mask		= 16,

	.enqueue_task		= enqueue_task_edf_cbs,
	.dequeue_task		= dequeue_task_edf_cbs,

	.yield_task		= yield_task_edf_cbs,
	.yield_to_task		= yield_to_task_edf_cbs,

	.wakeup_preempt		= wakeup_preempt_edf_cbs,
	.balance		= balance_edf_cbs,

	.pick_task		= pick_task_edf_cbs,
	.pick_next_task		= pick_next_task_edf_cbs,

	.put_prev_task		= put_prev_task_edf_cbs,
	.set_next_task		= set_next_task_edf_cbs,

	.select_task_rq		= select_task_rq_edf_cbs,
	.set_cpus_allowed	= set_cpus_allowed_common,

	.task_tick		= task_tick_edf_cbs,

	.switching_from		= switching_from_edf_cbs,
	.switching_to		= switching_to_edf_cbs,
	.switched_from		= switched_from_edf_cbs,
	.switched_to		= switched_to_edf_cbs,

	.reweight_task		= reweight_task_edf_cbs,
	.prio_changed		= prio_changed_edf_cbs,

	.update_curr		= update_curr_edf_cbs,
};
