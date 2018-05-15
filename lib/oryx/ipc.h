#ifndef __IPC_H__
#define __IPC_H__

#ifdef HAVE_APR
#define INIT_MUTEX_VAL	{NULL, PTHREAD_MUTEX_INITIALIZER}
#define INIT_COND_VAL	{NULL, PTHREAD_COND_INITIALIZER}
#define oryx_thread_mutex_t	apr_pthread_mutex_t
#define oryx_thread_cond_t	apr_pthread_cond_t

#define thread_region_lock(lock)	apr_thread_mutex_lock(lock)
#define thread_region_unlock(lock)	apr_thread_mutex_unlock(lock)
#define thread_region_trylock(lock) apr_thread_mutex_trylock(lock)
#define thread_destroy_lock(lock)	apr_thread_mutex_destroy(lock)

#else
#define INIT_MUTEX_VAL	PTHREAD_MUTEX_INITIALIZER
#define INIT_COND_VAL	PTHREAD_COND_INITIALIZER
#define oryx_thread_mutex_t	pthread_mutex_t
#define oryx_thread_cond_t	pthread_cond_t

#define thread_region_lock(lock)	pthread_mutex_lock(lock)
#define thread_region_unlock(lock)	pthread_mutex_unlock(lock)
#define thread_region_trylock(lock) pthread_mutex_trylock(lock)
#define thread_destroy_lock(lock)	pthread_mutex_destroy(lock)

#define thread_cond_signal(s) pthread_cond_signal(s)
#define thread_cond_wait(s,m) pthread_cond_wait(s,m)
#endif

#define INIT_MUTEX(name)\
    oryx_thread_mutex_t name = INIT_MUTEX_VAL;

#define INIT_COND(name)\
    oryx_thread_cond_t name = INIT_COND_VAL;

extern oryx_status_t oryx_thread_mutex_create(oryx_thread_mutex_t **mptr);
extern oryx_status_t oryx_thread_cond_create(oryx_thread_cond_t **cond);
extern oryx_status_t oryx_thread_mutex_destroy(oryx_thread_mutex_t *mptr);
static __oryx_always_inline__
oryx_status_t oryx_thread_mutex_lock(oryx_thread_mutex_t *mptr) { return thread_region_lock(mptr); }
static __oryx_always_inline__
oryx_status_t oryx_thread_mutex_trylock(oryx_thread_mutex_t *mptr) { return thread_region_trylock(mptr); }
static __oryx_always_inline__
oryx_status_t oryx_thread_mutex_unlock(oryx_thread_mutex_t *mptr) { return thread_region_unlock(mptr); }

static __oryx_always_inline__
oryx_status_t oryx_thread_cond_signal(oryx_thread_cond_t *cptr) { return thread_cond_signal(cptr); }
static __oryx_always_inline__
oryx_status_t oryx_thread_cond_wait(oryx_thread_cond_t *cptr, oryx_thread_mutex_t *mptr) { return thread_cond_wait(cptr, mptr); }


typedef oryx_thread_mutex_t	os_lock_t;
typedef oryx_thread_cond_t	os_cond_t;

#define do_lock(lock)\
	oryx_thread_mutex_lock(lock)
#define do_unlock(lock)\
	oryx_thread_mutex_unlock(lock)

/* mutex */
#define SCMutex pthread_mutex_t
#define SCMutexAttr pthread_mutexattr_t
#define SCMutexInit(mut, mutattr ) pthread_mutex_init(mut, mutattr)
#define SCMutexLock(mut) pthread_mutex_lock(mut)
#define SCMutexTrylock(mut) pthread_mutex_trylock(mut)
#define SCMutexUnlock(mut) pthread_mutex_unlock(mut)
#define SCMutexDestroy pthread_mutex_destroy
#define SCMUTEX_INITIALIZER PTHREAD_MUTEX_INITIALIZER


/* conditions */

#define SCCondT pthread_cond_t
#define SCCondInit pthread_cond_init
#define SCCondSignal pthread_cond_signal
#define SCCondDestroy pthread_cond_destroy
#define SCCondWait(cond, mut) pthread_cond_wait(cond, mut)

/* spinlocks */

extern __thread uint64_t spin_lock_contention;
extern __thread uint64_t spin_lock_wait_ticks;
extern __thread uint64_t spin_lock_cnt;


/* rwlocks */
#define SCRWLock pthread_rwlock_t
#define SCRWLockInit(rwl, rwlattr ) pthread_rwlock_init(rwl, rwlattr)
#define SCRWLockWRLock(rwl) pthread_rwlock_wrlock(rwl)
#define SCRWLockRDLock(rwl) pthread_rwlock_rdlock(rwl)
#define SCRWLockTryWRLock(rwl) pthread_rwlock_trywrlock(rwl)
#define SCRWLockTryRDLock(rwl) pthread_rwlock_tryrdlock(rwl)
#define SCRWLockUnlock(rwl) pthread_rwlock_unlock(rwl)
#define SCRWLockDestroy pthread_rwlock_destroy

/* ctrl mutex */
#define SCCtrlMutex pthread_mutex_t
#define SCCtrlMutexAttr pthread_mutexattr_t
#define SCCtrlMutexInit(mut, mutattr ) pthread_mutex_init(mut, mutattr)
#define SCCtrlMutexLock(mut) pthread_mutex_lock(mut)
#define SCCtrlMutexTrylock(mut) pthread_mutex_trylock(mut)
#define SCCtrlMutexUnlock(mut) pthread_mutex_unlock(mut)
#define SCCtrlMutexDestroy pthread_mutex_destroy

/* ctrl conditions */
#define SCCtrlCondT pthread_cond_t
#define SCCtrlCondInit pthread_cond_init
#define SCCtrlCondSignal pthread_cond_signal
#define SCCtrlCondTimedwait pthread_cond_timedwait
#define SCCtrlCondWait pthread_cond_wait
#define SCCtrlCondDestroy pthread_cond_destroy

/* spinlocks */
#if ((_POSIX_SPIN_LOCKS - 200112L) < 0L) || defined HELGRIND
#define SCSpinlock                              SCMutex
#define SCSpinLock(spin)                        SCMutexLock((spin))
#define SCSpinTrylock(spin)                     SCMutexTrylock((spin))
#define SCSpinUnlock(spin)                      SCMutexUnlock((spin))
#define SCSpinInit(spin, spin_attr)             SCMutexInit((spin), NULL)
#define SCSpinDestroy(spin)                     SCMutexDestroy((spin))
#else /* no spinlocks */
#define SCSpinlock                              pthread_spinlock_t
#define SCSpinLock(spin)                        pthread_spin_lock(spin)
#define SCSpinTrylock(spin)                     pthread_spin_trylock(spin)
#define SCSpinUnlock(spin)                      pthread_spin_unlock(spin)
#define SCSpinInit(spin, spin_attr)             pthread_spin_init(spin, spin_attr)
#define SCSpinDestroy(spin)                     pthread_spin_destroy(spin)
#endif /* no spinlocks */


#endif
