#include <linux/timekeeping.h>
#include "edf_cbs_task.h"
#include <linux/types.h>
#include <linux/sched.h>

static inline void refresh_task_deadline(struct task_struct *p)
{
	struct sched_edf_cbs_entity *sched_entity = &p->edf_cbs;
	u64 now = ktime_get_ns();

	/* Advance deadline until it is strictly in the future */
	do {
		sched_entity->absDL += sched_entity->relDL;
	} while ((s64)(sched_entity->absDL - now) <= 0);

	sched_entity->deadlineUpdate = false;
}