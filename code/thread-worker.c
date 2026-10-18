// File:	thread-worker.c

// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"

#include <errno.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>

// Global counter for total context switches and
// average turn around and response time
long tot_cntx_switches = 0;
double avg_turn_time = 0;
double avg_resp_time = 0;

// INITAILIZE ALL YOUR OTHER VARIABLES HERE
static int library_initialized = 0;
static worker_t next_worker_id = 1;
static tcb *current_worker = NULL;
static tcb main_thread;
static tcb *all_threads = NULL;
#ifdef MLFQ
static worker_queue ready_queues[MLFQ_LEVELS];
static uint64_t next_boost_ns;
#else
static worker_queue ready_queue;
#endif
static ucontext_t scheduler_context;
static void *scheduler_stack = NULL;
static uint64_t dispatch_cpu_ns = 0;
static uint64_t total_response_ns = 0;
static uint64_t total_turnaround_ns = 0;
static unsigned long completed_workers = 0;

static void schedule(void);
#ifdef MLFQ
static tcb *sched_mlfq(void);
#else
static tcb *sched_psjf(void);
#endif

static uint64_t time_ns(clockid_t clock_id)
{
	struct timespec now;
	if (clock_gettime(clock_id, &now) == -1)
	{
		_exit(1);
	}
	return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

static int block_timer(sigset_t *previous_mask)
{
	sigset_t timer_set;
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	return sigprocmask(SIG_BLOCK, &timer_set, previous_mask);
}

static void restore_signal_mask(const sigset_t *previous_mask)
{
	int saved_errno = errno;
	sigprocmask(SIG_SETMASK, previous_mask, NULL);
	errno = saved_errno;
}

static void queue_init(worker_queue *queue)
{
	queue->head = NULL;
	queue->tail = NULL;
	queue->size = 0;
}

static void queue_push(worker_queue *queue, tcb *thread)
{
	thread->next = NULL;
	if (queue->tail == NULL)
	{
		queue->head = thread;
	}
	else
	{
		queue->tail->next = thread;
	}
	queue->tail = thread;
	queue->size++;
}

static tcb *queue_pop(worker_queue *queue)
{
	tcb *thread = queue->head;
	if (thread == NULL)
	{
		return NULL;
	}

	queue->head = thread->next;
	if (queue->head == NULL)
	{
		queue->tail = NULL;
	}
	thread->next = NULL;
	queue->size--;
	return thread;
}

static void ready_push(tcb *thread)
{
#ifdef MLFQ
	queue_push(&ready_queues[thread->priority_level], thread);
#else
	queue_push(&ready_queue, thread);
#endif
}

static int ready_available(void)
{
#ifdef MLFQ
	for (int level = 0; level < MLFQ_LEVELS; level++)
	{
		if (ready_queues[level].head != NULL)
			return 1;
	}
	return 0;
#else
	return ready_queue.head != NULL;
#endif
}

#ifdef MLFQ
static uint64_t level_allotment_ns(int level)
{
	return (uint64_t)QUANTUM * 1000 * (1ULL << level);
}

static int higher_priority_ready(void)
{
	for (int level = 0; level < current_worker->priority_level; level++)
	{
		if (ready_queues[level].head != NULL)
			return 1;
	}
	return 0;
}

/* Called with SIGPROF blocked after making a worker ready. */
static void preempt_for_priority(void)
{
	if (higher_priority_ready())
	{
		current_worker->state = WORKER_READY;
		ready_push(current_worker);
		if (swapcontext(&current_worker->context, &scheduler_context) == -1)
			_exit(1);
	}
}

static void ready_remove(tcb *thread, int level)
{
	worker_queue *queue = &ready_queues[level];
	tcb *previous = NULL;
	tcb *item = queue->head;
	while (item != NULL && item != thread)
	{
		previous = item;
		item = item->next;
	}
	if (item == NULL)
		return;
	if (previous == NULL)
		queue->head = item->next;
	else
		previous->next = item->next;
	if (queue->tail == item)
		queue->tail = previous;
	item->next = NULL;
	queue->size--;
}

static void boost_priorities(void)
{
	for (tcb *thread = all_threads; thread != NULL; thread = thread->all_next)
	{
		thread->priority_level = 0;
		thread->level_cpu_ns = 0;
	}
	main_thread.priority_level = 0;
	main_thread.level_cpu_ns = 0;
	for (int level = 1; level < MLFQ_LEVELS; level++)
	{
		tcb *thread;
		while ((thread = queue_pop(&ready_queues[level])) != NULL)
			queue_push(&ready_queues[0], thread);
	}
}
#endif

/* SIGPROF is blocked while the scheduler and queue helpers run. */
static void timer_handler(int signum)
{
	int saved_errno = errno;
	(void)signum;

	if (library_initialized && current_worker != NULL &&
	    current_worker->state == WORKER_RUNNING &&
#ifdef MLFQ
	    (higher_priority_ready() || (ready_available() && current_worker->level_cpu_ns +
	      time_ns(CLOCK_PROCESS_CPUTIME_ID) - dispatch_cpu_ns >=
	      level_allotment_ns(current_worker->priority_level)) ||
	     time_ns(CLOCK_MONOTONIC) >= next_boost_ns))
#else
	    ready_available())
#endif
	{
		current_worker->state = WORKER_READY;
		ready_push(current_worker);
		if (swapcontext(&current_worker->context, &scheduler_context) == -1)
		{
			_exit(1);
		}
	}
	errno = saved_errno;
}

static void add_thread(tcb *thread)
{
	thread->all_next = all_threads;
	all_threads = thread;
}

static tcb *find_thread(worker_t id)
{
	tcb *thread = all_threads;
	while (thread != NULL)
	{
		if (thread->id == id)
		{
			return thread;
		}
		thread = thread->all_next;
	}
	return NULL;
}

static void remove_thread(tcb *target)
{
	tcb **thread = &all_threads;
	while (*thread != NULL && *thread != target)
	{
		thread = &(*thread)->all_next;
	}
	if (*thread != NULL)
	{
		*thread = target->all_next;
	}
}

/* Set up the scheduler's context; it does not schedule workers yet. */
static int initialize_library(void)
{
	struct sigaction action = {0};
	struct sigaction previous_action;
	struct itimerval timer = {0};

	if (library_initialized)
	{
		return 0;
	}

#ifdef MLFQ
	for (int level = 0; level < MLFQ_LEVELS; level++)
		queue_init(&ready_queues[level]);
	next_boost_ns = time_ns(CLOCK_MONOTONIC) + (uint64_t)MLFQ_BOOST_INTERVAL * 1000;
#else
	queue_init(&ready_queue);
#endif
	main_thread.id = 0;
	main_thread.state = WORKER_RUNNING;
	current_worker = &main_thread;
	scheduler_stack = malloc(SCHEDULER_STACK_SIZE);
	if (scheduler_stack == NULL)
	{
		errno = ENOMEM;
		return -1;
	}
	if (getcontext(&scheduler_context) == -1)
	{
		free(scheduler_stack);
		scheduler_stack = NULL;
		return -1;
	}

	scheduler_context.uc_stack.ss_sp = scheduler_stack;
	scheduler_context.uc_stack.ss_size = SCHEDULER_STACK_SIZE;
	scheduler_context.uc_stack.ss_flags = 0;
	scheduler_context.uc_link = NULL;
	makecontext(&scheduler_context, (void (*)(void))schedule, 0);

	action.sa_handler = timer_handler;
	sigemptyset(&action.sa_mask);
	action.sa_flags = SA_RESTART;
	if (sigaction(SIGPROF, &action, &previous_action) == -1)
	{
		free(scheduler_stack);
		scheduler_stack = NULL;
		return -1;
	}

	timer.it_interval.tv_sec = QUANTUM / 1000000;
	timer.it_interval.tv_usec = QUANTUM % 1000000;
	timer.it_value = timer.it_interval;
	if (setitimer(ITIMER_PROF, &timer, NULL) == -1)
	{
		int saved_errno = errno;
		sigaction(SIGPROF, &previous_action, NULL);
		free(scheduler_stack);
		scheduler_stack = NULL;
		errno = saved_errno;
		return -1;
	}
	library_initialized = 1;
	dispatch_cpu_ns = time_ns(CLOCK_PROCESS_CPUTIME_ID);
	return 0;
}

/* This will run on a worker's private stack once dispatch is implemented. */
static void worker_start(void)
{
	/* Enable the worker's original signal mask only after the context switch. */
	restore_signal_mask(&current_worker->start_mask);
	current_worker->return_value =
		current_worker->start_routine(current_worker->arg);
	worker_exit(current_worker->return_value);
}

/* create a new thread */
int worker_create(worker_t *thread, pthread_attr_t *attr,
				  void *(*function)(void *), void *arg)
{
	sigset_t previous_mask;

	// - create Thread Control Block (TCB)
	// - create and initialize the context of this worker thread
	// - allocate space of stack for this thread to run
	// after everything is set, push this thread into run queue and
	// - make it ready for the execution.

	(void)attr;
	if (thread == NULL || function == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (initialize_library() == -1)
	{
		restore_signal_mask(&previous_mask);
		return -1;
	}

	tcb *new_thread = calloc(1, sizeof(*new_thread));
	if (new_thread == NULL)
	{
		errno = ENOMEM;
		restore_signal_mask(&previous_mask);
		return -1;
	}
	new_thread->stack = malloc(WORKER_STACK_SIZE);
	if (new_thread->stack == NULL)
	{
		free(new_thread);
		errno = ENOMEM;
		restore_signal_mask(&previous_mask);
		return -1;
	}
	if (getcontext(&new_thread->context) == -1)
	{
		free(new_thread->stack);
		free(new_thread);
		restore_signal_mask(&previous_mask);
		return -1;
	}

	new_thread->id = next_worker_id++;
	new_thread->state = WORKER_READY;
	new_thread->start_routine = function;
	new_thread->arg = arg;
	new_thread->context.uc_stack.ss_sp = new_thread->stack;
	new_thread->context.uc_stack.ss_size = WORKER_STACK_SIZE;
	new_thread->context.uc_stack.ss_flags = 0;
	new_thread->context.uc_link = &scheduler_context;
	new_thread->start_mask = previous_mask;
	new_thread->context.uc_sigmask = previous_mask;
	sigaddset(&new_thread->context.uc_sigmask, SIGPROF);
	makecontext(&new_thread->context, (void (*)(void))worker_start, 0);

	new_thread->created_ns = time_ns(CLOCK_MONOTONIC);
	ready_push(new_thread);
	add_thread(new_thread);
	*thread = new_thread->id;
#ifdef MLFQ
	preempt_for_priority();
#endif
	restore_signal_mask(&previous_mask);
	return 0;
};

/* give CPU possession to other user-level worker threads voluntarily */
int worker_yield()
{
	sigset_t previous_mask;

	// - change worker thread's state from Running to Ready
	// - save context of this thread to its thread control block
	// - switch from thread context to scheduler context

	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (!library_initialized && initialize_library() == -1)
	{
		restore_signal_mask(&previous_mask);
		return -1;
	}

	current_worker->state = WORKER_READY;
	ready_push(current_worker);
	if (swapcontext(&current_worker->context, &scheduler_context) == -1)
	{
		restore_signal_mask(&previous_mask);
		return -1;
	}

	restore_signal_mask(&previous_mask);
	return 0;
};

/* terminate a thread */
void worker_exit(void *value_ptr)
{
	sigset_t previous_mask;
	// - de-allocate any dynamic memory created when starting this thread

	if (block_timer(&previous_mask) == -1)
	{
		_exit(1);
	}
	current_worker->return_value = value_ptr;
	current_worker->state = WORKER_ZOMBIE;
	if (current_worker != &main_thread)
	{
		total_response_ns += current_worker->response_ns;
		total_turnaround_ns += time_ns(CLOCK_MONOTONIC) - current_worker->created_ns;
		completed_workers++;
		avg_resp_time = (double)total_response_ns / completed_workers / 1000.0;
		avg_turn_time = (double)total_turnaround_ns / completed_workers / 1000.0;
	}

	if (current_worker->joiner != NULL)
	{
		current_worker->joiner->state = WORKER_READY;
		ready_push(current_worker->joiner);
	}

	setcontext(&scheduler_context);
	_exit(1);
};

/* Wait for thread termination */
int worker_join(worker_t thread, void **value_ptr)
{
	sigset_t previous_mask;

	// - wait for a specific thread to terminate
	// - de-allocate any dynamic memory created by the joining thread

	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (!library_initialized && initialize_library() == -1)
	{
		restore_signal_mask(&previous_mask);
		return -1;
	}
	if (thread == 0 || thread == current_worker->id)
	{
		errno = EDEADLK;
		restore_signal_mask(&previous_mask);
		return -1;
	}

	tcb *target = find_thread(thread);
	if (target == NULL)
	{
		errno = ESRCH;
		restore_signal_mask(&previous_mask);
		return -1;
	}
	if (target->joiner != NULL)
	{
		errno = EINVAL;
		restore_signal_mask(&previous_mask);
		return -1;
	}

	if (target->state != WORKER_ZOMBIE)
	{
		target->joiner = current_worker;
		current_worker->state = WORKER_BLOCKED;
		if (swapcontext(&current_worker->context, &scheduler_context) == -1)
		{
			restore_signal_mask(&previous_mask);
			return -1;
		}
	}

	if (value_ptr != NULL)
	{
		*value_ptr = target->return_value;
	}
	remove_thread(target);
	free(target->stack);
	free(target);
	restore_signal_mask(&previous_mask);
	return 0;
};

/* initialize the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex,
					  const pthread_mutexattr_t *mutexattr)
{
	sigset_t previous_mask;
	//- initialize data structures for this mutex

	(void)mutexattr;
	if (mutex == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}

	mutex->locked = 0;
	mutex->owner = NULL;
	queue_init(&mutex->wait_queue);
	restore_signal_mask(&previous_mask);
	return 0;
};

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex)
{
	sigset_t previous_mask;

	// - use the built-in test-and-set atomic function to test the mutex
	// - if the mutex is acquired successfully, enter the critical section
	// - if acquiring mutex fails, push current thread into block list and
	// context switch to the scheduler thread

	if (mutex == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (!library_initialized && initialize_library() == -1)
	{
		restore_signal_mask(&previous_mask);
		return -1;
	}

	while (__sync_lock_test_and_set(&mutex->locked, 1))
	{
		current_worker->state = WORKER_BLOCKED;
		queue_push(&mutex->wait_queue, current_worker);
		if (swapcontext(&current_worker->context, &scheduler_context) == -1)
		{
			restore_signal_mask(&previous_mask);
			return -1;
		}
	}

	mutex->owner = current_worker;
	restore_signal_mask(&previous_mask);
	return 0;
};

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex)
{
	sigset_t previous_mask;
	// - release mutex and make it available again.
	// - put threads in block list to run queue
	// so that they could compete for mutex later.

	if (mutex == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (mutex->owner == NULL || mutex->owner != current_worker)
	{
		errno = EPERM;
		restore_signal_mask(&previous_mask);
		return -1;
	}

	mutex->owner = NULL;
	__sync_lock_release(&mutex->locked);

	tcb *waiter = queue_pop(&mutex->wait_queue);
	if (waiter != NULL)
	{
		waiter->state = WORKER_READY;
		ready_push(waiter);
	}

#ifdef MLFQ
	preempt_for_priority();
#endif

	restore_signal_mask(&previous_mask);
	return 0;
};

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex)
{
	sigset_t previous_mask;
	// - de-allocate dynamic memory created in worker_mutex_init

	if (mutex == NULL)
	{
		errno = EINVAL;
		return -1;
	}
	if (block_timer(&previous_mask) == -1)
	{
		return -1;
	}
	if (mutex->locked || mutex->wait_queue.size != 0)
	{
		errno = EBUSY;
		restore_signal_mask(&previous_mask);
		return -1;
	}

	restore_signal_mask(&previous_mask);
	return 0;
};

/* scheduler */
static void schedule(void)
{
	// - every time a timer interrupt occurs, your worker thread library
	// should be contexted switched from a thread context to this
	// schedule() function

	// - invoke scheduling algorithms according to the policy (PSJF or MLFQ)

	// if (sched == PSJF)
	//		sched_psjf();
	// else if (sched == MLFQ)
	// 		sched_mlfq();

	while (1)
	{
		uint64_t now_cpu_ns = time_ns(CLOCK_PROCESS_CPUTIME_ID);
		uint64_t used_ns = now_cpu_ns - dispatch_cpu_ns;
		current_worker->cpu_time_ns += used_ns;
		current_worker->elapsed_quanta = current_worker->cpu_time_ns / ((uint64_t)QUANTUM * 1000);
#ifndef MLFQ
		tcb *next_thread = sched_psjf();
#else
		int old_level = current_worker->priority_level;
		current_worker->level_cpu_ns += used_ns;
		while (current_worker->level_cpu_ns >= level_allotment_ns(current_worker->priority_level))
		{
			current_worker->level_cpu_ns -= level_allotment_ns(current_worker->priority_level);
			if (current_worker->priority_level < MLFQ_LEVELS - 1)
				current_worker->priority_level++;
		}
		if (current_worker->state == WORKER_READY && old_level != current_worker->priority_level)
		{
			ready_remove(current_worker, old_level);
			ready_push(current_worker);
		}
		uint64_t now_wall_ns = time_ns(CLOCK_MONOTONIC);
		if (now_wall_ns >= next_boost_ns)
		{
			boost_priorities();
			next_boost_ns = now_wall_ns + (uint64_t)MLFQ_BOOST_INTERVAL * 1000;
		}
		tcb *next_thread = sched_mlfq();
#endif
		if (next_thread == NULL)
		{
			fprintf(stderr, "worker scheduler: no runnable workers\n");
			exit(EXIT_FAILURE);
		}

		current_worker = next_thread;
		current_worker->state = WORKER_RUNNING;
		if (current_worker != &main_thread && !current_worker->has_started)
		{
			current_worker->response_ns = time_ns(CLOCK_MONOTONIC) - current_worker->created_ns;
			current_worker->has_started = 1;
		}
		current_worker->context_switches++;
		tot_cntx_switches++;
		dispatch_cpu_ns = time_ns(CLOCK_PROCESS_CPUTIME_ID);
		if (swapcontext(&scheduler_context, &current_worker->context) == -1)
		{
			exit(EXIT_FAILURE);
		}
	}

// - schedule policy
#ifndef MLFQ
	// Choose PSJF
#else
	// Choose MLFQ
#endif
}

/* Pre-emptive Shortest Job First (POLICY_PSJF) scheduling algorithm */
#ifndef MLFQ
static tcb *sched_psjf(void)
{
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	tcb *thread = ready_queue.head;
	tcb *previous = NULL;
	tcb *best = thread;
	tcb *best_previous = NULL;

	while (thread != NULL)
	{
		if (thread->elapsed_quanta < best->elapsed_quanta)
		{
			best = thread;
			best_previous = previous;
		}
		previous = thread;
		thread = thread->next;
	}
	if (best == NULL)
	{
		return NULL;
	}
	if (best_previous == NULL)
	{
		return queue_pop(&ready_queue);
	}
	best_previous->next = best->next;
	if (ready_queue.tail == best)
	{
		ready_queue.tail = best_previous;
	}
	best->next = NULL;
	ready_queue.size--;
	return best;
}
#endif

/* Preemptive MLFQ scheduling algorithm */
#ifdef MLFQ
static tcb *sched_mlfq(void)
{
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	for (int level = 0; level < MLFQ_LEVELS; level++)
	{
		if (ready_queues[level].head != NULL)
			return queue_pop(&ready_queues[level]);
	}
	return NULL;
}
#endif

// DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void)
{

	fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
	fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
	fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}

// Feel free to add any other functions you need
