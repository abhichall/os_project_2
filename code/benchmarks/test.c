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
static int ran = 0;

static void *record_run(void *arg) {
	ran++;
	return arg;
}

int main(void) {
	pthread_t first;
	pthread_t second;
	int first_value = 1;
	int second_value = 2;

	assert(pthread_create(&first, NULL, record_run, &first_value) == 0);
	assert(pthread_create(&second, NULL, record_run, &second_value) == 0);
	assert(first != second);
	assert(ran == 0);

	puts("worker_create test passed: workers are READY and have not run.");

	return 0;
}
