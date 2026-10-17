// File:	worker_t.h

// List all group member's name:
// username of iLab:
// iLab Server:

#ifndef WORKER_T_H
#define WORKER_T_H

#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#define _XOPEN_SOURCE 700
#endif
#define _GNU_SOURCE

/* To use Linux pthread Library in Benchmark, you have to comment the USE_WORKERS macro */
#define USE_WORKERS 1

/* include lib header files that you need here: */
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ucontext.h>

typedef uint worker_t;

typedef enum worker_state {
	WORKER_READY,
	WORKER_RUNNING,
	WORKER_BLOCKED,
	WORKER_ZOMBIE
} worker_state;

#define WORKER_STACK_SIZE SIGSTKSZ
#define SCHEDULER_STACK_SIZE SIGSTKSZ

/* Timer quantum in microseconds (10 ms by default). */
#ifndef QUANTUM
#define QUANTUM 10000
#endif

typedef struct TCB {
	/* add important states in a thread control block */
	// thread Id
	// thread status
	// thread context
	// thread stack
	// thread priority
	// And more ...

	worker_t id;
	worker_state state;
	ucontext_t context;
	sigset_t start_mask;
	void *stack;
	void *(*start_routine)(void *);
	void *arg;
	void *return_value;
	struct TCB *joiner;
	struct TCB *next;
	struct TCB *all_next;
} tcb; 

/* define your data structures here: */
// Feel free to add your own auxiliary data structures (linked list or queue etc...)

typedef struct worker_queue {
	tcb *head;
	tcb *tail;
	size_t size;
} worker_queue;

/* mutex struct definition */
typedef struct worker_mutex_t {
	int locked;
	tcb *owner;
	worker_queue wait_queue;
} worker_mutex_t;


/* Function Declarations: */

/* create a new thread */
int worker_create(worker_t * thread, pthread_attr_t * attr, void
    *(*function)(void*), void * arg);

/* give CPU pocession to other user level worker threads voluntarily */
int worker_yield();

/* terminate a thread */
void worker_exit(void *value_ptr);

/* wait for thread termination */
int worker_join(worker_t thread, void **value_ptr);

/* initial the mutex lock */
int worker_mutex_init(worker_mutex_t *mutex, const pthread_mutexattr_t
    *mutexattr);

/* aquire the mutex lock */
int worker_mutex_lock(worker_mutex_t *mutex);

/* release the mutex lock */
int worker_mutex_unlock(worker_mutex_t *mutex);

/* destroy the mutex */
int worker_mutex_destroy(worker_mutex_t *mutex);


/* Function to print global statistics. Do not modify this function.*/
void print_app_stats(void);

#ifdef USE_WORKERS
#define pthread_t worker_t
#define pthread_mutex_t worker_mutex_t
#define pthread_create worker_create
#define pthread_exit worker_exit
#define pthread_join worker_join
#define pthread_mutex_init worker_mutex_init
#define pthread_mutex_lock worker_mutex_lock
#define pthread_mutex_unlock worker_mutex_unlock
#define pthread_mutex_destroy worker_mutex_destroy
#endif

#endif
