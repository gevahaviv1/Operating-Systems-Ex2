// uthreads.cpp

#include "uthreads.h"
#include <csignal>
#include <csetjmp>
#include <cstring>
#include <iostream>
#include <deque>
#include <list>
#include <sys/time.h>
#include <stdexcept>

#define JB_SP 6
#define JB_PC 7

struct SigBlocker {
    sigset_t oldmask;
    SigBlocker() {
        sigset_t m{};
        sigemptyset(&m);
        sigaddset(&m, SIGVTALRM);
        if (sigprocmask(SIG_BLOCK, &m, &oldmask) < 0) {
            std::perror("sigprocmask");
            std::exit(1);
        }
    }
    ~SigBlocker() {
        sigprocmask(SIG_SETMASK, &oldmask, nullptr);
    }
};

enum class ThreadState { RUNNING, READY, BLOCKED };
enum class SwitchCause { CYCLE, BLOCK, TERMINATE };

using address_t = unsigned long;

// low-level trampoline we set PC to when a thread first runs
static void thread_trampoline();

// one per thread
struct TCB {
    int tid;
    ThreadState state;
    sigjmp_buf env;
    thread_entry_point entry;
    char* stack;
    int sleep_quanta;
    int quantum_count;

    TCB(int _tid, thread_entry_point _entry)
      : tid(_tid),
        state(ThreadState::READY),
        entry(_entry),
        stack(nullptr),
        sleep_quanta(0),
        quantum_count(0)
    {
        // capture an initial context so we can poke SP and PC
        if (sigsetjmp(env, 1) != 0) {
            // never actually returns here
            entry();
            // if it ever falls through, terminate itself
            uthread_terminate(tid);
        }
        // compute new SP and PC into env

        if (entry){
	    stack = new char[STACK_SIZE];
            address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
            address_t pc = (address_t) thread_trampoline;
            // platform-specific encoding
            env->__jmpbuf[JB_SP] = rot_xor(sp);
            env->__jmpbuf[JB_PC] = rot_xor(pc);
            sigemptyset(&env->__saved_mask);
        }
    }

    ~TCB() {
        if (stack) {
            delete[] stack;
	    stack = nullptr;
        }
    }

    // cheap obfuscation of address so the kernel/jmpbuf trust it
    static address_t rot_xor(address_t a) {
        address_t r = a;
        asm volatile(
            "xor  %%fs:0x30, %0\n"
            "rol  $0x11, %0\n"
            : "=g"(r) : "0"(r)
        );
        return r;
    }
};

// manages all threads
class ThreadsManager {
  public:
    int              quantum_usecs{};
    int              total_quanta{};
    TCB*             current{};
    std::deque<TCB*> ready_queue;
    std::list<TCB*>  blocked_list;
    bool             used_tid[MAX_THREAD_NUM]{};

    ThreadsManager() = default;

    void init(int q_u) {
      if (q_u <= 0) {
        std::cerr << "thread library error: quantum must be positive\n";
        throw std::runtime_error("bad quantum");
      }
      quantum_usecs = q_u;
      total_quanta = 1;

      // main thread TCB
      current = new TCB(0, nullptr);
      current->state = ThreadState::RUNNING;
      current->quantum_count = 1;
      used_tid[0] = true;

      install_timer_handler();
      arm_timer();
    }

    int spawn(thread_entry_point entry) {
      SigBlocker block;
      if (!entry) {
        std::cerr << "thread library error: entry point null\n";
        return -1;
      }
      int tid = next_free_tid();
      if (tid < 0) {
        std::cerr << "thread library error: too many threads\n";
        return -1;
      }
      used_tid[tid] = true;
      TCB* t = nullptr;
      try {
        t = new TCB(tid, entry);
      } catch (std::bad_alloc&) {
        std::cerr << "system error: alloc failed\n";
        std::exit(1);
      }
      ready_queue.push_back(t);
      return tid;
    }

    int terminate(int tid) {
      SigBlocker block;
      if (tid < 0 || tid >= MAX_THREAD_NUM || !used_tid[tid]) {
        std::cerr << "thread library error: bad tid\n";
        return -1;
      }
      // kill everything
      if (tid == 0) {
        cleanup_all();
        std::exit(0);
      }
      used_tid[tid] = false;
      // remove from ready
      for (auto it = ready_queue.begin(); it != ready_queue.end(); ++it) {
        if ((*it)->tid == tid) {
          delete *it;
          ready_queue.erase(it);
          return 0;
        }
      }
      // remove from blocked
      for (auto it = blocked_list.begin(); it != blocked_list.end(); ++it) {
        if ((*it)->tid == tid) {
          delete *it;
          blocked_list.erase(it);
          return 0;
        }
      }
      // it must be current

      delete current;
      switch_to_next(SwitchCause::TERMINATE);
      return 0;
    }

    int block(int tid) {
      SigBlocker block;
      if (tid <= 0 || tid >= MAX_THREAD_NUM || !used_tid[tid]) {
        std::cerr << "thread library error: cannot block\n";
        return -1;
      }
      // find in ready
      for (auto it = ready_queue.begin(); it != ready_queue.end(); ++it) {
        if ((*it)->tid == tid) {
          (*it)->state = ThreadState::BLOCKED;
          blocked_list.push_back(*it);
          ready_queue.erase(it);
          return 0;
        }
      }
      // maybe it's running
      if (current->tid == tid) {
        current->state = ThreadState::BLOCKED;
        switch_to_next(SwitchCause::BLOCK);
        return 0;
      }
      return 0; // already blocked?
    }

    int resume(int tid) {
      SigBlocker block;
      if (tid < 0 || tid >= MAX_THREAD_NUM || !used_tid[tid]) {
        std::cerr << "thread library error: cannot resume\n";
        return -1;
      }
      for (auto it = blocked_list.begin(); it != blocked_list.end(); ++it) {
        if ((*it)->tid == tid) {
          (*it)->state = ThreadState::READY;
          ready_queue.push_back(*it);
          blocked_list.erase(it);
          return 0;
        }
      }
      return 0;
    }

    int sleep(int quantums) {
      SigBlocker block;
      if (quantums < 1 || current->tid == 0) {
        std::cerr << "thread library error: bad sleep\n";
        return -1;
      }
      current->sleep_quanta = quantums;
      current->state = ThreadState::BLOCKED;
      switch_to_next(SwitchCause::BLOCK);
      return 0;
    }

    int get_tid()    const { return current->tid; }
    int get_total()  const { return total_quanta; }
    int get_quantum(int tid) const {
      if (tid<0||tid>=MAX_THREAD_NUM||!used_tid[tid]) {
        std::cerr<<"thread library error\n"; return -1;
      }
      if (tid==current->tid) return current->quantum_count;
      for (auto* t: ready_queue) if (t->tid==tid) return t->quantum_count;
      for (auto* t: blocked_list) if (t->tid==tid) return t->quantum_count;
      return -1;
    }

  private:
    void install_timer_handler() {
      struct sigaction sa{};
      sa.sa_handler = +[](int){ ThreadsManager::instance().on_timer(0); };
      sigaction(SIGVTALRM, &sa, nullptr);
    }

    void arm_timer() {
      struct itimerval tv{};
      tv.it_value.tv_sec  = quantum_usecs/1000000;
      tv.it_value.tv_usec = quantum_usecs%1000000;
      tv.it_interval      = tv.it_value;
      if (setitimer(ITIMER_VIRTUAL,&tv,nullptr)<0) {
        std::perror("setitimer");
        std::exit(1);
      }
    }

    int next_free_tid() const {
      for (int i=1; i<MAX_THREAD_NUM; ++i)
        if (!used_tid[i]) return i;
      return -1;
    }

    void cleanup_all() {
      for (auto* t: ready_queue){
          if(t) {delete t;}
      }
      ready_queue.clear();

      for (auto* t: blocked_list) {
          if(t) {delete t;}
      }
      blocked_list.clear();
    }

    // called on each SIGVTALRM
    void on_timer(int sig) {
      SigBlocker block;

      // preempt current if it's still RUNNING
      if (current->state==ThreadState::RUNNING) {
        switch_to_next(SwitchCause::CYCLE);
      }
    }

    void switch_to_next(SwitchCause why) {
      SigBlocker block;
      // if we have to save current context...
      if (why!=SwitchCause::TERMINATE) {
        if (sigsetjmp(current->env,1)==0) {
          // move it to appropriate queue
          if (why==SwitchCause::CYCLE) {
            current->state = ThreadState::READY;
            ready_queue.push_back(current);
          }
          else if (why==SwitchCause::BLOCK) {
            blocked_list.push_back(current);
          }
          // pick a new one
          if (ready_queue.empty()) {
            // no one to run → just keep running the same thread
            arm_timer();
            return;
          }
          TCB* next = ready_queue.front();
          ready_queue.pop_front();
          next->state = ThreadState::RUNNING;
          current = next;
          total_quanta++;
          current->quantum_count++;

          // decrement sleepers & wake up
	  if (why == SwitchCause::CYCLE) {
		  for (auto it = blocked_list.begin(); it!=blocked_list.end();) {
		      TCB* t = *it;
		      if (t->sleep_quanta>0 && --t->sleep_quanta==0 && t->state==ThreadState::BLOCKED){
			  t->state = ThreadState::READY;
			  ready_queue.push_back(t);
			  it = blocked_list.erase(it);
			  continue;
		      }
		      ++it;
		  }
	  }

          arm_timer();
          siglongjmp(current->env,1);
        }
      } else {
        // terminated: just pick next

        if (ready_queue.empty()) std::exit(0);
        TCB* next = ready_queue.front();
        ready_queue.pop_front();
        next->state = ThreadState::RUNNING;
        current = next;
        total_quanta++;
        current->quantum_count++;
        arm_timer();
        siglongjmp(current->env,1);
      }
    }

  public:
    static ThreadsManager& instance() {
      static ThreadsManager mgr;
      return mgr;
    }
};

// low-level trampoline we set PC to when a thread first runs
static void thread_trampoline() {
    auto& mgr = ThreadsManager::instance();
    // grab the entry function for this thread
    thread_entry_point func = mgr.current->entry;
    // run the thread’s function (this will never return under normal use,
    // but if it does, we must terminate the thread)
    func();
    // once the thread function returns, terminate this thread
    uthread_terminate(mgr.get_tid());
    // (never reached)
}

//
// the C‐style wrappers that call into our manager singleton
//
int uthread_init(int q) {
  try {
    ThreadsManager::instance().init(q);
    return 0;
  } catch(...) {
    return -1;
  }
}
int uthread_spawn(thread_entry_point e) { return ThreadsManager::instance().spawn(e); }
int uthread_terminate(int t)        { return ThreadsManager::instance().terminate(t); }
int uthread_block(int t)            { return ThreadsManager::instance().block(t); }
int uthread_resume(int t)           { return ThreadsManager::instance().resume(t); }
int uthread_sleep(int q)            { return ThreadsManager::instance().sleep(q); }
int uthread_get_tid()               { return ThreadsManager::instance().get_tid(); }
int uthread_get_total_quantums()    { return ThreadsManager::instance().get_total(); }
int uthread_get_quantums(int t)     { return ThreadsManager::instance().get_quantum(t); }
