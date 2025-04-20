#include "uthreads.h"
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <cstring>
#include <csetjmp>
#include <deque>
#include <vector>
#include <iostream>

#ifdef __x86_64__
#define JB_SP 6
#define JB_PC 7

static address_t translate_address(address_t addr)
{
    address_t ret;
    asm volatile("xor %%fs:0x30,%0\n" "rol $0x11,%0"
                 : "=g" (ret)
                 : "0" (addr));
    return ret;
}
#else
#error "Unsupported architecture"
#endif

enum State { RUNNING, READY, BLOCKED };

struct Thread {
    int           tid {-1};
    State         state {READY};
    sigjmp_buf    env {};
    char          *stack {nullptr};
    unsigned long quantums {0};
    int           sleep_left {0};
};

static std::vector<Thread*> threads(MAX_THREAD_NUM, nullptr);
static std::deque<int> ready_q;
static Thread *current = nullptr;
static unsigned long total_quants = 0;
static int quantum_usecs = 0;

static sigset_t vtalrm_set;

static void scheduler(int sig);
static void setup_timer();
static void block_timer();
static void unblock_timer();

int uthread_init(int quantum_usecs_param)
{
    if (quantum_usecs_param <= 0) {
        std::cerr << "thread library error: quantum must be positive\n";
        return -1;
    }
    quantum_usecs = quantum_usecs_param;

    sigemptyset(&vtalrm_set);
    sigaddset(&vtalrm_set, SIGVTALRM);

    struct sigaction sa {};
    sa.sa_handler = scheduler;
    sa.sa_flags = SA_RESTART; // restart syscalls automatically
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGVTALRM, &sa, nullptr) == -1) {
        perror("system error: sigaction");
        exit(1);
    }

    auto *main_thr = new(std::nothrow) Thread;
    if (!main_thr) {
        perror("system error: new");
        exit(1);
    }
    main_thr->tid = 0;
    main_thr->state = RUNNING;
    threads[0] = main_thr;
    current = main_thr;

    // Save initial env for main thread
    sigsetjmp(main_thr->env, 1);

    setup_timer();

    return 0;
}

static void setup_timer()
{
    itimerval timer {};
    timer.it_value.tv_sec  = quantum_usecs / 1000000;
    timer.it_value.tv_usec = quantum_usecs % 1000000;
    timer.it_interval = timer.it_value;
    if (setitimer(ITIMER_VIRTUAL, &timer, nullptr) == -1) {
        perror("system error: setitimer");
        exit(1);
    }
}

static void block_timer()   { sigprocmask(SIG_BLOCK, &vtalrm_set, nullptr); }
static void unblock_timer() { sigprocmask(SIG_UNBLOCK, &vtalrm_set, nullptr); }

static void scheduler(int /*sig*/)
{
    ++total_quants;
    if (current)
        ++current->quantums;
}
