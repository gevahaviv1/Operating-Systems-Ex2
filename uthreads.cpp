// uthreads.cpp

#include "uthreads.h"
#include <csignal>
#include <csetjmp>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sys/time.h>

// Thread states.
enum class ThreadState {RUNNING, READY, BLOCKED};

// Define address.
typedef unsigned long address_t;

// Thread Control Block.
struct ThreadControlBlock {
    int tid;
    ThreadState state;
    sigjmp_buf env;
    thread_entry_point entry_point;
    char* stack;
    int sleep_count;
    int quantums;
};

// Private to this cpp file.
static ThreadControlBlock* threads[MAX_THREAD_NUM] = {nullptr};
static int current_tid = -1;
static int total_threads = 0;
static int quantum_usecs = 0;
static int total_quantums = 0;

// Simple buffers for READY tids.
static int ready_queue[MAX_THREAD_NUM];
static int rq_head, rq_tail;

// Forward declaration of scheduler handler (defined elsewhere).
static void schedule_handler(int);

// Helper: push tid onto READY queue.
static void enqueue_ready(int tid) {
    ready_queue[rq_tail] = tid;
    rq_tail = (rq_tail + 1) % MAX_THREAD_NUM;
}

// Helper: dequeue the next tid (assumes queue is non-empty).
static int dequeue_ready() {
  int tid = ready_queue[rq_head];
  rq_head = (rq_head + 1) % MAX_THREAD_NUM;
  return tid;
}

// Returns the number of tids currently enqueued in ready_queue[].
static int ready_size() {
    if (rq_tail >= rq_head) {
        // usual case: tail has not wrapped past head.
        return rq_tail - rq_head;
    }
    // wrapped around: head > tail.
    return rq_tail + MAX_THREAD_NUM - rq_head;
}

// ------------------------------------------------------------------
// Helpers to block/unblock our virtual‐timer signal (SIGVTALRM)
// ------------------------------------------------------------------
static void block_timer(sigset_t &prev_mask) {
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGVTALRM);
    if (sigprocmask(SIG_BLOCK, &mask, &prev_mask) < 0) {
        std::perror("sigprocmask");
        std::exit(1);
    }
}

static void restore_timer(const sigset_t &prev_mask) {
    if (sigprocmask(SIG_SETMASK, &prev_mask, nullptr) < 0) {
        std::perror("sigprocmask");
        std::exit(1);
    }
}

// ------------------------------------------------------------------
// Helper to remove a tid from the READY queue (if present)
// ------------------------------------------------------------------
static void remove_from_ready(int tid) {
    int new_tail = 0;
    int old_size = ready_size();
    for (int i = 0; i < old_size; ++i) {
	    int x = dequeue_ready();
	    if (x != tid) {
		    ready_queue[new_tail++] = x;
		    new_tail %= MAX_THREAD_NUM;
	    }
    }

    // Reinstall new queue indices.
    rq_head = 0;
    rq_tail = new_tail;
}

// Initializes the thread library.
int uthread_init(int q_usecs) {
    // Validate quantum.
    if (q_usecs <= 0) {
        std::fprintf(stderr, "thread library error: quantum must be positive\n");
        return -1;
    }
    quantum_usecs = q_usecs;

    // Block SIGVTALRM during setup.
    sigset_t mask;
    block_timer(mask);

    // Create and register main thread (tid 0).
    auto* main_tcb = new ThreadControlBlock;
    if (!main_tcb) {
        std::perror("malloc");
        std::exit(1);
    }
    main_tcb->tid = 0;
    main_tcb->state = ThreadState::RUNNING;
    main_tcb->entry_point = nullptr;
    main_tcb->stack = nullptr;
    main_tcb->sleep_count = 0;
    main_tcb->quantums  = 1;

    threads[0] = main_tcb;
    total_threads = 1;
    total_quantums = 1;
    current_tid = 0;

    // Save current context for thread 0.
    if (sigsetjmp(main_tcb->env, 1) != 0) {
        // When longjmp back here: resume main.
        return 0;
    }

    // Install SIGVTALRM handler.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = &schedule_handler;
    sa.sa_mask = mask; // Block SIGVTALRM inside handler.
    if (sigaction(SIGVTALRM, &sa, nullptr) < 0) {
        std::perror("sigaction");
        std::exit(1);
    }

    // Arm virtual timer for periodic SIGVTALRM.
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = quantum_usecs;
    timer.it_interval = timer.it_value;
    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) < 0) {
        std::perror("setitimer");
        std::exit(1);
    }

    // Initialize empty READY queue.
    rq_head = rq_tail = 0;

    // Unblock SIGVTALRM now that we’re ready.
    restore_timer(mask);

    return 0;
}

static void schedule_handler(int /*unused*/) {
    // Stop further SIGVTALRM while we rearrange.
    sigset_t prev;
    block_timer(prev);

    // Wake any sleeping threads whose countdown just reached zero.
    for (int i = 0; i < MAX_THREAD_NUM; ++i) {
        auto* t = threads[i];
        if (t && t->state == ThreadState::BLOCKED && t->sleep_count > 0) {
            if (--t->sleep_count == 0) {
                // time to wake!
                t->state = ThreadState::READY;
                enqueue_ready(t->tid);
            }
        }
    }

    // Preempt current running thread.
    ThreadControlBlock* me = threads[current_tid];
    if (me && me->state == ThreadState::RUNNING) {
        // save its context; sigsetjmp returns 0 *now*, and non-zero when we jump back
        if (sigsetjmp(me->env, 1) == 0) {
            // Demote RUNNING → READY (unless it blocked or terminated itself in the meantime)
            me->state = ThreadState::READY;
            enqueue_ready(current_tid);

            // (3) Pick the next READY thread
            if (ready_size() == 0) {
                // no one left → just exit
                std::exit(0);
            }
            int next_tid = dequeue_ready();
            ThreadControlBlock* next = threads[next_tid];
            next->state    = ThreadState::RUNNING;
            current_tid    = next_tid;

	    // A new quantum is starting.
	    total_quantums++;
	    threads[next_tid]->quantums++;

            // (4) resume it
            restore_timer(prev);
            siglongjmp(next->env, 1);
            // never returns
        }
    }

    // If we get here, either we returned from a siglongjmp back into this thread,
    // or the current thread wasn’t in RUNNING state (rare). Just restore mask.
    restore_timer(prev);
}

int uthread_spawn(thread_entry_point entry_point) {
    if (!entry_point) {
        fprintf(stderr, "thread library error: null entry_point.\n");
        return -1;
    }

    // Block SIGVTALRM around our setup to avoid races.
    sigset_t prev_mask;
    block_timer(prev_mask);

    // Check thread limit.
    if (total_threads >= MAX_THREAD_NUM) {
        // restore previous mask before returning
	restore_timer(prev_mask);
        return -1;
    }

    // Pick smallest free tid.
    int tid = -1;
    for (int i = 0; i < MAX_THREAD_NUM; ++i) {
        if (threads[i] == nullptr) {
            tid = i;
            break;
        }
    }
    if (tid < 0) {
	restore_timer(prev_mask);
        return -1;
    }

    // Allocate TCB and stack.
    auto* tcb = (ThreadControlBlock*)std::malloc(sizeof(ThreadControlBlock));
    if (!tcb) {
        std::perror("malloc");
        std::exit(1);
    }
    void* stk = std::malloc(STACK_SIZE);
    if (!stk) {
        std::perror("malloc");
        std::exit(1);
    }

    // Initialize TCB fields.
    tcb->tid = tid;
    tcb->state = ThreadState::READY;
    tcb->entry_point = entry_point;
    tcb->stack = (char*)stk;
    tcb->sleep_count = 0;
    tcb->quantums = 0;

    // Capture a baseline context, then patch its SP and PC.
    if (sigsetjmp(tcb->env, 1) == 0) {
        address_t sp = (address_t)tcb->stack + STACK_SIZE - sizeof(address_t);
        address_t pc = (address_t)thread_stub;  // trampoline that calls entry_point+terminate

        tcb->env->__jmpbuf[JB_SP] = translate_address(sp);
        tcb->env->__jmpbuf[JB_PC] = translate_address(pc);
    }

    // Register thread in our arrays & data structures.
    threads[tid] = tcb;
    ++total_threads;
    enqueue_ready(tid);

    // Restore the original signal mask.
    restore_timer(prev_mask);

    return tid;
}

int uthread_terminate(int tid) {
    // Block our timer signal so the clean-up is atomic.
    sigset_t prev;
    block_timer(prev);

    // Validate tid.
    if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: no such tid %d\n", tid);
	restore_timer(prev);
        return -1;
    }

    // If main thread → clean up everything and exit.
    if (tid == 0) {
        // free all other threads
        for (int i = 1; i < MAX_THREAD_NUM; ++i) {
            if (threads[i]) {
                std::free(threads[i]->stack);
                delete threads[i];
                threads[i] = nullptr;
            }
        }
        std::exit(0);
    }

    // Otherwise, tear down this one.
    ThreadControlBlock* victim = threads[tid];
    // remove from our thread table.
    threads[tid] = nullptr;
    --total_threads;

    // free its stack and TCB.
    std::free(victim->stack);
    delete victim;

    // Remove tid from the READY queue if it’s there.
    remove_from_ready(tid);

    // If we’re terminating *another* thread, we’re done.
    if (tid != current_tid) {
	restore_timer(prev);
        return 0;
    }

    // Else: the current thread is self-terminating → pick the next one.
    if (ready_size() == 0) {
        // No runnable threads → exit.
        std::exit(0);
    }
    int next = dequeue_ready();
    current_tid = next;
    threads[next]->state = ThreadState::RUNNING;

    // A new quantum is starting.
    total_quantums++;
    threads[next]->quantums++;

    // Jump to the next thread’s saved context.
    siglongjmp(threads[next]->env, 1);

    return 0;
}

//-----------------------------------------------------------
// uthread_block
//-----------------------------------------------------------
int uthread_block(int tid) {
    // 1) Block timer signal
    sigset_t prev;
    block_timer(prev);

    // 2) Validate tid
    if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: no such tid %d\n", tid);
        restore_timer(prev);
        return -1;
    }
    // 3) Cannot block main thread
    if (tid == 0) {
        std::fprintf(stderr, "thread library error: cannot block main thread\n");
        restore_timer(prev);
        return -1;
    }

    ThreadControlBlock* tcb = threads[tid];
    // 4) If already BLOCKED, nothing to do
    if (tcb->state == ThreadState::BLOCKED) {
        restore_timer(prev);
        return 0;
    }

    // 5) If READY, remove from ready queue
    if (tcb->state == ThreadState::READY) {
        remove_from_ready(tid);
        tcb->state = ThreadState::BLOCKED;
        restore_timer(prev);
        return 0;
    }

    // 6) If RUNNING
    if (tcb->state == ThreadState::RUNNING) {
        // a) mark it blocked
        tcb->state = ThreadState::BLOCKED;
        // b) pick next to run
        if (ready_size() == 0) {
            // no other runnable threads → exit
            std::exit(0);
        }
        int next = dequeue_ready();
        ThreadControlBlock* next_tcb = threads[next];
        next_tcb->state = ThreadState::RUNNING;
        current_tid = next;

	// A new quantum is starting.
	total_quantums++;
	threads[next]->quantums++;

        // c) context switch
        siglongjmp(next_tcb->env, 1);
        // never returns
    }

    // unreachable, but for form:
    restore_timer(prev);
    return 0;
}

int uthread_resume(int tid) {
    // Block SIGVTALRM so our operations are atomic.
    sigset_t prev_mask;
    block_timer(prev_mask);

    // Validate tid
    if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: no such tid %d\n", tid);
        restore_timer(prev_mask);
        return -1;
    }

    auto* tcb = threads[tid];

    // Only move BLOCKED → READY; everything else is a no-op.
    if (tcb->state == ThreadState::BLOCKED) {
        tcb->state = ThreadState::READY;
        enqueue_ready(tid);
    }

    // Unblock and return
    restore_timer(prev_mask);
    return 0;
}

int uthread_sleep(int num_quantums) {
    // You may not sleep the main thread.
    if (current_tid == 0) {
        std::fprintf(stderr, "thread library error: main thread cannot sleep\n");
        return -1;
    }

    if (num_quantums <= 0) {
        std::fprintf(stderr, "thread library error: sleep_quantums must be > 0\n");
        return -1;
    }

    // Block SIGVTALRM so we aren’t preempted halfway.
    sigset_t prev;
    block_timer(prev);

    // Grab the TCB of the thread that called us.
    ThreadControlBlock* me = threads[current_tid];

    // Mark it to sleep for that many quantums, then BLOCK it.
    me->sleep_count = num_quantums;
    me->state = ThreadState::BLOCKED;

    // Immediately schedule someone else.
    if (ready_size() == 0) {
        // No one else to run → exit.
        std::exit(0);
    }
    int next = dequeue_ready();
    ThreadControlBlock* nt = threads[next];
    nt->state = ThreadState::RUNNING;
    current_tid = next;
    
    total_quantums++;
    threads[next]->quantums++;

    // Unblock the timer and switch context.
    restore_timer(prev);
    siglongjmp(nt->env, 1);

    // Never reached.
    return 0;
}

int uthread_get_tid() {
    return current_tid;
}

int uthread_get_total_quantums() {
    return total_quantums;
}

int uthread_get_quantums(int tid) {
    // Invalid tid?
    if (tid < 0 || tid >= MAX_THREAD_NUM || threads[tid] == nullptr) {
        std::fprintf(stderr, "thread library error: no such tid %d\n", tid);
        return -1;
    }
    return threads[tid]->quantums;
}
