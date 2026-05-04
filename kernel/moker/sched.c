#include "../sched/sched.h"
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/types.h>

static void __insert_edf_tree(struct rq *rq, struct task_struct *p)
{
	struct rb_root *root = &rq->edf_cbs.tasks_tree;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;
	struct sched_edf_cbs_entity *se = &p->edf_cbs;

	if (!RB_EMPTY_NODE(&se->node))
		return;

	while (*link) {
		struct sched_edf_cbs_entity *entry;

		entry = rb_entry(*link, struct sched_edf_cbs_entity, node);
		parent = *link;

		if (se->absDL < entry->absDL)
			link = &(*link)->rb_left;
		else
			link = &(*link)->rb_right;
	}

	rb_link_node(&se->node, parent, link);
	rb_insert_color(&se->node, root);

	add_nr_running(rq, 1);
}

static void __update_edf_pick(struct rq *rq)
{
	struct rb_node *first = rb_first(&rq->edf_cbs.tasks_tree);

	if (first) {
		struct sched_edf_cbs_entity *first_entity;

		first_entity = rb_entry(first, struct sched_edf_cbs_entity, node);
		rq->edf_cbs.task = container_of(first_entity,
						struct task_struct,
						edf_cbs);
	} else {
		rq->edf_cbs.task = NULL;
	}
}

static void enqueue_hard_rt_task(struct rq *rq, struct task_struct *p)
{
	__insert_edf_tree(rq, p);
	__update_edf_pick(rq);
}

static void enqueue_soft_rt_task(struct rq *rq, struct task_struct *p)
{
	struct cbs_server *server;
	struct cbs_queue *member;
	struct cbs_queue *iter;
	bool was_empty;

	server = lookup_cbs_server(&rq->edf_cbs, p->edf_cbs.cbs_server_id);
	if (!server)
		return;

	/*
	 * Already the active task for this server.
	 * Nothing to enqueue.
	 */
	if (server->curr == p)
		return;

	/*
	 * Avoid duplicate FIFO entries for the same task.
	 */
	list_for_each_entry(iter, &server->queue_head, node) {
		if (iter->task == p)
			return;
	}

	was_empty = list_empty(&server->queue_head) && server->curr == NULL;

	member = kmalloc(sizeof(*member), GFP_ATOMIC);
	if (!member)
		return;

	member->task = p;
	INIT_LIST_HEAD(&member->node);

	list_add_tail(&member->node, &server->queue_head);

	if (was_empty) {
		struct cbs_queue *first;
		struct task_struct *next;

		first = list_first_entry(&server->queue_head,
					 struct cbs_queue,
					 node);
		next = first->task;

		list_del(&first->node);
		kfree(first);

		server->curr = next;
		next->edf_cbs.absDL = server->absDL;

		__insert_edf_tree(rq, next);
		__update_edf_pick(rq);
	}
}

static void enqueue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{	
	raw_spin_lock(&rq->edf_cbs.lock);

	if(p->edf_cbs.isHardRT == true)	
		enqueue_hard_rt_task(rq, p);
	else
		enqueue_soft_rt_task(rq, p);
	
	raw_spin_unlock(&rq->edf_cbs.lock);

#ifdef CONFIG_MOKER_TRACING
	moker_trace(ENQUEUE_RQ, p, -1);
#endif
}

static bool dequeue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{
	raw_spin_lock(&rq->edf_cbs.lock);

	if (!p->edf_cbs.isHardRT) {
		struct cbs_server *server;
		struct cbs_queue *entry, *tmp;
		bool was_curr = false;

		server = lookup_cbs_server(&rq->edf_cbs,
					   p->edf_cbs.cbs_server_id);

		if (server) {
			/*
			 * If p is currently active for this server, clear curr.
			 */
			if (server->curr == p) {
				server->curr = NULL;
				was_curr = true;
			}

			/*
			 * Remove p from EDF tree if it is there.
			 * This must happen even if server->curr != p,
			 * because soft sleep paths may clear curr before
			 * scheduler dequeue happens.
			 */
			if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
				rb_erase(&p->edf_cbs.node,
					 &rq->edf_cbs.tasks_tree);
				RB_CLEAR_NODE(&p->edf_cbs.node);
				sub_nr_running(rq, 1);
			}

			/*
			 * Remove p from server FIFO if it is waiting there.
			 */
			list_for_each_entry_safe(entry, tmp,
						 &server->queue_head, node) {
				if (entry->task == p) {
					list_del(&entry->node);
					kfree(entry);
					break;
				}
			}

			/*
			 * Promote next FIFO task if this dequeue freed the server.
			 */
			if ((was_curr || server->curr == NULL) &&
			    !list_empty(&server->queue_head)) {
				struct cbs_queue *next_entry;
				struct task_struct *next;

				next_entry = list_first_entry(&server->queue_head,
							      struct cbs_queue,
							      node);
				next = next_entry->task;

				list_del(&next_entry->node);
				kfree(next_entry);

				server->curr = next;
				next->edf_cbs.absDL = server->absDL;

				__insert_edf_tree(rq, next);
			}
		}
	} else {
		if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
			rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
			RB_CLEAR_NODE(&p->edf_cbs.node);
			sub_nr_running(rq, 1);
		}
	}

	__update_edf_pick(rq);

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

	if (task && task->edf_cbs.isHardRT == false) {
		struct cbs_server *server;

		server = lookup_cbs_server(&rq->edf_cbs,
					   task->edf_cbs.cbs_server_id);

		if (server &&
		    server->curr == task &&
		    server->currCapacity > 0 &&
		    !hrtimer_active(&server->capacityTimer)) {
			ktime_t now;
			ktime_t expires;

			now = ktime_get();
			server->capacityTimerStart = now;

			expires = ktime_add_ns(now, server->currCapacity);

			hrtimer_start(&server->capacityTimer,
				      expires,
				      HRTIMER_MODE_ABS);
		}
	}

	raw_spin_unlock(&rq->edf_cbs.lock);

	return task;
}

static struct task_struct *pick_next_task_edf_cbs(struct rq *rq,
						  struct task_struct *prev,
						  struct rq_flags *rf)
{
	struct task_struct *task;

	if (prev &&
	    prev->policy == SCHED_EDF_CBS &&
	    prev->edf_cbs.deadlineUpdate) {
		bool still_running;

		prev->edf_cbs.deadlineUpdate = false;
		still_running = READ_ONCE(prev->__state) == TASK_RUNNING;

		dequeue_task_edf_cbs(rq, prev, 0);

		if (still_running)
			enqueue_task_edf_cbs(rq, prev, 0);
	}

	raw_spin_lock(&rq->edf_cbs.lock);

	__update_edf_pick(rq);
	task = rq->edf_cbs.task;

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