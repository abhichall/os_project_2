#include <stdio.h>
#include <assert.h>
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

int main(void)
{
	test_worker_create();
	test_worker_yield();
	test_worker_join();

	return 0;
}
