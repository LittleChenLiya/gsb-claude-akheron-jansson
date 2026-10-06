/*
 * Copyright (c) 2026 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */

/*
 * Explicit alloc-failure injection contract.
 *
 * This test pins down the contract for allocation failures that happen
 * in the middle of a composite operation (array growth, object insert,
 * deep copy):
 *
 *  1. A custom allocator is bound to this test scope only: the real
 *     allocator configuration is saved with json_get_alloc_funcs() and
 *     restored with json_set_alloc_funcs() when the scope ends.  The
 *     normal allocator interface is untouched.
 *
 *  2. The injector counts only REAL allocations: every successful
 *     malloc is paired with exactly one free (with no realloc function
 *     installed, jansson emulates realloc via malloc+free, so the
 *     pairing stays exact).  Counts are observed, never recomputed by
 *     running the operation twice.
 *
 *  3. When the Nth allocation of an operation fails (the acceptance
 *     point uses N = 3), the operation returns all resources it
 *     acquired during this call: live allocations (allocs - frees)
 *     return to zero and alloc/free counters are pairwise equal.
 *
 *  4. The old container keeps its previous content and stays usable;
 *     a failed deep copy returns NULL instead of half-built JSON; the
 *     next legal construction with injection disabled succeeds.
 *
 * Injected failure locations (see doc/allocfailure.rst):
 *  - src/value.c    string_create()          string buffer / struct
 *  - src/value.c    json_array_grow()        array table realloc
 *  - src/hashtable.c init_pair()             object pair allocation
 *  - src/value.c    json_object()            object struct / buckets
 *  - src/value.c    do_deep_copy() recursion cleanup via json_decref()
 */

#include "util.h"
#include <jansson.h>
#include <string.h>

/* ---- scoped failure-injecting counting allocator ---- */

static size_t inj_allocs = 0; /* successful allocations */
static size_t inj_frees = 0;  /* releases                 */
static size_t inj_pos = 0;    /* current allocation attempt index */
static long inj_fail_at = -1; /* 1-based attempt to fail; -1 disables */
static const char *inj_op = "";

static void *inj_malloc(size_t size) {
    inj_pos++;
    if (inj_fail_at > 0 && (long)inj_pos == inj_fail_at) {
        fprintf(stderr, "inject-fail #%zu at %s\n", inj_pos, inj_op);
        return NULL;
    }
    inj_allocs++;
    return malloc(size);
}

static void inj_free(void *ptr) {
    inj_frees++;
    free(ptr);
}

static size_t inj_live(void) { return inj_allocs - inj_frees; }

static void inj_reset(long fail_at, const char *op) {
    inj_allocs = 0;
    inj_frees = 0;
    inj_pos = 0;
    inj_fail_at = fail_at;
    inj_op = op;
}

/* test-scope binding: saved on enter, restored on leave */
static json_malloc_t saved_malloc;
static json_free_t saved_free;

static void scope_enter(void) {
    json_get_alloc_funcs(&saved_malloc, &saved_free);
    json_set_alloc_funcs(inj_malloc, inj_free);
}

static void scope_leave(void) { json_set_alloc_funcs(saved_malloc, saved_free); }

/* ---- array: grow fails mid-append ---- */

static void test_array_midfail(void) {
    static const char *vals[8] = {"v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7"};
    json_t *arr = json_array();
    const char *s;
    size_t i;
    int ret;

    if (!arr)
        fail("json_array failed");

    /* fill to the initial capacity (8) so the next append must grow */
    for (i = 0; i < 8; i++) {
        if (json_array_append_new(arr, json_string(vals[i])))
            fail("array setup append failed");
    }

    /* third allocation of the operation fails:
       (1) string buffer, (2) string struct, (3) array table grow */
    inj_reset(3, "json_array_append_new -> json_array_grow (src/value.c)");
    ret = json_array_append_new(arr, json_string("vx"));

    if (ret != -1)
        fail("array: append should fail on injected OOM");
    if (inj_live() != 0)
        fail("array: live allocations not back to zero");
    if (inj_allocs != inj_frees)
        fail("array: alloc/free pairing broken");
    if (inj_allocs != 2)
        fail("array: expected exactly 2 successful allocations before failure");

    /* old container content unchanged */
    if (json_array_size(arr) != 8)
        fail("array: size changed by failed append");
    for (i = 0; i < 8; i++) {
        s = json_string_value(json_array_get(arr, i));
        if (!s || strcmp(s, vals[i]))
            fail("array: content changed by failed append");
    }

    /* old container stays usable: next legal construction works */
    inj_reset(-1, "array reuse after failure");
    if (json_array_append_new(arr, json_string("ok")))
        fail("array: append after failure broke");
    if (json_array_size(arr) != 9)
        fail("array: size wrong after reuse append");
    if (json_array_remove(arr, 8))
        fail("array: remove after reuse failed");
    if (json_array_size(arr) != 8)
        fail("array: size wrong after remove");

    json_decref(arr);
}

/* ---- object: pair allocation fails mid-insert ---- */

static void test_object_midfail(void) {
    json_t *obj = json_object();
    json_t *v;
    int ret;

    if (!obj)
        fail("json_object failed");
    if (json_object_set_new(obj, "a", json_integer(1)))
        fail("object setup failed");
    if (json_object_set_new(obj, "b", json_integer(2)))
        fail("object setup failed");

    /* third allocation of the operation fails:
       (1) string buffer, (2) string struct, (3) hashtable pair */
    inj_reset(3, "json_object_set_new -> init_pair (src/hashtable.c)");
    ret = json_object_set_new(obj, "c", json_string("v"));

    if (ret != -1)
        fail("object: set should fail on injected OOM");
    if (inj_live() != 0)
        fail("object: live allocations not back to zero");
    if (inj_allocs != inj_frees)
        fail("object: alloc/free pairing broken");
    if (inj_allocs != 2)
        fail("object: expected exactly 2 successful allocations before failure");

    /* no half-JSON: the failed key must not be present */
    if (json_object_size(obj) != 2)
        fail("object: size changed by failed set");
    if (json_object_get(obj, "c"))
        fail("object: half-inserted key present after failure");

    /* old content unchanged */
    v = json_object_get(obj, "a");
    if (!v || json_integer_value(v) != 1)
        fail("object: content changed by failed set");
    v = json_object_get(obj, "b");
    if (!v || json_integer_value(v) != 2)
        fail("object: content changed by failed set");

    /* old container stays usable */
    inj_reset(-1, "object reuse after failure");
    if (json_object_set_new(obj, "c", json_integer(3)))
        fail("object: set after failure broke");
    if (json_object_del(obj, "c"))
        fail("object: del after reuse failed");
    if (json_object_size(obj) != 2)
        fail("object: size wrong after reuse");

    json_decref(obj);
}

/* ---- deep copy: recursive construction fails midway ---- */

static void test_deep_copy_midfail(void) {
    json_t *src = json_pack("{s:[i,i,i], s:s}", "arr", 1, 2, 3, "txt", "hello");
    json_t *copy;
    char *snap;
    char *now;
    long fail_at;

    if (!src)
        fail("deep copy setup pack failed");

    /* snapshot taken with the real allocator, before the scope */
    snap = json_dumps(src, JSON_COMPACT);
    if (!snap)
        fail("deep copy snapshot failed");

    /* explicit acceptance point: the third allocation fails.
       (1) parents-set buckets, (2) result object struct,
       (3) result object buckets */
    inj_reset(3, "json_deep_copy -> json_object/hashtable_init (src/value.c)");
    copy = json_deep_copy(src);

    if (copy)
        fail("deepcopy: should return NULL, not half-built JSON");
    if (inj_live() != 0)
        fail("deepcopy: live allocations not back to zero");
    if (inj_allocs != inj_frees)
        fail("deepcopy: alloc/free pairing broken");

    /* sweep every failure position; each must fully clean up.
       Counts come from real allocations only, never recomputed. */
    for (fail_at = 1;; fail_at++) {
        inj_reset(fail_at, "json_deep_copy sweep");
        copy = json_deep_copy(src);
        if (copy)
            break;
        if (inj_live() != 0)
            fail("deepcopy sweep: leak at some failure position");
        if (inj_allocs != inj_frees)
            fail("deepcopy sweep: alloc/free pairing broken");
        if (fail_at > 100)
            fail("deepcopy sweep: too many failures");
    }

    /* next legal construction works and is complete */
    if (!json_equal(copy, src))
        fail("deepcopy: final copy not equal to source");
    json_decref(copy);

    /* disable injection for the remaining checks in this scope */
    inj_reset(-1, "deepcopy source check");

    /* source content unchanged; the dump is made inside the scope, so
       it is released with the paired injector free (which keeps the
       alloc/free counts paired and forwards to the real free) */
    now = json_dumps(src, JSON_COMPACT);
    if (!now)
        fail("deepcopy: post-scope dump failed");
    if (strcmp(now, snap))
        fail("deepcopy: source content changed");
    inj_free(now);
    free(snap); /* snap was allocated by the real allocator before the scope */

    json_decref(src);
}

/* ---- scope restore: real allocator config untouched ---- */

static void test_scope_restore(void) {
    json_malloc_t cur_malloc;
    json_free_t cur_free;
    json_t *obj;

    json_get_alloc_funcs(&cur_malloc, &cur_free);
    if (cur_malloc != saved_malloc || cur_free != saved_free)
        fail("allocator configuration not restored after scope");

    /* next legal construction with the normal allocator works */
    obj = json_object();
    if (!obj)
        fail("construction with restored allocator failed");
    json_decref(obj);
}

static void run_tests() {
    scope_enter();
    test_array_midfail();
    test_object_midfail();
    test_deep_copy_midfail();
    scope_leave();
    test_scope_restore();
}
