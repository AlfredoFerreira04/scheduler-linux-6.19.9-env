#include <linux/timekeeping.h>
#include "edf_cbs_task.h"
#include <linux/types.h>
#include <linux/sched.h>

static void refresh_task_deadline(struct task_struct *p)
{
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;
	u64 now = ktime_get_ns();

	/* Advance deadline until it is strictly in the future */
	do {
		sched_entity->absDL += sched_entity->relDL;
	} while ((s64)(sched_entity->absDL - now) <= 0);

	sched_entity->deadlineUpdate = false;
}

static void refresh_task_period(struct task_struct *p)
{
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;
	u64 now = ktime_get_ns();

	/* Advance period until it is strictly in the future */
	do {
		sched_entity->absT += sched_entity->relDL;
	} while ((s64)(sched_entity->absT - now) <= 0);

	printk(KERN_INFO
	       "refresh period id=%u server=%u now=%llu absT=%llu absDL=%llu deltaT=%lld deltaDL=%lld state=%u\n",
	       p->edf_cbs.id,
	       p->edf_cbs.cbs_server_id,
	       now,
	       p->edf_cbs.absT,
	       p->edf_cbs.absDL,
	       (s64)(p->edf_cbs.absT - now),
	       (s64)(p->edf_cbs.absDL - now),
	       p->__state);
}