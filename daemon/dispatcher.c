/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <sys/time.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <sys/errno.h>

#include "memcached.h"

#define TASK_CANCEL 1

typedef enum {
    EXECUTOR_DEAD,
    EXECUTOR_CREATING,
    EXECUTOR_RUNNING,
    EXECUTOR_WAITING,
    EXECUTOR_SHUTDOWN
} executor_state_t;

typedef enum {
    EXECUTOR_BLOCKING, EXECUTOR_NONBLOCKING
} executor_type_t;

struct log {
    struct timeval l_start; /* 16 bytes */
    uint64_t module; /* 8 bytes */
    uint64_t l_duration; /* 8 bytes */
    char l_descr[80]; /* total of 112 bytes == 8-byte aligned ;)) */
};

struct executor {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t id;
    executor_state_t state;
    executor_type_t type;
    size_t num_tasks;
    size_t max_tasks;
    memcached_task_t** tasks;
    int logsz;
    struct log *log;
    int logindex;
};

struct dispatcher {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    memcached_taskid_t next_taskid;
    size_t num_blocking;
    size_t num_executors;
    struct executor** executors;
} dispatcher;

/*
 * Each task contains a block of "reserved" bytes that is used by the scheduler.
 * To avoid making the knowledge of the internal layout known to the entire module,
 * the following helper functions is defined.
 *
 * The current layout is:
 * reserved[0] - Where we store the module pointer for the task
 * reserved[1] - I'm currently just using bit 0 if the task is cancelled or not
 * reserved[2] - The number of usec until the task times out
 * reserved[3] - The dynamic scheduling mask (currently this is fixed with
 *               the latency in the upper 4 bytes, and the priority in the
 *               lower 4 bytes)
 *
 */
static inline int same_module(const memcached_task_t *t, const void *m) {
    return t->reserved[0] == (uint64_t)m;
}

static inline void set_module(memcached_task_t *t, const void *module) {
    t->reserved[0] = (uint64_t)module;
}

static inline void set_sleeptime(memcached_task_t *t, uint32_t sleeptime) {
    t->reserved[2] = sleeptime;
}

static inline void decr_sleeptime(memcached_task_t *t, uint64_t delta) {
    if (t->reserved[2] > 0) {
        if (delta > t->reserved[2]) {
            t->reserved[2] = 0;
        } else {
            t->reserved[2] -= delta;
        }
    }
}

static inline uint64_t get_sleeptime(const memcached_task_t *t) {
    return t->reserved[2];
}

static inline uint64_t get_schedpri(const memcached_task_t *t) {
    return t->reserved[3];
}

static inline int cmp_schedpri(const memcached_task_t *a, const memcached_task_t *b) {
    if (get_schedpri(a) > get_schedpri(b)) {
        return -1;
    } else if (get_schedpri(b) > get_schedpri(a)) {
        return 1;
    }
    return 0;
}

static inline void set_schedpri(memcached_task_t *t,
                                memcached_task_latency_t latency,
                                memcached_task_priority_t pri) {
    t->reserved[3] = ((uint64_t)latency) << 32 | pri;
}

static inline int is_cancelled(const memcached_task_t *t) {
    return (t->reserved[1] & TASK_CANCEL) == TASK_CANCEL;
}

static inline void set_cancelled(memcached_task_t *t) {
    t->reserved[1] |= TASK_CANCEL;
}

/**
 * Add a task execution to the execution log.
 *
 * @param me the executor that performed the task
 * @param start when the execution started
 * @param duration for how long the task ran
 * @param task the task that ran
 */
static void log_entry(struct executor *me,
                      struct timeval *start,
                      uint64_t duration,
                      memcached_task_t *task)
{
    struct log *ent = &me->log[me->logindex % me->logsz];
    ent->l_start = *start;
    ent->l_duration = duration;
    /* keep it zero-terminated */
    strncpy(ent->l_descr, task->get_description(task), sizeof(ent->l_descr) - 1);
    me->logindex++;
}

/**
 * Some system calls just can't fail (if they do, we can't recover
 * from them), but I'd like to know where they occur, so that I can
 * figure out what happened and prevent that from occurring again.
 *
 * This is a common error macro for doing so..
 */
#define must_succeed(x) do { \
    int must_retcode = x ; \
    if (must_retcode != 0) { \
        fprintf(stderr, "FATAL ERROR. \"" #x "\" failed: %s\n", \
                strerror(must_retcode)); \
        abort(); \
    } \
} while (0);

/* Most of the Posix thread functions may return an error code. It would be
 * stupid to _not_ check if we hit one of those error conditions (even if we
 * don't expect any of them to happen, and we don't know what to do if they do.
 * To ease this I created a wrapper that asserts that they succeed.
 */
static void mutex_init(pthread_mutex_t *mutex) {
    pthread_mutexattr_t attr;
    must_succeed(pthread_mutexattr_init(&attr));
    must_succeed(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
    must_succeed(pthread_mutex_init(mutex, &attr));
    must_succeed(pthread_mutexattr_destroy(&attr));
}

static void cond_init(pthread_cond_t *cond) {
    must_succeed(pthread_cond_init(cond, NULL));
}

static void cond_destroy(pthread_cond_t *cond) {
    must_succeed(pthread_cond_destroy(cond));
}

static void mutex_destroy(pthread_mutex_t *mutex) {
    must_succeed(pthread_mutex_destroy(mutex));
}

static void mutex_lock(pthread_mutex_t *mutex) {
    must_succeed(pthread_mutex_lock(mutex));
}

static void mutex_unlock(pthread_mutex_t *mutex) {
    must_succeed(pthread_mutex_unlock(mutex));
}

static void cond_signal(pthread_cond_t *cond) {
    must_succeed(pthread_cond_signal(cond));
}

static void cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    must_succeed(pthread_cond_wait(cond, mutex));
}

static void cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    int ret = pthread_cond_timedwait(cond, mutex, abstime);
    if (ret != 0 && ret != ETIMEDOUT) {
        must_succeed(ret);
    }
}

static void wait_for_thread(pthread_t id) {
    must_succeed(pthread_join(id, NULL));
}

/*
 * Helper functions to lock/unlock/ping the dispatcher
 */
static void lock_dispatcher() {
    mutex_lock(&dispatcher.mutex);
}

static void unlock_dispatcher() {
    mutex_unlock(&dispatcher.mutex);
}

static void signal_dispatcher() {
    lock_dispatcher();
    cond_signal(&dispatcher.cond);
    unlock_dispatcher();
}

/**
 * Comparator for figuring the execution order between two tasks
 *
 * @param a pointer to the pointer to the first task
 * @param b pointer to the pointer to the second task
 * @return -1 if task a should be executed before b, 0 if it doesn't matter or
 *         1 if task a should be executed after task b.
 */
static int schedule_cmp(const void *a, const void *b) {
    const memcached_task_t *e1 = *(const memcached_task_t * const *)a;
    const memcached_task_t *e2 = *(const memcached_task_t * const *)b;

    if (e1 == e2) { /* Pointing to the same task (most likely NULL) */
        return 0;
    }

    if (e1 != NULL && e2 == NULL) {
        return -1;
    }

    if (e2 != NULL && e1 == NULL) {
        return 1;
    }

    /* ok, we're pointing to two real objects... */

    /* easy check: one wants to be executed later than the other */
    if (get_sleeptime(e1) < get_sleeptime(e2)) {
        return -1;
    } else if (get_sleeptime(e2) < get_sleeptime(e1)) {
        return 1;
    }

    /* Look at the dynamic scheduling (@todo improve me) */
    return cmp_schedpri(e1, e2);
}

/**
 * This is the "main" loop for each executor (aka thread).
 *
 * @param arg Pointer to the thread context
 * @return NULL always
 */
static void *executor_main(void* arg) {
    struct executor *me = arg;
    int ii;

    mutex_lock(&me->mutex);
    me->state = EXECUTOR_WAITING;

    signal_dispatcher();

    do {
        // resort the executor queue:
        qsort(me->tasks, me->num_tasks + 1, sizeof(memcached_task_t*),
              schedule_cmp);
        do {
            me->state = EXECUTOR_RUNNING;

            if (me->tasks[0] != NULL && get_sleeptime(me->tasks[0]) == 0) {
                struct timeval before, after;
                uint64_t diff;

                memcached_task_t *task = me->tasks[0];
                /* We don't want to keep the lock while we're running the task */
                mutex_unlock(&me->mutex);
                must_succeed(gettimeofday(&before, NULL));
                task->run(task);
                must_succeed(gettimeofday(&after, NULL));
                mutex_lock(&me->mutex);

                after.tv_sec -= before.tv_sec;
                diff = (uint64_t)after.tv_sec * 1000000;
                diff += (after.tv_usec - before.tv_usec);

                log_entry(me, &before, diff, task);

                // Run through and adjust the sleeptime
                for (ii = 1; ii < me->num_tasks; ++ii) {
                    if (me->tasks[ii] != NULL) {
                        decr_sleeptime(me->tasks[ii], diff);
                    }
                }

                if (!task->recurrent || is_cancelled(task)) {
                    // The task shall not be rescheduled, free it
                    me->num_tasks--;
                    me->tasks[0] = NULL;
                    task->destructor(task);
                } else {
                    set_sleeptime(me->tasks[0], me->tasks[0]->sleeptime);
                }
            }
            // resort the executor queue:
            // @todo I should have a "task generation" counter so that I
            //       could avoid reshuffeling the list if nothing changed...
            //       please note that sleeptime expirations may change the
            //       order..
            qsort(me->tasks, me->num_tasks + 1, sizeof(memcached_task_t*),
                  schedule_cmp);

        } while (me->state != EXECUTOR_SHUTDOWN &&
                 (me->tasks[0] != NULL && get_sleeptime(me->tasks[0]) == 0));

        if (me->state != EXECUTOR_SHUTDOWN) {
            me->state = EXECUTOR_WAITING;
            if (me->tasks[0] == NULL) {
                /*
                 * We don't have any tasks in the list.. just sleep until
                 * someone adds a task
                 */
                cond_wait(&me->cond, &me->mutex);
            } else {
                /* Sleep until the next task expire */
                uint64_t diff;
                struct timeval before, after;
                struct timespec ts;

                /* Calculate the time for the next timeout */
                must_succeed(gettimeofday(&before, NULL));
                diff = (get_sleeptime(me->tasks[0]) + before.tv_usec) * 1000;
                ts.tv_sec = before.tv_sec + diff / 1000000000;
                ts.tv_nsec = diff % 1000000000;

                cond_timedwait(&me->cond, &me->mutex, &ts);
                must_succeed(gettimeofday(&after, NULL));

                after.tv_sec -= before.tv_sec;
                diff = (uint64_t)after.tv_sec * 1000000;
                diff += (after.tv_usec - before.tv_usec);

                // Run through and adjust the sleeptime
                for (ii = 0; ii < me->num_tasks; ++ii) {
                    if (me->tasks[ii] != NULL) {
                        decr_sleeptime(me->tasks[ii], diff);
                    }
                }
            }
        }
    } while (me->state == EXECUTOR_WAITING);

    /** Cancel all remaining tasks! */
    for (ii = 0; ii < me->num_tasks; ++ii) {
        if (me->tasks[ii] != NULL) {
            me->tasks[ii]->destructor(me->tasks[ii]);
            me->tasks[0] = NULL;
        }
    }
    me->num_tasks = 0;
    me->state = EXECUTOR_DEAD;
    cond_signal(&me->cond);
    mutex_unlock(&me->mutex);
    return NULL;
}

/**
 * Initialize an executor
 *
 * @param executor the executor to initialize
 * @param type the type of executor (blocking / nonblocking)
 * @param num_tasks The initial size of the task-queue for the executor
 * @return 0 for success -1 otherwise
 */
static int initialize_executor(struct executor **ex, executor_type_t type,
                               int num_tasks, int logsz) {
    struct executor *executor = calloc(1, sizeof(*executor));
    *ex = executor;
    mutex_init(&executor->mutex);
    cond_init(&executor->cond);
    executor->state = EXECUTOR_CREATING;
    executor->type = type;
    executor->max_tasks = num_tasks;
    executor->logsz = logsz;
    executor->tasks = calloc(executor->max_tasks, sizeof(struct executor*));
    executor->log = calloc(executor->logsz, sizeof(struct log));
    if (executor->tasks == NULL || executor->log == NULL) {
        return -1;
    }

    return 0;
}

static void destroy_executor(struct executor *ex) {
    cond_destroy(&ex->cond);
    mutex_destroy(&ex->mutex);
    free(ex->log);
    free(ex->tasks);
    free(ex);
}

static int start_executor(struct executor *executor) {
    return pthread_create(&executor->id, NULL, executor_main, executor);
}

static int lauch_executors(size_t num, size_t offset, executor_type_t type,
                           int num_tasks, int logsz) {
    size_t ii;

    for (ii = 0; ii < num; ++ii) {
        if (initialize_executor(&dispatcher.executors[ii + offset], type,
                                num_tasks, logsz) != 0) {
            /* @todo fixme */
            return -1;
        }
    }

    for (ii = 0; ii < num; ++ii) {
        if (start_executor(dispatcher.executors[ii + offset]) != 0) {
            /* @todo fix terminate! */
            return -1;
        }
        // wait for the executor to get started
        do {
            cond_wait(&dispatcher.cond, &dispatcher.mutex);
        } while (dispatcher.executors[ii + offset]->state == EXECUTOR_CREATING);

        if (dispatcher.executors[ii + offset]->state == EXECUTOR_DEAD) {
            /* @todo fixme */
            return -1;
        }
    }
    return 0;
}

int dispatcher_setup(void) {
    memset(&dispatcher, 0, sizeof(dispatcher));
    mutex_init(&dispatcher.mutex);
    cond_init(&dispatcher.cond);

    dispatcher.num_executors = settings.dispatcher.num_executors;
    dispatcher.num_blocking = settings.dispatcher.num_blocking;
    dispatcher.executors = calloc(dispatcher.num_executors,
                                  sizeof(struct executor*));
    if (dispatcher.executors == NULL) {
        return -1;
    }

    lock_dispatcher();
    lauch_executors(dispatcher.num_blocking, 0, EXECUTOR_BLOCKING, 64,
                    settings.dispatcher.num_log_elements);
    lauch_executors(dispatcher.num_executors - dispatcher.num_blocking,
                    dispatcher.num_blocking, EXECUTOR_NONBLOCKING, 64,
                    settings.dispatcher.num_log_elements);
    unlock_dispatcher();

    return 0;
}

void dispatcher_shutdown(void) {
    int ii;

    /* remember ensure that no other is touching the dispatchers! */
    for (ii = 0; ii < dispatcher.num_executors; ++ii) {
        mutex_lock(&dispatcher.executors[ii]->mutex);
        dispatcher.executors[ii]->state = EXECUTOR_SHUTDOWN;
        cond_signal(&dispatcher.executors[ii]->cond);
        mutex_unlock(&dispatcher.executors[ii]->mutex);
    }

    for (ii = 0; ii < dispatcher.num_executors; ++ii) {
        wait_for_thread(dispatcher.executors[ii]->id);
        destroy_executor(dispatcher.executors[ii]);
    }

    free(dispatcher.executors);
    dispatcher.executors = 0;
    dispatcher.num_executors = 0;
}

int dispatcher_schedule(const void *module, memcached_task_t *task) {
    struct executor *ex;
    int ret = 0;

    /* Both module and task must be set */
    if (module == NULL || task == NULL) {
        return -1;
    }

    /* Save the module */
    set_module(task, module);
    set_sleeptime(task, task->sleeptime);
    set_schedpri(task, task->latency, task->priority);

    /* Lock the scheduler and pick the correct executor */
    lock_dispatcher();
    task->taskid = ++dispatcher.next_taskid;
    unlock_dispatcher();

    // select the executor to run the task @todo utilize all of them ;)
    if (task->blocking == TASK_BLOCKING) {
        // schedule this at one of the blocking schedulers
        ex = dispatcher.executors[0];
    } else {
        // schedule this at the non-blocking schedulers
        ex = dispatcher.executors[dispatcher.num_blocking];
    }

    mutex_lock(&ex->mutex);
    if (ex->num_tasks == ex->max_tasks) {
        // we need to reallocate
        size_t next = ex->max_tasks << 1;
        memcached_task_t** t = realloc(ex->tasks,
                                       next * sizeof(memcached_task_t*));
        if (t == NULL) {
            ret = -1;
        } else {
            int ii;
            for (ii = ex->max_tasks; ii < next; ++ii) {
                t[ii] = NULL;
            }
            ex->max_tasks = next;
            ex->tasks = t;
        }
    }

    /* Rerunning the test since we might have reallocated stuff */
    if (ex->num_tasks < ex->max_tasks) {
        ex->tasks[ex->num_tasks++] = task;
        cond_signal(&ex->cond);
    }
    mutex_unlock(&ex->mutex);

    return ret;
}

static inline int task_eq(const memcached_task_t *t, memcached_taskid_t id) {
    return (t->taskid == id) || (id == 0);
}

void dispatcher_cancel(const void *module, memcached_taskid_t taskid) {
    int nrunning;
    int ii;

    do {
        nrunning = 0;
        for (ii = 0; ii < dispatcher.num_executors; ++ii) {
            struct executor *e = dispatcher.executors[ii];
            int jj;
            mutex_lock(&e->mutex);
            if (e->tasks[0] != NULL) {
                if (same_module(e->tasks[0], module) && task_eq(e->tasks[0], taskid)) {
                    if (get_sleeptime(e->tasks[0]) != 0) {
                        e->tasks[0]->destructor(e->tasks[0]);
                        e->tasks[0] = NULL;
                        e->num_tasks--;
                    } else {
                        /*
                         * we can't cancel the running task, but we
                         * can ensure that recurrent tasks wont get
                         * scheduled again
                         */
                        set_cancelled(e->tasks[0]);
                        ++nrunning;
                    }
                }

                for (jj = 1; jj < e->max_tasks; ++jj) {
                    if (e->tasks[jj] != NULL && same_module(e->tasks[jj], module) &&
                        task_eq(e->tasks[jj], taskid))
                    {
                        e->tasks[jj]->destructor(e->tasks[jj]);
                        e->tasks[jj] = NULL;
                        e->num_tasks--;
                    }
                }
            }
            mutex_unlock(&e->mutex);
        }
        if (nrunning) {
            // avoid busy-wait
            usleep(250);
        }
    } while (nrunning > 0);
}

void dispatcher_stats(ADD_STAT add_stats, void *c) {
    /* @todo figure out how we want it to look ;-)) */
}
