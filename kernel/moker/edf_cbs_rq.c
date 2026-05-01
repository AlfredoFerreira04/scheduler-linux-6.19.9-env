#include "edf_cbs_rq.h"

void init_edf_cbs_rq(struct edf_cbs_rq *rq)
{
    raw_spin_lock_init(&rq->lock);
    rq->tasks_tree = RB_ROOT;
    rq->task = NULL;
}
