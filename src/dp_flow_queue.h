#ifndef FLOW_QUEUE_H
#define FLOW_QUEUE_H

#include "flow.h"

/** Spinlocks or Mutex for the flow queues. */
//#define FQLOCK_SPIN
#define FQLOCK_MUTEX

#ifdef FQLOCK_SPIN
    #ifdef FQLOCK_MUTEX
        #error Cannot enable both FQLOCK_SPIN and FQLOCK_MUTEX
    #endif
#endif

/* Define a queue for storing flows */
typedef struct FlowQueue_
{
    Flow *top;
    Flow *bot;
    uint32_t len;
#ifdef DBG_PERF
    uint32_t dbg_maxlen;
#endif /* DBG_PERF */
#ifdef FQLOCK_MUTEX
    SCMutex m;
#elif defined FQLOCK_SPIN
    SCSpinlock s;
#else
    #error Enable FQLOCK_SPIN or FQLOCK_MUTEX
#endif
} FlowQueue;

#ifdef FQLOCK_SPIN
    #define FQLOCK_INIT(q) SCSpinInit(&(q)->s, 0)
    #define FQLOCK_DESTROY(q) SCSpinDestroy(&(q)->s)
    #define FQLOCK_LOCK(q) SCSpinLock(&(q)->s)
    #define FQLOCK_TRYLOCK(q) SCSpinTrylock(&(q)->s)
    #define FQLOCK_UNLOCK(q) SCSpinUnlock(&(q)->s)
#elif defined FQLOCK_MUTEX
    #define FQLOCK_INIT(q) SCMutexInit(&(q)->m, NULL)
    #define FQLOCK_DESTROY(q) SCMutexDestroy(&(q)->m)
    #define FQLOCK_LOCK(q) SCMutexLock(&(q)->m)
    #define FQLOCK_TRYLOCK(q) SCMutexTrylock(&(q)->m)
    #define FQLOCK_UNLOCK(q) SCMutexUnlock(&(q)->m)
#else
    #error Enable FQLOCK_SPIN or FQLOCK_MUTEX
#endif

/** spare/unused/prealloced flows live here */
FlowQueue flow_spare_q;

/** queue to pass flows to cleanup/log thread(s) */
FlowQueue flow_recycle_q;

/* prototypes */
FlowQueue *FlowQueueNew(void);
FlowQueue *FlowQueueInit(FlowQueue *);
void FlowQueueDestroy (FlowQueue *);

void FlowEnqueue (FlowQueue *, Flow *);
Flow *FlowDequeue (FlowQueue *);

void FlowMoveToSpare(Flow *);

#endif
