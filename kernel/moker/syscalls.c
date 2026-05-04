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

int do_delay_edf_cbs_task_until_next_T(void){

#ifdef CONFIG_MOKER_EDF_CBS_POLICY

	// Gets absolute deadline and sets a timeout to it
	// returns 1 if deadline had passed already, returns 0 if waited
	// This basically moves the task to the waitqueue by sleeping it
	// Then wakes it up with hrtimer precision upon deadline expiring
	// https://elixir.bootlin.com/linux/v6.19/source/kernel/time/sleep_timeout.c#L227

	// Additionally this syscall will flag the task/sched entity for a deadline update
	// This is to basically mimic a periodic hard RT task for the purpose of the project

	if(edf_cbs_policy(current->policy)){
		ktime_t expires = ns_to_ktime(current->edf_cbs.absDL);

		refresh_task_deadline(current);

		ktime_t now = ktime_get();

		if (ktime_compare(expires, now) <= 0) {
			current->edf_cbs.deadlineUpdate = true;
			set_tsk_need_resched(current);
			return 1;
		}

		set_current_state(TASK_INTERRUPTIBLE);
		schedule_hrtimeout(&expires, HRTIMER_MODE_ABS);
		__set_current_state(TASK_RUNNING);	


		// Debugging purposes
		now = ktime_get_ns();
		s64 delta = (s64)(current->edf_cbs.absDL - now);

		printk(KERN_INFO "delay enter id=%u now=%llu absDL=%llu delta=%lld state=%u\n",
			current->edf_cbs.id, now, current->edf_cbs.absDL, delta, current->__state);
	}

#endif
	return 0;
}

