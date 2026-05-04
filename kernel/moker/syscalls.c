#include "syscalls.h"
#include "edf_cbs_task.h"
#include "linux/hrtimer.h"
#include "linux/sched.h"
#include "linux/timekeeping.h"
#include <linux/syscalls.h>
#include "../sched/sched.h"
#include "trace.h"
#include "edf_cbs.h"

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

SYSCALL_DEFINE0(delay_edf_cbs_task_until_next_T){
	printk(KERN_INFO "Delay current edf_cbs task of id [%u] until it's next period]", current->edf_cbs.id);
	do_delay_edf_cbs_task_until_next_T();
	return 1;
}

int do_delay_edf_cbs_task_until_next_T(void)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	if (edf_cbs_policy(current->policy)) {
		ktime_t expires;
		ktime_t now;

		if (current->edf_cbs.isHardRT) {
			expires = ns_to_ktime(current->edf_cbs.absDL);

			refresh_task_deadline(current);

			now = ktime_get();

			if (ktime_compare(expires, now) <= 0) {
				current->edf_cbs.deadlineUpdate = true;
				set_tsk_need_resched(current);
				return 1;
			}

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
			__set_current_state(TASK_RUNNING);

			{
				u64 now_ns = ktime_get_ns();
				s64 delta = (s64)(current->edf_cbs.absDL - now_ns);

				printk(KERN_INFO
				       "delay hard id=%u now=%llu absDL=%llu delta=%lld state=%u\n",
				       current->edf_cbs.id,
				       now_ns,
				       current->edf_cbs.absDL,
				       delta,
				       current->__state);
			}

			return 0;
		} else {
			struct rq *rq;
			struct rq_flags rf;
			struct cbs_server *server;

			/*
			 * Soft RT:
			 * Account consumed CBS capacity, but do not modify
			 * server FIFO placement here. The scheduler dequeue
			 * and enqueue paths should handle that.
			 */
			rq = task_rq_lock(current, &rf);

			server = lookup_cbs_server(&rq->edf_cbs,
						   current->edf_cbs.cbs_server_id);

			if (server && server->curr == current) {
				ktime_t end;
				s64 elapsed;

				hrtimer_cancel(&server->capacityTimer);

				end = ktime_get();
				elapsed = ktime_to_ns(
					ktime_sub(end, server->capacityTimerStart)
				);

				if (elapsed > 0) {
					if ((u64)elapsed >= server->currCapacity)
						server->currCapacity = 0;
					else
						server->currCapacity -= (u64)elapsed;
				}
			}

			current->edf_cbs.deadlineUpdate = true;

			task_rq_unlock(rq, current, &rf);

			/*
			 * Refresh this soft task's own period deadline.
			 * It keeps its own period bookkeeping, while CBS
			 * scheduling priority remains server-deadline based.
			 */
			refresh_task_deadline(current);

			expires = ns_to_ktime(current->edf_cbs.absDL);
			now = ktime_get();

			if (ktime_compare(expires, now) <= 0) {
				set_tsk_need_resched(current);
				return 1;
			}

			set_current_state(TASK_INTERRUPTIBLE);
			schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
			__set_current_state(TASK_RUNNING);

			{
				u64 now_ns = ktime_get_ns();
				s64 delta = (s64)(current->edf_cbs.absDL - now_ns);

				printk(KERN_INFO
				       "delay soft id=%u server=%u now=%llu absDL=%llu delta=%lld state=%u\n",
				       current->edf_cbs.id,
				       current->edf_cbs.cbs_server_id,
				       now_ns,
				       current->edf_cbs.absDL,
				       delta,
				       current->__state);
			}

			return 0;
		}
	}
#endif
	return 0;
}


SYSCALL_DEFINE2(create_moker_cbs_server, u64, relDL, u64, capacity)
{
	printk(KERN_INFO "MOKER [%d] | create CBS server relDL -> [%llu], capacity -> [%llu]\n",
	       current->pid,
	       (unsigned long long)relDL,
	       (unsigned long long)capacity);

	return do_create_moker_cbs_server(relDL, capacity);
}

long do_create_moker_cbs_server(u64 relDL, u64 capacity)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	struct rq *rq;
	struct rq_flags rf;
	struct cbs_server *server;
	int id;

	rq = task_rq_lock(current, &rf);

	server = create_cbs_server(&rq->edf_cbs, -1, relDL, capacity);
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

SYSCALL_DEFINE3(setup_moker_edf_cbs_soft_task,
		u32, server_id,
		u64, startInstant,
		u64, relDL)
{
	printk(KERN_INFO "MOKER [%d] | soft_server_id -> [%u], startInstant -> [%llu], relDL -> [%llu]\n",
	       current->pid,
	       server_id,
	       (unsigned long long)startInstant,
	       (unsigned long long)relDL);

	return do_setup_moker_edf_cbs_soft_task(server_id, startInstant, relDL);
}

int do_setup_moker_edf_cbs_soft_task(u32 server_id, u64 startInstant, u64 relDL)
{
#ifdef CONFIG_MOKER_EDF_CBS_POLICY
	struct sched_edf_cbs_entity *sched_entity = &current->edf_cbs;
	struct rq *rq;
	struct rq_flags rf;
	struct cbs_server *server;
	u64 server_absDL;
	struct sched_param param = { 0 };

	rq = task_rq_lock(current, &rf);

	server = lookup_cbs_server(&rq->edf_cbs, server_id);
	if (!server) {
		task_rq_unlock(rq, current, &rf);
		return -EINVAL;
	}

	if (server->absDL == 0)
		server->absDL = startInstant + server->relDL;

	server_absDL = server->absDL;

	task_rq_unlock(rq, current, &rf);

	sched_entity->startInstant = startInstant;
	sched_entity->relDL = relDL;              /* task period/timekeeping */
	sched_entity->absDL = server_absDL;       /* CBS scheduling deadline */
	sched_entity->deadlineUpdate = false;
	sched_entity->cbs_server_id = server_id;
	sched_entity->isHardRT = false;

	return sched_setscheduler(current, SCHED_EDF_CBS, &param);
#endif
	return -ENOSYS;
}