/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include <assert.h>

/*
 * Include the source code for the dispatcher here so that we can unit
 * test the individual functions
 */
#include "daemon/dispatcher.c"

struct settings settings = {
    .dispatcher = {
        .num_executors = 1,
        .num_blocking = 1,
        .num_log_elements = 100
    }
};

//static int schedule_cmp(const void *a, const void *b);

static void test_schedule_sorting()
{
    memcached_task_t *e1 = NULL;
    memcached_task_t *e2 = NULL;

    /* Nil pointers, order shouldn't matter */
    assert(schedule_cmp(&e1, &e2) == 0);
    assert(schedule_cmp(&e2, &e1) == 0);

    /* Single task should always come first */
    memcached_task_t t1 = { 0 };
    e1 = &t1;
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    /* With two "equal" tasks, order shouldn't matter */
    memcached_task_t t2 = { 0 };
    e2 = &t2;
    assert(schedule_cmp(&e1, &e2) == 0);
    assert(schedule_cmp(&e2, &e1) == 0);

    /* The one with the least sleeptime should always come first */
    set_sleeptime(e1, 5);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);
    set_sleeptime(e2, 6);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);
    set_sleeptime(e1, 0);
    set_sleeptime(e2, 0);

    /* Look at the scheduler priorities */
    set_schedpri(e1, TASK_HIGH_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 0);
    assert(schedule_cmp(&e2, &e1) == 0);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_NORMAL_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_HIGH_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_OVERRIDE_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);

    set_schedpri(e1, TASK_NORMAL_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_NORMAL_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_HIGH_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_OVERRIDE_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e1, TASK_LOW_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_NORMAL_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_HIGH_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);

    set_schedpri(e2, TASK_HIGH_LATENCY, TASK_OVERRIDE_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == -1);
    assert(schedule_cmp(&e2, &e1) == 1);


    set_schedpri(e1, TASK_HIGH_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_NORMAL_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);

    set_schedpri(e1, TASK_NORMAL_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_NORMAL_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 0);
    assert(schedule_cmp(&e2, &e1) == 0);

    set_schedpri(e1, TASK_NORMAL_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_LOW_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 1);
    assert(schedule_cmp(&e2, &e1) == -1);

    set_schedpri(e1, TASK_LOW_LATENCY, TASK_LOW_PRIORITY);
    set_schedpri(e2, TASK_LOW_LATENCY, TASK_LOW_PRIORITY);
    assert(schedule_cmp(&e1, &e2) == 0);
    assert(schedule_cmp(&e2, &e1) == 0);
}

int main(void)
{
    test_schedule_sorting();
    return 0;
}

