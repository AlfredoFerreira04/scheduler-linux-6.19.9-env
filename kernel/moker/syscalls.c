#include "syscalls.h"
#include "edf_cbs_task.h"
#include "linux/hrtimer.h"
#include "linux/sched.h"
#include "linux/timekeeping.h"
#include <linux/syscalls.h>
#include "../sched/sched.h"
#include "trace.h"
#include "edf_cbs.h"
#include "utils.h"

SYSCALL_DEFINE1(moker_tracing, unsigned int, enable)
{
	printk("MOKER: moker_tracing:[%d][%d]\n", (int) enable, current->pid);
	return do_moker_tracing(enable);
}

int do_moker_tracing (unsigned int enable){
#ifdef CONFIG_MOKER_TRACING
	printk("MOKER: sys_moker_tracing:[%d][%d]\n", (int) enable, current->pid);
	enable_tracing(enable);
#endif
	return 0;
}

SYSCALL_DEFINE3(setup_moker_edf_cbs_task, u32, id,u64, startInstant, u64, deadline)
{
		printk("MOKER [%d] | edf_task_id -> [%d], edf_cbs_start_instant -> [%llu], edf_cbs_deadline -> [%llu]\n",
			current->pid,
			id,
			(unsigned long long)startInstant,
			(unsigned long long)deadline);
	   	return do_setup_moker_edf_cbs_task(id, startInstant, deadline);
}

int do_setup_moker_edf_cbs_task(u32 id,u64 startInstant, u64 deadline)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	struct sched_edf_cbs_entity *sched_entity = &current->edf_cbs;

	RB_CLEAR_NODE(&sched_entity->node);
	sched_entity->absDL = startInstant + deadline;
	sched_entity->startInstant = startInstant;
	sched_entity->relDL = deadline;
	sched_entity->deadlineUpdate = false;	// deadline will enter already setup
	sched_entity->id = id;
	sched_entity->isHardRT = true;

	struct sched_param param = { 0 };
	return sched_setscheduler(current, SCHED_EDF_CBS, &param);
#endif
	return 1;
}

SYSCALL_DEFINE0(delay_edf_cbs_task_until_next_T)
{
	printk(KERN_INFO
	       "Delay current edf_cbs task of id [%u] until its next period\n",
	       current->edf_cbs.id);

	return do_delay_edf_cbs_task_until_next_T();
}

int do_delay_edf_cbs_task_until_next_T(void)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	if (edf_cbs_policy(current->policy)) {
		ktime_t expires;
		ktime_t now;

		if (current->edf_cbs.isHardRT) {
			ktime_t now = ktime_get();
			ktime_t expires = ns_to_ktime(current->edf_cbs.absDL);

			 /* Hard RT:
			  * Sleep until the next period deadline, without accounting or
			  * releasing CBS server ownership. This is a hard delay, as the
			  * task keeps its current scheduling priority and remains at the
			  * head of its server's queue if it has one.
			  */
			refresh_task_deadline(current);
			printk(KERN_INFO
				"hard delay after refresh id=%u nextT=%llu absDL=%llu now=%llu delta_ms=%lld\n",
				current->edf_cbs.id,
				ktime_to_ns(expires),
				current->edf_cbs.absDL,
				ktime_get_ns(),
				(s64)(current->edf_cbs.absDL - ktime_get_ns()) / 1000000);

			if (ktime_compare(expires, now) <= 0) {
				current->edf_cbs.deadlineUpdate = true;
				set_tsk_need_resched(current);
				return 1;
			}

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
			__set_current_state(TASK_RUNNING);

			return 0;
		} else {
			struct rq *rq;
			struct rq_flags rf;
			struct cbs_server *server;
			u64 now_before_account;

			/*
			* Soft RT:
			* Account consumed CBS capacity and release CBS ownership before
			* voluntarily sleeping until the next period.
			* FIFO promotion remains handled by scheduler enqueue/dequeue paths.
			*/
			now_before_account = ktime_get_ns();
			rq = task_rq_lock(current, &rf);

			server = lookup_assigned_cbs_server(&rq->edf_cbs,
						   current->edf_cbs.cbs_server_id);

			if (server && server->curr == current) {
				account_cbs_runtime(server);
				printk(KERN_INFO
					"soft delay after account server=%u from=%u cap=%llu active=%d start=%llu curr=%u consumed_span=%llu now_ms=%llu\n",
					server->id,
					current->edf_cbs.id,
					server->currCapacity,
					server->capacity_active ? 1 : 0,
					server->capacityTimerStart,
					server->curr ? server->curr->edf_cbs.id : 0,
					now_before_account - server->capacityTimerStart,
					now_before_account / 1000000ULL);

				if (!RB_EMPTY_NODE(&current->edf_cbs.node)) {
					rb_erase(&current->edf_cbs.node, &rq->edf_cbs.tasks_tree);
					RB_CLEAR_NODE(&current->edf_cbs.node);
					sub_nr_running(rq, 1);
				}

				server->curr = NULL;

				if (!list_empty(&server->queue_head)) {
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
					next->edf_cbs.deadlineUpdate = false;

					insert_edf_tree(rq, next);
				}

				update_edf_pick(rq);

				printk(KERN_INFO
					"soft delay release/promote server=%u from=%u curr=%u remainingCap=%llu\n",
					current->edf_cbs.cbs_server_id,
					current->edf_cbs.id,
					server->curr ? server->curr->edf_cbs.id : 0,
					server->currCapacity);
			}
			
			current->edf_cbs.deadlineUpdate = true;

			task_rq_unlock(rq, current, &rf);

			/*
			 * Refresh this soft task's own period deadline.
			 * It keeps its own period bookkeeping, while CBS
			 * scheduling priority remains server-deadline based.
			 */
			refresh_task_period(current);
			printk(KERN_INFO
				"soft delay after refresh id=%u absT=%llu absDL=%llu now=%llu deltaT_ms=%lld deltaDL_ms=%lld\n",
				current->edf_cbs.id,
				current->edf_cbs.absT,
				current->edf_cbs.absDL,
				ktime_get_ns(),
				(s64)(current->edf_cbs.absT - ktime_get_ns()) / 1000000,
				(s64)(current->edf_cbs.absDL - ktime_get_ns()) / 1000000);

			expires = ns_to_ktime(current->edf_cbs.absT);
			printk(KERN_INFO
				"soft delay post-refresh id=%u absT=%llu now=%llu delta_ms=%lld\n",
				current->edf_cbs.id,
				current->edf_cbs.absT,
				ktime_get_ns(),
				(s64)(current->edf_cbs.absT - ktime_get_ns()) / 1000000);

			now = ktime_get();
			if (ktime_compare(expires, now) <= 0) {
				printk(KERN_INFO
					"soft delay past deadline id=%u absT=%llu now=%llu, rescheduling\n",
					current->edf_cbs.id,
					current->edf_cbs.absT,
					ktime_to_ns(now));
				set_tsk_need_resched(current);
				return 1;
			}

			printk(KERN_INFO
				"soft delay sleep id=%u sleep_until=%llu now=%llu delta_ms=%lld\n",
				current->edf_cbs.id,
				current->edf_cbs.absT,
				ktime_to_ns(now),
				(s64)(current->edf_cbs.absT - ktime_to_ns(now)) / 1000000);

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
			__set_current_state(TASK_RUNNING);
			printk(KERN_INFO
				"soft delay woken id=%u now=%llu state=%u\n",
				current->edf_cbs.id,
				ktime_get_ns() / 1000000ULL,
				READ_ONCE(current->__state));

		{

			/* Tracing */
			u64 now_ns = ktime_get_ns();
			s64 deltaT = (s64)(current->edf_cbs.absT - now_ns);
			s64 deltaDL = (s64)(current->edf_cbs.absDL - now_ns);

			printk(KERN_INFO
				"soft delay return id=%u server=%u now=%llu absT=%llu absDL=%llu deltaT=%lld deltaDL=%lld state=%u\n",
				current->edf_cbs.id,
				current->edf_cbs.cbs_server_id,
				now_ns,
				current->edf_cbs.absT,
				current->edf_cbs.absDL,
				deltaT,
				deltaDL,
				READ_ONCE(current->__state));
		}

			return 0;
		}
	}
#endif
	return 0;
}


SYSCALL_DEFINE4(create_moker_cbs_server, u64, relDL, u64, capacity, u64, start_instant,
		u32, utilization)
{
	return do_create_moker_cbs_server(relDL, capacity, start_instant, utilization);
}

long do_create_moker_cbs_server(u64 relDL, u64 capacity, u64 start_instant, u32 utilization)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	struct rq *rq;
	struct rq_flags rf;
	struct cbs_server *server;
	int id;

	if (utilization == 0 || utilization >= 100)
		return -EINVAL;

	rq = task_rq_lock(current, &rf);

	server = create_cbs_server(&rq->edf_cbs, start_instant, relDL, capacity, utilization);
	if (!server) {
		task_rq_unlock(rq, current, &rf);
		return -ENOMEM;
	}

	id = server->id;

	task_rq_unlock(rq, current, &rf);

	return id;
#endif
	return -ENOSYS;
}

SYSCALL_DEFINE4(setup_moker_edf_cbs_soft_task,
		u32, server_id,
		u32, task_id,
		u64, startInstant,
		u64, relDL)
{
	printk("MOKER [%d] | task=%u, soft_server_id -> [%u], startInstant -> [%llu], relDL -> [%llu]\n",
	       current->pid,
		   	task_id,
	       server_id,
	       (unsigned long long)startInstant,
	       (unsigned long long)relDL);

	return do_setup_moker_edf_cbs_soft_task(server_id, task_id, startInstant, relDL);
}

int do_setup_moker_edf_cbs_soft_task(u32 server_id, u32 task_id, u64 startInstant, u64 relDL)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	struct sched_edf_cbs_entity *sched_entity = &current->edf_cbs;
	struct rq *rq;
	struct rq_flags rf;
	struct cbs_server *server;
	u64 server_absDL;
	struct sched_param param = { 0 };

	rq = task_rq_lock(current, &rf);

	server = lookup_assigned_cbs_server(&rq->edf_cbs, server_id);
	if (!server) {
		task_rq_unlock(rq, current, &rf);
		return -EINVAL;
	}

	server_absDL = server->absDL;

	task_rq_unlock(rq, current, &rf);

	RB_CLEAR_NODE(&sched_entity->node);
	sched_entity->startInstant = startInstant;
	sched_entity->absT = startInstant;
	sched_entity->relDL = relDL;              /* task period/timekeeping */
	sched_entity->absDL = server_absDL;       /* CBS scheduling deadline */
	sched_entity->deadlineUpdate = false;
	sched_entity->cbs_server_id = server_id;
	sched_entity->id = task_id;
	sched_entity->isHardRT = false;

	return sched_setscheduler(current, SCHED_EDF_CBS, &param);
#endif
	return -ENOSYS;
}

SYSCALL_DEFINE1(destroy_moker_cbs_server, u32, server_id)
{
	printk(KERN_INFO "MOKER [%d] | destroy CBS server id -> [%u]\n",
	       current->pid,
	       server_id);

	return do_destroy_moker_cbs_server(server_id);
}

int do_destroy_moker_cbs_server(u32 server_id)
{
	struct cbs_server *server;
	struct cbs_queue *entry, *tmp;
	struct rq *rq;

	rq = cpu_rq(task_cpu(current));

	raw_spin_lock(&rq->edf_cbs.lock);

	server = lookup_assigned_cbs_server(&rq->edf_cbs, server_id);
	if (!server) {
		raw_spin_unlock(&rq->edf_cbs.lock);
		return -ENOENT;
	}

	/*
	 * Stop timers before freeing the server.
	 *
	 * hrtimer_cancel() waits for an active callback to finish, so the
	 * server won't be freed while its timer function is still running.
	 */
	hrtimer_cancel(&server->capacityTimer);
	hrtimer_cancel(&server->deadlineTimer);

	/*
	 * If the current server task is still in the EDF tree, remove it.
	 */
	if (server->curr) {
		struct task_struct *p = server->curr;

		if (!RB_EMPTY_NODE(&p->edf_cbs.node)) {
			rb_erase(&p->edf_cbs.node, &rq->edf_cbs.tasks_tree);
			RB_CLEAR_NODE(&p->edf_cbs.node);
			sub_nr_running(rq, 1);
		}

		p->edf_cbs.cbs_server_id = -1;
		server->curr = NULL;
	}

	/*
	 * Free waiting soft tasks from the CBS FIFO.
	 */
	list_for_each_entry_safe(entry, tmp, &server->queue_head, node) {
		if (entry->task)
			entry->task->edf_cbs.cbs_server_id = -1;

		list_del(&entry->node);
		kfree(entry);
	}

	rb_erase(&server->node, &rq->edf_cbs.servers);

	raw_spin_unlock(&rq->edf_cbs.lock);

	kfree(server);

	printk(KERN_INFO "MOKER: destroyed CBS server id=%u\n", server_id);

	return 0;
}