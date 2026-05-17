#ifndef __SYCALLS_H
#define __SYCALLS_H
#include <linux/types.h>

int do_moker_tracing (unsigned int enable);

#ifdef CONFIG_MOKER_EDF_CBS_POLICY
int do_setup_moker_edf_cbs_task(u32 id, u64 startInstant, u64 deadline);
int do_delay_edf_cbs_task_until_next_T(void);
long do_create_moker_cbs_server(u64 relDL, u64 capacity, u64 start_instant, u32 utilization);
int do_setup_moker_edf_cbs_soft_task(u32 server_id, u32 task_id, u64 startInstant, u64 relDL);
int do_destroy_moker_cbs_server(u32 server_id);
#endif
#endif

