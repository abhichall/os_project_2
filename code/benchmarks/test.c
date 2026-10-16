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

	/* since creation does not actually run worker threads. */
	assert(workers_ran == 0);

	puts("worker_create test passed: workers are READY and have not run.");
}

int main(void)
{
	test_worker_create();

	return 0;
}
