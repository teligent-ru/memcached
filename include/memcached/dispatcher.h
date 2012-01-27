/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * The dispatcher
 */
#ifndef MEMCACHED_DISPATCHER_H
#define MEMCACHED_DISPATCHER_H 1

#ifdef __cplusplus
extern "C" {
#endif

    typedef uint64_t task_api_version_t;
    typedef uint64_t memcached_taskid_t;

    struct memcached_task_st;
    typedef struct memcached_task_st memcached_task_t;

    /**
     * The definition of the different priorities a task may have. The
     * priority is used to determine the tasks position in the scheduler.
     */
    typedef enum {
        /**
         * The task is of a low priority and may be scheduled when we
         * don't have any other tasks to run.
         */
        TASK_LOW_PRIORITY,
        /**
         * The average task should specify normal priority. That ensures
         * that all tasks get their time to run.
         */
        TASK_NORMAL_PRIORITY,
        /**
         * Some tasks needs a higher priority and should be able to cut
         * the lines in the dispatcher queue.
         */
        TASK_HIGH_PRIORITY,
        /**
         * If you really really really need to have something executed
         * immediately you may use the override priority. This may result
         * in a new thread to be started. Should only be used when you
         * <b>really</b> need it (please note that you need to set
         * the latency to TASK_LOW_LATENCY.. if not you just said it
         * could wait for a while ;).
         */
        TASK_OVERRIDE_PRIORITY
    } memcached_task_priority_t;

    /**
     * The definition of the different latencies the client needs for
     * this task.. The latency is used to determine when the task
     * should be scheduled.
     */
    typedef enum {
        /**
         * The task doesn't need a special kind of latency, and may be
         * scheduled when we feel like it.
         */
        TASK_HIGH_LATENCY,
        /**
         * The average task should specify normal latency. That ensures
         * that all tasks get their time to run.
         */
        TASK_NORMAL_LATENCY,
        /**
         * If you need this done yesterday....
         */
        TASK_LOW_LATENCY
    } memcached_task_latency_t;

    typedef enum {
        /** The task will never block */
        TASK_NON_BLOCKING,
        /** The task might block waiting for a mutex or something like that */
        TASK_BLOCKING
    } memcached_task_blocking_t;

    /**
     * You might want to have only one type of tasks running at the same
     * time. If you're running with mutiple backends you might allow one
     * type of task per engine to run concurrently.
     */
    typedef struct {
        /**
         * The kind of stuff you're mutual exclusive with. 0 and -1
         * have special meanings:
         * 0  - disable
         * -1 - All tasks from that object
         */
        uint64_t type;
        /**
         * The object to check with.
         */
        uint64_t object;
    } memcached_task_exclusive_t;

    /**
     * Definition of the task structure. The "client" should assign the
     * members with the requested values before scheduling the object.
     * If the client wants to change the scheduling properties it may
     * do so during the invocation of the <b>run</b> callback (and it will
     * take effect for the next callback).
     *
     * Please note that the scheduler keeps a reference to the object
     * until it calls the destructor for the task.
     */
    struct memcached_task_st {
        /**
         * The version of the task API this structure represent.
         * This is needed for the dispatcher to know which elements
         * to expect and their sizes.
         */
        task_api_version_t api_version;

        /**
         * A small memory chunk reserved for the dispatcher to
         * use for whatever it wants.
         */
        intptr_t reserved[4];

        /**
         * The task id for this task. This is assigned by the scheduler,
         * and should <b>not</b> be modified by anyone else. Doing so
         * will most likely result in a crash.
         */
        memcached_taskid_t taskid;

        /**
         * The priority of this task.
         */
        memcached_task_priority_t priority;

        /**
         * When you need the stuff done..
         */
        memcached_task_latency_t latency;

        /**
         * If the task may be blocking or not
         */
        memcached_task_blocking_t blocking;

        /**
         * Is this task exclusive
         */
        memcached_task_exclusive_t exclusive;

        /**
         * Should the task be rescheduled.
         */
        int recurrent;

        /**
         * Minimum delay until next invocation (in us)
         */
        uint32_t sleeptime;

        /**
         * Get a textual description of the task
         *
         * @param task the task to query
         * @return A textual string representing the task. The string
         *         needs to be valid until the next invocation of the
         *         callback (or the destructor is called)
         */
        const char *(*get_description)(memcached_task_t *task);

        /**
         * Run the task.
         *
         * The run method is free to change all of the properties of the
         * task, and it will be reevaluated by the dispatcher when
         * the run method returns.
         *
         * @param task pointer to the task itself
         */
        void (*run)(memcached_task_t *task);

        /**
         * When the task is complete (not to be executed anymore),
         * the dispatcher will call the destructor function when it is
         * safe to release/reuse the memory allocated by the task.
         *
         * @param task pointer to the task itself
         */
        void (*destructor)(memcached_task_t *task);
    };

    /**
     * The API exported for utilizing the dispatcher.
     */
    typedef struct {
        /**
         * Schedule a task for execution. The memory area used by
         * the task may be valid until the destructor for the task
         * is called.
         *
         * @param module the module that owns the task
         * @param task pointer to the task to add for execution
         * @return 0 success, -1 failure
         */
        int (*schedule)(const void *module, memcached_task_t *task);

        /**
         * Try to cancel the execution of a task. It is not possible to
         * cancel the running task, so the task may or may not be cancelled.
         *
         * @param module The module that owns the task
         * @param taskid The task identifier to cancel, or 0 to cancel
         *               all tasks owned by this module.
         */
        void (*cancel)(const void *module, memcached_taskid_t taskid);
    } SERVER_DISPATCHER_API;

#ifdef __cplusplus
}
#endif

#endif
