#include <linux/timekeeping.h>
#include "edf_cbs_task.h"
#include <linux/types.h>
#include <linux/sched.h>

static void refresh_task_deadline(struct task_struct *p)
{
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;
	/* Advance deadline until it is strictly in the future */
	sched_entity->absDL += sched_entity->relDL;
	sched_entity->startInstant += sched_entity->relDL;
	sched_entity->deadlineUpdate = false;
}

static void refresh_task_period(struct task_struct *p)
{
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;
	/* Advance period until it is strictly in the future */
	sched_entity->absT += sched_entity->relDL;
	sched_entity->startInstant += sched_entity->relDL;
}