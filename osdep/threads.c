/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <limits.h>
#include <assert.h>

#include "common/common.h"
#include "threads.h"

static void get_pthread_time(struct timespec *out_ts)
{
#if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0
    clock_gettime(CLOCK_REALTIME, out_ts);
#else
    // OSX
    struct timeval tv;
    gettimeofday(&tv, NULL);
    out_ts->tv_sec = tv.tv_sec;
    out_ts->tv_nsec = tv.tv_usec * 1000UL;
#endif
}

static void timespec_add_seconds(struct timespec *ts, double seconds)
{
    if (seconds > INT_MAX)
        seconds = INT_MAX;
    unsigned long secs = (int)seconds;
    unsigned long nsecs = (seconds - secs) * 1000000000UL;
    if (nsecs + ts->tv_nsec >= 1000000000UL) {
        secs += 1;
        nsecs -= 1000000000UL;
    }
    ts->tv_sec += secs;
    ts->tv_nsec += nsecs;
}

// Return the argument to pass to e.g. pthread_cond_timedwait().
// (Note that pthread_cond_t supports multiple clocks; this function computes
// the time value needed by the default clock.)
struct timespec mpthread_get_deadline(double timeout)
{
    struct timespec ts;
    get_pthread_time(&ts);
    timespec_add_seconds(&ts, timeout);
    return ts;
}

// Call pthread_cond_timedwait() with a relative timeout in seconds
int mpthread_cond_timed_wait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                             double timeout)
{
    struct timespec ts = mpthread_get_deadline(timeout);
    return pthread_cond_timedwait(cond, mutex, &ts);
}

// Helper to reduce boiler plate.
int mpthread_mutex_init_recursive(pthread_mutex_t *mutex)
{
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int r = pthread_mutex_init(mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    return r;
}

struct mp_dispatch_queue {
    struct mp_dispatch_item *head, *tail;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int suspend_requested;
    bool suspended;
    bool locked;
    void (*wakeup_fn)(void *wakeup_ctx);
    void *wakeup_ctx;
};

struct mp_dispatch_item {
    mp_dispatch_fn fn;
    void *fn_data;
    bool asynchronous;
    bool completed;
    struct mp_dispatch_item *next;
};

static void queue_dtor(void *p)
{
    struct mp_dispatch_queue *queue = p;
    assert(!queue->head);
    assert(!queue->suspend_requested);
    assert(!queue->suspended);
    assert(!queue->locked);
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->lock);
}

// A dispatch queue lets other threads runs callbacks in s target thread.
// The target thread is the thread which created the queue and which calls
// mp_dispatch_queue_process().
// Free the dispatch queue with talloc_free(). (It must be empty.)
struct mp_dispatch_queue *mp_dispatch_create(void *talloc_parent)
{
    struct mp_dispatch_queue *queue = talloc_ptrtype(talloc_parent, queue);
    *queue = (struct mp_dispatch_queue){0};
    talloc_set_destructor(queue, queue_dtor);
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->cond, NULL);
    return queue;
}

// Set a custom function that should be called to guarantee that the target
// thread wakes up. This is intended for use with code that needs to block
// on non-pthread primitives, such as e.g. select(). In the case of select(),
// the wakeup_fn could for example write a byte into a "wakeup" pipe in order
// to unblock the select(). The wakeup_fn is called from the dispatch queue
// when there are new dispatch items, and the target thread should then enter
// mp_dispatch_queue_process() as soon as possible. Note that wakeup_fn is
// called under no lock, so you might have to do synchronization yourself.
void mp_dispatch_set_wakeup_fn(struct mp_dispatch_queue *queue,
                               void (*wakeup_fn)(void *wakeup_ctx),
                               void *wakeup_ctx)
{
    queue->wakeup_fn = wakeup_fn;
    queue->wakeup_ctx = wakeup_ctx;
}

static void mp_dispatch_append(struct mp_dispatch_queue *queue,
                               struct mp_dispatch_item *item)
{
    pthread_mutex_lock(&queue->lock);
    if (queue->tail) {
        queue->tail->next = item;
    } else {
        queue->head = item;
    }
    queue->tail = item;
    // Wake up the main thread; note that other threads might wait on this
    // condition for reasons, so broadcast the condition.
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
    if (queue->wakeup_fn)
        queue->wakeup_fn(queue->wakeup_ctx);
}

// Let the dispatch item process asynchronously. item->fn will be run in the
// target thread's context, but note that mp_dispatch_enqueue() will usually
// return long before that happens. It's up to the user to signal completion
// of the callback. It's also up to the user to guarantee that the context
// (fn_data) has correct lifetime, i.e. lives until the callback is run, and
// is freed after that.
void mp_dispatch_enqueue(struct mp_dispatch_queue *queue,
                         mp_dispatch_fn fn, void *fn_data)
{
    struct mp_dispatch_item *item = talloc_ptrtype(NULL, item);
    *item = (struct mp_dispatch_item){
        .fn = fn,
        .fn_data = fn_data,
        .asynchronous = true,
    };
    mp_dispatch_append(queue, item);
}

// Like mp_dispatch_enqueue(), but the queue code will call talloc_free(fn_data)
// after the fn callback has been run. (The callback could trivially do that
// itself, but it makes it easier to implement synchronous and asynchronous
// requests with the same callback implementation.)
void mp_dispatch_enqueue_autofree(struct mp_dispatch_queue *queue,
                                  mp_dispatch_fn fn, void *fn_data)
{
    struct mp_dispatch_item *item = talloc_ptrtype(NULL, item);
    *item = (struct mp_dispatch_item){
        .fn = fn,
        .fn_data = talloc_steal(item, fn_data),
        .asynchronous = true,
    };
    mp_dispatch_append(queue, item);
}

// Run the dispatch item synchronously. item->fn will be run in the target
// thread's context, and this function will wait until it's done.
void mp_dispatch_run(struct mp_dispatch_queue *queue,
                     mp_dispatch_fn fn, void *fn_data)
{
    struct mp_dispatch_item item = {
        .fn = fn,
        .fn_data = fn_data,
    };
    mp_dispatch_append(queue, &item);

    pthread_mutex_lock(&queue->lock);
    while (!item.completed)
        pthread_cond_wait(&queue->cond, &queue->lock);
    pthread_mutex_unlock(&queue->lock);
}

// Process any outstanding dispatch items in the queue. This also handles
// suspending or locking the target thread.
// The timeout specifies the maximum wait time, but the actual time spent in
// this function can be much higher if the suspending/locking functions are
// used, or if executing the dispatch items takes time.
// TODO: implement timeout
void mp_dispatch_queue_process(struct mp_dispatch_queue *queue, double timeout)
{
    pthread_mutex_lock(&queue->lock);
    queue->suspended = true;
    // Wake up thread which called mp_dispatch_suspend().
    pthread_cond_broadcast(&queue->cond);
    while (queue->head || queue->suspend_requested) {
        if (queue->head && !queue->locked) {
            struct mp_dispatch_item *item = queue->head;
            queue->head = item->next;
            if (!queue->head)
                queue->tail = NULL;
            item->next = NULL;
            // Unlock, because we want to allow other threads to queue items
            // while the dispatch item is processed.
            pthread_mutex_unlock(&queue->lock);
            item->fn(item->fn_data);
            pthread_mutex_lock(&queue->lock);
            if (item->asynchronous) {
                talloc_free(item);
            } else {
                item->completed = true;
                // Wakeup mp_dispatch_run()
                pthread_cond_broadcast(&queue->cond);
            }
        } else {
            pthread_cond_wait(&queue->cond, &queue->lock);
        }
    }
    queue->suspended = false;
    pthread_mutex_unlock(&queue->lock);
}

// Set the target thread into suspend mode: in this mode, the thread will enter
// mp_dispatch_queue_process(), process any outstanding dispatch items, and
// wait for new items when done (instead of exiting the process function).
// Multiple threads can enter suspend mode at the same time. Suspend mode is
// not a synchronization mechanism; it merely makes sure the target thread does
// not leave mp_dispatch_queue_process(), even if it's done. mp_dispatch_lock()
// can be used for exclusive access.
void mp_dispatch_suspend(struct mp_dispatch_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    queue->suspend_requested++;
    while (!queue->suspended)
        pthread_cond_wait(&queue->cond, &queue->lock);
    pthread_mutex_unlock(&queue->lock);
}

// Undo mp_dispatch_suspend().
void mp_dispatch_resume(struct mp_dispatch_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    assert(queue->suspend_requested > 0);
    queue->suspend_requested--;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}

// Grant exclusive access to the target thread's state. While this is active,
// no other thread can return from mp_dispatch_lock() (i.e. it behaves like
// a pthread mutex), and no other thread can get dispatch items completed.
// Other threads can still queue asynchronous dispatch items without waiting,
// and the mutex behavior applies to this function only.
void mp_dispatch_lock(struct mp_dispatch_queue *queue)
{
    // TODO: acquiring a lock should probably be serialized with other
    //       dispatch items to guarantee minimum fairness.
    pthread_mutex_lock(&queue->lock);
    queue->suspend_requested++;
    while (1) {
        if (queue->suspended && !queue->locked) {
            queue->locked = true;
            break;
        }
        pthread_cond_wait(&queue->cond, &queue->lock);
    }
    pthread_mutex_unlock(&queue->lock);
}

// Undo mp_dispatch_lock().
void mp_dispatch_unlock(struct mp_dispatch_queue *queue)
{
    pthread_mutex_lock(&queue->lock);
    assert(queue->locked);
    assert(queue->suspend_requested > 0);
    queue->locked = false;
    queue->suspend_requested--;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->lock);
}
