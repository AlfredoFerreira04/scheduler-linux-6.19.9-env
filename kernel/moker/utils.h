#ifndef __MOKER_UTILS_H
#define __MOKER_UTILS_H

#include "../sched/sched.h"
#include "edf_cbs_rq.h"

void account_cbs_runtime(struct cbs_server *server);
void insert_edf_tree(struct rq *rq, struct task_struct *p);
void update_edf_pick(struct rq *rq);

#endif