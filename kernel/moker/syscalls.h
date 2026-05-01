#ifndef __SYCALLS_H
#define __SYCALLS_H
#include <linux/types.h>

int do_moker_tracing (unsigned int enable);

#ifdef CONFIG_MOKER_EDF_CBS_POLICY
int do_setup_moker_edf_cbs_task(u32 id, u64 startInstant, u64 deadline);
int do_delay_edf_cbs_task_until_next_T(void);
#endif

#endif

