#include <litmus/sched_plugin.h>
#include <litmus/preempt.h>

static struct task_struct* simple_schedule(struct task_struct * prev)
{
  /* This mandatory. It triggers a transition in the LITMUS^RT remote
   * preemption state machine. Call this AFTER the plugin has made a local
   * scheduling decision.
   */
  sched_state_task_picked();

  /* We don't schedule anything for now. NULL means "schedule background work". */
  return NULL;
}

static long simple_admit_task(struct task_struct *tsk)
{
  /* Reject every task. */
  return -EINVAL;
}

static struct sched_plugin simple_plugin = {
  .plugin_name            = "SIMPLE",
  .schedule               = simple_schedule,
  .admit_task             = simple_admit_task,
};

static int __init init_simple(void)
{
  return register_sched_plugin(&simple_plugin);
}

module_init(init_simple);
