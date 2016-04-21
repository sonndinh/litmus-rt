#include <linux/percpu.h>
#include <linux/sched.h>

#include <litmus/litmus.h>
#include <litmus/rt_domain.h>
#include <litmus/edf_common.h>
#include <litmus/sched_plugin.h>
#include <litmus/preempt.h>
#include <litmus/debug_trace.h>
#include <litmus/jobs.h>
#include <litmus/budget.h>
#include <litmus/litmus_proc.h>

struct pedf_cpu_state {
  rt_domain_t     local_queues;
  int             cpu;
  
  struct task_struct* scheduled;
};

static DEFINE_PER_CPU(struct pedf_cpu_state, pedf_cpu_state);

#define cpu_state_for(cpu_id)   (&per_cpu(pedf_cpu_state, cpu_id))
//#define local_cpu_state()       (&__get_cpu_var(pedf_cpu_state))
#define local_cpu_state()   this_cpu_ptr(&pedf_cpu_state)

static struct domain_proc_info pedf_domain_proc_info;

static long pedf_get_domain_proc_info(struct domain_proc_info **ret)
{
  *ret = &pedf_domain_proc_info;
  return 0;
}

/* this helper is called when task `prev` exhausted its budget or when
 * it signaled a job completion */
static void pedf_job_completion(struct task_struct *prev, int budget_exhausted)
{
  /* call common helper code to compute the next release time, deadline,
   * etc. */
  prepare_for_next_period(prev);
}


/* Add the task `tsk` to the appropriate queue. Assumes caller holds the ready lock.
 */
static void pedf_requeue(struct task_struct *tsk, struct pedf_cpu_state *cpu_state)
{
  if (is_released(tsk, litmus_clock())) {
    /* Uses __add_ready() instead of add_ready() because we already
     * hold the ready lock. */
    __add_ready(&cpu_state->local_queues, tsk);
  } else {
    /* Uses add_release() because we DON'T have the release lock. */
    add_release(&cpu_state->local_queues, tsk);
  }
}


static struct task_struct* pedf_schedule(struct task_struct * prev)
{
  struct pedf_cpu_state *local_state = local_cpu_state();

  /* next == NULL means "schedule background work". */
  struct task_struct *next = NULL;

  /* prev's task state */
  int exists, out_of_time, job_completed, self_suspends, preempt, resched;

  raw_spin_lock(&local_state->local_queues.ready_lock);

  BUG_ON(local_state->scheduled && local_state->scheduled != prev);
  BUG_ON(local_state->scheduled && !is_realtime(prev));

  exists = local_state->scheduled != NULL;
  self_suspends = exists && !is_current_running();
  out_of_time   = exists && budget_enforced(prev)
    && budget_exhausted(prev);
  job_completed = exists && is_completed(prev);

  /* preempt is true if task `prev` has lower priority than something on
   * the ready queue. */
  preempt = edf_preemption_needed(&local_state->local_queues, prev);

  /* check all conditions that make us reschedule */
  resched = preempt;

  /* if `prev` suspends, it CANNOT be scheduled anymore => reschedule */
  if (self_suspends)
    resched = 1;

  /* also check for (in-)voluntary job completions */
  if (out_of_time || job_completed) {
    pedf_job_completion(prev, out_of_time);
    resched = 1;
  }

  if (resched) {
    /* First check if the previous task goes back onto the ready
     * queue, which it does if it did not self_suspend.
     */
    if (exists && !self_suspends)
      pedf_requeue(prev, local_state);
    next = __take_ready(&local_state->local_queues);
  } else
    /* No preemption is required. */
    next = local_state->scheduled;

  local_state->scheduled = next;


  if (exists && prev != next)
    TRACE_TASK(prev, "descheduled.\n");
  if (next)
    TRACE_TASK(next, "scheduled.\n");

  /* This mandatory. It triggers a transition in the LITMUS^RT remote
   * preemption state machine. Call this AFTER the plugin has made a local
   * scheduling decision.
   */
  sched_state_task_picked();

  raw_spin_unlock(&local_state->local_queues.ready_lock);

  return next;
}


static int pedf_check_for_preemption_on_release(rt_domain_t *local_queues)
{
  struct pedf_cpu_state *state = container_of(local_queues, struct pedf_cpu_state,
					      local_queues);

  /* Because this is a callback from rt_domain_t we already hold
   * the necessary lock for the ready queue.
   */

  if (edf_preemption_needed(local_queues, state->scheduled)) {
    preempt_if_preemptable(state->scheduled, state->cpu);
    return 1;
  } else
    return 0;
}

static void pedf_task_new(struct task_struct *tsk, int on_runqueue,
                          int is_running)
{
  unsigned long flags; /* needed to store the IRQ flags */
  struct pedf_cpu_state *state = cpu_state_for(get_partition(tsk));
  lt_t now;

  TRACE_TASK(tsk, "is a new RT task %llu (on_rq:%d, running:%d)\n",
	     litmus_clock(), on_runqueue, is_running);

  /* acquire the lock protecting the state and disable interrupts */
  raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);

  now = litmus_clock();

  /* the first job exists starting as of right now */
  release_at(tsk, now);

  if (is_running) {
    /* if tsk is running, then no other task can be running
     * on the local CPU */
    BUG_ON(state->scheduled != NULL);
    state->scheduled = tsk;
  } else if (on_runqueue) {
    pedf_requeue(tsk, state);
  }

  if (edf_preemption_needed(&state->local_queues, state->scheduled))
    preempt_if_preemptable(state->scheduled, state->cpu);

  raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}


static void pedf_task_exit(struct task_struct *tsk)
{
  unsigned long flags; /* needed to store the IRQ flags */
  struct pedf_cpu_state *state = cpu_state_for(get_partition(tsk));

  /* acquire the lock protecting the state and disable interrupts */
  raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);

  if (state->scheduled == tsk)
    state->scheduled = NULL;

  /* For simplicity, we assume here that the task is no longer queued anywhere else. This
   * is the case when tasks exit by themselves; additional queue management is
   * is required if tasks are forced out of real-time mode by other tasks. */

  raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}

/* Called when the state of tsk changes back to TASK_RUNNING.
 * We need to requeue the task.
 *
 * NOTE: if a sporadic task suspended for a long time,
 * this might actually be an event-driven release of a new job.
 *
 */
static void pedf_task_resume(struct task_struct  *tsk)
{
  unsigned long flags; /* needed to store the IRQ flags */
  struct pedf_cpu_state *state = cpu_state_for(get_partition(tsk));
  lt_t now;

  TRACE_TASK(tsk, "wake_up at %llu\n", litmus_clock());

  /* acquire the lock protecting the state and disable interrupts */
  raw_spin_lock_irqsave(&state->local_queues.ready_lock, flags);

  now = litmus_clock();

  if (is_sporadic(tsk) && is_tardy(tsk, now)) {
    /* This sporadic task was gone for a "long" time and woke up past
     * its deadline. Give it a new budget by triggering a job
     * release. */
    release_at(tsk, now);
  }

  /* This check is required to avoid races with tasks that resume before
   * the scheduler "noticed" that it resumed. That is, the wake up may
   * race with the call to schedule(). */
  if (state->scheduled != tsk) {
    pedf_requeue(tsk, state);
    if (edf_preemption_needed(&state->local_queues, state->scheduled))
      preempt_if_preemptable(state->scheduled, state->cpu);
  }

  raw_spin_unlock_irqrestore(&state->local_queues.ready_lock, flags);
}


static long pedf_admit_task(struct task_struct *tsk)
{
  if (task_cpu(tsk) == get_partition(tsk)) {
    TRACE_TASK(tsk, "accepted by p-edf plugin.\n");
    return 0;
  } else
    return -EINVAL;
}

static void pedf_setup_domain_proc(void)
{
  int i, cpu;
  int num_rt_cpus = num_online_cpus();

  struct cd_mapping *cpu_map, *domain_map;

  memset(&pedf_domain_proc_info, 0, sizeof(pedf_domain_proc_info));
  init_domain_proc_info(&pedf_domain_proc_info, num_rt_cpus, num_rt_cpus);
  pedf_domain_proc_info.num_cpus = num_rt_cpus;
  pedf_domain_proc_info.num_domains = num_rt_cpus;

  i = 0;
  for_each_online_cpu(cpu) {
    cpu_map = &pedf_domain_proc_info.cpu_to_domains[i];
    domain_map = &pedf_domain_proc_info.domain_to_cpus[i];

    cpu_map->id = cpu;
    domain_map->id = i;
    cpumask_set_cpu(i, cpu_map->mask);
    cpumask_set_cpu(cpu, domain_map->mask);
    ++i;
  }
}

static long pedf_activate_plugin(void)
{
  int cpu;
  struct pedf_cpu_state *state;

  for_each_online_cpu(cpu) {
    TRACE("Initializing CPU%d...\n", cpu);

    state = cpu_state_for(cpu);

    state->cpu = cpu;
    state->scheduled = NULL;
    edf_domain_init(&state->local_queues, 
		    pedf_check_for_preemption_on_release, 
		    NULL);
  }

  pedf_setup_domain_proc();

  return 0;
}

static long pedf_deactivate_plugin(void)
{
  destroy_domain_proc_info(&pedf_domain_proc_info);
  return 0;
}

static struct sched_plugin pedf_plugin = {
  .plugin_name            = "P-EDF",
  .schedule               = pedf_schedule,
  .task_wake_up           = pedf_task_resume,
  .admit_task             = pedf_admit_task,
  .task_new               = pedf_task_new,
  .task_exit              = pedf_task_exit,
  .get_domain_proc_info   = pedf_get_domain_proc_info,
  .activate_plugin        = pedf_activate_plugin,
  .deactivate_plugin      = pedf_deactivate_plugin,
};

static int __init init_pedf(void)
{
  return register_sched_plugin(&pedf_plugin);
}

module_init(init_pedf);
