#ifndef __EDF_CBS_RQ_H_
#define __EDF_CBS_RQ_H_

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/rbtree.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/slab.h>

/* A queue member holding a pointer to a task */
struct cbs_queue {
	struct list_head node;      // Link to other tasks in this server
	struct task_struct *task;   // Pointer to the task (standard practice)
};

/* The CBS Server entity */
struct cbs_server {
	u32 id;                     // Unique identifier for bookkeeping
	struct list_head queue_head; // List of tasks assigned to this server
	struct task_struct *curr;   // The task currently being served
	struct rb_node node;        // Link node for servers rb_tree in edf_cbs_rq
	u64 relDL;          	/* relative deadline / period */
	u64 absDL;          	/* absolute deadline */
	u32 utilization;     	/* percentage in [1..99], representing U in ]0,1[ */

	struct rq *rq;
	
	u64 maximumCapacity;
	u64 currCapacity;
	u64 capacityTimerStart;
	bool capacity_active;
	
	struct hrtimer capacityTimer;
	struct hrtimer deadlineTimer;
};

/* The global Run Queue structure */
struct edf_cbs_rq {
	struct rb_root tasks_tree;		// EDF priority sorted tree for all tasks
	struct task_struct *task;	// highest priority task
	struct rb_root servers;     // RB-tree of all registered CBS servers (keyed by id)
	int server_count;           // Total number of servers
	raw_spinlock_t lock;		// RQ spinlock for synchronization
};

/* Function Prototypes */
void init_edf_cbs_rq(struct edf_cbs_rq *rq);
struct cbs_server *create_cbs_server(struct edf_cbs_rq *rq, u64 start_instant, u64 relDL,
				     u64 capacity, u32 utilization);
void reinsert_cbs_server_tree_locked(struct edf_cbs_rq *rq, struct cbs_server *server);
void destroy_cbs_server(struct rq *rq, int id, bool transfer_flag);
struct cbs_server *lookup_assigned_cbs_server(struct edf_cbs_rq *rq, int id);

#endif
