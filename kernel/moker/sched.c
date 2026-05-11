#include "../sched/sched.h"
#include "utils.h"

#define DEQUEUE_UPDATE_DEADLINE 0x10000000


// WARNING: WHOEVER CALLS THIS FUNCTION MUST HOLD rq->edf_cbs.lock
void print_edf_tree(struct rq *rq)
{
    struct rb_node *node;
    struct task_struct *task;

    for (node = rb_first(&rq->edf_cbs.tasks_tree); node; node = rb_next(node)) {
        task = container_of(node, struct task_struct, edf_cbs.node);
        printk(KERN_INFO "[EDF TREE] task id=%u pid=%d absDL=%llu state=%u\n",
               task->edf_cbs.id,
               task->pid,
               task->edf_cbs.absDL,
               task->__state);
    }

}

static void activate_soft_task_in_tree_locked(struct rq *rq,
					      struct task_struct *p,
					      struct cbs_server *server,
					      const char *action)
{
	p->edf_cbs.absDL = server->absDL;

	if (RB_EMPTY_NODE(&p->edf_cbs.node))
		insert_edf_tree(rq, p);

	update_edf_pick(rq);

	printk(KERN_INFO
	       "soft enqueue %s task=%u pid=%d server=%u absDL=%llu cap=%llu\n",
	       action,
	       p->edf_cbs.id,
	       p->pid,
	       p->edf_cbs.cbs_server_id,
	       p->edf_cbs.absDL,
	       server->currCapacity);
}

// WARNING: WHOEVER CALLS THIS FUNCTION MUST HOLD rq->edf_cbs.lock
void enqueue_hard_rt_task(struct rq *rq, struct task_struct *p)
{
	printk(KERN_INFO
		"hard enqueue active curr task=%u pid=%d absDL=%llu\n",
		p->edf_cbs.id,
		p->pid,
		p->edf_cbs.absDL);

	
	insert_edf_tree(rq, p);
	update_edf_pick(rq);

	/* If the current running task has a later deadline, request reschedule */
	if (rq->curr && rq->curr->policy == SCHED_EDF_CBS) {
		if (rq->curr->edf_cbs.absDL > p->edf_cbs.absDL) {
			printk(KERN_INFO "EDF preempt: curr_pid=%d curr_absDL=%llu new_pid=%d new_absDL=%llu\n",
				   rq->curr->pid,
				   rq->curr->edf_cbs.absDL,
				   p->pid,
				   p->edf_cbs.absDL);
			resched_curr(rq);
		}
	}
}

// WARNING: WHOEVER CALLS THIS FUNCTION MUST HOLD rq->edf_cbs.lock
void enqueue_soft_rt_task(struct rq *rq, struct task_struct *p)
{
	struct cbs_server *server;
	struct cbs_queue *member;
	bool was_empty;

	server = lookup_cbs_server(&rq->edf_cbs, p->edf_cbs.cbs_server_id);
	if (!server)
		return;

	p->edf_cbs.absDL = server->absDL;

	/*
	 * Already the active task for this server.
	 */
	if (server->curr == p) {
		activate_soft_task_in_tree_locked(rq, p, server, "active curr");

		return;
	}

	was_empty = list_empty(&server->queue_head) && server->curr == NULL;

	/*
	 * If this server is idle and has capacity, promote directly.
	 * This makes the task the server owner immediately, instead of
	 * putting it behind the FIFO queue.
	 */
	if (was_empty && server->currCapacity > 0) {
		server->curr = p;
		activate_soft_task_in_tree_locked(rq, p, server, "promoted");

		return;
	}

	/*
	 * Otherwise wait behind the current CBS task or wait for capacity.
	 */
	member = kmalloc(sizeof(*member), GFP_ATOMIC);
	if (!member) {
		printk(KERN_ERR "[CRITICAL] MOKER: failed to allocate memory for FIFO member for task %u\n", p->edf_cbs.id);
		return;
	}

	/*
	 * No immediate capacity or ownership, so the task stays queued until
	 * the active CBS owner blocks, yields, or runs out of budget.
	 */
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

// This wrapper acquires rq->edf_cbs.lock before dispatching to the per-task enqueue helpers.
static void enqueue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{	

	raw_spin_lock(&rq->edf_cbs.lock);
	
	printk(KERN_INFO "=== EDF TREE DUMP BEFORE ENQUEUE START ===\n");
	print_edf_tree(rq);
	printk(KERN_INFO "=== EDF TREE DUMP BEFORE ENQUEUE START ===\n");
	
	if(p->edf_cbs.isHardRT == true)	
		enqueue_hard_rt_task(rq, p);
	else
		enqueue_soft_rt_task(rq, p);
	
	printk(KERN_INFO "=== EDF TREE DUMP AFTER ENQUEUE END ===\n");
	print_edf_tree(rq);
	printk(KERN_INFO "=== EDF TREE DUMP AFTER ENQUEUE END ===\n");

	raw_spin_unlock(&rq->edf_cbs.lock);

#ifdef CONFIG_MOKER_TRACING
	moker_trace(ENQUEUE_RQ, p, -1);
#endif
}

// WARNING: WHOEVER CALLS THIS FUNCTION MUST HOLD rq->edf_cbs.lock
void dequeue_hard_rt_task_edf_cbs(struct rq *rq, struct task_struct *p)
{
	if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
		rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
		RB_CLEAR_NODE(&p->edf_cbs.node);
		sub_nr_running(rq, 1);
	}
}

static void dump_soft_queue_locked(struct cbs_server *server)
{
	struct cbs_queue *entry;

	list_for_each_entry(entry, &server->queue_head, node) {
		printk(KERN_INFO
		       "soft fifo member server=%u member=%u pid=%d state=%u node_empty=%d\n",
		       server->id,
		       entry->task ? entry->task->edf_cbs.id : 0,
		       entry->task ? entry->task->pid : -1,
		       entry->task ? entry->task->__state : 0,
		       entry->task ? (RB_EMPTY_NODE(&entry->task->edf_cbs.node) ? 1 : 0) : -1);
	}
}

static void release_soft_task_locked(struct cbs_server *server)
{
	ktime_t end;
	s64 elapsed;

	elapsed = 0;
	if (server->capacity_active) {
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
	}

	printk(KERN_INFO
	       "soft dequeue release curr task=%u pid=%d elapsed=%lld remainingCap=%llu\n",
	       server->curr ? server->curr->edf_cbs.id : 0,
	       server->curr ? server->curr->pid : -1,
	       elapsed,
	       server->currCapacity);

	server->curr = NULL;
}

static void promote_next_soft_task_locked(struct rq *rq, struct cbs_server *server)
{
	struct cbs_queue *next_entry;
	struct task_struct *next;

	if (list_empty(&server->queue_head))
		return;

	next_entry = list_first_entry(&server->queue_head,
				      struct cbs_queue,
				      node);
	next = next_entry->task;

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
}

// WARNING: WHOEVER CALLS THIS FUNCTION MUST HOLD rq->edf_cbs.lock
void dequeue_soft_rt_task_edf_cbs(struct rq *rq,
				     struct task_struct *p,
				     int flags,
				     bool deadline_update)
{
	struct cbs_server *server;
	bool was_curr = false;

	server = lookup_cbs_server(&rq->edf_cbs, p->edf_cbs.cbs_server_id);

	if (!server) {
		printk(KERN_INFO
		       "soft dequeue no server task=%u pid=%d server_id=%u flags=%x deadline_update=%d\n",
		       p->edf_cbs.id,
		       p->pid,
		       p->edf_cbs.cbs_server_id,
		       flags,
		       deadline_update ? 1 : 0);
		return;
	}

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

	/* Debugging to see what members are in the FIFO queue when this dequeuing is happening */
	dump_soft_queue_locked(server);

	/*
	 * Always remove p from the EDF tree if present.
	 * This keeps the runqueue accounting aligned with the task's actual
	 * runnable state before any CBS ownership transfer happens.
	 */
	if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
		printk(KERN_INFO
		       "soft dequeue erase tree task=%u pid=%d\n",
		       p->edf_cbs.id,
		       p->pid);

		rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
		sub_nr_running(rq, 1);
		RB_CLEAR_NODE(&p->edf_cbs.node);
	}

	if (deadline_update) {
		printk(KERN_INFO
		       "soft dequeue skip real-release task=%u due deadline_update flags=%x\n",
		       p->edf_cbs.id,
		       flags);
		return;
	}

	/*
	 * Real dequeue/block/exit:
	 * release CBS ownership and stop budget timer.
	 */
	if (server->curr == p) {
		/*
		 * Real dequeue/block/exit of the active soft task:
		 * stop budget timer and charge elapsed runtime.
		 */
		release_soft_task_locked(server);
		was_curr = true;
	} else {
		printk(KERN_INFO
		       "soft dequeue not curr task=%u pid=%d server_curr=%u\n",
		       p->edf_cbs.id,
		       p->pid,
		       server->curr ? server->curr->edf_cbs.id : 0);
	}

	/*
	 * Promote next waiting soft task only on real dequeue.
	 * Deadline updates do not release ownership, so they must not advance
	 * the FIFO queue.
	 */
	if ((was_curr || server->curr == NULL) &&
	    !list_empty(&server->queue_head)) {
		promote_next_soft_task_locked(rq, server);
	} else {
		printk(KERN_INFO
		       "soft dequeue no promote task=%u reason curr=%u fifo_empty=%d cap=%llu was_curr=%d\n",
		       p->edf_cbs.id,
		       server->curr ? server->curr->edf_cbs.id : 0,
		       list_empty(&server->queue_head) ? 1 : 0,
		       server->currCapacity,
		       was_curr ? 1 : 0);
	}
}

static bool dequeue_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags)
{
	bool deadline_update = flags & DEQUEUE_UPDATE_DEADLINE;

	raw_spin_lock(&rq->edf_cbs.lock);

	printk(KERN_INFO "=== EDF TREE DUMP BEFORE DEQUEUE START ===\n");
	print_edf_tree(rq);
	printk(KERN_INFO "=== EDF TREE DUMP BEFORE DEQUEUE START ===\n");

	if (!p->edf_cbs.isHardRT)
		dequeue_soft_rt_task_edf_cbs(rq, p, flags, deadline_update);
	else
		dequeue_hard_rt_task_edf_cbs(rq, p);

	update_edf_pick(rq);

	printk(KERN_INFO "=== EDF TREE DUMP AFTER DEQUEUE END ===\n");
	print_edf_tree(rq);
	printk(KERN_INFO "=== EDF TREE DUMP AFTER DEQUEUE END ===\n");

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
	if (!server) {
		printk(KERN_WARNING
		       "MOKER: soft task %u could not find server %u\n",
		       p->edf_cbs.id,
		       p->edf_cbs.cbs_server_id);
		return;
	}

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
    u64 curr_cap = 0;               // safe default

    raw_spin_lock(&rq->edf_cbs.lock);

    update_edf_pick(rq);  // optional
    task = rq->edf_cbs.task;

    if (task && !task->edf_cbs.isHardRT) {
        activate_picked_soft_task_locked(rq, task);

        // safely get currCapacity for soft tasks
        struct cbs_server *server = lookup_cbs_server(&rq->edf_cbs,
                                                       task->edf_cbs.cbs_server_id);
        if (server)
            curr_cap = server->currCapacity;
    }

    printk(KERN_INFO
           "pick_task EDF task=%d mid=%u soft=%d currCap=%llu absDL=%llu\n",
           task ? task->pid : -1,
           task ? task->edf_cbs.id : 0,
           task ? !task->edf_cbs.isHardRT : 0,
           curr_cap,
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

	/* Hold lock while checking and updating deadlineUpdate flag */
	raw_spin_lock(&rq->edf_cbs.lock);
	
	if (prev &&
	    prev->policy == SCHED_EDF_CBS &&
	    prev->edf_cbs.deadlineUpdate)
		prev->edf_cbs.deadlineUpdate = false;

	update_edf_pick(rq);
	task = rq->edf_cbs.task;

	if (task && !task->edf_cbs.isHardRT) {
		soft = true;
		server = lookup_cbs_server(&rq->edf_cbs,
					   task->edf_cbs.cbs_server_id);

		if (server)
			curr_cap = server->currCapacity;
	}

	if (task && !task->edf_cbs.isHardRT)
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