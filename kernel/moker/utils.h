#ifndef __MOKER_UTILS_H
#define __MOKER_UTILS_H

#include "../sched/sched.h"
#include "edf_cbs_rq.h"

void account_cbs_runtime(struct cbs_server *server);
void insert_edf_tree(struct rq *rq, struct task_struct *p);
void update_edf_pick(struct rq *rq);

void print_edf_tree(struct rq *rq);
void enqueue_hard_rt_task(struct rq *rq, struct task_struct *p);
void enqueue_soft_rt_task(struct rq *rq, struct task_struct *p);
void dequeue_hard_rt_task_edf_cbs(struct rq *rq, struct task_struct *p);
void dequeue_soft_rt_task_edf_cbs(struct rq *rq, struct task_struct *p, int flags, bool deadline_update);

#endif