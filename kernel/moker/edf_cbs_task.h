#ifndef __LF_TASK_EDF_CBS
#define __LF_TASK_EDF_CBS

#include <linux/rbtree_types.h>
#include <linux/types.h>

struct sched_edf_cbs_entity {
	u32 id;
	struct rb_node node;

	u64 startInstant;   /* first release time (absolute) */
	u64 relDL;          /* relative deadline / period */
	u64 absDL;          /* absolute deadline */

	bool deadlineUpdate; /* flag to update DL on enqueue */

	/* (very likely present / implied for CBS later) */
	/* u64 runtime; */
	/* u64 budget; */
	/* struct moker_cbs_server *server; */
};
#endif
