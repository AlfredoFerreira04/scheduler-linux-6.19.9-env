#include "../sched/sched.h"
#include "linux/ktime.h"
#include "edf_cbs_rq.h"

void init_edf_cbs_rq(struct edf_cbs_rq *rq)
{
    raw_spin_lock_init(&rq->lock);
    rq->tasks_tree = RB_ROOT;
    rq->task = NULL;
    INIT_LIST_HEAD(&rq->servers);
    rq->server_count = 0;
}

struct cbs_server *lookup_cbs_server(struct edf_cbs_rq *rq, int id)
{
	struct cbs_server *server;

	if (!rq)
		return NULL;

	list_for_each_entry(server, &rq->servers, list_node) {
		if (server->id == id)
			return server;
	}

	return NULL;
}

static enum hrtimer_restart cbs_capacity_timer_fn(struct hrtimer *timer)
{
	struct cbs_server *server;
	struct task_struct *p;

	server = container_of(timer, struct cbs_server, capacityTimer);

	/*
	 * Capacity exhausted:
	 * - replenish capacity
	 * - push server deadline
	 * - update current soft task deadline
	 */
	server->currCapacity = server->maximumCapacity;
	server->absDL += server->relDL;

	p = server->curr;
	if (p) {
		p->edf_cbs.absDL = server->absDL;
		p->edf_cbs.relDL = server->relDL;
		p->edf_cbs.deadlineUpdate = true;
		set_tsk_need_resched(p);
	}

	return HRTIMER_NORESTART;
}

static enum hrtimer_restart cbs_deadline_timer_fn(struct hrtimer *timer)
{
	struct cbs_server *server;

	server = container_of(timer, struct cbs_server, deadlineTimer);

	server->currCapacity = server->maximumCapacity;
	server->absDL += server->relDL;

	if (server->curr) {
		server->curr->edf_cbs.absDL = server->absDL;
		server->curr->edf_cbs.deadlineUpdate = true;
		set_tsk_need_resched(server->curr);
	}

	return HRTIMER_NORESTART;
}

struct cbs_server *create_cbs_server(struct edf_cbs_rq *rq,
				     int id_unused,
				     u64 relDL,
				     u64 capacity)
{
	struct cbs_server *server;
	u32 new_id = 0;
	bool exists;

	if (!rq)
		return NULL;

	do {
		struct cbs_server *tmp;

		exists = false;

		list_for_each_entry(tmp, &rq->servers, list_node) {
			if (tmp->id == new_id) {
				exists = true;
				new_id++;
				break;
			}
		}
	} while (exists);

	server = kmalloc(sizeof(*server), GFP_ATOMIC);
	if (!server)
		return NULL;

	server->id = new_id;
	INIT_LIST_HEAD(&server->queue_head);
	server->curr = NULL;
	INIT_LIST_HEAD(&server->list_node);
	server->relDL = relDL;
	server->absDL = 0;
	server->maximumCapacity = capacity;
	server->currCapacity = capacity;

    hrtimer_setup(&server->capacityTimer,
            cbs_capacity_timer_fn,
            CLOCK_MONOTONIC,
            HRTIMER_MODE_ABS);

    hrtimer_setup(&server->deadlineTimer,
            cbs_deadline_timer_fn,
            CLOCK_MONOTONIC,
            HRTIMER_MODE_ABS);

	list_add_tail(&server->list_node, &rq->servers);
	rq->server_count++;

	return server;
}

static struct cbs_server *next_transfer_server(struct edf_cbs_rq *edf_rq,
					       struct cbs_server *victim,
					       struct cbs_server *last)
{
	struct cbs_server *server;

	if (edf_rq->server_count <= 1)
		return NULL;

	if (last && last->list_node.next != &edf_rq->servers)
		server = list_next_entry(last, list_node);
	else
		server = list_first_entry(&edf_rq->servers,
					  struct cbs_server,
					  list_node);

	if (server == victim) {
		if (server->list_node.next != &edf_rq->servers)
			server = list_next_entry(server, list_node);
		else
			server = list_first_entry(&edf_rq->servers,
						  struct cbs_server,
						  list_node);
	}

	if (server == victim)
		return NULL;

	return server;
}


void destroy_cbs_server(struct rq *rq, int id, bool transfer_flag)
{
	struct edf_cbs_rq *edf_rq = &rq->edf_cbs;
	struct cbs_server *server;
	struct cbs_queue *entry, *tmp;
	struct cbs_server *dst = NULL;

	if (!edf_rq)
		return;

	server = lookup_cbs_server(edf_rq, id);
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

	list_del(&server->list_node);
	edf_rq->server_count--;
	kfree(server);
}