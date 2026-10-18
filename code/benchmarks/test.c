#include <stdio.h>
#include <assert.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include "../thread-worker.h"

/* A scratch program template on which to call and
 * test thread-worker library functions as you implement
 * them.
 *
 * You can modify and use this program as much as possible.
 * This will not be graded.
 */
extern long tot_cntx_switches;
extern double avg_turn_time;
extern double avg_resp_time;

/* ---------- Test: scheduling statistics ---------- */

static void *stats_worker(void *arg)
{
	struct timespec delay = {0, 30000000};
	(void)arg;
	nanosleep(&delay, NULL);
	return NULL;
}

static void test_scheduling_stats(void)
{
	pthread_t worker;
	assert(pthread_create(&worker, NULL, stats_worker, NULL) == 0);
	assert(pthread_join(worker, NULL) == 0);
	assert(tot_cntx_switches > 0);
	assert(avg_resp_time >= 0 && avg_turn_time >= 20000);
	assert(avg_turn_time >= avg_resp_time);
	puts("scheduling statistics test passed.");
}

/* ---------- Test: worker_create ---------- */

static int workers_ran = 0;

static void *record_run(void *arg)
{
	workers_ran++;
	return arg;
}

static void test_worker_create(void)
{
	pthread_t first;
	pthread_t second;
	int first_value = 1;
	int second_value = 2;

	/* creates two workers. */
	assert(pthread_create(&first, NULL, record_run, &first_value) == 0);
	assert(pthread_create(&second, NULL, record_run, &second_value) == 0);

	/* IDs should be unique. */
	assert(first != second);

	/* Creation does not run workers. */
	assert(workers_ran == 0);

	/* Join both workers to run and clean them up. */
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(workers_ran == 2);
	puts("worker_create test passed.");
}

/* ---------- Test: worker_yield ---------- */

static char run_order[4];
static int run_order_length = 0;

static void record(char label)
{
	run_order[run_order_length++] = label;
}

static void *worker_a(void *arg)
{
	(void)arg;
	record('A');
	assert(worker_yield() == 0);
	record('A');
	return NULL;
}

static void *worker_b(void *arg)
{
	(void)arg;
	record('B');
	assert(worker_yield() == 0);
	record('B');
	return NULL;
}

static void test_worker_yield(void)
{
	pthread_t first;
	pthread_t second;

	assert(pthread_create(&first, NULL, worker_a, NULL) == 0);
	assert(pthread_create(&second, NULL, worker_b, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(run_order_length == 4);
	assert(run_order[0] == 'A' && run_order[1] == 'B');
	assert(run_order[2] == 'A' && run_order[3] == 'B');
	puts("worker_yield test passed.");
}

/* ---------- Test: worker_exit and worker_join ---------- */

static void *return_value(void *arg)
{
	return arg;
}

static void test_worker_join(void)
{
	pthread_t worker;
	void *result = NULL;
	int value = 42;

	assert(pthread_create(&worker, NULL, return_value, &value) == 0);
	assert(pthread_join(worker, &result) == 0);
	assert(result == &value);
	puts("worker_join test passed.");
}

/* ---------- Test: worker_mutex API ---------- */

static void test_worker_mutex_api(void)
{
	pthread_mutex_t mutex;

	assert(pthread_mutex_init(&mutex, NULL) == 0);
	assert(pthread_mutex_lock(&mutex) == 0);
	assert(pthread_mutex_unlock(&mutex) == 0);
	assert(pthread_mutex_destroy(&mutex) == 0);
	puts("worker_mutex API test passed.");
}

/* ---------- Test: worker_mutex blocking ---------- */

static pthread_mutex_t blocking_mutex;
static char mutex_order[4];
static int mutex_order_length = 0;

static void record_mutex(char label)
{
	mutex_order[mutex_order_length++] = label;
}

static void *mutex_holder(void *arg)
{
	(void)arg;
	assert(pthread_mutex_lock(&blocking_mutex) == 0);
	record_mutex('A');
	assert(worker_yield() == 0);
	record_mutex('A');
	assert(pthread_mutex_unlock(&blocking_mutex) == 0);
	return NULL;
}

static void *mutex_waiter(void *arg)
{
	(void)arg;
	record_mutex('B');
	assert(pthread_mutex_lock(&blocking_mutex) == 0);
	record_mutex('B');
	assert(pthread_mutex_unlock(&blocking_mutex) == 0);
	return NULL;
}

static void test_worker_mutex_blocking(void)
{
	pthread_t holder;
	pthread_t waiter;

	assert(pthread_mutex_init(&blocking_mutex, NULL) == 0);
	assert(pthread_create(&holder, NULL, mutex_holder, NULL) == 0);
	assert(pthread_create(&waiter, NULL, mutex_waiter, NULL) == 0);
	assert(pthread_join(holder, NULL) == 0);
	assert(pthread_join(waiter, NULL) == 0);
	assert(mutex_order_length == 4);
	assert(mutex_order[0] == 'A' && mutex_order[1] == 'B');
	assert(mutex_order[2] == 'A' && mutex_order[3] == 'B');
	assert(pthread_mutex_destroy(&blocking_mutex) == 0);
	puts("worker_mutex blocking test passed.");
}

/* ---------- Test: protected shared counter ---------- */

static pthread_mutex_t counter_mutex;
static int shared_counter = 0;

static void *increment_counter(void *arg)
{
	int i;
	(void)arg;

	for (i = 0; i < 3; i++) {
		int value;
		assert(pthread_mutex_lock(&counter_mutex) == 0);
		value = shared_counter;
		assert(worker_yield() == 0);
		shared_counter = value + 1;
		assert(pthread_mutex_unlock(&counter_mutex) == 0);
	}
	return NULL;
}

static void test_shared_counter(void)
{
	pthread_t first;
	pthread_t second;

	shared_counter = 0;
	assert(pthread_mutex_init(&counter_mutex, NULL) == 0);
	assert(pthread_create(&first, NULL, increment_counter, NULL) == 0);
	assert(pthread_create(&second, NULL, increment_counter, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(shared_counter == 6);
	assert(pthread_mutex_destroy(&counter_mutex) == 0);
	puts("worker_mutex shared-counter test passed.");
}

/* ---------- Test: timer preemption ---------- */

static volatile sig_atomic_t second_started = 0;
static volatile sig_atomic_t first_observed_second = 0;
static volatile sig_atomic_t second_observed_first = 0;

static void *cpu_worker_a(void *arg)
{
	clock_t start = clock();
	(void)arg;

	while (!second_started && clock() - start < 2 * CLOCKS_PER_SEC)
	{
		/* Wait without yielding. */
	}
	first_observed_second = second_started;
	return NULL;
}

static void *cpu_worker_b(void *arg)
{
	clock_t start = clock();
	(void)arg;

	second_started = 1;
	while (!first_observed_second && clock() - start < 2 * CLOCKS_PER_SEC)
	{
		/* Wait without yielding. */
	}
	second_observed_first = first_observed_second;
	return NULL;
}

static void test_timer_preemption(void)
{
	pthread_t first;
	pthread_t second;

	assert(pthread_create(&first, NULL, cpu_worker_a, NULL) == 0);
	assert(pthread_create(&second, NULL, cpu_worker_b, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(second, NULL) == 0);
	assert(first_observed_second && second_observed_first);
	puts("timer preemption test passed.");
}

/* ---------- Test: preemption while holding a mutex ---------- */

static pthread_mutex_t preemption_mutex;
static volatile sig_atomic_t preemption_waiter_started = 0;
static volatile sig_atomic_t holder_observed_waiter = 0;

static void *preempted_holder(void *arg)
{
	clock_t start;
	(void)arg;

	assert(pthread_mutex_lock(&preemption_mutex) == 0);
	start = clock();
	while (!preemption_waiter_started && clock() - start < 2 * CLOCKS_PER_SEC)
	{
		/* Wait without yielding while holding the mutex. */
	}
	holder_observed_waiter = preemption_waiter_started;
	assert(pthread_mutex_unlock(&preemption_mutex) == 0);
	return NULL;
}

static void *preemption_waiter(void *arg)
{
	(void)arg;
	preemption_waiter_started = 1;
	assert(pthread_mutex_lock(&preemption_mutex) == 0);
	assert(pthread_mutex_unlock(&preemption_mutex) == 0);
	return NULL;
}

static void test_preemptive_mutex(void)
{
	pthread_t holder;
	pthread_t waiter;

	assert(pthread_mutex_init(&preemption_mutex, NULL) == 0);
	assert(pthread_create(&holder, NULL, preempted_holder, NULL) == 0);
	assert(pthread_create(&waiter, NULL, preemption_waiter, NULL) == 0);
	assert(pthread_join(holder, NULL) == 0);
	assert(pthread_join(waiter, NULL) == 0);
	assert(holder_observed_waiter);
	assert(pthread_mutex_destroy(&preemption_mutex) == 0);
	puts("preemptive mutex test passed.");
}

/* ---------- Test: PSJF favors workers with fewer quanta ---------- */

#ifndef MLFQ
static volatile sig_atomic_t psjf_done = 0;
static char psjf_order[6];
static int psjf_order_length = 0;
static pthread_t psjf_first_short, psjf_second_short;
static char psjf_b = 'B', psjf_c = 'C';

static void *psjf_short_worker(void *arg);

static void *psjf_long_worker(void *arg)
{
	clock_t start = clock();
	(void)arg;
	psjf_order[psjf_order_length++] = 'A';
	while (clock() - start < 2 * (clock_t)QUANTUM * CLOCKS_PER_SEC / 1000000)
	{
		/* Accumulate CPU time before adding shorter workers. */
	}
	assert(pthread_create(&psjf_first_short, NULL, psjf_short_worker, &psjf_b) == 0);
	assert(pthread_create(&psjf_second_short, NULL, psjf_short_worker, &psjf_c) == 0);
	assert(worker_yield() == 0);
	psjf_order[psjf_order_length++] = 'a';
	return NULL;
}

static void *psjf_short_worker(void *arg)
{
	char label = *(char *)arg;
	psjf_order[psjf_order_length++] = label;
	assert(worker_yield() == 0);
	psjf_order[psjf_order_length++] = label + ('a' - 'A');
	psjf_done++;
	return NULL;
}

static void test_psjf_order(void)
{
	pthread_t long_worker;
	sigset_t timer_set, previous_mask;

	/* Keep the selection order independent of timer delivery. */
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	assert(sigprocmask(SIG_BLOCK, &timer_set, &previous_mask) == 0);
	assert(pthread_create(&long_worker, NULL, psjf_long_worker, NULL) == 0);
	assert(pthread_join(long_worker, NULL) == 0);
	assert(pthread_join(psjf_first_short, NULL) == 0);
	assert(pthread_join(psjf_second_short, NULL) == 0);
	assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
	assert(psjf_done == 2);
	assert(psjf_order_length == 6);
	assert(psjf_order[0] == 'A' && psjf_order[1] == 'B');
	assert(psjf_order[2] == 'C' && psjf_order[3] == 'b');
	assert(psjf_order[4] == 'c' && psjf_order[5] == 'a');
	puts("PSJF scheduling order test passed.");
}
#else
/* ---------- Test: MLFQ accumulates time across yields ---------- */

static char mlfq_order[5];
static int mlfq_order_length;
static pthread_t mlfq_second;

static void burn_cpu(unsigned long microseconds)
{
	clock_t start = clock();
	while (clock() - start < (clock_t)microseconds * CLOCKS_PER_SEC / 1000000)
	{
		/* Consume CPU without yielding. */
	}
}

static void *mlfq_second_worker(void *arg)
{
	(void)arg;
	mlfq_order[mlfq_order_length++] = 'B';
	burn_cpu(QUANTUM * 5 / 4);
	assert(worker_yield() == 0);
	mlfq_order[mlfq_order_length++] = 'b';
	return NULL;
}

static void *mlfq_first_worker(void *arg)
{
	(void)arg;
	mlfq_order[mlfq_order_length++] = 'A';
	burn_cpu(QUANTUM * 3 / 4);
	assert(pthread_create(&mlfq_second, NULL, mlfq_second_worker, NULL) == 0);
	assert(worker_yield() == 0);
	mlfq_order[mlfq_order_length++] = 'X';
	burn_cpu(QUANTUM * 3 / 4);
	assert(worker_yield() == 0);
	mlfq_order[mlfq_order_length++] = 'a';
	return NULL;
}

static void test_mlfq_demotion(void)
{
	pthread_t first;
	sigset_t timer_set, previous_mask;
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	assert(sigprocmask(SIG_BLOCK, &timer_set, &previous_mask) == 0);
	assert(pthread_create(&first, NULL, mlfq_first_worker, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(mlfq_second, NULL) == 0);
	assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
	assert(mlfq_order_length == 5);
	assert(mlfq_order[0] == 'A' && mlfq_order[1] == 'B');
	assert(mlfq_order[2] == 'X' && mlfq_order[3] == 'b' && mlfq_order[4] == 'a');
	puts("MLFQ demotion and yield test passed.");
}

/* ---------- Test: MLFQ priority boost ---------- */

static char boost_order[5];
static int boost_order_length;
static pthread_t boost_second, boost_new;

static void *boost_new_worker(void *arg)
{
	(void)arg;
	boost_order[boost_order_length++] = 'C';
	return NULL;
}

static void *boost_second_worker(void *arg)
{
	struct timespec delay = {
		(MLFQ_BOOST_INTERVAL + QUANTUM) / 1000000,
		((MLFQ_BOOST_INTERVAL + QUANTUM) % 1000000) * 1000
	};
	(void)arg;
	boost_order[boost_order_length++] = 'B';
	assert(nanosleep(&delay, NULL) == 0);
	assert(worker_yield() == 0);
	boost_order[boost_order_length++] = 'b';
	assert(pthread_create(&boost_new, NULL, boost_new_worker, NULL) == 0);
	assert(worker_yield() == 0);
	return NULL;
}

static void *boost_first_worker(void *arg)
{
	(void)arg;
	boost_order[boost_order_length++] = 'A';
	burn_cpu(QUANTUM * 5 / 4);
	assert(pthread_create(&boost_second, NULL, boost_second_worker, NULL) == 0);
	assert(worker_yield() == 0);
	boost_order[boost_order_length++] = 'a';
	return NULL;
}

static void test_mlfq_boost(void)
{
	pthread_t first;
	sigset_t timer_set, previous_mask;
	struct timespec delay = {
		(MLFQ_BOOST_INTERVAL + QUANTUM) / 1000000,
		((MLFQ_BOOST_INTERVAL + QUANTUM) % 1000000) * 1000
	};
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	assert(sigprocmask(SIG_BLOCK, &timer_set, &previous_mask) == 0);
	/* Start the scenario immediately after a boost. */
	assert(nanosleep(&delay, NULL) == 0);
	assert(worker_yield() == 0);
	assert(pthread_create(&first, NULL, boost_first_worker, NULL) == 0);
	assert(pthread_join(first, NULL) == 0);
	assert(pthread_join(boost_second, NULL) == 0);
	assert(pthread_join(boost_new, NULL) == 0);
	assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
	assert(boost_order_length == 5);
	assert(boost_order[0] == 'A' && boost_order[1] == 'B');
	assert(boost_order[2] == 'b' && boost_order[3] == 'a' && boost_order[4] == 'C');
	puts("MLFQ priority boost test passed.");
}

/* ---------- Test: a new higher-priority worker runs first ---------- */

static char priority_order[3];
static int priority_order_length;
static pthread_t priority_new;

static void *priority_new_worker(void *arg)
{
	(void)arg;
	priority_order[priority_order_length++] = 'B';
	return NULL;
}

static void *priority_old_worker(void *arg)
{
	(void)arg;
	priority_order[priority_order_length++] = 'A';
	burn_cpu(QUANTUM * 5 / 4);
	assert(worker_yield() == 0);
	assert(pthread_create(&priority_new, NULL, priority_new_worker, NULL) == 0);
	priority_order[priority_order_length++] = 'a';
	return NULL;
}

static void test_mlfq_priority(void)
{
	pthread_t old;
	sigset_t timer_set, previous_mask;
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	assert(sigprocmask(SIG_BLOCK, &timer_set, &previous_mask) == 0);
	assert(pthread_create(&old, NULL, priority_old_worker, NULL) == 0);
	assert(pthread_join(old, NULL) == 0);
	assert(pthread_join(priority_new, NULL) == 0);
	assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);
	assert(priority_order_length == 3);
	assert(priority_order[0] == 'A' && priority_order[1] == 'B' && priority_order[2] == 'a');
	puts("MLFQ higher-priority preemption test passed.");
}
#endif

int main(void)
{
	sigset_t timer_set;
	sigset_t previous_mask;

	/* Keep the cooperative FIFO tests deterministic. */
	sigemptyset(&timer_set);
	sigaddset(&timer_set, SIGPROF);
	assert(sigprocmask(SIG_BLOCK, &timer_set, &previous_mask) == 0);
	test_scheduling_stats();
	test_worker_create();
	test_worker_yield();
	test_worker_join();
	test_worker_mutex_api();
	test_worker_mutex_blocking();
	test_shared_counter();
	assert(sigprocmask(SIG_SETMASK, &previous_mask, NULL) == 0);

	test_timer_preemption();
	test_preemptive_mutex();
#ifdef MLFQ
	test_mlfq_demotion();
	test_mlfq_boost();
	test_mlfq_priority();
#else
	test_psjf_order();
#endif

	return 0;
}
