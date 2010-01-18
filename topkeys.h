#ifndef TOPKEYS_H
#define TOPKEYS_H 1

#include "memcached/engine.h"
#include "genhash.h"

/* A list of operations for which we have hit and miss stats */
#define TK_CMDS(C) C(get_hits) C(get_misses) C(cmd_set) C(incr_hits) \
                   C(incr_misses) C(decr_hits) C(decr_misses) \
                   C(delete_hits) C(delete_misses)

/**
 * Whenever TK_OPS is expanded, the macro TK_CUR will get expanded
 * once for each stat, with the name of the stat as its argument.
 */
#define TK_OPS TK_CMDS(TK_CUR)

/* Update the correct stat for a given operation */
#define TK(tk, op, key, nkey) { \
    if (tk) { \
        assert(key); \
        assert(nkey > 0); \
        pthread_mutex_lock(&tk->mutex); \
        topkey_item_t *tmp = topkeys_item_get_or_create( \
            (tk), (key), (nkey)); \
        tmp->op++; \
        pthread_mutex_unlock(&tk->mutex); \
    } \
}

typedef struct dlist {
    struct dlist *next;
    struct dlist *prev;
} dlist_t;

typedef struct topkey_item {
    dlist_t list; /* Must be at the beginning because we downcast! */
    int nkey;
#define TK_CUR(name) int name;
    TK_OPS
#undef TK_CUR
    char key[]; /* A variable length array in the struct itself */
} topkey_item_t;

typedef struct topkeys {
    dlist_t list;
    pthread_mutex_t mutex;
    genhash_t *hash;
    int nkeys;
    int max_keys;
} topkeys_t;

topkeys_t *topkeys_init(int max_keys);
void topkeys_free(topkeys_t *topkeys);
topkey_item_t *topkeys_item_get_or_create(topkeys_t *tk, const void *key, size_t nkey);
ENGINE_ERROR_CODE topkeys_stats(topkeys_t *tk, const void *cookie, ADD_STAT add_stat);

#endif
