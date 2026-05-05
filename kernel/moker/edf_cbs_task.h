#ifndef __LF_TASK_EDF_CBS
#define __LF_TASK_EDF_CBS

#include <linux/rbtree_types.h>
#include <linux/types.h>

struct sched_edf_cbs_entity {
	u32 id;					/* unique identifier for tracing mechanism */
	struct rb_node node;	/* associated node for tree inserted */

	u64 startInstant;   	/* first release time (absolute) */
	u64 relDL;          	/* relative deadline / period */
	u64 absDL;          	/* absolute deadline */
	u64 absT;

	bool deadlineUpdate;	/* flag to update DL on enqueue */

	bool isHardRT;			/* distinguish between soft and hard RT tasks*/
	u32  cbs_server_id;		/* associated server id for soft RT tasks */
};
#endif
