/*
 * RT-Mutexes: simple blocking mutual exclusion locks with PI support
 *
 * started by Ingo Molnar and Thomas Gleixner.
 *
 *  Copyright (C) 2004-2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 *  Copyright (C) 2005-2006 Timesys Corp., Thomas Gleixner <tglx@timesys.com>
 *  Copyright (C) 2005 Kihon Technologies Inc., Steven Rostedt
 *  Copyright (C) 2006 Esben Nielsen
 *  Adaptive Spinlocks:
 *  Copyright (C) 2008 Novell, Inc., Gregory Haskins, Sven Dietrich,
 *				     and Peter Morreale,
 *  Adaptive Spinlocks simplification:
 *  Copyright (C) 2008 Red Hat, Inc., Steven Rostedt <srostedt@redhat.com>
 *
 *  See Documentation/locking/rt-mutex-design.txt for details.
 */
#include <linux/spinlock.h>
#include <linux/export.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/sched/deadline.h>
#include <linux/timer.h>
#include <linux/ww_mutex.h>
#include <linux/blkdev.h>

#include "rtmutex_common.h"

/*
 * lock->owner state tracking:
 *
 * lock->owner holds the task_struct pointer of the owner. Bit 0
 * is used to keep track of the "lock has waiters" state.
 *
 * owner	bit0
 * NULL		0	lock is free (fast acquire possible)
 * NULL		1	lock is free and has waiters and the top waiter
 *				is going to take the lock*
 * taskpointer	0	lock is held (fast release possible)
 * taskpointer	1	lock is held and has waiters**
 *
 * The fast atomic compare exchange based acquire and release is only
 * possible when bit 0 of lock->owner is 0.
 *
 * (*) It also can be a transitional state when grabbing the lock
 * with ->wait_lock is held. To prevent any fast path cmpxchg to the lock,
 * we need to set the bit0 before looking at the lock, and the owner may be
 * NULL in this small time, hence this can be a transitional state.
 *
 * (**) There is a small time when bit 0 is set but there are no
 * waiters. This can happen when grabbing the lock in the slow path.
 * To prevent a cmpxchg of the owner releasing the lock, we need to
 * set this bit before looking at the lock.
 */

static void
rt_mutex_set_owner(struct rt_mutex *lock, struct task_struct *owner)
{
	unsigned long val = (unsigned long)owner;

	if (rt_mutex_has_waiters(lock))
		val |= RT_MUTEX_HAS_WAITERS;

	lock->owner = (struct task_struct *)val;
}

static inline void clear_rt_mutex_waiters(struct rt_mutex *lock)
{
	lock->owner = (struct task_struct *)
			((unsigned long)lock->owner & ~RT_MUTEX_HAS_WAITERS);
}

static void fixup_rt_mutex_waiters(struct rt_mutex *lock)
{
	unsigned long owner, *p = (unsigned long *) &lock->owner;

	if (rt_mutex_has_waiters(lock))
		return;

	/*
	 * The rbtree has no waiters enqueued, now make sure that the
	 * lock->owner still has the waiters bit set, otherwise the
	 * following can happen:
	 *
	 * CPU 0	CPU 1		CPU2
	 * l->owner=T1
	 *		rt_mutex_lock(l)
	 *		lock(l->lock)
	 *		l->owner = T1 | HAS_WAITERS;
	 *		enqueue(T2)
	 *		boost()
	 *		  unlock(l->lock)
	 *		block()
	 *
	 *				rt_mutex_lock(l)
	 *				lock(l->lock)
	 *				l->owner = T1 | HAS_WAITERS;
	 *				enqueue(T3)
	 *				boost()
	 *				  unlock(l->lock)
	 *				block()
	 *		signal(->T2)	signal(->T3)
	 *		lock(l->lock)
	 *		dequeue(T2)
	 *		deboost()
	 *		  unlock(l->lock)
	 *				lock(l->lock)
	 *				dequeue(T3)
	 *				 ==> wait list is empty
	 *				deboost()
	 *				 unlock(l->lock)
	 *		lock(l->lock)
	 *		fixup_rt_mutex_waiters()
	 *		  if (wait_list_empty(l) {
	 *		    l->owner = owner
	 *		    owner = l->owner & ~HAS_WAITERS;
	 *		      ==> l->owner = T1
	 *		  }
	 *				lock(l->lock)
	 * rt_mutex_unlock(l)		fixup_rt_mutex_waiters()
	 *				  if (wait_list_empty(l) {
	 *				    owner = l->owner & ~HAS_WAITERS;
	 * cmpxchg(l->owner, T1, NULL)
	 *  ===> Success (l->owner = NULL)
	 *
	 *				    l->owner = owner
	 *				      ==> l->owner = T1
	 *				  }
	 *
	 * With the check for the waiter bit in place T3 on CPU2 will not
	 * overwrite. All tasks fiddling with the waiters bit are
	 * serialized by l->lock, so nothing else can modify the waiters
	 * bit. If the bit is set then nothing can change l->owner either
	 * so the simple RMW is safe. The cmpxchg() will simply fail if it
	 * happens in the middle of the RMW because the waiters bit is
	 * still set.
	 */
	owner = READ_ONCE(*p);
	if (owner & RT_MUTEX_HAS_WAITERS)
		WRITE_ONCE(*p, owner & ~RT_MUTEX_HAS_WAITERS);
}

static int rt_mutex_real_waiter(struct rt_mutex_waiter *waiter)
{
	return waiter && waiter != PI_WAKEUP_INPROGRESS &&
		waiter != PI_REQUEUE_INPROGRESS;
}

/*
 * We can speed up the acquire/release, if there's no debugging state to be
 * set up.
 */
#ifndef CONFIG_DEBUG_RT_MUTEXES
# define rt_mutex_cmpxchg_relaxed(l,c,n) (cmpxchg_relaxed(&l->owner, c, n) == c)
# define rt_mutex_cmpxchg_acquire(l,c,n) (cmpxchg_acquire(&l->owner, c, n) == c)
# define rt_mutex_cmpxchg_release(l,c,n) (cmpxchg_release(&l->owner, c, n) == c)

/*
 * Callers must hold the ->wait_lock -- which is the whole purpose as we force
 * all future threads that attempt to [Rmw] the lock to the slowpath. As such
 * relaxed semantics suffice.
 */
static inline void mark_rt_mutex_waiters(struct rt_mutex *lock)
{
	unsigned long owner, *p = (unsigned long *) &lock->owner;

	do {
		owner = *p;
	} while (cmpxchg_relaxed(p, owner,
				 owner | RT_MUTEX_HAS_WAITERS) != owner);
}

/*
 * Safe fastpath aware unlock:
 * 1) Clear the waiters bit
 * 2) Drop lock->wait_lock
 * 3) Try to unlock the lock with cmpxchg
 */
static inline bool unlock_rt_mutex_safe(struct rt_mutex *lock,
					unsigned long flags)
	__releases(lock->wait_lock)
{
	struct task_struct *owner = rt_mutex_owner(lock);

	clear_rt_mutex_waiters(lock);
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	/*
	 * If a new waiter comes in between the unlock and the cmpxchg
	 * we have two situations:
	 *
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 * cmpxchg(p, owner, 0) == owner
	 *					mark_rt_mutex_waiters(lock);
	 *					acquire(lock);
	 * or:
	 *
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 *					mark_rt_mutex_waiters(lock);
	 *
	 * cmpxchg(p, owner, 0) != owner
	 *					enqueue_waiter();
	 *					unlock(wait_lock);
	 * lock(wait_lock);
	 * wake waiter();
	 * unlock(wait_lock);
	 *					lock(wait_lock);
	 *					acquire(lock);
	 */
	return rt_mutex_cmpxchg_release(lock, owner, NULL);
}

#else
# define rt_mutex_cmpxchg_relaxed(l,c,n)	(0)
# define rt_mutex_cmpxchg_acquire(l,c,n)	(0)
# define rt_mutex_cmpxchg_release(l,c,n)	(0)

static inline void mark_rt_mutex_waiters(struct rt_mutex *lock)
{
	lock->owner = (struct task_struct *)
			((unsigned long)lock->owner | RT_MUTEX_HAS_WAITERS);
}

/*
 * Simple slow path only version: lock->owner is protected by lock->wait_lock.
 */
static inline bool unlock_rt_mutex_safe(struct rt_mutex *lock,
					unsigned long flags)
	__releases(lock->wait_lock)
{
	lock->owner = NULL;
	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	return true;
}
#endif

static inline int
rt_mutex_waiter_less(struct rt_mutex_waiter *left,
		     struct rt_mutex_waiter *right)
{
	if (left->prio < right->prio)
		return 1;

	/*
	 * If both waiters have dl_prio(), we check the deadlines of the
	 * associated tasks.
	 * If left waiter has a dl_prio(), and we didn't return 1 above,
	 * then right waiter has a dl_prio() too.
	 */
	if (dl_prio(left->prio))
		return dl_time_before(left->task->dl.deadline,
				      right->task->dl.deadline);

	return 0;
}

static void
rt_mutex_enqueue(struct rt_mutex *lock, struct rt_mutex_waiter *waiter)
{
	struct rb_node **link = &lock->waiters.rb_node;
	struct rb_node *parent = NULL;
	struct rt_mutex_waiter *entry;
	int leftmost = 1;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct rt_mutex_waiter, tree_entry);
		if (rt_mutex_waiter_less(waiter, entry)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		lock->waiters_leftmost = &waiter->tree_entry;

	rb_link_node(&waiter->tree_entry, parent, link);
	rb_insert_color(&waiter->tree_entry, &lock->waiters);
}

static void
rt_mutex_dequeue(struct rt_mutex *lock, struct rt_mutex_waiter *waiter)
{
	if (RB_EMPTY_NODE(&waiter->tree_entry))
		return;

	if (lock->waiters_leftmost == &waiter->tree_entry)
		lock->waiters_leftmost = rb_next(&waiter->tree_entry);

	rb_erase(&waiter->tree_entry, &lock->waiters);
	RB_CLEAR_NODE(&waiter->tree_entry);
}

static void
rt_mutex_enqueue_pi(struct task_struct *task, struct rt_mutex_waiter *waiter)
{
	struct rb_node **link = &task->pi_waiters.rb_node;
	struct rb_node *parent = NULL;
	struct rt_mutex_waiter *entry;
	int leftmost = 1;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct rt_mutex_waiter, pi_tree_entry);
		if (rt_mutex_waiter_less(waiter, entry)) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = 0;
		}
	}

	if (leftmost)
		task->pi_waiters_leftmost = &waiter->pi_tree_entry;

	rb_link_node(&waiter->pi_tree_entry, parent, link);
	rb_insert_color(&waiter->pi_tree_entry, &task->pi_waiters);
}

static void
rt_mutex_dequeue_pi(struct task_struct *task, struct rt_mutex_waiter *waiter)
{
	if (RB_EMPTY_NODE(&waiter->pi_tree_entry))
		return;

	if (task->pi_waiters_leftmost == &waiter->pi_tree_entry)
		task->pi_waiters_leftmost = rb_next(&waiter->pi_tree_entry);

	rb_erase(&waiter->pi_tree_entry, &task->pi_waiters);
	RB_CLEAR_NODE(&waiter->pi_tree_entry);
}

/*
 * Calculate task priority from the waiter tree priority
 *
 * Return task->normal_prio when the waiter tree is empty or when
 * the waiter is not allowed to do priority boosting
 */
int rt_mutex_getprio(struct task_struct *task)
{
	if (likely(!task_has_pi_waiters(task)))
		return task->normal_prio;

	return min(task_top_pi_waiter(task)->prio,
		   task->normal_prio);
}

struct task_struct *rt_mutex_get_top_task(struct task_struct *task)
{
	if (likely(!task_has_pi_waiters(task)))
		return NULL;

	return task_top_pi_waiter(task)->task;
}

/*
 * Called by sched_setscheduler() to get the priority which will be
 * effective after the change.
 */
int rt_mutex_get_effective_prio(struct task_struct *task, int newprio)
{
	if (!task_has_pi_waiters(task))
		return newprio;

	if (task_top_pi_waiter(task)->task->prio <= newprio)
		return task_top_pi_waiter(task)->task->prio;
	return newprio;
}

/*
 * Adjust the priority of a task, after its pi_waiters got modified.
 *
 * This can be both boosting and unboosting. task->pi_lock must be held.
 */
static void __rt_mutex_adjust_prio(struct task_struct *task)
{
	int prio = rt_mutex_getprio(task);

	if (task->prio != prio || dl_prio(prio))
		rt_mutex_setprio(task, prio);
}

/*
 * Adjust task priority (undo boosting). Called from the exit path of
 * rt_mutex_slowunlock() and rt_mutex_slowlock().
 *
 * (Note: We do this outside of the protection of lock->wait_lock to
 * allow the lock to be taken while or before we readjust the priority
 * of task. We do not use the spin_xx_mutex() variants here as we are
 * outside of the debug path.)
 */
void rt_mutex_adjust_prio(struct task_struct *task)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&task->pi_lock, flags);
	__rt_mutex_adjust_prio(task);
	raw_spin_unlock_irqrestore(&task->pi_lock, flags);
}

/*
 * Deadlock detection is conditional:
 *
 * If CONFIG_DEBUG_RT_MUTEXES=n, deadlock detection is only conducted
 * if the detect argument is == RT_MUTEX_FULL_CHAINWALK.
 *
 * If CONFIG_DEBUG_RT_MUTEXES=y, deadlock detection is always
 * conducted independent of the detect argument.
 *
 * If the waiter argument is NULL this indicates the deboost path and
 * deadlock detection is disabled independent of the detect argument
 * and the config settings.
 */
static bool rt_mutex_cond_detect_deadlock(struct rt_mutex_waiter *waiter,
					  enum rtmutex_chainwalk chwalk)
{
	/*
	 * This is just a wrapper function for the following call,
	 * because debug_rt_mutex_detect_deadlock() smells like a magic
	 * debug feature and I wanted to keep the cond function in the
	 * main source file along with the comments instead of having
	 * two of the same in the headers.
	 */
	return debug_rt_mutex_detect_deadlock(waiter, chwalk);
}

static void rt_mutex_wake_waiter(struct rt_mutex_waiter *waiter)
{
	if (waiter->savestate)
		wake_up_lock_sleeper(waiter->task);
	else
		wake_up_process(waiter->task);
}

/*
 * Max number of times we'll walk the boosting chain:
 */
int max_lock_depth = 1024;

static inline struct rt_mutex *task_blocked_on_lock(struct task_struct *p)
{
	return rt_mutex_real_waiter(p->pi_blocked_on) ?
		p->pi_blocked_on->lock : NULL;
}

/*
 * Adjust the priority chain. Also used for deadlock detection.
 * Decreases task's usage by one - may thus free the task.
 *
 * @task:	the task owning the mutex (owner) for which a chain walk is
 *		probably needed
 * @chwalk:	do we have to carry out deadlock detection?
 * @orig_lock:	the mutex (can be NULL if we are walking the chain to recheck
 *		things for a task that has just got its priority adjusted, and
 *		is waiting on a mutex)
 * @next_lock:	the mutex on which the owner of @orig_lock was blocked before
 *		we dropped its pi_lock. Is never dereferenced, only used for
 *		comparison to detect lock chain changes.
 * @orig_waiter: rt_mutex_waiter struct for the task that has just donated
 *		its priority to the mutex owner (can be NULL in the case
 *		depicted above or if the top waiter is gone away and we are
 *		actually deboosting the owner)
 * @top_task:	the current top waiter
 *
 * Returns 0 or -EDEADLK.
 *
 * Chain walk basics and protection scope
 *
 * [R] refcount on task
 * [P] task->pi_lock held
 * [L] rtmutex->wait_lock held
 *
 * Step	Description				Protected by
 *	function arguments:
 *	@task					[R]
 *	@orig_lock if != NULL			@top_task is blocked on it
 *	@next_lock				Unprotected. Cannot be
 *						dereferenced. Only used for
 *						comparison.
 *	@orig_waiter if != NULL			@top_task is blocked on it
 *	@top_task				current, or in case of proxy
 *						locking protected by calling
 *						code
 *	again:
 *	  loop_sanity_check();
 *	retry:
 * [1]	  lock(task->pi_lock);			[R] acquire [P]
 * [2]	  waiter = task->pi_blocked_on;		[P]
 * [3]	  check_exit_conditions_1();		[P]
 * [4]	  lock = waiter->lock;			[P]
 * [5]	  if (!try_lock(lock->wait_lock)) {	[P] try to acquire [L]
 *	    unlock(task->pi_lock);		release [P]
 *	    goto retry;
 *	  }
 * [6]	  check_exit_conditions_2();		[P] + [L]
 * [7]	  requeue_lock_waiter(lock, waiter);	[P] + [L]
 * [8]	  unlock(task->pi_lock);		release [P]
 *	  put_task_struct(task);		release [R]
 * [9]	  check_exit_conditions_3();		[L]
 * [10]	  task = owner(lock);			[L]
 *	  get_task_struct(task);		[L] acquire [R]
 *	  lock(task->pi_lock);			[L] acquire [P]
 * [11]	  requeue_pi_waiter(tsk, waiters(lock));[P] + [L]
 * [12]	  check_exit_conditions_4();		[P] + [L]
 * [13]	  unlock(task->pi_lock);		release [P]
 *	  unlock(lock->wait_lock);		release [L]
 *	  goto again;
 */
static int rt_mutex_adjust_prio_chain(struct task_struct *task,
				      enum rtmutex_chainwalk chwalk,
				      struct rt_mutex *orig_lock,
				      struct rt_mutex *next_lock,
				      struct rt_mutex_waiter *orig_waiter,
				      struct task_struct *top_task)
{
	struct rt_mutex_waiter *waiter, *top_waiter = orig_waiter;
	struct rt_mutex_waiter *prerequeue_top_waiter;
	int ret = 0, depth = 0;
	struct rt_mutex *lock;
	bool detect_deadlock;
	bool requeue = true;

	detect_deadlock = rt_mutex_cond_detect_deadlock(orig_waiter, chwalk);

	/*
	 * The (de)boosting is a step by step approach with a lot of
	 * pitfalls. We want this to be preemptible and we want hold a
	 * maximum of two locks per step. So we have to check
	 * carefully whether things change under us.
	 */
 again:
	/*
	 * We limit the lock chain length for each invocation.
	 */
	if (++depth > max_lock_depth) {
		static int prev_max;

		/*
		 * Print this only once. If the admin changes the limit,
		 * print a new message when reaching the limit again.
		 */
		if (prev_max != max_lock_depth) {
			prev_max = max_lock_depth;
			printk(KERN_WARNING "Maximum lock depth %d reached "
			       "task: %s (%d)\n", max_lock_depth,
			       top_task->comm, task_pid_nr(top_task));
		}
		put_task_struct(task);

		return -EDEADLK;
	}

	/*
	 * We are fully preemptible here and only hold the refcount on
	 * @task. So everything can have changed under us since the
	 * caller or our own code below (goto retry/again) dropped all
	 * locks.
	 */
 retry:
	/*
	 * [1] Task cannot go away as we did a get_task() before !
	 */
	raw_spin_lock_irq(&task->pi_lock);

	/*
	 * [2] Get the waiter on which @task is blocked on.
	 */
	waiter = task->pi_blocked_on;

	/*
	 * [3] check_exit_conditions_1() protected by task->pi_lock.
	 */

	/*
	 * Check whether the end of the boosting chain has been
	 * reached or the state of the chain has changed while we
	 * dropped the locks.
	 */
	if (!rt_mutex_real_waiter(waiter))
		goto out_unlock_pi;

	/*
	 * Check the orig_waiter state. After we dropped the locks,
	 * the previous owner of the lock might have released the lock.
	 */
	if (orig_waiter && !rt_mutex_owner(orig_lock))
		goto out_unlock_pi;

	/*
	 * We dropped all locks after taking a refcount on @task, so
	 * the task might have moved on in the lock chain or even left
	 * the chain completely and blocks now on an unrelated lock or
	 * on @orig_lock.
	 *
	 * We stored the lock on which @task was blocked in @next_lock,
	 * so we can detect the chain change.
	 */
	if (next_lock != waiter->lock)
		goto out_unlock_pi;

	/*
	 * Drop out, when the task has no waiters. Note,
	 * top_waiter can be NULL, when we are in the deboosting
	 * mode!
	 */
	if (top_waiter) {
		if (!task_has_pi_waiters(task))
			goto out_unlock_pi;
		/*
		 * If deadlock detection is off, we stop here if we
		 * are not the top pi waiter of the task. If deadlock
		 * detection is enabled we continue, but stop the
		 * requeueing in the chain walk.
		 */
		if (top_waiter != task_top_pi_waiter(task)) {
			if (!detect_deadlock)
				goto out_unlock_pi;
			else
				requeue = false;
		}
	}

	/*
	 * If the waiter priority is the same as the task priority
	 * then there is no further priority adjustment necessary.  If
	 * deadlock detection is off, we stop the chain walk. If its
	 * enabled we continue, but stop the requeueing in the chain
	 * walk.
	 */
	if (waiter->prio == task->prio) {
		if (!detect_deadlock)
			goto out_unlock_pi;
		else
			requeue = false;
	}

	/*
	 * [4] Get the next lock
	 */
	lock = waiter->lock;
	/*
	 * [5] We need to trylock here as we are holding task->pi_lock,
	 * which is the reverse lock order versus the other rtmutex
	 * operations.
	 */
	if (!raw_spin_trylock(&lock->wait_lock)) {
		raw_spin_unlock_irq(&task->pi_lock);
		cpu_relax();
		goto retry;
	}

	/*
	 * [6] check_exit_conditions_2() protected by task->pi_lock and
	 * lock->wait_lock.
	 *
	 * Deadlock detection. If the lock is the same as the original
	 * lock which caused us to walk the lock chain or if the
	 * current lock is owned by the task which initiated the chain
	 * walk, we detected a deadlock.
	 */
	if (lock == orig_lock || rt_mutex_owner(lock) == top_task) {
		debug_rt_mutex_deadlock(chwalk, orig_waiter, lock);
		raw_spin_unlock(&lock->wait_lock);
		ret = -EDEADLK;
		goto out_unlock_pi;
	}

	/*
	 * If we just follow the lock chain for deadlock detection, no
	 * need to do all the requeue operations. To avoid a truckload
	 * of conditionals around the various places below, just do the
	 * minimum chain walk checks.
	 */
	if (!requeue) {
		/*
		 * No requeue[7] here. Just release @task [8]
		 */
		raw_spin_unlock(&task->pi_lock);
		put_task_struct(task);

		/*
		 * [9] check_exit_conditions_3 protected by lock->wait_lock.
		 * If there is no owner of the lock, end of chain.
		 */
		if (!rt_mutex_owner(lock)) {
			raw_spin_unlock_irq(&lock->wait_lock);
			return 0;
		}

		/* [10] Grab the next task, i.e. owner of @lock */
		task = rt_mutex_owner(lock);
		get_task_struct(task);
		raw_spin_lock(&task->pi_lock);

		/*
		 * No requeue [11] here. We just do deadlock detection.
		 *
		 * [12] Store whether owner is blocked
		 * itself. Decision is made after dropping the locks
		 */
		next_lock = task_blocked_on_lock(task);
		/*
		 * Get the top waiter for the next iteration
		 */
		top_waiter = rt_mutex_top_waiter(lock);

		/* [13] Drop locks */
		raw_spin_unlock(&task->pi_lock);
		raw_spin_unlock_irq(&lock->wait_lock);

		/* If owner is not blocked, end of chain. */
		if (!next_lock)
			goto out_put_task;
		goto again;
	}

	/*
	 * Store the current top waiter before doing the requeue
	 * operation on @lock. We need it for the boost/deboost
	 * decision below.
	 */
	prerequeue_top_waiter = rt_mutex_top_waiter(lock);

	/* [7] Requeue the waiter in the lock waiter tree. */
	rt_mutex_dequeue(lock, waiter);
	waiter->prio = task->prio;
	rt_mutex_enqueue(lock, waiter);

	/* [8] Release the task */
	raw_spin_unlock(&task->pi_lock);
	put_task_struct(task);

	/*
	 * [9] check_exit_conditions_3 protected by lock->wait_lock.
	 *
	 * We must abort the chain walk if there is no lock owner even
	 * in the dead lock detection case, as we have nothing to
	 * follow here. This is the end of the chain we are walking.
	 */
	if (!rt_mutex_owner(lock)) {
		struct rt_mutex_waiter *lock_top_waiter;

		/*
		 * If the requeue [7] above changed the top waiter,
		 * then we need to wake the new top waiter up to try
		 * to get the lock.
		 */
		lock_top_waiter = rt_mutex_top_waiter(lock);
		if (prerequeue_top_waiter != lock_top_waiter)
			rt_mutex_wake_waiter(lock_top_waiter);
		raw_spin_unlock_irq(&lock->wait_lock);
		return 0;
	}

	/* [10] Grab the next task, i.e. the owner of @lock */
	task = rt_mutex_owner(lock);
	get_task_struct(task);
	raw_spin_lock(&task->pi_lock);

	/* [11] requeue the pi waiters if necessary */
	if (waiter == rt_mutex_top_waiter(lock)) {
		/*
		 * The waiter became the new top (highest priority)
		 * waiter on the lock. Replace the previous top waiter
		 * in the owner tasks pi waiters tree with this waiter
		 * and adjust the priority of the owner.
		 */
		rt_mutex_dequeue_pi(task, prerequeue_top_waiter);
		rt_mutex_enqueue_pi(task, waiter);
		__rt_mutex_adjust_prio(task);

	} else if (prerequeue_top_waiter == waiter) {
		/*
		 * The waiter was the top waiter on the lock, but is
		 * no longer the top prority waiter. Replace waiter in
		 * the owner tasks pi waiters tree with the new top
		 * (highest priority) waiter and adjust the priority
		 * of the owner.
		 * The new top waiter is stored in @waiter so that
		 * @waiter == @top_waiter evaluates to true below and
		 * we continue to deboost the rest of the chain.
		 */
		rt_mutex_dequeue_pi(task, waiter);
		waiter = rt_mutex_top_waiter(lock);
		rt_mutex_enqueue_pi(task, waiter);
		__rt_mutex_adjust_prio(task);
	} else {
		/*
		 * Nothing changed. No need to do any priority
		 * adjustment.
		 */
	}

	/*
	 * [12] check_exit_conditions_4() protected by task->pi_lock
	 * and lock->wait_lock. The actual decisions are made after we
	 * dropped the locks.
	 *
	 * Check whether the task which owns the current lock is pi
	 * blocked itself. If yes we store a pointer to the lock for
	 * the lock chain change detection above. After we dropped
	 * task->pi_lock next_lock cannot be dereferenced anymore.
	 */
	next_lock = task_blocked_on_lock(task);
	/*
	 * Store the top waiter of @lock for the end of chain walk
	 * decision below.
	 */
	top_waiter = rt_mutex_top_waiter(lock);

	/* [13] Drop the locks */
	raw_spin_unlock(&task->pi_lock);
	raw_spin_unlock_irq(&lock->wait_lock);

	/*
	 * Make the actual exit decisions [12], based on the stored
	 * values.
	 *
	 * We reached the end of the lock chain. Stop right here. No
	 * point to go back just to figure that out.
	 */
	if (!next_lock)
		goto out_put_task;

	/*
	 * If the current waiter is not the top waiter on the lock,
	 * then we can stop the chain walk here if we are not in full
	 * deadlock detection mode.
	 */
	if (!detect_deadlock && waiter != top_waiter)
		goto out_put_task;

	goto again;

 out_unlock_pi:
	raw_spin_unlock_irq(&task->pi_lock);
 out_put_task:
	put_task_struct(task);

	return ret;
}

#define STEAL_NORMAL  0
#define STEAL_LATERAL 1

/*
 * Note that RT tasks are excluded from lateral-steals to prevent the
 * introduction of an unbounded latency
 */
static inline int lock_is_stealable(struct task_struct *task,
				    struct task_struct *pendowner, int mode)
{
    if (mode == STEAL_NORMAL || rt_task(task)) {
	    if (task->prio >= pendowner->prio)
		    return 0;
    } else if (task->prio > pendowner->prio)
	    return 0;
    return 1;
}

/*
 * Try to take an rt-mutex
 *
 * Must be called with lock->wait_lock held and interrupts disabled
 *
 * @lock:   The lock to be acquired.
 * @task:   The task which wants to acquire the lock
 * @waiter: The waiter that is queued to the lock's wait tree if the
 *	    callsite called task_blocked_on_lock(), otherwise NULL
 */
static int __try_to_take_rt_mutex(struct rt_mutex *lock,
				  struct task_struct *task,
				  struct rt_mutex_waiter *waiter, int mode)
{
	/*
	 * Before testing whether we can acquire @lock, we set the
	 * RT_MUTEX_HAS_WAITERS bit in @lock->owner. This forces all
	 * other tasks which try to modify @lock into the slow path
	 * and they serialize on @lock->wait_lock.
	 *
	 * The RT_MUTEX_HAS_WAITERS bit can have a transitional state
	 * as explained at the top of this file if and only if:
	 *
	 * - There is a lock owner. The caller must fixup the
	 *   transient state if it does a trylock or leaves the lock
	 *   function due to a signal or timeout.
	 *
	 * - @task acquires the lock and there are no other
	 *   waiters. This is undone in rt_mutex_set_owner(@task) at
	 *   the end of this function.
	 */
	mark_rt_mutex_waiters(lock);

	/*
	 * If @lock has an owner, give up.
	 */
	if (rt_mutex_owner(lock))
		return 0;

	/*
	 * If @waiter != NULL, @task has already enqueued the waiter
	 * into @lock waiter tree. If @waiter == NULL then this is a
	 * trylock attempt.
	 */
	if (waiter) {
		/*
		 * If waiter is not the highest priority waiter of
		 * @lock, give up.
		 */
		if (waiter != rt_mutex_top_waiter(lock)) {
			/* XXX lock_is_stealable() ? */
			return 0;
		}

		/*
		 * We can acquire the lock. Remove the waiter from the
		 * lock waiters tree.
		 */
		rt_mutex_dequeue(lock, waiter);

	} else {
		/*
		 * If the lock has waiters already we check whether @task is
		 * eligible to take over the lock.
		 *
		 * If there are no other waiters, @task can acquire
		 * the lock.  @task->pi_blocked_on is NULL, so it does
		 * not need to be dequeued.
		 */
		if (rt_mutex_has_waiters(lock)) {
			struct task_struct *pown = rt_mutex_top_waiter(lock)->task;

			if (task != pown && !lock_is_stealable(task, pown, mode))
				return 0;
			/*
			 * The current top waiter stays enqueued. We
			 * don't have to change anything in the lock
			 * waiters order.
			 */
		} else {
			/*
			 * No waiters. Take the lock without the
			 * pi_lock dance.@task->pi_blocked_on is NULL
			 * and we have no waiters to enqueue in @task
			 * pi waiters tree.
			 */
			goto takeit;
		}
	}

	/*
	 * Clear @task->pi_blocked_on. Requires protection by
	 * @task->pi_lock. Redundant operation for the @waiter == NULL
	 * case, but conditionals are more expensive than a redundant
	 * store.
	 */
	raw_spin_lock(&task->pi_lock);
	task->pi_blocked_on = NULL;
	/*
	 * Finish the lock acquisition. @task is the new owner. If
	 * other waiters exist we have to insert the highest priority
	 * waiter into @task->pi_waiters tree.
	 */
	if (rt_mutex_has_waiters(lock))
		rt_mutex_enqueue_pi(task, rt_mutex_top_waiter(lock));
	raw_spin_unlock(&task->pi_lock);

takeit:
	/* We got the lock. */
	debug_rt_mutex_lock(lock);

	/*
	 * This either preserves the RT_MUTEX_HAS_WAITERS bit if there
	 * are still waiters or clears it.
	 */
	rt_mutex_set_owner(lock, task);

	return 1;
}

#ifdef CONFIG_PREEMPT_RT_FULL
/*
 * preemptible spin_lock functions:
 */
static inline void rt_spin_lock_fastlock(struct rt_mutex *lock,
					 void  (*slowfn)(struct rt_mutex *lock,
							 bool mg_off),
					 bool do_mig_dis)
{
	might_sleep_no_state_check();

	if (do_mig_dis)
		migrate_disable();

	if (likely(rt_mutex_cmpxchg_acquire(lock, NULL, current)))
		return;
	else
		slowfn(lock, do_mig_dis);
}

static inline int rt_spin_lock_fastunlock(struct rt_mutex *lock,
					   int  (*slowfn)(struct rt_mutex *lock))
{
	if (likely(rt_mutex_cmpxchg_release(lock, current, NULL)))
		return 0;
	return slowfn(lock);
}
#ifdef CONFIG_SMP
/*
 * Note that owner is a speculative pointer and dereferencing relies
 * on rcu_read_lock() and the check against the lock owner.
 */
static int adaptive_wait(struct rt_mutex *lock,
			 struct task_struct *owner)
{
	int res = 0;

	rcu_read_lock();
	for (;;) {
		if (owner != rt_mutex_owner(lock))
			break;
		/*
		 * Ensure that owner->on_cpu is dereferenced _after_
		 * checking the above to be valid.
		 */
		barrier();
		if (!owner->on_cpu) {
			res = 1;
			break;
		}
		cpu_relax();
	}
	rcu_read_unlock();
	return res;
}
#else
static int adaptive_wait(struct rt_mutex *lock,
			 struct task_struct *orig_owner)
{
	return 1;
}
#endif

static int task_blocks_on_rt_mutex(struct rt_mutex *lock,
				   struct rt_mutex_waiter *waiter,
				   struct task_struct *task,
				   enum rtmutex_chainwalk chwalk);
/*
 * Slow path lock function spin_lock style: this variant is very
 * careful not to miss any non-lock wakeups.
 *
 * We store the current state under p->pi_lock in p->saved_state and
 * the try_to_wake_up() code handles this accordingly.
 */
static void  noinline __sched rt_spin_lock_slowlock(struct rt_mutex *lock,
						    bool mg_off)
{
	struct task_struct *lock_owner, *self = current;
	struct rt_mutex_waiter waiter, *top_waiter;
	unsigned long flags;
	int ret;

	rt_mutex_init_waiter(&waiter, true);

	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	if (__try_to_take_rt_mutex(lock, self, NULL, STEAL_LATERAL)) {
		raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
		return;
	}

	BUG_ON(rt_mutex_owner(lock) == self);

	/*
	 * We save whatever state the task is in and we'll restore it
	 * after acquiring the lock taking real wakeups into account
	 * as well. We are serialized via pi_lock against wakeups. See
	 * try_to_wake_up().
	 */
	raw_spin_lock(&self->pi_lock);
	self->saved_state = self->state;
	__set_current_state_no_track(TASK_UNINTERRUPTIBLE);
	raw_spin_unlock(&self->pi_lock);

	ret = task_blocks_on_rt_mutex(lock, &waiter, self, RT_MUTEX_MIN_CHAINWALK);
	BUG_ON(ret);

	for (;;) {
		/* Try to acquire the lock again. */
		if (__try_to_take_rt_mutex(lock, self, &waiter, STEAL_LATERAL))
			break;

		top_waiter = rt_mutex_top_waiter(lock);
		lock_owner = rt_mutex_owner(lock);

		raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

		debug_rt_mutex_print_deadlock(&waiter);

		if (top_waiter != &waiter || adaptive_wait(lock, lock_owner)) {
			if (mg_off)
				migrate_enable();
			schedule();
			if (mg_off)
				migrate_disable();
		}

		raw_spin_lock_irqsave(&lock->wait_lock, flags);

		raw_spin_lock(&self->pi_lock);
		__set_current_state_no_track(TASK_UNINTERRUPTIBLE);
		raw_spin_unlock(&self->pi_lock);
	}

	/*
	 * Restore the task state to current->saved_state. We set it
	 * to the original state above and the try_to_wake_up() code
	 * has possibly updated it when a real (non-rtmutex) wakeup
	 * happened while we were blocked. Clear saved_state so
	 * try_to_wakeup() does not get confused.
	 */
	raw_spin_lock(&self->pi_lock);
	__set_current_state_no_track(self->saved_state);
	self->saved_state = TASK_RUNNING;
	raw_spin_unlock(&self->pi_lock);

	/*
	 * try_to_take_rt_mutex() sets the waiter bit
	 * unconditionally. We might have to fix that up:
	 */
	fixup_rt_mutex_waiters(lock);

	BUG_ON(rt_mutex_has_waiters(lock) && &waiter == rt_mutex_top_waiter(lock));
	BUG_ON(!RB_EMPTY_NODE(&waiter.tree_entry));

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	debug_rt_mutex_free_waiter(&waiter);
}

static void mark_wakeup_next_waiter(struct wake_q_head *wake_q,
				    struct wake_q_head *wake_sleeper_q,
				    struct rt_mutex *lock);
/*
 * Slow path to release a rt_mutex spin_lock style
 */
static int noinline __sched rt_spin_lock_slowunlock(struct rt_mutex *lock)
{
	unsigned long flags;
	WAKE_Q(wake_q);
	WAKE_Q(wake_sleeper_q);

	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	debug_rt_mutex_unlock(lock);

	if (!rt_mutex_has_waiters(lock)) {
		lock->owner = NULL;
		raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
		return 0;
	}

	mark_wakeup_next_waiter(&wake_q, &wake_sleeper_q, lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	wake_up_q(&wake_q);
	wake_up_q_sleeper(&wake_sleeper_q);

	/* Undo pi boosting.when necessary */
	rt_mutex_adjust_prio(current);
	return 0;
}

static int noinline __sched rt_spin_lock_slowunlock_no_deboost(struct rt_mutex *lock)
{
	unsigned long flags;
	WAKE_Q(wake_q);
	WAKE_Q(wake_sleeper_q);

	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	debug_rt_mutex_unlock(lock);

	if (!rt_mutex_has_waiters(lock)) {
		lock->owner = NULL;
		raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
		return 0;
	}

	mark_wakeup_next_waiter(&wake_q, &wake_sleeper_q, lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
	wake_up_q(&wake_q);
	wake_up_q_sleeper(&wake_sleeper_q);
	return 1;
}

void __lockfunc rt_spin_lock__no_mg(spinlock_t *lock)
{
	rt_spin_lock_fastlock(&lock->lock, rt_spin_lock_slowlock, false);
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
}
EXPORT_SYMBOL(rt_spin_lock__no_mg);

void __lockfunc rt_spin_lock(spinlock_t *lock)
{
	rt_spin_lock_fastlock(&lock->lock, rt_spin_lock_slowlock, true);
	spin_acquire(&lock->dep_map, 0, 0, _RET_IP_);
}
EXPORT_SYMBOL(rt_spin_lock);

void __lockfunc __rt_spin_lock(struct rt_mutex *lock)
{
	rt_spin_lock_fastlock(lock, rt_spin_lock_slowlock, true);
}
EXPORT_SYMBOL(__rt_spin_lock);

void __lockfunc __rt_spin_lock__no_mg(struct rt_mutex *lock)
{
	rt_spin_lock_fastlock(lock, rt_spin_lock_slowlock, false);
}
EXPORT_SYMBOL(__rt_spin_lock__no_mg);

#ifdef CONFIG_DEBUG_LOCK_ALLOC
void __lockfunc rt_spin_lock_nested(spinlock_t *lock, int subclass)
{
	spin_acquire(&lock->dep_map, subclass, 0, _RET_IP_);
	rt_spin_lock_fastlock(&lock->lock, rt_spin_lock_slowlock, true);
}
EXPORT_SYMBOL(rt_spin_lock_nested);
#endif

void __lockfunc rt_spin_unlock__no_mg(spinlock_t *lock)
{
	/* NOTE: we always pass in '1' for nested, for simplicity */
	spin_release(&lock->dep_map, 1, _RET_IP_);
	rt_spin_lock_fastunlock(&lock->lock, rt_spin_lock_slowunlock);
}
EXPORT_SYMBOL(rt_spin_unlock__no_mg);

void __lockfunc rt_spin_unlock(spinlock_t *lock)
{
	/* NOTE: we always pass in '1' for nested, for simplicity */
	spin_release(&lock->dep_map, 1, _RET_IP_);
	rt_spin_lock_fastunlock(&lock->lock, rt_spin_lock_slowunlock);
	migrate_enable();
}
EXPORT_SYMBOL(rt_spin_unlock);

int __lockfunc rt_spin_unlock_no_deboost(spinlock_t *lock)
{
	int ret;

	/* NOTE: we always pass in '1' for nested, for simplicity */
	spin_release(&lock->dep_map, 1, _RET_IP_);
	ret = rt_spin_lock_fastunlock(&lock->lock, rt_spin_lock_slowunlock_no_deboost);
	migrate_enable();
	return ret;
}

void __lockfunc __rt_spin_unlock(struct rt_mutex *lock)
{
	rt_spin_lock_fastunlock(lock, rt_spin_lock_slowunlock);

EXPORT_SYMBOL(__rt_spin_unlock);

/*
 * Wait for the lock to get unlocked: instead of polling for an unlock
 * (like raw spinlocks do), we lock and unlock, to force the kernel to
 * schedule if there's contention:
 */
void __lockfunc rt_spin_unlock_wait(spinlock_t *lock)
{
	spin_lock(lock);
	spin_unlock(lock);
}
EXPORT_SYMBOL(rt_spin_unlock_wait);

int __lockfunc __rt_spin_trylock(struct rt_mutex *lock)
{
	return rt_mutex_trylock(lock);
}

int __lockfunc rt_spin_trylock__no_mg(spinlock_t *lock)
{
	int ret;

	ret = rt_mutex_trylock(&lock->lock);
	if (ret)
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	return ret;
}
EXPORT_SYMBOL(rt_spin_trylock__no_mg);

int __lockfunc rt_spin_trylock(spinlock_t *lock)
{
	int ret;

	migrate_disable();
	ret = rt_mutex_trylock(&lock->lock);
	if (ret)
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	else
		migrate_enable();
	return ret;
}
EXPORT_SYMBOL(rt_spin_trylock);

int __lockfunc rt_spin_trylock_bh(spinlock_t *lock)
{
	int ret;

	local_bh_disable();
	ret = rt_mutex_trylock(&lock->lock);
	if (ret) {
		migrate_disable();
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	} else
		local_bh_enable();
	return ret;
}
EXPORT_SYMBOL(rt_spin_trylock_bh);

int __lockfunc rt_spin_trylock_irqsave(spinlock_t *lock, unsigned long *flags)
{
	int ret;

	*flags = 0;
	ret = rt_mutex_trylock(&lock->lock);
	if (ret) {
		migrate_disable();
		spin_acquire(&lock->dep_map, 0, 1, _RET_IP_);
	}
	return ret;
}
EXPORT_SYMBOL(rt_spin_trylock_irqsave);

int atomic_dec_and_spin_lock(atomic_t *atomic, spinlock_t *lock)
{
	/* Subtract 1 from counter unless that drops it to 0 (ie. it was 1) */
	if (atomic_add_unless(atomic, -1, 1))
		return 0;
	rt_spin_lock(lock);
	if (atomic_dec_and_test(atomic))
		return 1;
	rt_spin_unlock(lock);
	return 0;
}
EXPORT_SYMBOL(atomic_dec_and_spin_lock);

	void
__rt_spin_lock_init(spinlock_t *lock, char *name, struct lock_class_key *key)
{
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	/*
	 * Make sure we are not reinitializing a held lock:
	 */
	debug_check_no_locks_freed((void *)lock, sizeof(*lock));
	lockdep_init_map(&lock->dep_map, name, key, 0);
#endif
}
EXPORT_SYMBOL(__rt_spin_lock_init);

#endif /* PREEMPT_RT_FULL */

#ifdef CONFIG_PREEMPT_RT_FULL
	static inline int __sched
__mutex_lock_check_stamp(struct rt_mutex *lock, struct ww_acquire_ctx *ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base.lock);
	struct ww_acquire_ctx *hold_ctx = ACCESS_ONCE(ww->ctx);

	if (!hold_ctx)
		return 0;

	if (unlikely(ctx == hold_ctx))
		return -EALREADY;

	if (ctx->stamp - hold_ctx->stamp <= LONG_MAX &&
	    (ctx->stamp != hold_ctx->stamp || ctx > hold_ctx)) {
#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(ctx->contending_lock);
		ctx->contending_lock = ww;
#endif
		return -EDEADLK;
	}

	return 0;
}
#else
	static inline int __sched
__mutex_lock_check_stamp(struct rt_mutex *lock, struct ww_acquire_ctx *ctx)
{
	BUG();
	return 0;
}

#endif

static inline int
try_to_take_rt_mutex(struct rt_mutex *lock, struct task_struct *task,
		     struct rt_mutex_waiter *waiter)
{
	return __try_to_take_rt_mutex(lock, task, waiter, STEAL_NORMAL);
}

/*
 * Task blocks on lock.
 *
 * Prepare waiter and propagate pi chain
 *
 * This must be called with lock->wait_lock held and interrupts disabled
 */
static int task_blocks_on_rt_mutex(struct rt_mutex *lock,
				   struct rt_mutex_waiter *waiter,
				   struct task_struct *task,
				   enum rtmutex_chainwalk chwalk)
{
	struct task_struct *owner = rt_mutex_owner(lock);
	struct rt_mutex_waiter *top_waiter = waiter;
	struct rt_mutex *next_lock;
	int chain_walk = 0, res;

	/*
	 * Early deadlock detection. We really don't want the task to
	 * enqueue on itself just to untangle the mess later. It's not
	 * only an optimization. We drop the locks, so another waiter
	 * can come in before the chain walk detects the deadlock. So
	 * the other will detect the deadlock and return -EDEADLOCK,
	 * which is wrong, as the other waiter is not in a deadlock
	 * situation.
	 */
	if (owner == task)
		return -EDEADLK;

	raw_spin_lock(&task->pi_lock);

	/*
	 * In the case of futex requeue PI, this will be a proxy
	 * lock. The task will wake unaware that it is enqueueed on
	 * this lock. Avoid blocking on two locks and corrupting
	 * pi_blocked_on via the PI_WAKEUP_INPROGRESS
	 * flag. futex_wait_requeue_pi() sets this when it wakes up
	 * before requeue (due to a signal or timeout). Do not enqueue
	 * the task if PI_WAKEUP_INPROGRESS is set.
	 */
	if (task != current && task->pi_blocked_on == PI_WAKEUP_INPROGRESS) {
		raw_spin_unlock(&task->pi_lock);
		return -EAGAIN;
	}

	BUG_ON(rt_mutex_real_waiter(task->pi_blocked_on));

	__rt_mutex_adjust_prio(task);
	waiter->task = task;
	waiter->lock = lock;
	waiter->prio = task->prio;

	/* Get the top priority waiter on the lock */
	if (rt_mutex_has_waiters(lock))
		top_waiter = rt_mutex_top_waiter(lock);
	rt_mutex_enqueue(lock, waiter);

	task->pi_blocked_on = waiter;

	raw_spin_unlock(&task->pi_lock);

	if (!owner)
		return 0;

	raw_spin_lock(&owner->pi_lock);
	if (waiter == rt_mutex_top_waiter(lock)) {
		rt_mutex_dequeue_pi(owner, top_waiter);
		rt_mutex_enqueue_pi(owner, waiter);

		__rt_mutex_adjust_prio(owner);
		if (rt_mutex_real_waiter(owner->pi_blocked_on))
			chain_walk = 1;
	} else if (rt_mutex_cond_detect_deadlock(waiter, chwalk)) {
		chain_walk = 1;
	}

	/* Store the lock on which owner is blocked or NULL */
	next_lock = task_blocked_on_lock(owner);

	raw_spin_unlock(&owner->pi_lock);
	/*
	 * Even if full deadlock detection is on, if the owner is not
	 * blocked itself, we can avoid finding this out in the chain
	 * walk.
	 */
	if (!chain_walk || !next_lock)
		return 0;

	/*
	 * The owner can't disappear while holding a lock,
	 * so the owner struct is protected by wait_lock.
	 * Gets dropped in rt_mutex_adjust_prio_chain()!
	 */
	get_task_struct(owner);

	raw_spin_unlock_irq(&lock->wait_lock);

	res = rt_mutex_adjust_prio_chain(owner, chwalk, lock,
					 next_lock, waiter, task);

	raw_spin_lock_irq(&lock->wait_lock);

	return res;
}

/*
 * Remove the top waiter from the current tasks pi waiter tree and
 * queue it up.
 *
 * Called with lock->wait_lock held and interrupts disabled.
 */
static void mark_wakeup_next_waiter(struct wake_q_head *wake_q,
				    struct wake_q_head *wake_sleeper_q,
				    struct rt_mutex *lock)
{
	struct rt_mutex_waiter *waiter;

	raw_spin_lock(&current->pi_lock);

	waiter = rt_mutex_top_waiter(lock);

	/*
	 * Remove it from current->pi_waiters. We do not adjust a
	 * possible priority boost right now. We execute wakeup in the
	 * boosted mode and go back to normal after releasing
	 * lock->wait_lock.
	 */
	rt_mutex_dequeue_pi(current, waiter);

	/*
	 * As we are waking up the top waiter, and the waiter stays
	 * queued on the lock until it gets the lock, this lock
	 * obviously has waiters. Just set the bit here and this has
	 * the added benefit of forcing all new tasks into the
	 * slow path making sure no task of lower priority than
	 * the top waiter can steal this lock.
	 */
	lock->owner = (void *) RT_MUTEX_HAS_WAITERS;

	raw_spin_unlock(&current->pi_lock);

	if (waiter->savestate)
		wake_q_add_sleeper(wake_sleeper_q, waiter->task);
	else
		wake_q_add(wake_q, waiter->task);
}

/*
 * Remove a waiter from a lock and give up
 *
 * Must be called with lock->wait_lock held and interrupts disabled. I must
 * have just failed to try_to_take_rt_mutex().
 */
static void remove_waiter(struct rt_mutex *lock,
			  struct rt_mutex_waiter *waiter)
{
	bool is_top_waiter = (waiter == rt_mutex_top_waiter(lock));
	struct task_struct *owner = rt_mutex_owner(lock);
	struct rt_mutex *next_lock = NULL;

	raw_spin_lock(&current->pi_lock);
	rt_mutex_dequeue(lock, waiter);
	current->pi_blocked_on = NULL;
	raw_spin_unlock(&current->pi_lock);

	/*
	 * Only update priority if the waiter was the highest priority
	 * waiter of the lock and there is an owner to update.
	 */
	if (!owner || !is_top_waiter)
		return;

	raw_spin_lock(&owner->pi_lock);

	rt_mutex_dequeue_pi(owner, waiter);

	if (rt_mutex_has_waiters(lock))
		rt_mutex_enqueue_pi(owner, rt_mutex_top_waiter(lock));

	__rt_mutex_adjust_prio(owner);

	/* Store the lock on which owner is blocked or NULL */
	if (rt_mutex_real_waiter(owner->pi_blocked_on))
		next_lock = task_blocked_on_lock(owner);

	raw_spin_unlock(&owner->pi_lock);

	/*
	 * Don't walk the chain, if the owner task is not blocked
	 * itself.
	 */
	if (!next_lock)
		return;

	/* gets dropped in rt_mutex_adjust_prio_chain()! */
	get_task_struct(owner);

	raw_spin_unlock_irq(&lock->wait_lock);

	rt_mutex_adjust_prio_chain(owner, RT_MUTEX_MIN_CHAINWALK, lock,
				   next_lock, NULL, current);

	raw_spin_lock_irq(&lock->wait_lock);
}

/*
 * Recheck the pi chain, in case we got a priority setting
 *
 * Called from sched_setscheduler
 */
void rt_mutex_adjust_pi(struct task_struct *task)
{
	struct rt_mutex_waiter *waiter;
	struct rt_mutex *next_lock;
	unsigned long flags;

	raw_spin_lock_irqsave(&task->pi_lock, flags);

	waiter = task->pi_blocked_on;
	if (!rt_mutex_real_waiter(waiter) || (waiter->prio == task->prio &&
			!dl_prio(task->prio))) {
		raw_spin_unlock_irqrestore(&task->pi_lock, flags);
		return;
	}
	next_lock = waiter->lock;

	/* gets dropped in rt_mutex_adjust_prio_chain()! */
	get_task_struct(task);

	raw_spin_unlock_irqrestore(&task->pi_lock, flags);
	rt_mutex_adjust_prio_chain(task, RT_MUTEX_MIN_CHAINWALK, NULL,
				   next_lock, NULL, task);
}

void rt_mutex_init_waiter(struct rt_mutex_waiter *waiter, bool savestate)
{
	debug_rt_mutex_init_waiter(waiter);
	RB_CLEAR_NODE(&waiter->pi_tree_entry);
	RB_CLEAR_NODE(&waiter->tree_entry);
	waiter->task = NULL;
	waiter->savestate = savestate;
}

/**
 * __rt_mutex_slowlock() - Perform the wait-wake-try-to-take loop
 * @lock:		 the rt_mutex to take
 * @state:		 the state the task should block in (TASK_INTERRUPTIBLE
 *			 or TASK_UNINTERRUPTIBLE)
 * @timeout:		 the pre-initialized and started timer, or NULL for none
 * @waiter:		 the pre-initialized rt_mutex_waiter
 *
 * Must be called with lock->wait_lock held and interrupts disabled
 */
static int __sched
__rt_mutex_slowlock(struct rt_mutex *lock, int state,
		    struct hrtimer_sleeper *timeout,
		    struct rt_mutex_waiter *waiter,
		    struct ww_acquire_ctx *ww_ctx)
{
	int ret = 0;

	for (;;) {
		/* Try to acquire the lock: */
		if (try_to_take_rt_mutex(lock, current, waiter))
			break;

		if (timeout && !timeout->task) {
			ret = -ETIMEDOUT;
			break;
		}
		if (signal_pending_state(state, current)) {
			ret = -EINTR;
			break;
		}

		if (ww_ctx && ww_ctx->acquired > 0) {
			ret = __mutex_lock_check_stamp(lock, ww_ctx);
			if (ret)
				break;
		}

		raw_spin_unlock_irq(&lock->wait_lock);

		debug_rt_mutex_print_deadlock(waiter);

		schedule();

		raw_spin_lock_irq(&lock->wait_lock);
		set_current_state(state);
	}

	__set_current_state(TASK_RUNNING);
	return ret;
}

static void rt_mutex_handle_deadlock(int res, int detect_deadlock,
				     struct rt_mutex_waiter *w)
{
	/*
	 * If the result is not -EDEADLOCK or the caller requested
	 * deadlock detection, nothing to do here.
	 */
	if (res != -EDEADLOCK || detect_deadlock)
		return;

	/*
	 * Yell lowdly and stop the task right here.
	 */
	rt_mutex_print_deadlock(w);
	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		schedule();
	}
}

static __always_inline void ww_mutex_lock_acquired(struct ww_mutex *ww,
						   struct ww_acquire_ctx *ww_ctx)
{
#ifdef CONFIG_DEBUG_MUTEXES
	/*
	 * If this WARN_ON triggers, you used ww_mutex_lock to acquire,
	 * but released with a normal mutex_unlock in this call.
	 *
	 * This should never happen, always use ww_mutex_unlock.
	 */
	DEBUG_LOCKS_WARN_ON(ww->ctx);

	/*
	 * Not quite done after calling ww_acquire_done() ?
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->done_acquire);

	if (ww_ctx->contending_lock) {
		/*
		 * After -EDEADLK you tried to
		 * acquire a different ww_mutex? Bad!
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->contending_lock != ww);

		/*
		 * You called ww_mutex_lock after receiving -EDEADLK,
		 * but 'forgot' to unlock everything else first?
		 */
		DEBUG_LOCKS_WARN_ON(ww_ctx->acquired > 0);
		ww_ctx->contending_lock = NULL;
	}

	/*
	 * Naughty, using a different class will lead to undefined behavior!
	 */
	DEBUG_LOCKS_WARN_ON(ww_ctx->ww_class != ww->ww_class);
#endif
	ww_ctx->acquired++;
}

#ifdef CONFIG_PREEMPT_RT_FULL
static void ww_mutex_account_lock(struct rt_mutex *lock,
				  struct ww_acquire_ctx *ww_ctx)
{
	struct ww_mutex *ww = container_of(lock, struct ww_mutex, base.lock);
	struct rt_mutex_waiter *waiter, *n;

	/*
	 * This branch gets optimized out for the common case,
	 * and is only important for ww_mutex_lock.
	 */
	ww_mutex_lock_acquired(ww, ww_ctx);
	ww->ctx = ww_ctx;

	/*
	 * Give any possible sleeping processes the chance to wake up,
	 * so they can recheck if they have to back off.
	 */
	rbtree_postorder_for_each_entry_safe(waiter, n, &lock->waiters,
					     tree_entry) {
		/* XXX debug rt mutex waiter wakeup */

		BUG_ON(waiter->lock != lock);
		rt_mutex_wake_waiter(waiter);
	}
}

#else

static void ww_mutex_account_lock(struct rt_mutex *lock,
				  struct ww_acquire_ctx *ww_ctx)
{
	BUG();
}
#endif

/*
 * Slow path lock function:
 */
static int __sched
rt_mutex_slowlock(struct rt_mutex *lock, int state,
		  struct hrtimer_sleeper *timeout,
		  enum rtmutex_chainwalk chwalk,
		  struct ww_acquire_ctx *ww_ctx)
{
	struct rt_mutex_waiter waiter;
	unsigned long flags;
	int ret = 0;

	rt_mutex_init_waiter(&waiter, false);

	/*
	 * Technically we could use raw_spin_[un]lock_irq() here, but this can
	 * be called in early boot if the cmpxchg() fast path is disabled
	 * (debug, no architecture support). In this case we will acquire the
	 * rtmutex with lock->wait_lock held. But we cannot unconditionally
	 * enable interrupts in that early boot case. So we need to use the
	 * irqsave/restore variants.
	 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	/* Try to acquire the lock again: */
	if (try_to_take_rt_mutex(lock, current, NULL)) {
		if (ww_ctx)
			ww_mutex_account_lock(lock, ww_ctx);
		raw_spin_unlock_irqrestore(&lock->wait_lock, flags);
		return 0;
	}

	set_current_state(state);

	/* Setup the timer, when timeout != NULL */
	if (unlikely(timeout))
		hrtimer_start_expires(&timeout->timer, HRTIMER_MODE_ABS);

	ret = task_blocks_on_rt_mutex(lock, &waiter, current, chwalk);

	if (likely(!ret))
		/* sleep on the mutex */
		ret = __rt_mutex_slowlock(lock, state, timeout, &waiter,
					  ww_ctx);
	else if (ww_ctx) {
		/* ww_mutex received EDEADLK, let it become EALREADY */
		ret = __mutex_lock_check_stamp(lock, ww_ctx);
		BUG_ON(!ret);
	}

	if (unlikely(ret)) {
		__set_current_state(TASK_RUNNING);
		if (rt_mutex_has_waiters(lock))
			remove_waiter(lock, &waiter);
		/* ww_mutex want to report EDEADLK/EALREADY, let them */
		if (!ww_ctx)
			rt_mutex_handle_deadlock(ret, chwalk, &waiter);
	} else if (ww_ctx) {
		ww_mutex_account_lock(lock, ww_ctx);
	}

	/*
	 * try_to_take_rt_mutex() sets the waiter bit
	 * unconditionally. We might have to fix that up.
	 */
	fixup_rt_mutex_waiters(lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	/* Remove pending timer: */
	if (unlikely(timeout))
		hrtimer_cancel(&timeout->timer);

	debug_rt_mutex_free_waiter(&waiter);

	return ret;
}

static inline int __rt_mutex_slowtrylock(struct rt_mutex *lock)
{
	int ret = try_to_take_rt_mutex(lock, current, NULL);

	/*
	 * try_to_take_rt_mutex() sets the lock waiters bit
	 * unconditionally. Clean this up.
	 */
	fixup_rt_mutex_waiters(lock);

	return ret;
}

/*
 * Slow path try-lock function:
 */
static inline int rt_mutex_slowtrylock(struct rt_mutex *lock)
{
	unsigned long flags;
	int ret;

	/*
	 * If the lock already has an owner we fail to get the lock.
	 * This can be done without taking the @lock->wait_lock as
	 * it is only being read, and this is a trylock anyway.
	 */
	if (rt_mutex_owner(lock))
		return 0;

	/*
	 * The mutex has currently no owner. Lock the wait lock and try to
	 * acquire the lock. We use irqsave here to support early boot calls.
	 */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	ret = __rt_mutex_slowtrylock(lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	return ret;
}

/*
 * Slow path to release a rt-mutex.
 * Return whether the current task needs to undo a potential priority boosting.
 */
static bool __sched rt_mutex_slowunlock(struct rt_mutex *lock,
					struct wake_q_head *wake_q,
					struct wake_q_head *wake_sleeper_q)
{
	unsigned long flags;

	/* irqsave required to support early boot calls */
	raw_spin_lock_irqsave(&lock->wait_lock, flags);

	debug_rt_mutex_unlock(lock);

	/*
	 * We must be careful here if the fast path is enabled. If we
	 * have no waiters queued we cannot set owner to NULL here
	 * because of:
	 *
	 * foo->lock->owner = NULL;
	 *			rtmutex_lock(foo->lock);   <- fast path
	 *			free = atomic_dec_and_test(foo->refcnt);
	 *			rtmutex_unlock(foo->lock); <- fast path
	 *			if (free)
	 *				kfree(foo);
	 * raw_spin_unlock(foo->lock->wait_lock);
	 *
	 * So for the fastpath enabled kernel:
	 *
	 * Nothing can set the waiters bit as long as we hold
	 * lock->wait_lock. So we do the following sequence:
	 *
	 *	owner = rt_mutex_owner(lock);
	 *	clear_rt_mutex_waiters(lock);
	 *	raw_spin_unlock(&lock->wait_lock);
	 *	if (cmpxchg(&lock->owner, owner, 0) == owner)
	 *		return;
	 *	goto retry;
	 *
	 * The fastpath disabled variant is simple as all access to
	 * lock->owner is serialized by lock->wait_lock:
	 *
	 *	lock->owner = NULL;
	 *	raw_spin_unlock(&lock->wait_lock);
	 */
	while (!rt_mutex_has_waiters(lock)) {
		/* Drops lock->wait_lock ! */
		if (unlock_rt_mutex_safe(lock, flags) == true)
			return false;
		/* Relock the rtmutex and try again */
		raw_spin_lock_irqsave(&lock->wait_lock, flags);
	}

	/*
	 * The wakeup next waiter path does not suffer from the above
	 * race. See the comments there.
	 *
	 * Queue the next waiter for wakeup once we release the wait_lock.
	 */
	mark_wakeup_next_waiter(wake_q, wake_sleeper_q, lock);

	raw_spin_unlock_irqrestore(&lock->wait_lock, flags);

	/* check PI boosting */
	return true;
}

/*
 * debug aware fast / slowpath lock,trylock,unlock
 *
 * The atomic acquire/release ops are compiled away, when either the
 * architecture does not support cmpxchg or when debugging is enabled.
 */
static inline int
rt_mutex_fastlock(struct rt_mutex *lock, int state,
		  struct ww_acquire_ctx *ww_ctx,
		  int (*slowfn)(struct rt_mutex *lock, int state,
				struct hrtimer_sleeper *timeout,
				enum rtmutex_chainwalk chwalk,
				struct ww_acquire_ctx *ww_ctx))
{
	if (likely(rt_mutex_cmpxchg_acquire(lock, NULL, current)))
		return 0;

	/*
	 * If rt_mutex blocks, the function sched_submit_work will not call
	 * blk_schedule_flush_plug (because tsk_is_pi_blocked would be true).
	 * We must call blk_schedule_flush_plug here, if we don't call it,
	 * a deadlock in device mapper may happen.
	 */
	if (unlikely(blk_needs_flush_plug(current)))
		blk_schedule_flush_plug(current);

	return slowfn(lock, state, NULL, RT_MUTEX_MIN_CHAINWALK,
		      ww_ctx);
}

static inline int
rt_mutex_timed_fastlock(struct rt_mutex *lock, int state,
			struct hrtimer_sleeper *timeout,
			enum rtmutex_chainwalk chwalk,
			struct ww_acquire_ctx *ww_ctx,
			int (*slowfn)(struct rt_mutex *lock, int state,
				      struct hrtimer_sleeper *timeout,
				      enum rtmutex_chainwalk chwalk,
				      struct ww_acquire_ctx *ww_ctx))
{
	if (chwalk == RT_MUTEX_MIN_CHAINWALK &&
	    likely(rt_mutex_cmpxchg_acquire(lock, NULL, current)))
		return 0;

	if (unlikely(blk_needs_flush_plug(current)))
		blk_schedule_flush_plug(current);

	return slowfn(lock, state, timeout, chwalk, ww_ctx);
}

static inline int
rt_mutex_fasttrylock(struct rt_mutex *lock,
		     int (*slowfn)(struct rt_mutex *lock))
{
	if (likely(rt_mutex_cmpxchg_acquire(lock, NULL, current)))
		return 1;

	return slowfn(lock);
}

static inline void
rt_mutex_fastunlock(struct rt_mutex *lock,
		    bool (*slowfn)(struct rt_mutex *lock,
				   struct wake_q_head *wqh,
				   struct wake_q_head *wq_sleeper))
{
	WAKE_Q(wake_q);
	WAKE_Q(wake_sleeper_q);
	bool deboost;

	if (likely(rt_mutex_cmpxchg_release(lock, current, NULL)))
		return;

	deboost = slowfn(lock, &wake_q, &wake_sleeper_q);

	wake_up_q(&wake_q);
	wake_up_q_sleeper(&wake_sleeper_q);

	/* Undo pi boosting if necessary: */
	if (deboost)
		rt_mutex_adjust_prio(current);
}

/**
 * rt_mutex_lock - lock a rt_mutex
 *
 * @lock: the rt_mutex to be locked
 */
void __sched rt_mutex_lock(struct rt_mutex *lock)
{
	might_sleep();

	rt_mutex_fastlock(lock, TASK_UNINTERRUPTIBLE, NULL, rt_mutex_slowlock);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock);

/**
 * rt_mutex_lock_interruptible - lock a rt_mutex interruptible
 *
 * @lock:		the rt_mutex to be locked
 *
 * Returns:
 *  0		on success
 * -EINTR	when interrupted by a signal
 */
int __sched rt_mutex_lock_interruptible(struct rt_mutex *lock)
{
	might_sleep();

	return rt_mutex_fastlock(lock, TASK_INTERRUPTIBLE, NULL, rt_mutex_slowlock);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock_interruptible);

/*
 * Futex variant, must not use fastpath.
 */
int __sched rt_mutex_futex_trylock(struct rt_mutex *lock)
{
	return rt_mutex_slowtrylock(lock);
}

int __sched __rt_mutex_futex_trylock(struct rt_mutex *lock)
{
	return __rt_mutex_slowtrylock(lock);
}

/**
 * rt_mutex_lock_killable - lock a rt_mutex killable
 *
 * @lock:              the rt_mutex to be locked
 * @detect_deadlock:   deadlock detection on/off
 *
 * Returns:
 *  0          on success
 * -EINTR      when interrupted by a signal
 * -EDEADLK    when the lock would deadlock (when deadlock detection is on)
 */
int __sched rt_mutex_lock_killable(struct rt_mutex *lock)
{
	might_sleep();

	return rt_mutex_fastlock(lock, TASK_KILLABLE, NULL, rt_mutex_slowlock);
}
EXPORT_SYMBOL_GPL(rt_mutex_lock_killable);

/**
 * rt_mutex_timed_lock - lock a rt_mutex interruptible
 *			the timeout structure is provided
 *			by the caller
 *
 * @lock:		the rt_mutex to be locked
 * @timeout:		timeout structure or NULL (no timeout)
 *
 * Returns:
 *  0		on success
 * -EINTR	when interrupted by a signal
 * -ETIMEDOUT	when the timeout expired
 */
int
rt_mutex_timed_lock(struct rt_mutex *lock, struct hrtimer_sleeper *timeout)
{
	might_sleep();

	return rt_mutex_timed_fastlock(lock, TASK_INTERRUPTIBLE, timeout,
				       RT_MUTEX_MIN_CHAINWALK,
				       NULL,
				       rt_mutex_slowlock);
}
EXPORT_SYMBOL_GPL(rt_mutex_timed_lock);

/**
 * rt_mutex_trylock - try to lock a rt_mutex
 *
 * @lock:	the rt_mutex to be locked
 *
 * This function can only be called in thread context. It's safe to
 * call it from atomic regions, but not from hard interrupt or soft
 * interrupt context.
 *
 * Returns 1 on success and 0 on contention
 */
int __sched rt_mutex_trylock(struct rt_mutex *lock)
{
#ifdef CONFIG_PREEMPT_RT_FULL
	if (WARN_ON_ONCE(in_irq() || in_nmi()))
#else
	if (WARN_ON(in_irq() || in_nmi() || in_serving_softirq()))
#endif
		return 0;

	return rt_mutex_fasttrylock(lock, rt_mutex_slowtrylock);
}
EXPORT_SYMBOL_GPL(rt_mutex_trylock);

/**
 * rt_mutex_unlock - unlock a rt_mutex
 *
 * @lock: the rt_mutex to be unlocked
 */
void __sched rt_mutex_unlock(struct rt_mutex *lock)
{
	rt_mutex_fastunlock(lock, rt_mutex_slowunlock);
}
EXPORT_SYMBOL_GPL(rt_mutex_unlock);

/**
 * Futex variant, that since futex variants do not use the fast-path, can be
 * simple and will not need to retry.
 */
bool __sched __rt_mutex_futex_unlock(struct rt_mutex *lock,
				     struct wake_q_head *wake_q,
				     struct wake_q_head *wq_sleeper)
{
	lockdep_assert_held(&lock->wait_lock);

	debug_rt_mutex_unlock(lock);

	if (!rt_mutex_has_waiters(lock)) {
		lock->owner = NULL;
		return false; /* done */
	}

	mark_wakeup_next_waiter(wake_q, wq_sleeper, lock);
	return true; /* deboost and wakeups */
}

void __sched rt_mutex_futex_unlock(struct rt_mutex *lock)
{
	WAKE_Q(wake_q);
	WAKE_Q(wake_sleeper_q);
	bool deboost;

 	raw_spin_lock_irq(&lock->wait_lock);
	deboost = __rt_mutex_futex_unlock(lock, &wake_q, &wake_sleeper_q);
	raw_spin_unlock_irq(&lock->wait_lock);

	if (deboost)
		rt_mutex_slowunlock(lock, &wake_q, &wake_sleeper_q);
}

/**
 * rt_mutex_destroy - mark a mutex unusable
 * @lock: the mutex to be destroyed
 *
 * This function marks the mutex uninitialized, and any subsequent
 * use of the mutex is forbidden. The mutex must not be locked when
 * this function is called.
 */
void rt_mutex_destroy(struct rt_mutex *lock)
{
	WARN_ON(rt_mutex_is_locked(lock));
#ifdef CONFIG_DEBUG_RT_MUTEXES
	lock->magic = NULL;
#endif
}

EXPORT_SYMBOL_GPL(rt_mutex_destroy);

/**
 * __rt_mutex_init - initialize the rt lock
 *
 * @lock: the rt lock to be initialized
 *
 * Initialize the rt lock to unlocked state.
 *
 * Initializing of a locked rt lock is not allowed
 */
void __rt_mutex_init(struct rt_mutex *lock, const char *name)
{
	lock->owner = NULL;
	raw_spin_lock_init(&lock->wait_lock);
	lock->waiters = RB_ROOT;
	lock->waiters_leftmost = NULL;

	debug_rt_mutex_init(lock, name);
}
EXPORT_SYMBOL(__rt_mutex_init);

/**
 * rt_mutex_init_proxy_locked - initialize and lock a rt_mutex on behalf of a
 *				proxy owner
 *
 * @lock: 	the rt_mutex to be locked
 * @proxy_owner:the task to set as owner
 *
 * No locking. Caller has to do serializing itself
 * Special API call for PI-futex support
 */
void rt_mutex_init_proxy_locked(struct rt_mutex *lock,
				struct task_struct *proxy_owner)
{
	rt_mutex_init(lock);
	debug_rt_mutex_proxy_lock(lock, proxy_owner);
	rt_mutex_set_owner(lock, proxy_owner);
}

/**
 * rt_mutex_proxy_unlock - release a lock on behalf of owner
 *
 * @lock: 	the rt_mutex to be locked
 *
 * No locking. Caller has to do serializing itself
 * Special API call for PI-futex support
 */
void rt_mutex_proxy_unlock(struct rt_mutex *lock)
{
	debug_rt_mutex_proxy_unlock(lock);
	rt_mutex_set_owner(lock, NULL);
}

/**
 * __rt_mutex_start_proxy_lock() - Start lock acquisition for another task
 * @lock:		the rt_mutex to take
 * @waiter:		the pre-initialized rt_mutex_waiter
 * @task:		the task to prepare
 *
 * Starts the rt_mutex acquire; it enqueues the @waiter and does deadlock
 * detection. It does not wait, see rt_mutex_wait_proxy_lock() for that.
 *
 * NOTE: does _NOT_ remove the @waiter on failure; must either call
 * rt_mutex_wait_proxy_lock() or rt_mutex_cleanup_proxy_lock() after this.
 *
 * Returns:
 *  0 - task blocked on lock
 *  1 - acquired the lock for task, caller should wake it up
 * <0 - error
 *
 * Special API call for PI-futex support.
 */
int __rt_mutex_start_proxy_lock(struct rt_mutex *lock,
			      struct rt_mutex_waiter *waiter,
			      struct task_struct *task)
{
	int ret;

	lockdep_assert_held(&lock->wait_lock);

	if (try_to_take_rt_mutex(lock, task, NULL))
		return 1;

#ifdef CONFIG_PREEMPT_RT_FULL
	/*
	 * In PREEMPT_RT there's an added race.
	 * If the task, that we are about to requeue, times out,
	 * it can set the PI_WAKEUP_INPROGRESS. This tells the requeue
	 * to skip this task. But right after the task sets
	 * its pi_blocked_on to PI_WAKEUP_INPROGRESS it can then
	 * block on the spin_lock(&hb->lock), which in RT is an rtmutex.
	 * This will replace the PI_WAKEUP_INPROGRESS with the actual
	 * lock that it blocks on. We *must not* place this task
	 * on this proxy lock in that case.
	 *
	 * To prevent this race, we first take the task's pi_lock
	 * and check if it has updated its pi_blocked_on. If it has,
	 * we assume that it woke up and we return -EAGAIN.
	 * Otherwise, we set the task's pi_blocked_on to
	 * PI_REQUEUE_INPROGRESS, so that if the task is waking up
	 * it will know that we are in the process of requeuing it.
	 */
	raw_spin_lock(&task->pi_lock);
	if (task->pi_blocked_on) {
		raw_spin_unlock(&task->pi_lock);
		return -EAGAIN;
	}
	task->pi_blocked_on = PI_REQUEUE_INPROGRESS;
	raw_spin_unlock(&task->pi_lock);
#endif

	/* We enforce deadlock detection for futexes */
	ret = task_blocks_on_rt_mutex(lock, waiter, task,
				      RT_MUTEX_FULL_CHAINWALK);

	if (ret && !rt_mutex_owner(lock)) {
		/*
		 * Reset the return value. We might have
		 * returned with -EDEADLK and the owner
		 * released the lock while we were walking the
		 * pi chain.  Let the waiter sort it out.
		 */
		ret = 0;
	}

	debug_rt_mutex_print_deadlock(waiter);

	return ret;
}

/**
 * rt_mutex_start_proxy_lock() - Start lock acquisition for another task
 * @lock:		the rt_mutex to take
 * @waiter:		the pre-initialized rt_mutex_waiter
 * @task:		the task to prepare
 *
 * Starts the rt_mutex acquire; it enqueues the @waiter and does deadlock
 * detection. It does not wait, see rt_mutex_wait_proxy_lock() for that.
 *
 * NOTE: unlike __rt_mutex_start_proxy_lock this _DOES_ remove the @waiter
 * on failure.
 *
 * Returns:
 *  0 - task blocked on lock
 *  1 - acquired the lock for task, caller should wake it up
 * <0 - error
 *
 * Special API call for PI-futex support.
 */
int rt_mutex_start_proxy_lock(struct rt_mutex *lock,
			      struct rt_mutex_waiter *waiter,
			      struct task_struct *task)
{
	int ret;

	raw_spin_lock_irq(&lock->wait_lock);
	ret = __rt_mutex_start_proxy_lock(lock, waiter, task);
	if (ret && rt_mutex_has_waiters(lock))
		remove_waiter(lock, waiter);
	raw_spin_unlock_irq(&lock->wait_lock);

	return ret;
}

/**
 * rt_mutex_next_owner - return the next owner of the lock
 *
 * @lock: the rt lock query
 *
 * Returns the next owner of the lock or NULL
 *
 * Caller has to serialize against other accessors to the lock
 * itself.
 *
 * Special API call for PI-futex support
 */
struct task_struct *rt_mutex_next_owner(struct rt_mutex *lock)
{
	if (!rt_mutex_has_waiters(lock))
		return NULL;

	return rt_mutex_top_waiter(lock)->task;
}

/**
 * rt_mutex_wait_proxy_lock() - Wait for lock acquisition
 * @lock:		the rt_mutex we were woken on
 * @to:			the timeout, null if none. hrtimer should already have
 *			been started.
 * @waiter:		the pre-initialized rt_mutex_waiter
 *
 * Wait for the the lock acquisition started on our behalf by
 * rt_mutex_start_proxy_lock(). Upon failure, the caller must call
 * rt_mutex_cleanup_proxy_lock().
 *
 * Returns:
 *  0 - success
 * <0 - error, one of -EINTR, -ETIMEDOUT
 *
 * Special API call for PI-futex support
 */
int rt_mutex_wait_proxy_lock(struct rt_mutex *lock,
			       struct hrtimer_sleeper *to,
			       struct rt_mutex_waiter *waiter)
{
	int ret;

	raw_spin_lock_irq(&lock->wait_lock);
	/* sleep on the mutex */
	set_current_state(TASK_INTERRUPTIBLE);
	ret = __rt_mutex_slowlock(lock, TASK_INTERRUPTIBLE, to, waiter, NULL);
	/*
	 * try_to_take_rt_mutex() sets the waiter bit unconditionally. We might
	 * have to fix that up.
	 */
	fixup_rt_mutex_waiters(lock);

	raw_spin_unlock_irq(&lock->wait_lock);

	return ret;
}

/**
 * rt_mutex_cleanup_proxy_lock() - Cleanup failed lock acquisition
 * @lock:		the rt_mutex we were woken on
 * @waiter:		the pre-initialized rt_mutex_waiter
 *
 * Attempt to clean up after a failed rt_mutex_wait_proxy_lock().
 *
 * Unless we acquired the lock; we're still enqueued on the wait-list and can
 * in fact still be granted ownership until we're removed. Therefore we can
 * find we are in fact the owner and must disregard the
 * rt_mutex_wait_proxy_lock() failure.
 *
 * Returns:
 *  true  - did the cleanup, we done.
 *  false - we acquired the lock after rt_mutex_wait_proxy_lock() returned,
 *          caller should disregards its return value.
 *
 * Special API call for PI-futex support
 */
bool rt_mutex_cleanup_proxy_lock(struct rt_mutex *lock,
				 struct rt_mutex_waiter *waiter)
{
	bool cleanup = false;

	raw_spin_lock_irq(&lock->wait_lock);
	/*
	 * Do an unconditional try-lock, this deals with the lock stealing
	 * state where __rt_mutex_futex_unlock() -> mark_wakeup_next_waiter()
	 * sets a NULL owner.
	 *
	 * We're not interested in the return value, because the subsequent
	 * test on rt_mutex_owner() will infer that. If the trylock succeeded,
	 * we will own the lock and it will have removed the waiter. If we
	 * failed the trylock, we're still not owner and we need to remove
	 * ourselves.
	 */
	try_to_take_rt_mutex(lock, current, waiter);
	/*
	 * Unless we're the owner; we're still enqueued on the wait_list.
	 * So check if we became owner, if not, take us off the wait_list.
	 */
	if (rt_mutex_owner(lock) != current) {
		remove_waiter(lock, waiter);
		cleanup = true;
	}
	/*
	 * try_to_take_rt_mutex() sets the waiter bit unconditionally. We might
	 * have to fix that up.
	 */
	fixup_rt_mutex_waiters(lock);

	raw_spin_unlock_irq(&lock->wait_lock);

	return cleanup;
}

static inline int
ww_mutex_deadlock_injection(struct ww_mutex *lock, struct ww_acquire_ctx *ctx)
{
#ifdef CONFIG_DEBUG_WW_MUTEX_SLOWPATH
	unsigned tmp;

	if (ctx->deadlock_inject_countdown-- == 0) {
		tmp = ctx->deadlock_inject_interval;
		if (tmp > UINT_MAX/4)
			tmp = UINT_MAX;
		else
			tmp = tmp*2 + tmp + tmp/2;

		ctx->deadlock_inject_interval = tmp;
		ctx->deadlock_inject_countdown = tmp;
		ctx->contending_lock = lock;

		ww_mutex_unlock(lock);

		return -EDEADLK;
	}
#endif

	return 0;
}

#ifdef CONFIG_PREEMPT_RT_FULL
int __sched
__ww_mutex_lock_interruptible(struct ww_mutex *lock, struct ww_acquire_ctx *ww_ctx)
{
	int ret;

	might_sleep();

	mutex_acquire_nest(&lock->base.dep_map, 0, 0, &ww_ctx->dep_map, _RET_IP_);
	ret = rt_mutex_slowlock(&lock->base.lock, TASK_INTERRUPTIBLE, NULL, 0, ww_ctx);
	if (ret)
		mutex_release(&lock->base.dep_map, 1, _RET_IP_);
	else if (!ret && ww_ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ww_ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(__ww_mutex_lock_interruptible);

int __sched
__ww_mutex_lock(struct ww_mutex *lock, struct ww_acquire_ctx *ww_ctx)
{
	int ret;

	might_sleep();

	mutex_acquire_nest(&lock->base.dep_map, 0, 0, &ww_ctx->dep_map, _RET_IP_);
	ret = rt_mutex_slowlock(&lock->base.lock, TASK_UNINTERRUPTIBLE, NULL, 0, ww_ctx);
	if (ret)
		mutex_release(&lock->base.dep_map, 1, _RET_IP_);
	else if (!ret && ww_ctx->acquired > 1)
		return ww_mutex_deadlock_injection(lock, ww_ctx);

	return ret;
}
EXPORT_SYMBOL_GPL(__ww_mutex_lock);

void __sched ww_mutex_unlock(struct ww_mutex *lock)
{
	int nest = !!lock->ctx;

	/*
	 * The unlocking fastpath is the 0->1 transition from 'locked'
	 * into 'unlocked' state:
	 */
	if (nest) {
#ifdef CONFIG_DEBUG_MUTEXES
		DEBUG_LOCKS_WARN_ON(!lock->ctx->acquired);
#endif
		if (lock->ctx->acquired > 0)
			lock->ctx->acquired--;
		lock->ctx = NULL;
	}

	mutex_release(&lock->base.dep_map, nest, _RET_IP_);
	rt_mutex_unlock(&lock->base.lock);
}
EXPORT_SYMBOL(ww_mutex_unlock);
#endif
