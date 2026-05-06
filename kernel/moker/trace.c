#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <asm/uaccess.h>
#include <linux/uaccess.h>
#include "trace.h"

struct trace_evt_buffer trace;

unsigned int enabled = 0;

static void increment(int *item) {
    *item = *item + 1;
    if (*item >= TRACE_BUFFER_SIZE) {
        *item = 0;
    }
}

static int is_empty(int r, int w) {
    // empty if r == w
    return !(r ^ w); // xor
}

static int is_full(int r, int w) {
    int write = w;
    increment(&write);
    return write == r;
}

static int dequeue(char *buffer)
{
	int len;
	char evt[10];
	struct trace_evt e;

	spin_lock(&trace.lock);

	if (is_empty(trace.read_item, trace.write_item)) {
		spin_unlock(&trace.lock);
		return 0;
	}

	e = trace.events[trace.read_item];
	increment(&trace.read_item);

	spin_unlock(&trace.lock);

	switch ((int)e.event) {
	case MISC_EVT:
		strcpy(evt, "MC_EVT");
		break;
	case SCHED_TICK:
		strcpy(evt, "SCH_TK");
		break;
	case SWITCH_AWAY:
		strcpy(evt, "SWT_AY");
		break;
	case SWITCH_TO:
		strcpy(evt, "SWT_TO");
		break;
	case ENQUEUE_RQ:
		strcpy(evt, "ENQ_RQ");
		break;
	case DEQUEUE_RQ:
		strcpy(evt, "DEQ_RQ");
		break;
	default:
		strcpy(evt, "UK_EVT");
	}

	len = sprintf(buffer, "%llu,", e.time);
	len += sprintf(buffer + len, "%s,", evt);
	len += sprintf(buffer + len, "*%d*,", (int)e.number);
	len += sprintf(buffer + len, "%d,", (int)e.policy);
	len += sprintf(buffer + len, "%d,", (int)e.prio);
	len += sprintf(buffer + len, "%d,", (int)e.pid);
	len += sprintf(buffer + len, "%u,", e.moker_id);
	len += sprintf(buffer + len, "%d,", (int)e.state);
	len += sprintf(buffer + len, "%s\n", e.comm);

	return 1;
}

static int enqueue(enum evt event, unsigned long long time, int number, struct task_struct *p) {
	if (!p)
		return 0;

    spin_lock(&trace.lock);

    if (is_full(trace.read_item, trace.write_item)) {
        increment(&trace.read_item);
    }

    trace.events[trace.write_item].number = number;
    trace.events[trace.write_item].event = event;
    trace.events[trace.write_item].time = time;
	trace.events[trace.write_item].pid = p->pid;
	trace.events[trace.write_item].moker_id = p->edf_cbs.id;
    trace.events[trace.write_item].state = p->__state;
    trace.events[trace.write_item].prio = p->prio;
    trace.events[trace.write_item].policy = p->policy;

	strscpy(trace.events[trace.write_item].comm,
			p->comm,
			sizeof(trace.events[trace.write_item].comm));

    increment(&trace.write_item);

    spin_unlock(&trace.lock);
    return 1;
}

ssize_t trace_read(struct file *filp, char __user *buf, size_t count, loff_t *f_pos) {
    char buffer[TRACE_STRING_BUFFER_SIZE];
    int ret = 0, len = 0;

    if (!dequeue(buffer))
        return 0;

    len = strlen(buffer);

    if (len <= 0)
        return -EFAULT;

    if (count < len)
        return -EFAULT;

    ret = copy_to_user(buf, buffer, len);

    if (ret != 0)
        return -EFAULT;

    return len;
}

static const struct proc_ops trace_ops = {
    .proc_read = trace_read,
};

static int __init proc_trace_init(void) {
    proc_create(TRACE_ENTRY_NAME, 0444, NULL, &trace_ops);

    printk("MOKER:/proc/%s created\n", TRACE_ENTRY_NAME);

    spin_lock_init(&trace.lock);
    trace.write_item = 0;
    trace.read_item = 0;

    return 0;
}

module_init(proc_trace_init);

void moker_trace(enum evt event, struct task_struct *p, int number)
{
	unsigned long long time;
	if (enabled && p) {
		time = ktime_to_ns(ktime_get());
		enqueue(event,time, number, p);
	}
}
void enable_tracing (unsigned int e)
{
	enabled = e;
}
