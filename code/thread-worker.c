// File:	thread-worker.c

// List all group member's name:
// username of iLab:
// iLab Server:

#include "thread-worker.h"

#include <errno.h>
#include <signal.h>
#include <sys/time.h>

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
static worker_queue ready_queue;
static ucontext_t scheduler_context;
static void *scheduler_stack = NULL;

static void schedule(void);

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

/* SIGPROF is blocked while the scheduler and queue helpers run. */
static void timer_handler(int signum)
{
	int saved_errno = errno;
	(void)signum;

	if (library_initialized && current_worker != NULL &&
	    current_worker->state == WORKER_RUNNING && ready_queue.head != NULL)
	{
		current_worker->state = WORKER_READY;
		queue_push(&ready_queue, current_worker);
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

	queue_init(&ready_queue);
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

	queue_push(&ready_queue, new_thread);
	add_thread(new_thread);
	*thread = new_thread->id;
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
	queue_push(&ready_queue, current_worker);
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

	if (current_worker->joiner != NULL)
	{
		current_worker->joiner->state = WORKER_READY;
		queue_push(&ready_queue, current_worker->joiner);
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
		queue_push(&ready_queue, waiter);
	}

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
		tcb *next_thread = queue_pop(&ready_queue);
		if (next_thread == NULL)
		{
			fprintf(stderr, "worker scheduler: no runnable workers\n");
			exit(EXIT_FAILURE);
		}

		current_worker = next_thread;
		current_worker->state = WORKER_RUNNING;
		tot_cntx_switches++;
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
static void sched_psjf()
{
	// - your own implementation of PSJF
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE
}

/* Preemptive MLFQ scheduling algorithm */
static void sched_mlfq()
{
	// - your own implementation of MLFQ
	// (feel free to modify arguments and return types)

	// YOUR CODE HERE
}

// DO NOT MODIFY THIS FUNCTION
/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void)
{

	fprintf(stderr, "Total context switches %ld \n", tot_cntx_switches);
	fprintf(stderr, "Average turnaround time %lf \n", avg_turn_time);
	fprintf(stderr, "Average response time  %lf \n", avg_resp_time);
}

// Feel free to add any other functions you need
