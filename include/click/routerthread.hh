// -*- c-basic-offset: 4; related-file-name: "../../lib/routerthread.cc" -*-
#ifndef CLICK_ROUTERTHREAD_HH
#define CLICK_ROUTERTHREAD_HH
#include <click/sync.hh>
#include <click/vector.hh>
#if CLICK_LINUXMODULE
# include <click/cxxprotect.h>
CLICK_CXX_PROTECT
# include <linux/sched.h>
CLICK_CXX_UNPROTECT
# include <click/cxxunprotect.h>
#endif
#if CLICK_BSDMODULE
# include <click/cxxprotect.h>
CLICK_CXX_PROTECT
# include <sys/systm.h>
CLICK_CXX_UNPROTECT
# include <click/cxxunprotect.h>
#endif

#define CLICK_DEBUG_SCHEDULING 0

// NB: user must #include <click/task.hh> before <click/routerthread.hh>.
// We cannot #include <click/task.hh> ourselves because of circular #include
// dependency.
CLICK_DECLS

class RouterThread
#ifndef HAVE_TASK_HEAP
    : private Task
#endif
{ public:

    enum { THREAD_QUIESCENT = -1, THREAD_STRONG_UNSCHEDULE = -2,
	   THREAD_UNKNOWN = -1000 };
    
    inline int thread_id() const;

    // Task list functions
    inline bool active() const;
    inline Task *task_begin() const;
    inline Task *task_next(Task *task) const;
    inline Task *task_end() const;
    
    inline void lock_tasks();
    inline bool attempt_lock_tasks();
    inline void unlock_tasks();

    inline Master* master() const;
    void driver();
    void driver_once();

    void unschedule_router_tasks(Router*);

#ifdef HAVE_ADAPTIVE_SCHEDULER
    // min_cpu_share() and max_cpu_share() are expressed on a scale with
    // Task::MAX_UTILIZATION == 100%.
    unsigned min_cpu_share() const	{ return _min_click_share; }
    unsigned max_cpu_share() const	{ return _max_click_share; }
    unsigned cur_cpu_share() const	{ return _cur_click_share; }
    void set_cpu_share(unsigned min_share, unsigned max_share);
#endif

#if CLICK_LINUXMODULE || CLICK_BSDMODULE
    bool greedy() const			{ return _greedy; }
    void set_greedy(bool g)		{ _greedy = g; }
#endif
    
    inline void wake();

#if CLICK_DEBUG_SCHEDULING
    enum { S_RUNNING, S_PAUSED, S_TIMER, S_BLOCKED };
    int thread_state() const		{ return _thread_state; }
    static String thread_state_name(int);
    uint32_t driver_epoch() const	{ return _driver_epoch; }
    uint32_t driver_task_epoch() const	{ return _driver_task_epoch; }
    timeval task_epoch_time(uint32_t epoch) const;
# if CLICK_LINUXMODULE
    struct task_struct *sleeper() const	{ return _linux_task; }
# endif
#endif

    unsigned _tasks_per_iter;
    unsigned _iters_per_timers;
    unsigned _iters_per_os;

  private:

#ifdef HAVE_TASK_HEAP
    Vector<Task*> _task_heap;
    int _task_heap_hole;
    unsigned _pass;
#endif
    
    Master *_master;
    int _id;

#if CLICK_LINUXMODULE
    struct task_struct *_linux_task;
    spinlock_t _lock;
    atomic_uint32_t _task_lock_waiting;
#endif
    
    uint32_t _any_pending;

#if CLICK_LINUXMODULE
    bool _greedy;
#endif
    
#if CLICK_BSDMODULE
    // XXX FreeBSD
    u_int64_t _old_tsc; /* MARKO - temp. */
    void *_sleep_ident;
    int _oticks;
    bool _greedy;
#endif

#ifdef HAVE_ADAPTIVE_SCHEDULER
    enum { C_CLICK, C_KERNEL, NCLIENTS };
    struct Client {			// top-level stride clients
	unsigned pass;
	unsigned stride;
	int tickets;
	Client() : pass(0), tickets(0)	{ }
    };
    Client _clients[NCLIENTS];
    unsigned _global_pass;		// global pass
    unsigned _max_click_share;		// maximum allowed Click share of CPU
    unsigned _min_click_share;		// minimum allowed Click share of CPU
    unsigned _cur_click_share;		// current Click share
#endif

#if CLICK_DEBUG_SCHEDULING
    int _thread_state;
    uint32_t _driver_epoch;
    uint32_t _driver_task_epoch;
    enum { TASK_EPOCH_BUFSIZ = 32 };
    uint32_t _task_epoch_first;
    timeval _task_epoch_time[TASK_EPOCH_BUFSIZ];
#endif
    
    // called by Master
    RouterThread(Master *, int);
    ~RouterThread();

    // task requests
    inline void add_pending();

    // task running functions
    inline void driver_lock_tasks();
    inline void driver_unlock_tasks();
    inline void run_tasks(int ntasks);
    inline void run_os();
#ifdef HAVE_ADAPTIVE_SCHEDULER
    void client_set_tickets(int client, int tickets);
    inline void client_update_pass(int client, const struct timeval &before, const struct timeval &after);
    inline void check_restride(struct timeval &before, const struct timeval &now, int &restride_iter);
#endif
#ifdef HAVE_TASK_HEAP
    void task_reheapify_from(int pos, Task*);
#endif
    
    friend class Task;
    friend class Master;

};


/** @brief Returns this thread's ID.
 *
 * The result is >= 0 for true threads, and < 0 for threads that never run any
 * of their associated Tasks.
 */
inline int
RouterThread::thread_id() const
{
    return _id;
}

/** @brief Returns this thread's associated Master. */
inline Master*
RouterThread::master() const
{
    return _master;
}

/** @brief Returns whether any tasks are scheduled.
 *
 * Returns false iff no tasks are scheduled and no events are pending.  Since
 * not all events actually matter (for example, a Task might have been
 * scheduled and then subsequently unscheduled), active() may temporarily
 * return true even when no real events are outstanding.
 */
inline bool
RouterThread::active() const
{
#ifdef HAVE_TASK_HEAP
    return _task_heap.size() != 0 || _any_pending;
#else
    return ((const Task *)_next != this) || _any_pending;
#endif
}

/** @brief Returns the beginning of the scheduled task list.
 *
 * Each RouterThread maintains a list of all currently-scheduled tasks.
 * Elements may traverse this list with the task_begin(), task_next(), and
 * task_end() functions, using iterator-like code such as:
 *
 * @code
 * thread->lock_tasks();
 * for (Task *t = thread->task_begin();
 *      t != thread->task_end();
 *      t = thread->task_next(t)) {
 *     // ... do something with t...
 * }
 * thread->unlock_tasks();
 * @endcode
 *
 * The thread's task lock must be held during the traversal, as shown above.
 *
 * The return value may not be a real task.  Test it against task_end() before
 * use.
 *
 * @sa task_next, task_end, lock_tasks, unlock_tasks
 */
inline Task *
RouterThread::task_begin() const
{
#ifdef HAVE_TASK_HEAP
    int p = _task_heap_hole;
    return (p < _task_heap.size() ? _task_heap[p] : 0);
#else
    return _next;
#endif
}

/** @brief Returns the task following @a task in the scheduled task list.
 * @param task the current task
 *
 * The return value may not be a real task.  Test it against task_end() before
 * use.  However, the @a task argument must be a real task; do not attempt to
 * call task_next(task_end()).
 *
 * @sa task_begin for usage, task_end
 */
inline Task *
RouterThread::task_next(Task *task) const
{
#ifdef HAVE_TASK_HEAP
    int p = task->_schedpos + 1;
    return (p < _task_heap.size() ? _task_heap[p] : 0);
#else
    return task->_next;
#endif
}

/** @brief Returns the end of the scheduled task list.
 *
 * The return value is not a real task
 *
 * @sa task_begin for usage, task_next
 */
inline Task *
RouterThread::task_end() const
{
#ifdef HAVE_TASK_HEAP
    return 0;
#else
    return (Task *) this;
#endif
}

inline void
RouterThread::lock_tasks()
{
#if CLICK_LINUXMODULE
    if (unlikely(current != _linux_task)) {
	_task_lock_waiting++;
	spin_lock(&_lock);
	_task_lock_waiting--;
    }
#endif
}

inline bool
RouterThread::attempt_lock_tasks()
{
#if CLICK_LINUXMODULE
    if (likely(current == _linux_task))
	return true;
    return spin_trylock(&_lock);
#else
    return true;
#endif
}

inline void
RouterThread::unlock_tasks()
{
#if CLICK_LINUXMODULE
    if (unlikely(current != _linux_task))
	spin_unlock(&_lock);
#endif
}

inline void
RouterThread::wake()
{
#if CLICK_LINUXMODULE
    if (_linux_task)
	wake_up_process(_linux_task);
#endif
#if CLICK_BSDMODULE && !BSD_NETISRSCHED
    if (_sleep_ident)
	wakeup_one(&_sleep_ident);
#endif
}

inline void
RouterThread::add_pending()
{
    _any_pending = 1;
    wake();
}

CLICK_ENDDECLS
#endif
