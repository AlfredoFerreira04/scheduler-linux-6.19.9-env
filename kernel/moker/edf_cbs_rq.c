#include "../sched/sched.h"
#include "linux/container_of.h"
#include "linux/ktime.h"
#include <linux/bug.h>
#include "edf_cbs_rq.h"
#include "utils.h"

void init_edf_cbs_rq(struct edf_cbs_rq *rq)
{
    raw_spin_lock_init(&rq->lock);
    rq->tasks_tree = RB_ROOT;
    rq->task = NULL;
	rq->servers = RB_ROOT;
    rq->server_count = 0;
}

static void insert_server_tree_locked(struct edf_cbs_rq *rq,
				      struct cbs_server *server)
{
	struct rb_node **link = &rq->servers.rb_node;
	struct rb_node *parent = NULL;
	struct cbs_server *entry;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct cbs_server, node);

		if (server->absDL < entry->absDL)
			link = &parent->rb_left;
		else if (server->absDL > entry->absDL)
			link = &parent->rb_right;
		else if (server->id < entry->id)
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	rb_link_node(&server->node, parent, link);
	rb_insert_color(&server->node, &rq->servers);
}

static void reinsert_server_tree_locked(struct edf_cbs_rq *rq,
					 struct cbs_server *server)
{
	if (RB_EMPTY_NODE(&server->node))
		return;

	rb_erase(&server->node, &rq->servers);
	RB_CLEAR_NODE(&server->node);
	insert_server_tree_locked(rq, server);
}

void reinsert_cbs_server_tree_locked(struct edf_cbs_rq *rq,
					struct cbs_server *server)
{
	reinsert_server_tree_locked(rq, server);
}

struct cbs_server *lookup_assigned_cbs_server(struct edf_cbs_rq *rq, int id)
{
	struct cbs_server *server;

	if (!rq)
		return NULL;

	for (server = rb_entry_safe(rb_first(&rq->servers), struct cbs_server, node);
	     server;
	     server = rb_entry_safe(rb_next(&server->node), struct cbs_server, node)) {
		if (server->id == id)
			return server;
	}

	return NULL;
}

static enum hrtimer_restart cbs_capacity_timer_fn(struct hrtimer *timer)
{
	struct cbs_server *server;
	struct task_struct *p;
	struct rq_flags rf;
	struct rq *rq;
	u64 now;

	server = container_of(timer, struct cbs_server, capacityTimer);
	now = ktime_get_ns();

	/*
	 * Lock the rq that owns the current CBS task.
	 *
	 * If there is no current task, we can only safely touch server state
	 * if we find another stable way to find the owning rq. In the normal
	 * capacity-timer case, server->curr should exist.
	 */
	p = READ_ONCE(server->curr);
	if (!p) {
		server->capacity_active = false;
		return HRTIMER_NORESTART;
	}

	rq = task_rq_lock(p, &rf);
	raw_spin_lock(&rq->edf_cbs.lock);

	/*
	 * Re-read under the rq/CBS lock. The task may have blocked/exited
	 * while the timer callback was waiting for the lock.
	 */
	if (server->curr != p) {
		raw_spin_unlock(&rq->edf_cbs.lock);
		task_rq_unlock(rq, p, &rf);
		return HRTIMER_NORESTART;
	}

	/*
	 * Capacity exhausted:
	 * - replenish capacity
	 * - push server deadline
	 * - update current soft task deadline
	 * - reschedule deadline timer to new deadline
	 *
	 * If the task is currently in the EDF tree, remove-and-reinsert
	 * under the rq/edf_cbs lock to update its sort key atomically and
	 * keep runqueue accounting consistent.
	 */
	server->currCapacity = server->maximumCapacity;
	server->absDL += server->relDL;
	server->capacity_active = false;
	/*
	 * Reschedule the deadline timer to the new absolute deadline.
	 * Use hrtimer_start in ABS mode with the absolute ktime to avoid
	 * treating `server->absDL` as a relative interval (which
	 * hrtimer_forward_now expects).
	 */
	
	hrtimer_start(&server->deadlineTimer,
			 ns_to_ktime(server->absDL),
			 HRTIMER_MODE_ABS);
	reinsert_server_tree_locked(&rq->edf_cbs, server);

	if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
		//WARN_ON_ONCE(p->edf_cbs.absDL != server->absDL);
		rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
		RB_CLEAR_NODE(&p->edf_cbs.node);
		sub_nr_running(rq, 1);
	}

	p->edf_cbs.absDL = server->absDL;
	p->edf_cbs.deadlineUpdate = true;

	/* re-insert with updated deadline */
	insert_edf_tree(rq, p);
	update_edf_pick(rq);

	printk(KERN_WARNING
		"MOKER_CBS_CAPACITY_EXHAUSTED now_ms=%llu "
		"server=%p curr_pid=%d task_id=%u cap=%llu absDL=%llu state=%u on_rq=%d\n",
		now / 1000000ULL,
		server,
		p->pid,
		p->edf_cbs.id,
		server->currCapacity,
		server->absDL,
		READ_ONCE(p->__state),
		READ_ONCE(p->on_rq));

	raw_spin_unlock(&rq->edf_cbs.lock);

	resched_curr(rq);

	task_rq_unlock(rq, p, &rf);

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart cbs_deadline_timer_fn(struct hrtimer *timer)
{
	struct cbs_server *server;
	struct task_struct *p;
	struct rq *rq;
	struct rq_flags rf;

	server = container_of(timer, struct cbs_server, deadlineTimer);
	rq = server->rq;

	/* If there is a current task, take its rq lock so we can safely
	 * update runqueue accounting when changing the deadline key.
	 */
	p = server->curr;

	server->currCapacity = server->maximumCapacity;
	server->absDL += server->relDL;
	reinsert_server_tree_locked(&rq->edf_cbs, server);

	if (!p && list_empty(&server->queue_head)) {
		return HRTIMER_NORESTART;
	}

	rq = task_rq_lock(p, &rf);
	raw_spin_lock(&rq->edf_cbs.lock);

	/* Re-check current under locks */
	if (server->curr == p) {
		if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
			//WARN_ON_ONCE(p->edf_cbs.absDL != server->absDL);
			rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
			RB_CLEAR_NODE(&p->edf_cbs.node);
			sub_nr_running(rq, 1);
		}

		p->edf_cbs.absDL = server->absDL;
		p->edf_cbs.deadlineUpdate = true;

		/* re-insert with the updated deadline */
		insert_edf_tree(rq, p);
		update_edf_pick(rq);

		set_tsk_need_resched(p);
	}

	printk(KERN_INFO
		   "[%llu ms] CBS deadline recharge: server=%u cap=%llu absDL=%llu curr=%d mid=%u\n",
		   ktime_get_ns() / 1000000ULL,
		   server->id,
		   server->currCapacity,
		   server->absDL,
		   p ? p->pid : -1,
		   p ? p->edf_cbs.id : 0);

	raw_spin_unlock(&rq->edf_cbs.lock);
	task_rq_unlock(rq, p, &rf);

	hrtimer_forward_now(&server->deadlineTimer, ns_to_ktime(server->relDL));
	return HRTIMER_RESTART;
}

struct cbs_server *create_cbs_server(struct edf_cbs_rq *rq,
					 u64 start_instant,
				     u64 relDL,
				     u64 capacity,
				     u32 utilization)
{
	struct cbs_server *server;
	u32 new_id = 0;

	if (!rq)
		return NULL;

	while (lookup_assigned_cbs_server(rq, new_id))
		new_id++;

	server = kmalloc(sizeof(*server), GFP_ATOMIC);
	if (!server)
		return NULL;

	server->id = new_id;
	INIT_LIST_HEAD(&server->queue_head);
	server->curr = NULL;
	RB_CLEAR_NODE(&server->node);
	server->relDL = relDL;
	server->absDL = start_instant + relDL;
	server->utilization = utilization;
	server->maximumCapacity = capacity;
	server->currCapacity = capacity;
	server->rq = container_of(rq, struct rq, edf_cbs);

    hrtimer_setup(&server->capacityTimer,
            cbs_capacity_timer_fn,
            CLOCK_MONOTONIC,
            HRTIMER_MODE_ABS);

    hrtimer_setup(&server->deadlineTimer,
            cbs_deadline_timer_fn,
            CLOCK_MONOTONIC,
            HRTIMER_MODE_ABS);

	server->capacity_active = false;
	server->capacityTimerStart = 0;

	hrtimer_start(&server->deadlineTimer,
		      ns_to_ktime(server->absDL),
		      HRTIMER_MODE_ABS);

	printk(KERN_INFO
		"[%llu ms] CBS deadline create: server=%p id=%u absDL=%llu relDL=%llu cap=%llu util=%u%%\n",
		start_instant / 1000000ULL,
		server,
		server->id,
		server->absDL,
		server->relDL,
		server->currCapacity,
		server->utilization);

	insert_server_tree_locked(rq, server);
	rq->server_count++;

	printk(KERN_INFO "MOKER [%d] | create CBS server ID -> [%u] relDL -> [%llu], capacity -> [%llu], utilization -> [%u]\n",
		current->pid,
		(u32)server->id,
		(u64)relDL,
		(u64)capacity,
		server->utilization);


	return server;
}

static struct cbs_server *next_transfer_server(struct edf_cbs_rq *edf_rq,
					       struct cbs_server *victim,
					       struct cbs_server *last)
{
	struct cbs_server *server;

	if (edf_rq->server_count <= 1)
		return NULL;

	{
		struct rb_node *n;

		if (last)
			n = rb_next(&last->node);
		else
			n = rb_first(&edf_rq->servers);

		if (!n)
			n = rb_first(&edf_rq->servers);

		server = rb_entry(n, struct cbs_server, node);

		if (server == victim) {
			n = rb_next(n);
			if (!n)
				n = rb_first(&edf_rq->servers);

			server = rb_entry(n, struct cbs_server, node);
		}

		if (server == victim)
			return NULL;

		return server;
	}
}


void destroy_cbs_server(struct rq *rq, int id, bool transfer_flag)
{
	struct edf_cbs_rq *edf_rq = &rq->edf_cbs;
	struct cbs_server *server;
	struct cbs_queue *entry, *tmp;
	struct cbs_server *dst = NULL;

	if (!edf_rq)
		return;

	server = lookup_assigned_cbs_server(edf_rq, id);
	if (!server)
		return;

	/*
	 * The only task actually on the runqueue is server->curr.
	 * Queued CBS tasks are only waiting inside the server FIFO.
	 */
	if (server->curr) {
		struct task_struct *p = server->curr;

		server->curr = NULL;

		if (READ_ONCE(p->__state) == TASK_RUNNING) {
			dequeue_task(rq, p, 0);
		} else if (transfer_flag && edf_rq->server_count > 1) {
			dst = next_transfer_server(edf_rq, server, dst);
			if (dst) {
				p->edf_cbs.cbs_server_id = dst->id;
				enqueue_task(rq, p, 0);
			}
		}
	}

	if (transfer_flag && edf_rq->server_count > 1) {
        list_for_each_entry_safe(entry, tmp, &server->queue_head, node) {
            struct task_struct *p = entry->task;
            list_del(&entry->node);
            kfree(entry);
            if (!p)
                continue;
            dst = next_transfer_server(edf_rq, server, dst);
            if (!dst)
                continue;
            p->edf_cbs.cbs_server_id = dst->id;
            /*
             * These tasks were not on the runqueue.
             * They are only moved between CBS server FIFOs.
             */
            {
                struct cbs_queue *new_entry;
                bool was_empty;
                new_entry = kmalloc(sizeof(*new_entry), GFP_ATOMIC);
                if (!new_entry)
                    continue;
                new_entry->task = p;
                INIT_LIST_HEAD(&new_entry->node);
                was_empty = list_empty(&dst->queue_head);
                list_add_tail(&new_entry->node, &dst->queue_head);
                if (was_empty && !dst->curr) {
                    dst->curr = p;
                    p->edf_cbs.absDL = dst->absDL;
                }
            }
        }
    } else {
        list_for_each_entry_safe(entry, tmp, &server->queue_head, node) {
            list_del(&entry->node);
            kfree(entry);
        }
    }

    rb_erase(&server->node, &edf_rq->servers);
    edf_rq->server_count--;
    hrtimer_cancel(&server->capacityTimer);
    hrtimer_cancel(&server->deadlineTimer);
    kfree(server);
}