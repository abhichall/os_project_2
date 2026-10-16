# Rutgers CS416 Project 2: User-Level Thread Library and Scheduler

## Plan

Status: ✅ complete · 🟡 in progress · ⬜ not started

1. ✅ ~~**Learn Context Switching**~~ — Learn `getcontext()`, `makecontext()`,
   `swapcontext()`, and `setcontext()` by working through a small two-context
   program.
2. ✅ ~~**Design Our Thread System**~~ — Define Thread Control Blocks (TCBs), worker
   states, a scheduler context, and ready queues.

   Implemented so far:

   - A `worker_state` enum to model worker lifecycle states.
   - A TCB containing a worker ID, state, `ucontext_t`, private-stack pointer,
     start routine, argument, return-value slot, and queue link.
   - A FIFO `worker_queue` with head, tail, and size fields.
   - Private scheduler context and scheduler-stack storage in `thread-worker.c`.
   - Private queue initialization and enqueue helpers.
   - One-time scheduler-context initialization.
   - A `worker_create()` foundation that allocates a TCB and stack, initializes
     a user context, assigns an ID, marks the worker ready, and enqueues it.
   - A focused test that creates two workers, verifies unique IDs, and verifies
     that a worker does not run simply because it was created.

3. ✅ ~~**Implement Basic Thread Operations**~~ — Implement `worker_create()`,
   `worker_yield()`, `worker_exit()`, and `worker_join()` using a simple FIFO
   scheduler.

   Implemented:

   - FIFO dequeue and dispatch through the scheduler context.
   - `worker_yield()` to save a worker context and resume it later.
   - `worker_exit()` to save a return value, mark completion, and wake its
     joiner.
   - `worker_join()` to block for a running worker, receive its return value,
     and clean up its stack and TCB.
   - Focused tests for creation, FIFO yielding (`A, B, A, B`), and join return
     values.
4. ⬜ **Implement Mutexes** — Implement the worker mutex APIs and blocking mutex
   wait queues.
5. ⬜ **Implement Timer-Based Preemption** — Use `sigaction()` and `setitimer()`
   to trigger scheduling without a voluntary yield.
6. ⬜ **Implement PSJF Scheduling** — Implement Preemptive Shortest Job First and
   collect the required scheduling statistics.
7. ⬜ **Implement MLFQ Scheduling** — Add multiple priority queues, demotion, time
   slices, and priority boosts to prevent starvation.
8. ⬜ **Benchmark and Write the Report** — Compare PSJF, MLFQ, and Linux pthreads
   using the provided benchmarks; then prepare the final report and submission.

