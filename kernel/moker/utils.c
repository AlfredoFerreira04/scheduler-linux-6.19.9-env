#include "utils.h"
#include <linux/rbtree.h>
#include <linux/timekeeping.h>

void account_cbs_runtime(struct cbs_server *server)
{
    u64 now_ns;
    u64 elapsed;

    if (!server)
        return;

    if (!server->capacity_active) {
        server->capacityTimerStart = 0;
        return;
    }

    int cancelled = hrtimer_try_to_cancel(&server->capacityTimer);

	printk(KERN_INFO
		"CBS account runtime server=%u cancel=%d active=%d start=%llu cap_before=%llu\n",
		server->id,
		cancelled,
		server->capacity_active ? 1 : 0,
		server->capacityTimerStart,
		server->currCapacity);

		now_ns = ktime_get_ns();

    if (now_ns > server->capacityTimerStart) {
        elapsed = now_ns - server->capacityTimerStart;

        if (elapsed >= server->currCapacity)
            server->currCapacity = 0;
        else
            server->currCapacity -= elapsed;
    }

    server->capacity_active = false;
    server->capacityTimerStart = 0;
}

void insert_edf_tree(struct rq *rq, struct task_struct *p)
{
    struct rb_root *root = &rq->edf_cbs.tasks_tree;
    struct rb_node **link = &root->rb_node;
    struct rb_node *parent = NULL;
    struct sched_edf_cbs_entity *se = &p->edf_cbs;

    /* Skip if already in tree (already enqueued) */
    if (!RB_EMPTY_NODE(&se->node))
		return;
    
    /* Initialize the rbtree node for first insertion */
    RB_CLEAR_NODE(&se->node);

    /* Standard BST search to find insertion point */
    while (*link) {
        struct sched_edf_cbs_entity *entry;

        entry = rb_entry(*link, struct sched_edf_cbs_entity, node);
        parent = *link;

        if (se->absDL < entry->absDL)
            link = &(*link)->rb_left;
        else
            link = &(*link)->rb_right;
    }

    /* Link the new node and rebalance the red-black tree */
    rb_link_node(&se->node, parent, link);
    rb_insert_color(&se->node, root);

    /* Update runqueue count */
    add_nr_running(rq, 1);
}

void update_edf_pick(struct rq *rq)
{
	struct rb_node *first = rb_first(&rq->edf_cbs.tasks_tree);

	if (first) {
		struct sched_edf_cbs_entity *first_entity;

		first_entity = rb_entry(first, struct sched_edf_cbs_entity, node);
		rq->edf_cbs.task = container_of(first_entity,
						struct task_struct,
						edf_cbs);
	} else {
		rq->edf_cbs.task = NULL;
	}
}