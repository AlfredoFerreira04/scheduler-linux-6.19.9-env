#include "../sched/sched.h"
#include "utils.h"

#define DEQUEUE_UPDATE_DEADLINE 0x10000000

static void enqueue_hard_rt_task(struct rq *rq, struct task_struct *p)
{
	insert_edf_tree(rq, p);
	update_edf_pick(rq);
}



static void enqueue_soft_rt_task(struct rq *rq, struct task_struct *p)
{
	struct cbs_server *server;
	struct cbs_queue *member;
	struct cbs_queue *iter;
	bool was_empty;
	u64 now_ns;

	server = lookup_cbs_server(&rq->edf_cbs, p->edf_cbs.cbs_server_id);
	if (!server)
		return;

	/*
	 * First activation of this CBS server.
	 * This must happen before promotion, otherwise the first soft task
	 * sees currCapacity == 0 and never becomes server->curr.
	 */
	if (server->absDL == 0) {
		now_ns = ktime_get_ns();

		server->absDL = now_ns + server->relDL;
		server->currCapacity = server->maximumCapacity;
		server->capacity_active = false;

		p->edf_cbs.absDL = server->absDL;

		hrtimer_start(&server->deadlineTimer,
			      ns_to_ktime(server->absDL),
			      HRTIMER_MODE_ABS);

		printk(KERN_INFO
		       "[%llu ms] CBS deadline init: server=%p task=%d mid=%u absDL=%llu relDL=%llu cap=%llu\n",
		       now_ns / 1000000ULL,
		       server,
		       p->pid,
		       p->edf_cbs.id,
		       server->absDL,
		       server->relDL,
		       server->currCapacity);
	}

	/*
	 * Already the active task for this server.
	 */
	if (server->curr == p) {
		p->edf_cbs.absDL = server->absDL;

		if (RB_EMPTY_NODE(&p->edf_cbs.node))
			insert_edf_tree(rq, p);

		update_edf_pick(rq);

		printk(KERN_INFO
		       "soft enqueue active curr task=%u pid=%d server=%u absDL=%llu cap=%llu\n",
		       p->edf_cbs.id,
		       p->pid,
		       p->edf_cbs.cbs_server_id,
		       p->edf_cbs.absDL,
		       server->currCapacity);

		return;
	}

	/*
	 * Avoid duplicate FIFO entries.
	 */
	list_for_each_entry(iter, &server->queue_head, node) {
		if (iter->task == p)
			return;
	}

	was_empty = list_empty(&server->queue_head) && server->curr == NULL;

	/*
	 * If this server is idle and has capacity, promote directly.
	 * Do not put the task in the FIFO first.
	 */
	if (was_empty && server->currCapacity > 0) {
		server->curr = p;
		p->edf_cbs.absDL = server->absDL;

		if (RB_EMPTY_NODE(&p->edf_cbs.node))
			insert_edf_tree(rq, p);

		update_edf_pick(rq);

		printk(KERN_INFO
		       "soft enqueue promoted task=%u pid=%d server=%u absDL=%llu cap=%llu\n",
		       p->edf_cbs.id,
		       p->pid,
		       p->edf_cbs.cbs_server_id,
		       p->edf_cbs.absDL,
		       server->currCapacity);

		return;
	}

	/*
	 * Otherwise wait behind the current CBS task or wait for capacity.
	 */
	member = kmalloc(sizeof(*member), GFP_ATOMIC);
	if (!member)
		return;

	member->task = p;
	INIT_LIST_HEAD(&member->node);
	list_add_tail(&member->node, &server->queue_head);

	printk(KERN_INFO
		"soft enqueue fifo task=%u pid=%d server=%u curr=%u absDL=%llu cap=%llu node_empty=%d\n",
		p->edf_cbs.id,
		p->pid,
		p->edf_cbs.cbs_server_id,
		server->curr ? server->curr->edf_cbs.id : 0,
		server->absDL,
		server->currCapacity,
		RB_EMPTY_NODE(&p->edf_cbs.node) ? 1 : 0);

	update_edf_pick(rq);
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
	bool deadline_update = flags & DEQUEUE_UPDATE_DEADLINE;

	raw_spin_lock(&rq->edf_cbs.lock);

	if (!p->edf_cbs.isHardRT) {
		struct cbs_server *server;
		struct cbs_queue *entry, *tmp;
		bool was_curr = false;

		server = lookup_cbs_server(&rq->edf_cbs,
					   p->edf_cbs.cbs_server_id);

		if (server) {
			printk(KERN_INFO
			       "soft dequeue enter task=%u pid=%d flags=%x deadline_update=%d server=%u curr=%u node_empty=%d fifo_empty=%d cap=%llu state=%u\n",
			       p->edf_cbs.id,
			       p->pid,
			       flags,
			       deadline_update ? 1 : 0,
			       p->edf_cbs.cbs_server_id,
			       server->curr ? server->curr->edf_cbs.id : 0,
			       RB_EMPTY_NODE(&p->edf_cbs.node) ? 1 : 0,
			       list_empty(&server->queue_head) ? 1 : 0,
			       server->currCapacity,
			       p->__state);

			list_for_each_entry(entry, &server->queue_head, node) {
				printk(KERN_INFO
				       "soft fifo member server=%u member=%u pid=%d state=%u node_empty=%d\n",
				       server->id,
				       entry->task ? entry->task->edf_cbs.id : 0,
				       entry->task ? entry->task->pid : -1,
				       entry->task ? entry->task->__state : 0,
				       entry->task ? (RB_EMPTY_NODE(&entry->task->edf_cbs.node) ? 1 : 0) : -1);
			}

			/*
			 * Always remove p from the EDF tree if present.
			 */
			if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
				printk(KERN_INFO
				       "soft dequeue erase tree task=%u pid=%d\n",
				       p->edf_cbs.id,
				       p->pid);

				rb_erase(&p->edf_cbs.node,
					 &rq->edf_cbs.tasks_tree);
				RB_CLEAR_NODE(&p->edf_cbs.node);
				sub_nr_running(rq, 1);
			}

			if (!deadline_update) {
				/*
				 * Real dequeue/block/exit:
				 * release CBS ownership and stop budget timer.
				 */
				if (server->curr == p) {
					ktime_t end;
					s64 elapsed;

					/*
					 * Real dequeue/block/exit of the active soft task:
					 * stop budget timer and charge elapsed runtime.
					 */
					hrtimer_try_to_cancel(&server->capacityTimer);
					server->capacity_active = false;

					end = ktime_get();
					elapsed = ktime_to_ns(ktime_sub(end, server->capacityTimerStart));

					if (elapsed > 0) {
						if ((u64)elapsed >= server->currCapacity)
							server->currCapacity = 0;
						else
							server->currCapacity -= (u64)elapsed;
					}

					printk(KERN_INFO
					       "soft dequeue release curr task=%u pid=%d elapsed=%lld remainingCap=%llu\n",
					       p->edf_cbs.id,
					       p->pid,
					       elapsed,
					       server->currCapacity);

					server->curr = NULL;
					was_curr = true;
				} else {
					printk(KERN_INFO
					       "soft dequeue not curr task=%u pid=%d server_curr=%u\n",
					       p->edf_cbs.id,
					       p->pid,
					       server->curr ? server->curr->edf_cbs.id : 0);
				}

				/*
				 * Remove p from server FIFO if it was waiting.
				 */
				list_for_each_entry_safe(entry, tmp,
							 &server->queue_head, node) {
					if (entry->task == p) {
						printk(KERN_INFO
						       "soft dequeue remove waiting fifo task=%u pid=%d\n",
						       p->edf_cbs.id,
						       p->pid);

						list_del(&entry->node);
						kfree(entry);
						break;
					}
				}

				printk(KERN_INFO
				       "soft dequeue promote check task=%u was_curr=%d curr=%u fifo_empty=%d cap=%llu\n",
				       p->edf_cbs.id,
				       was_curr ? 1 : 0,
				       server->curr ? server->curr->edf_cbs.id : 0,
				       list_empty(&server->queue_head) ? 1 : 0,
				       server->currCapacity);

				/*
				 * Promote next waiting soft task only on real dequeue.
				 */
				if ((was_curr || server->curr == NULL) &&
				    !list_empty(&server->queue_head)) {
					struct cbs_queue *next_entry;
					struct task_struct *next;

					next_entry = list_first_entry(&server->queue_head,
								      struct cbs_queue,
								      node);
					next = next_entry->task;

					printk(KERN_INFO
					       "soft dequeue promote selected from=%u to=%u pid=%d state=%u node_empty=%d absDL_old=%llu server_absDL=%llu cap=%llu\n",
					       p->edf_cbs.id,
					       next ? next->edf_cbs.id : 0,
					       next ? next->pid : -1,
					       next ? next->__state : 0,
					       next ? (RB_EMPTY_NODE(&next->edf_cbs.node) ? 1 : 0) : -1,
					       next ? next->edf_cbs.absDL : 0,
					       server->absDL,
					       server->currCapacity);

					list_del(&next_entry->node);
					kfree(next_entry);

					server->curr = next;
					next->edf_cbs.absDL = server->absDL;

					insert_edf_tree(rq, next);

					printk(KERN_INFO
					       "soft dequeue promoted to=%u pid=%d node_empty_after=%d curr=%u\n",
					       next->edf_cbs.id,
					       next->pid,
					       RB_EMPTY_NODE(&next->edf_cbs.node) ? 1 : 0,
					       server->curr ? server->curr->edf_cbs.id : 0);
				} else {
					printk(KERN_INFO
					       "soft dequeue no promote task=%u reason curr=%u fifo_empty=%d cap=%llu was_curr=%d\n",
					       p->edf_cbs.id,
					       server->curr ? server->curr->edf_cbs.id : 0,
					       list_empty(&server->queue_head) ? 1 : 0,
					       server->currCapacity,
					       was_curr ? 1 : 0);
				}
			} else {
				printk(KERN_INFO
				       "soft dequeue skip real-release task=%u due deadline_update flags=%x\n",
				       p->edf_cbs.id,
				       flags);
			}
		} else {
			printk(KERN_INFO
			       "soft dequeue no server task=%u pid=%d server_id=%u flags=%x deadline_update=%d\n",
			       p->edf_cbs.id,
			       p->pid,
			       p->edf_cbs.cbs_server_id,
			       flags,
			       deadline_update ? 1 : 0);
		}
	} else {
		if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
			rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
			RB_CLEAR_NODE(&p->edf_cbs.node);
			sub_nr_running(rq, 1);
		}
	}

	update_edf_pick(rq);

	printk(KERN_INFO
	       "dequeue exit task=%u pid=%d pick=%u pick_pid=%d\n",
	       p->edf_cbs.id,
	       p->pid,
	       rq->edf_cbs.task ? rq->edf_cbs.task->edf_cbs.id : 0,
	       rq->edf_cbs.task ? rq->edf_cbs.task->pid : -1);

	raw_spin_unlock(&rq->edf_cbs.lock);

#ifdef CONFIG_MOKER_TRACING
	moker_trace(DEQUEUE_RQ, p,
		    deadline_update ? DEQUEUE_UPDATE_DEADLINE : -1);
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

static void activate_picked_soft_task_locked(struct rq *rq, struct task_struct *p)
{
	struct cbs_server *server;
	u64 now_ns;
	ktime_t expires;

	if (!p || p->edf_cbs.isHardRT)
		return;

	server = lookup_cbs_server(&rq->edf_cbs, p->edf_cbs.cbs_server_id);
	if (!server)
		return;

	/*
	 * Only the current CBS owner may consume/arm CBS budget.
	 */
	if (server->curr != p)
		return;

	printk(KERN_INFO
	       "pick_task activate soft task=%u pid=%d server=%p curr=%u cap=%llu armed=%d start=%llu absDL=%llu\n",
	       p->edf_cbs.id,
	       p->pid,
	       server,
	       server->curr ? server->curr->edf_cbs.id : 0,
	       server->currCapacity,
	       server->capacity_active ? 1 : 0,
	       server->capacityTimerStart,
	       server->absDL);

	/*
	 * pick_task() may be called repeatedly for the same task.
	 * Arm only once per actual running slice.
	 */
	if (server->capacity_active)
		return;

	now_ns = ktime_get_ns();
	server->capacityTimerStart = now_ns;

	expires = ns_to_ktime(now_ns + server->currCapacity);

	hrtimer_start(&server->capacityTimer,
		      expires,
		      HRTIMER_MODE_ABS);

	server->capacity_active = true;

	printk(KERN_INFO
	       "[%llu ms] CBS capacity arm: server=%p task=%d mid=%u cap=%llu expires=%llu absDL=%llu\n",
	       now_ns / 1000000ULL,
	       server,
	       p->pid,
	       p->edf_cbs.id,
	       server->currCapacity,
	       now_ns + server->currCapacity,
	       server->absDL);
}

static struct task_struct *pick_task_edf_cbs(struct rq *rq, struct rq_flags *rf)
{
	struct task_struct *task;

	raw_spin_lock(&rq->edf_cbs.lock);

	update_edf_pick(rq);
	task = rq->edf_cbs.task;

	if (task)
		activate_picked_soft_task_locked(rq, task);

	printk(KERN_INFO
	       "pick_task EDF task=%d mid=%u soft=%d currCap=%llu absDL=%llu\n",
	       task ? task->pid : -1,
	       task ? task->edf_cbs.id : 0,
	       task ? !task->edf_cbs.isHardRT : 0,
	       (!task || task->edf_cbs.isHardRT) ? 0 :
		       lookup_cbs_server(&rq->edf_cbs,
					 task->edf_cbs.cbs_server_id)->currCapacity,
	       task ? task->edf_cbs.absDL : 0);

	raw_spin_unlock(&rq->edf_cbs.lock);

	return task;
}

static struct task_struct *pick_next_task_edf_cbs(struct rq *rq,
						  struct task_struct *prev,
						  struct rq_flags *rf)
{
	struct task_struct *task;
	struct cbs_server *server = NULL;
	u64 curr_cap = 0;
	bool soft = false;

	if (prev &&
	    prev->policy == SCHED_EDF_CBS &&
	    prev->edf_cbs.deadlineUpdate) {
		bool still_running;

		prev->edf_cbs.deadlineUpdate = false;
		still_running = READ_ONCE(prev->__state) == TASK_RUNNING;

		dequeue_task_edf_cbs(rq, prev, DEQUEUE_UPDATE_DEADLINE);

		if (still_running)
			enqueue_task_edf_cbs(rq, prev, 0);
	}

	raw_spin_lock(&rq->edf_cbs.lock);

	update_edf_pick(rq);
	task = rq->edf_cbs.task;

	if (task && !task->edf_cbs.isHardRT) {
		soft = true;
		server = lookup_cbs_server(&rq->edf_cbs,
					   task->edf_cbs.cbs_server_id);

		if (server)
			curr_cap = server->currCapacity;
	}

	if (task)
		activate_picked_soft_task_locked(rq, task);

	printk(KERN_INFO
	       "pick_next EDF task=%d mid=%u soft=%d currCap=%llu absDL=%llu\n",
	       task ? task->pid : -1,
	       task ? task->edf_cbs.id : 0,
	       soft ? 1 : 0,
	       curr_cap,
	       task ? task->edf_cbs.absDL : 0);

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