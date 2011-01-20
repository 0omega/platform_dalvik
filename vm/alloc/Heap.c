/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Garbage-collecting memory allocator.
 */
#include "Dalvik.h"
#include "alloc/HeapBitmap.h"
#include "alloc/Verify.h"
#include "alloc/HeapTable.h"
#include "alloc/Heap.h"
#include "alloc/HeapInternal.h"
#include "alloc/DdmHeap.h"
#include "alloc/HeapSource.h"
#include "alloc/MarkSweep.h"

#include "utils/threads.h"      // need Android thread priorities
#define kInvalidPriority        10000

#include <cutils/sched_policy.h>

#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include <errno.h>

static const char* GcReasonStr[] = {
    [GC_FOR_MALLOC] = "GC_FOR_MALLOC",
    [GC_CONCURRENT] = "GC_CONCURRENT",
    [GC_EXPLICIT] = "GC_EXPLICIT"
};

/*
 * Initialize the GC heap.
 *
 * Returns true if successful, false otherwise.
 */
bool dvmHeapStartup()
{
    GcHeap *gcHeap;

    if (gDvm.heapGrowthLimit == 0) {
        gDvm.heapGrowthLimit = gDvm.heapMaximumSize;
    }

    gcHeap = dvmHeapSourceStartup(gDvm.heapStartingSize,
                                  gDvm.heapMaximumSize,
                                  gDvm.heapGrowthLimit);
    if (gcHeap == NULL) {
        return false;
    }
    gcHeap->heapWorkerCurrentObject = NULL;
    gcHeap->heapWorkerCurrentMethod = NULL;
    gcHeap->heapWorkerInterpStartTime = 0LL;
    gcHeap->ddmHpifWhen = 0;
    gcHeap->ddmHpsgWhen = 0;
    gcHeap->ddmHpsgWhat = 0;
    gcHeap->ddmNhsgWhen = 0;
    gcHeap->ddmNhsgWhat = 0;
    gDvm.gcHeap = gcHeap;

    /* Set up the lists and lock we'll use for finalizable
     * and reference objects.
     */
    dvmInitMutex(&gDvm.heapWorkerListLock);
    gcHeap->finalizableRefs = NULL;
    gcHeap->pendingFinalizationRefs = NULL;
    gcHeap->referenceOperations = NULL;

    if (!dvmCardTableStartup(gDvm.heapMaximumSize)) {
        LOGE_HEAP("card table startup failed.");
        return false;
    }

    /* Initialize the HeapWorker locks and other state
     * that the GC uses.
     */
    dvmInitializeHeapWorkerState();

    return true;
}

bool dvmHeapStartupAfterZygote(void)
{
    return dvmHeapSourceStartupAfterZygote();
}

void dvmHeapShutdown()
{
//TODO: make sure we're locked
    if (gDvm.gcHeap != NULL) {
        dvmCardTableShutdown();
         /* Tables are allocated on the native heap; they need to be
         * cleaned up explicitly.  The process may stick around, so we
         * don't want to leak any native memory.
         */
        dvmHeapFreeLargeTable(gDvm.gcHeap->finalizableRefs);
        gDvm.gcHeap->finalizableRefs = NULL;

        dvmHeapFreeLargeTable(gDvm.gcHeap->pendingFinalizationRefs);
        gDvm.gcHeap->pendingFinalizationRefs = NULL;

        dvmHeapFreeLargeTable(gDvm.gcHeap->referenceOperations);
        gDvm.gcHeap->referenceOperations = NULL;

        /* Destroy the heap.  Any outstanding pointers will point to
         * unmapped memory (unless/until someone else maps it).  This
         * frees gDvm.gcHeap as a side-effect.
         */
        dvmHeapSourceShutdown(&gDvm.gcHeap);
    }
}

/*
 * Shutdown any threads internal to the heap.
 */
void dvmHeapThreadShutdown(void)
{
    dvmHeapSourceThreadShutdown();
}

/*
 * We've been asked to allocate something we can't, e.g. an array so
 * large that (length * elementWidth) is larger than 2^31.
 *
 * _The Java Programming Language_, 4th edition, says, "you can be sure
 * that all SoftReferences to softly reachable objects will be cleared
 * before an OutOfMemoryError is thrown."
 *
 * It's unclear whether that holds for all situations where an OOM can
 * be thrown, or just in the context of an allocation that fails due
 * to lack of heap space.  For simplicity we just throw the exception.
 *
 * (OOM due to actually running out of space is handled elsewhere.)
 */
void dvmThrowBadAllocException(const char* msg)
{
    dvmThrowException("Ljava/lang/OutOfMemoryError;", msg);
}

/*
 * Grab the lock, but put ourselves into THREAD_VMWAIT if it looks like
 * we're going to have to wait on the mutex.
 */
bool dvmLockHeap()
{
    if (dvmTryLockMutex(&gDvm.gcHeapLock) != 0) {
        Thread *self;
        ThreadStatus oldStatus;

        self = dvmThreadSelf();
        oldStatus = dvmChangeStatus(self, THREAD_VMWAIT);
        dvmLockMutex(&gDvm.gcHeapLock);
        dvmChangeStatus(self, oldStatus);
    }

    return true;
}

void dvmUnlockHeap()
{
    dvmUnlockMutex(&gDvm.gcHeapLock);
}

/* Pop an object from the list of pending finalizations and
 * reference clears/enqueues, and return the object.
 * The caller must call dvmReleaseTrackedAlloc()
 * on the object when finished.
 *
 * Typically only called by the heap worker thread.
 */
Object *dvmGetNextHeapWorkerObject(HeapWorkerOperation *op)
{
    Object *obj;
    GcHeap *gcHeap = gDvm.gcHeap;

    assert(op != NULL);

    dvmLockMutex(&gDvm.heapWorkerListLock);

    obj = dvmHeapGetNextObjectFromLargeTable(&gcHeap->referenceOperations);
    if (obj != NULL) {
        *op = WORKER_ENQUEUE;
    } else {
        obj = dvmHeapGetNextObjectFromLargeTable(
                &gcHeap->pendingFinalizationRefs);
        if (obj != NULL) {
            *op = WORKER_FINALIZE;
        }
    }

    if (obj != NULL) {
        /* Don't let the GC collect the object until the
         * worker thread is done with it.
         */
        dvmAddTrackedAlloc(obj, NULL);
    }

    dvmUnlockMutex(&gDvm.heapWorkerListLock);

    return obj;
}

/* Do a full garbage collection, which may grow the
 * heap as a side-effect if the live set is large.
 */
static void gcForMalloc(bool collectSoftReferences)
{
    if (gDvm.allocProf.enabled) {
        Thread* self = dvmThreadSelf();
        gDvm.allocProf.gcCount++;
        if (self != NULL) {
            self->allocProf.gcCount++;
        }
    }
    /* This may adjust the soft limit as a side-effect.
     */
    LOGD_HEAP("dvmMalloc initiating GC%s\n",
            collectSoftReferences ? "(collect SoftReferences)" : "");
    dvmCollectGarbageInternal(collectSoftReferences, GC_FOR_MALLOC);
}

/* Try as hard as possible to allocate some memory.
 */
static void *tryMalloc(size_t size)
{
    void *ptr;

    /* Don't try too hard if there's no way the allocation is
     * going to succeed.  We have to collect SoftReferences before
     * throwing an OOME, though.
     */
    if (size >= gDvm.heapGrowthLimit) {
        LOGW_HEAP("dvmMalloc(%zu/0x%08zx): "
                "someone's allocating a huge buffer\n", size, size);
        ptr = NULL;
        goto collect_soft_refs;
    }

//TODO: figure out better heuristics
//    There will be a lot of churn if someone allocates a bunch of
//    big objects in a row, and we hit the frag case each time.
//    A full GC for each.
//    Maybe we grow the heap in bigger leaps
//    Maybe we skip the GC if the size is large and we did one recently
//      (number of allocations ago) (watch for thread effects)
//    DeflateTest allocs a bunch of ~128k buffers w/in 0-5 allocs of each other
//      (or, at least, there are only 0-5 objects swept each time)

    ptr = dvmHeapSourceAlloc(size);
    if (ptr != NULL) {
        return ptr;
    }

    /*
     * The allocation failed.  If the GC is running, block until it
     * completes and retry.
     */
    if (gDvm.gcHeap->gcRunning) {
        /*
         * The GC is concurrently tracing the heap.  Release the heap
         * lock, wait for the GC to complete, and retrying allocating.
         */
        dvmWaitForConcurrentGcToComplete();
        ptr = dvmHeapSourceAlloc(size);
        if (ptr != NULL) {
            return ptr;
        }
    }
    /*
     * Another failure.  Our thread was starved or there may be too
     * many live objects.  Try a foreground GC.  This will have no
     * effect if the concurrent GC is already running.
     */
    gcForMalloc(false);
    ptr = dvmHeapSourceAlloc(size);
    if (ptr != NULL) {
        return ptr;
    }

    /* Even that didn't work;  this is an exceptional state.
     * Try harder, growing the heap if necessary.
     */
    ptr = dvmHeapSourceAllocAndGrow(size);
    if (ptr != NULL) {
        size_t newHeapSize;

        newHeapSize = dvmHeapSourceGetIdealFootprint();
//TODO: may want to grow a little bit more so that the amount of free
//      space is equal to the old free space + the utilization slop for
//      the new allocation.
        LOGI_HEAP("Grow heap (frag case) to "
                "%zu.%03zuMB for %zu-byte allocation\n",
                FRACTIONAL_MB(newHeapSize), size);
        return ptr;
    }

    /* Most allocations should have succeeded by now, so the heap
     * is really full, really fragmented, or the requested size is
     * really big.  Do another GC, collecting SoftReferences this
     * time.  The VM spec requires that all SoftReferences have
     * been collected and cleared before throwing an OOME.
     */
//TODO: wait for the finalizers from the previous GC to finish
collect_soft_refs:
    LOGI_HEAP("Forcing collection of SoftReferences for %zu-byte allocation\n",
            size);
    gcForMalloc(true);
    ptr = dvmHeapSourceAllocAndGrow(size);
    if (ptr != NULL) {
        return ptr;
    }
//TODO: maybe wait for finalizers and try one last time

    LOGE_HEAP("Out of memory on a %zd-byte allocation.\n", size);
//TODO: tell the HeapSource to dump its state
    dvmDumpThread(dvmThreadSelf(), false);

    return NULL;
}

/* Throw an OutOfMemoryError if there's a thread to attach it to.
 * Avoid recursing.
 *
 * The caller must not be holding the heap lock, or else the allocations
 * in dvmThrowException() will deadlock.
 */
static void throwOOME()
{
    Thread *self;

    if ((self = dvmThreadSelf()) != NULL) {
        /* If the current (failing) dvmMalloc() happened as part of thread
         * creation/attachment before the thread became part of the root set,
         * we can't rely on the thread-local trackedAlloc table, so
         * we can't keep track of a real allocated OOME object.  But, since
         * the thread is in the process of being created, it won't have
         * a useful stack anyway, so we may as well make things easier
         * by throwing the (stackless) pre-built OOME.
         */
        if (dvmIsOnThreadList(self) && !self->throwingOOME) {
            /* Let ourselves know that we tried to throw an OOM
             * error in the normal way in case we run out of
             * memory trying to allocate it inside dvmThrowException().
             */
            self->throwingOOME = true;

            /* Don't include a description string;
             * one fewer allocation.
             */
            dvmThrowException("Ljava/lang/OutOfMemoryError;", NULL);
        } else {
            /*
             * This thread has already tried to throw an OutOfMemoryError,
             * which probably means that we're running out of memory
             * while recursively trying to throw.
             *
             * To avoid any more allocation attempts, "throw" a pre-built
             * OutOfMemoryError object (which won't have a useful stack trace).
             *
             * Note that since this call can't possibly allocate anything,
             * we don't care about the state of self->throwingOOME
             * (which will usually already be set).
             */
            dvmSetException(self, gDvm.outOfMemoryObj);
        }
        /* We're done with the possible recursion.
         */
        self->throwingOOME = false;
    }
}

/*
 * Allocate storage on the GC heap.  We guarantee 8-byte alignment.
 *
 * The new storage is zeroed out.
 *
 * Note that, in rare cases, this could get called while a GC is in
 * progress.  If a non-VM thread tries to attach itself through JNI,
 * it will need to allocate some objects.  If this becomes annoying to
 * deal with, we can block it at the source, but holding the allocation
 * mutex should be enough.
 *
 * In rare circumstances (JNI AttachCurrentThread) we can be called
 * from a non-VM thread.
 *
 * Use ALLOC_DONT_TRACK when we either don't want to track an allocation
 * (because it's being done for the interpreter "new" operation and will
 * be part of the root set immediately) or we can't (because this allocation
 * is for a brand new thread).
 *
 * Returns NULL and throws an exception on failure.
 *
 * TODO: don't do a GC if the debugger thinks all threads are suspended
 */
void* dvmMalloc(size_t size, int flags)
{
    GcHeap *gcHeap = gDvm.gcHeap;
    void *ptr;

    dvmLockHeap();

    /* Try as hard as possible to allocate some memory.
     */
    ptr = tryMalloc(size);
    if (ptr != NULL) {
        /* We've got the memory.
         */
        if ((flags & ALLOC_FINALIZABLE) != 0) {
            /* This object is an instance of a class that
             * overrides finalize().  Add it to the finalizable list.
             */
            if (!dvmHeapAddRefToLargeTable(&gcHeap->finalizableRefs,
                                    (Object *)ptr))
            {
                LOGE_HEAP("dvmMalloc(): no room for any more "
                        "finalizable objects\n");
                dvmAbort();
            }
        }

        if (gDvm.allocProf.enabled) {
            Thread* self = dvmThreadSelf();
            gDvm.allocProf.allocCount++;
            gDvm.allocProf.allocSize += size;
            if (self != NULL) {
                self->allocProf.allocCount++;
                self->allocProf.allocSize += size;
            }
        }
    } else {
        /* The allocation failed.
         */

        if (gDvm.allocProf.enabled) {
            Thread* self = dvmThreadSelf();
            gDvm.allocProf.failedAllocCount++;
            gDvm.allocProf.failedAllocSize += size;
            if (self != NULL) {
                self->allocProf.failedAllocCount++;
                self->allocProf.failedAllocSize += size;
            }
        }
    }

    dvmUnlockHeap();

    if (ptr != NULL) {
        /*
         * If caller hasn't asked us not to track it, add it to the
         * internal tracking list.
         */
        if ((flags & ALLOC_DONT_TRACK) == 0) {
            dvmAddTrackedAlloc((Object*)ptr, NULL);
        }
    } else {
        /*
         * The allocation failed; throw an OutOfMemoryError.
         */
        throwOOME();
    }

    return ptr;
}

/*
 * Returns true iff <obj> points to a valid allocated object.
 */
bool dvmIsValidObject(const Object* obj)
{
    /* Don't bother if it's NULL or not 8-byte aligned.
     */
    if (obj != NULL && ((uintptr_t)obj & (8-1)) == 0) {
        /* Even if the heap isn't locked, this shouldn't return
         * any false negatives.  The only mutation that could
         * be happening is allocation, which means that another
         * thread could be in the middle of a read-modify-write
         * to add a new bit for a new object.  However, that
         * RMW will have completed by the time any other thread
         * could possibly see the new pointer, so there is no
         * danger of dvmIsValidObject() being called on a valid
         * pointer whose bit isn't set.
         *
         * Freeing will only happen during the sweep phase, which
         * only happens while the heap is locked.
         */
        return dvmHeapSourceContains(obj);
    }
    return false;
}

size_t dvmObjectSizeInHeap(const Object *obj)
{
    return dvmHeapSourceChunkSize(obj);
}

static void verifyRootsAndHeap(void)
{
    dvmVerifyRoots();
    dvmVerifyBitmap(dvmHeapSourceGetLiveBits());
}

/*
 * Initiate garbage collection.
 *
 * NOTES:
 * - If we don't hold gDvm.threadListLock, it's possible for a thread to
 *   be added to the thread list while we work.  The thread should NOT
 *   start executing, so this is only interesting when we start chasing
 *   thread stacks.  (Before we do so, grab the lock.)
 *
 * We are not allowed to GC when the debugger has suspended the VM, which
 * is awkward because debugger requests can cause allocations.  The easiest
 * way to enforce this is to refuse to GC on an allocation made by the
 * JDWP thread -- we have to expand the heap or fail.
 */
void dvmCollectGarbageInternal(bool clearSoftRefs, GcReason reason)
{
    GcHeap *gcHeap = gDvm.gcHeap;
    u4 rootSuspend, rootSuspendTime, rootStart, rootEnd;
    u4 dirtySuspend, dirtyStart, dirtyEnd;
    u4 totalTime;
    size_t numObjectsFreed, numBytesFreed;
    size_t currAllocated, currFootprint;
    size_t percentFree;
    GcMode gcMode;
    int oldThreadPriority = kInvalidPriority;

    /* The heap lock must be held.
     */

    if (gcHeap->gcRunning) {
        LOGW_HEAP("Attempted recursive GC\n");
        return;
    }

    gcMode = (reason == GC_FOR_MALLOC) ? GC_PARTIAL : GC_FULL;
    gcHeap->gcRunning = true;

    /*
     * Grab the heapWorkerLock to prevent the HeapWorker thread from
     * doing work.  If it's executing a finalizer or an enqueue operation
     * it won't be holding the lock, so this should return quickly.
     */
    dvmLockMutex(&gDvm.heapWorkerLock);

    rootSuspend = dvmGetRelativeTimeMsec();
    dvmSuspendAllThreads(SUSPEND_FOR_GC);
    rootStart = dvmGetRelativeTimeMsec();
    rootSuspendTime = rootStart - rootSuspend;

    /*
     * If we are not marking concurrently raise the priority of the
     * thread performing the garbage collection.
     */
    if (reason != GC_CONCURRENT) {
        /* Get the priority (the "nice" value) of the current thread.  The
         * getpriority() call can legitimately return -1, so we have to
         * explicitly test errno.
         */
        errno = 0;
        int priorityResult = getpriority(PRIO_PROCESS, 0);
        if (errno != 0) {
            LOGI_HEAP("getpriority(self) failed: %s\n", strerror(errno));
        } else if (priorityResult > ANDROID_PRIORITY_NORMAL) {
            /* Current value is numerically greater than "normal", which
             * in backward UNIX terms means lower priority.
             */

            if (priorityResult >= ANDROID_PRIORITY_BACKGROUND) {
                set_sched_policy(dvmGetSysThreadId(), SP_FOREGROUND);
            }

            if (setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_NORMAL) != 0) {
                LOGI_HEAP("Unable to elevate priority from %d to %d\n",
                          priorityResult, ANDROID_PRIORITY_NORMAL);
            } else {
                /* priority elevated; save value so we can restore it later */
                LOGD_HEAP("Elevating priority from %d to %d\n",
                          priorityResult, ANDROID_PRIORITY_NORMAL);
                oldThreadPriority = priorityResult;
            }
        }
    }

    /* Make sure that the HeapWorker thread hasn't become
     * wedged inside interp code.  If it has, this call will
     * print a message and abort the VM.
     */
    dvmAssertHeapWorkerThreadRunning();

    /* Lock the pendingFinalizationRefs list.
     *
     * Acquire the lock after suspending so the finalizer
     * thread can't block in the RUNNING state while
     * we try to suspend.
     */
    dvmLockMutex(&gDvm.heapWorkerListLock);

    if (gDvm.preVerify) {
        LOGV_HEAP("Verifying roots and heap before GC");
        verifyRootsAndHeap();
    }

    dvmMethodTraceGCBegin();

    /* Set up the marking context.
     */
    if (!dvmHeapBeginMarkStep(gcMode)) {
        LOGE_HEAP("dvmHeapBeginMarkStep failed; aborting\n");
        dvmAbort();
    }

    /* Mark the set of objects that are strongly reachable from the roots.
     */
    LOGD_HEAP("Marking...");
    dvmHeapMarkRootSet();

    /* dvmHeapScanMarkedObjects() will build the lists of known
     * instances of the Reference classes.
     */
    gcHeap->softReferences = NULL;
    gcHeap->weakReferences = NULL;
    gcHeap->phantomReferences = NULL;

    if (reason == GC_CONCURRENT) {
        /*
         * Resume threads while tracing from the roots.  We unlock the
         * heap to allow mutator threads to allocate from free space.
         */
        rootEnd = dvmGetRelativeTimeMsec();
        dvmClearCardTable();
        dvmUnlockHeap();
        dvmResumeAllThreads(SUSPEND_FOR_GC);
    }

    /* Recursively mark any objects that marked objects point to strongly.
     * If we're not collecting soft references, soft-reachable
     * objects will also be marked.
     */
    LOGD_HEAP("Recursing...");
    dvmHeapScanMarkedObjects();

    if (reason == GC_CONCURRENT) {
        /*
         * Re-acquire the heap lock and perform the final thread
         * suspension.
         */
        dvmLockHeap();
        dirtySuspend = dvmGetRelativeTimeMsec();
        dvmSuspendAllThreads(SUSPEND_FOR_GC);
        dirtyStart = dvmGetRelativeTimeMsec();
        /*
         * As no barrier intercepts root updates, we conservatively
         * assume all roots may be gray and re-mark them.
         */
        dvmHeapReMarkRootSet();
        /*
         * With the exception of reference objects and weak interned
         * strings, all gray objects should now be on dirty cards.
         */
        if (gDvm.verifyCardTable) {
            dvmVerifyCardTable();
        }
        /*
         * Recursively mark gray objects pointed to by the roots or by
         * heap objects dirtied during the concurrent mark.
         */
        dvmHeapReScanMarkedObjects();
    }

    /*
     * All strongly-reachable objects have now been marked.  Process
     * weakly-reachable objects discovered while tracing.
     */
    dvmHeapProcessReferences(&gcHeap->softReferences, clearSoftRefs,
                             &gcHeap->weakReferences,
                             &gcHeap->phantomReferences);

#if defined(WITH_JIT)
    /*
     * Patching a chaining cell is very cheap as it only updates 4 words. It's
     * the overhead of stopping all threads and synchronizing the I/D cache
     * that makes it expensive.
     *
     * Therefore we batch those work orders in a queue and go through them
     * when threads are suspended for GC.
     */
    dvmCompilerPerformSafePointChecks();
#endif

    LOGD_HEAP("Sweeping...");

    dvmHeapSweepSystemWeaks();

    /*
     * Live objects have a bit set in the mark bitmap, swap the mark
     * and live bitmaps.  The sweep can proceed concurrently viewing
     * the new live bitmap as the old mark bitmap, and vice versa.
     */
    dvmHeapSourceSwapBitmaps();

    if (gDvm.postVerify) {
        LOGV_HEAP("Verifying roots and heap after GC");
        verifyRootsAndHeap();
    }

    if (reason == GC_CONCURRENT) {
        dirtyEnd = dvmGetRelativeTimeMsec();
        dvmUnlockHeap();
        dvmResumeAllThreads(SUSPEND_FOR_GC);
    }
    dvmHeapSweepUnmarkedObjects(gcMode, reason == GC_CONCURRENT,
                                &numObjectsFreed, &numBytesFreed);
    LOGD_HEAP("Cleaning up...");
    dvmHeapFinishMarkStep();
    if (reason == GC_CONCURRENT) {
        dvmLockHeap();
    }

    LOGD_HEAP("Done.");

    /* Now's a good time to adjust the heap size, since
     * we know what our utilization is.
     *
     * This doesn't actually resize any memory;
     * it just lets the heap grow more when necessary.
     */
    dvmHeapSourceGrowForUtilization();

    currAllocated = dvmHeapSourceGetValue(HS_BYTES_ALLOCATED, NULL, 0);
    currFootprint = dvmHeapSourceGetValue(HS_FOOTPRINT, NULL, 0);

    /* Now that we've freed up the GC heap, return any large
     * free chunks back to the system.  They'll get paged back
     * in the next time they're used.  Don't do it immediately,
     * though;  if the process is still allocating a bunch of
     * memory, we'll be taking a ton of page faults that we don't
     * necessarily need to.
     *
     * Cancel any old scheduled trims, and schedule a new one.
     */
    dvmScheduleHeapSourceTrim(5);  // in seconds

    dvmMethodTraceGCEnd();
    LOGV_HEAP("GC finished");

    gcHeap->gcRunning = false;

    LOGV_HEAP("Resuming threads");
    dvmUnlockMutex(&gDvm.heapWorkerListLock);
    dvmUnlockMutex(&gDvm.heapWorkerLock);

    if (reason == GC_CONCURRENT) {
        /*
         * Wake-up any threads that blocked after a failed allocation
         * request.
         */
        dvmBroadcastCond(&gDvm.gcHeapCond);
    }

    if (reason != GC_CONCURRENT) {
        dirtyEnd = dvmGetRelativeTimeMsec();
        dvmResumeAllThreads(SUSPEND_FOR_GC);
        if (oldThreadPriority != kInvalidPriority) {
            if (setpriority(PRIO_PROCESS, 0, oldThreadPriority) != 0) {
                LOGW_HEAP("Unable to reset priority to %d: %s\n",
                          oldThreadPriority, strerror(errno));
            } else {
                LOGD_HEAP("Reset priority to %d\n", oldThreadPriority);
            }

            if (oldThreadPriority >= ANDROID_PRIORITY_BACKGROUND) {
                set_sched_policy(dvmGetSysThreadId(), SP_BACKGROUND);
            }
        }
    }

    percentFree = 100 - (size_t)(100.0f * (float)currAllocated / currFootprint);
    if (reason != GC_CONCURRENT) {
        u4 markSweepTime = dirtyEnd - rootStart;
        bool isSmall = numBytesFreed > 0 && numBytesFreed < 1024;
        totalTime = rootSuspendTime + markSweepTime;
        LOGD("%s freed %s%zdK, %d%% free %zdK/%zdK, paused %ums",
             GcReasonStr[reason],
             isSmall ? "<" : "",
             numBytesFreed ? MAX(numBytesFreed / 1024, 1) : 0,
             percentFree,
             currAllocated / 1024, currFootprint / 1024,
             markSweepTime);
    } else {
        u4 rootTime = rootEnd - rootStart;
        u4 dirtySuspendTime = dirtyStart - dirtySuspend;
        u4 dirtyTime = dirtyEnd - dirtyStart;
        bool isSmall = numBytesFreed > 0 && numBytesFreed < 1024;
        totalTime = rootSuspendTime + rootTime + dirtySuspendTime + dirtyTime;
        LOGD("%s freed %s%zdK, %d%% free %zdK/%zdK, paused %ums+%ums",
             GcReasonStr[reason],
             isSmall ? "<" : "",
             numBytesFreed ? MAX(numBytesFreed / 1024, 1) : 0,
             percentFree,
             currAllocated / 1024, currFootprint / 1024,
             rootTime, dirtyTime);
    }
    if (gcHeap->ddmHpifWhen != 0) {
        LOGD_HEAP("Sending VM heap info to DDM\n");
        dvmDdmSendHeapInfo(gcHeap->ddmHpifWhen, false);
    }
    if (gcHeap->ddmHpsgWhen != 0) {
        LOGD_HEAP("Dumping VM heap to DDM\n");
        dvmDdmSendHeapSegments(false, false);
    }
    if (gcHeap->ddmNhsgWhen != 0) {
        LOGD_HEAP("Dumping native heap to DDM\n");
        dvmDdmSendHeapSegments(false, true);
    }
}

/*
 * If the concurrent GC is running, wait for it to finish.  The caller
 * must hold the heap lock.
 *
 * Note: the second dvmChangeStatus() could stall if we were in RUNNING
 * on entry, and some other thread has asked us to suspend.  In that
 * case we will be suspended with the heap lock held, which can lead to
 * deadlock if the other thread tries to do something with the managed heap.
 * For example, the debugger might suspend us and then execute a method that
 * allocates memory.  We can avoid this situation by releasing the lock
 * before self-suspending.  (The developer can work around this specific
 * situation by single-stepping the VM.  Alternatively, we could disable
 * concurrent GC when the debugger is attached, but that might change
 * behavior more than is desirable.)
 *
 * This should not be a problem in production, because any GC-related
 * activity will grab the lock before issuing a suspend-all.  (We may briefly
 * suspend when the GC thread calls dvmUnlockHeap before dvmResumeAllThreads,
 * but there's no risk of deadlock.)
 */
void dvmWaitForConcurrentGcToComplete(void)
{
    Thread *self = dvmThreadSelf();
    assert(self != NULL);
    while (gDvm.gcHeap->gcRunning) {
        ThreadStatus oldStatus = dvmChangeStatus(self, THREAD_VMWAIT);
        dvmWaitCond(&gDvm.gcHeapCond, &gDvm.gcHeapLock);
        dvmChangeStatus(self, oldStatus);
    }
}
