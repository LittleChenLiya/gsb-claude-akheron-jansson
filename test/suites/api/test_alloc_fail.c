/*
 * Explicit allocation-failure injection contract test.
 *
 * Contract:
 *   1. When an allocation fails in the middle of building an array, an
 *      object or a deep copy, every resource acquired by this operation
 *      must be returned (live allocations drop back to the pre-operation
 *      baseline, i.e. zero), the old container stays usable and
 *      unchanged, and the error path never returns half a JSON value
 *      (it returns NULL/-1).
 *   2. The custom allocator is bound to this test scope: scope_enter()
 *      saves the current allocator and installs the counting/injecting
 *      one, scope_leave() restores it; the real system allocator
 *      configuration is not changed. The plain allocator interface
 *      (json_set_alloc_funcs/json_get_alloc_funcs) is kept as is.
 *   3. Only actual allocations are counted (paired malloc/free counting
 *      inside the allocator hooks); counts are not recomputed by
 *      rebuilding anything.
 *
 * Acceptance: with the 3rd allocation failing (fail_at = 3), live
 * allocations return to zero, the old object content is unchanged and
 * the next legal construction succeeds. Reclamation is verified by
 * paired counting: count_alloc == count_free.
 *
 * Failure injection locations (allocation points that may fail):
 *   - src/value.c: json_object()/json_array() structs and the
 *     hashtable_init() bucket array;
 *   - src/hashtable.c: hashtable_set() pair and key strndup, rehash
 *     bucket array;
 *   - src/value.c: json_object_deep_copy()/json_array_deep_copy()
 *     json_decref(result) on mid-way failure returns every resource
 *     acquired by this operation;
 *   - src/value.c: json_object_setn_new_nocheck()/
 *     json_array_append_new() return -1 immediately for a NULL value,
 *     leaving the old container untouched;
 *   - src/pack_unpack.c / src/load.c: mid-construction failures are
 *     released up the call chain.
 *
 * Run (single-job small command, -j1):
 *   cmake -S . -B build
 *   cmake --build build --target test_alloc_fail -j1
 *   cd build && ctest -R "^test_alloc_fail$" -j1 --output-on-failure
 *
 * Copyright (c) 2026 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify it
 * under the terms of the MIT license. See LICENSE for details.
 */

#include <string.h>

#include <jansson.h>

#include "util.h"

/* paired counting + failure injection allocator: counts only actual
   allocations */
static size_t count_alloc; /* successful allocations */
static size_t count_free;  /* successful frees */
static size_t alloc_seq;   /* ordinal of the current allocation request */
static long fail_at;       /* 1-based ordinal to fail; 0 = never fail */

static void *counting_malloc(size_t size) {
    alloc_seq++;
    if (fail_at > 0 && alloc_seq == (size_t)fail_at)
        return NULL;

    count_alloc++;
    return malloc(size);
}

static void counting_free(void *ptr) {
    count_free++;
    free(ptr);
}

/* live allocations = acquired and not yet returned */
static long live_allocs(void) { return (long)count_alloc - (long)count_free; }

static void check_live_zero(const char *where) {
    if (live_allocs() != 0) {
        failhdr;
        fprintf(stderr, "%s: live allocations = %ld, expected 0\n", where,
                live_allocs());
        exit(1);
    }
}

/* test-scope allocator binding: save on enter, restore on leave */
static json_malloc_t saved_malloc;
static json_free_t saved_free;

static void scope_enter(void) {
    json_get_alloc_funcs(&saved_malloc, &saved_free);
    count_alloc = count_free = alloc_seq = 0;
    fail_at = 0;
    json_set_alloc_funcs(counting_malloc, counting_free);
}

static void scope_leave(void) { json_set_alloc_funcs(saved_malloc, saved_free); }

/* paired counting: without injection, build and destroy a complex
   object; allocations and frees must pair up */
static void test_paired_counting(void) {
    json_t *obj;

    scope_enter();

    obj = json_pack("{s:i,s:n,s:b,s:{s:s},s:[i,i,i]}", "foo", 42, "bar", "baz",
                    1, "alice", "bar", "baz", "bob", 9, 8, 7);
    if (!obj)
        fail("paired counting: construction failed without injection");

    if (count_alloc == 0)
        fail("paired counting: no allocations counted");

    json_decref(obj);

    if (count_alloc != count_free)
        fail("paired counting: alloc/free counts differ");

    check_live_zero("paired counting");
    scope_leave();
}

/* object: 3rd allocation fails mid-way, old object content unchanged */
static void test_object_fail_at_3(void) {
    json_t *old;
    json_t *ref;
    json_t *value;

    /* old object and reference are built outside the scope (system
       allocator) */
    old = json_pack("{s:s}", "keep", "me");
    ref = json_pack("{s:s}", "keep", "me");
    if (!old || !ref)
        fail("object: failed to create old object");

    scope_enter();

    /* fail the 3rd allocation: json_pack fails mid-way, no half JSON */
    fail_at = 3;
    value = json_pack("{s:[i,i,i]}", "added", 1, 2, 3);
    if (value)
        fail("object: injection at 3rd allocation did not fail");

    if (json_object_set_new(old, "added", value) != -1)
        fail("object: set_new with failed value did not return -1");

    check_live_zero("object: fail at 3rd allocation");

    /* old object content unchanged and still usable */
    if (!json_equal(old, ref))
        fail("object: old object content changed after failed op");

    /* the next legal construction works */
    fail_at = 0;
    value = json_pack("{s:[i,i,i]}", "added", 1, 2, 3);
    if (!value)
        fail("object: legal construction failed after injection");

    if (json_object_set_new(old, "added", value))
        fail("object: legal set_new failed after injection");

    if (!json_is_object(json_object_get(old, "added")))
        fail("object: old object not usable after legal set");

    /* undo the addition to keep the paired counts balanced */
    if (json_object_del(old, "added"))
        fail("object: failed to remove added key");

    check_live_zero("object: after legal construction");
    scope_leave();

    json_decref(old);
    json_decref(ref);
}

/* array: 3rd allocation fails mid-way, old array content unchanged */
static void test_array_fail_at_3(void) {
    json_t *old;
    json_t *ref;
    json_t *value;

    old = json_pack("[i,i]", 1, 2);
    ref = json_pack("[i,i]", 1, 2);
    if (!old || !ref)
        fail("array: failed to create old array");

    scope_enter();

    fail_at = 3;
    value = json_pack("{s:i}", "added", 7);
    if (value)
        fail("array: injection at 3rd allocation did not fail");

    if (json_array_append_new(old, value) != -1)
        fail("array: append_new with failed value did not return -1");

    check_live_zero("array: fail at 3rd allocation");

    if (json_array_size(old) != 2 || !json_equal(old, ref))
        fail("array: old array content changed after failed op");

    /* the next legal construction works */
    fail_at = 0;
    value = json_pack("{s:i}", "added", 7);
    if (!value)
        fail("array: legal construction failed after injection");

    if (json_array_append_new(old, value))
        fail("array: legal append_new failed after injection");

    if (json_array_size(old) != 3)
        fail("array: old array not usable after legal append");

    if (json_array_remove(old, 2))
        fail("array: failed to remove appended value");

    check_live_zero("array: after legal construction");
    scope_leave();

    json_decref(old);
    json_decref(ref);
}

/* deep copy: 3rd allocation fails, NULL is returned, everything
   acquired by this operation is returned, original unchanged */
static void test_deep_copy_fail_at_3(void) {
    json_t *original;
    json_t *ref;
    json_t *copy;

    original = json_pack("{s:[i,{s:s}],s:s}", "a", 1, "b", "x", "c", "keep");
    ref = json_pack("{s:[i,{s:s}],s:s}", "a", 1, "b", "x", "c", "keep");
    if (!original || !ref)
        fail("deep copy: failed to create original");

    scope_enter();

    fail_at = 3;
    copy = json_deep_copy(original);
    if (copy)
        fail("deep copy: injection at 3rd allocation did not fail");

    check_live_zero("deep copy: fail at 3rd allocation");

    if (!json_equal(original, ref))
        fail("deep copy: original changed after failed copy");

    /* the next legal construction works */
    fail_at = 0;
    copy = json_deep_copy(original);
    if (!copy)
        fail("deep copy: legal copy failed after injection");

    if (!json_equal(copy, original))
        fail("deep copy: legal copy not equal to original");

    json_decref(copy);
    check_live_zero("deep copy: after legal construction");
    scope_leave();

    json_decref(original);
    json_decref(ref);
}

/* sweep every failure position: no matter which allocation fails, the
   resources acquired by this operation must be returned */
static void test_fail_sweep(void) {
    json_t *original;
    json_t *copy;
    long k;

    original = json_pack("{s:[i,{s:s}],s:s}", "a", 1, "b", "x", "c", "keep");
    if (!original)
        fail("sweep: failed to create original");

    scope_enter();

    /* deep copy: inject at each allocation position in turn */
    for (k = 1;; k++) {
        if (k > 200)
            fail("sweep: too many allocation failures in deep copy");

        alloc_seq = 0;
        fail_at = k;
        copy = json_deep_copy(original);
        fail_at = 0;

        if (!copy) {
            check_live_zero("sweep: deep copy");
            continue;
        }

        if (!json_equal(copy, original))
            fail("sweep: deep copy not equal to original");

        json_decref(copy);
        check_live_zero("sweep: deep copy final");
        break;
    }

    /* construction (mixed arrays and objects): inject at each position */
    for (k = 1;; k++) {
        if (k > 200)
            fail("sweep: too many allocation failures in pack");

        alloc_seq = 0;
        fail_at = k;
        copy = json_pack("{s:[i,{s:s}],s:s}", "a", 1, "b", "x", "c", "keep");
        fail_at = 0;

        if (!copy) {
            check_live_zero("sweep: pack");
            continue;
        }

        if (!json_equal(copy, original))
            fail("sweep: pack result not equal to original");

        json_decref(copy);
        check_live_zero("sweep: pack final");
        break;
    }

    scope_leave();
    json_decref(original);
}

/* after the scope ends the system allocator configuration is restored
   and the plain allocator interface behaves as before */
static void test_scope_restore(void) {
    json_malloc_t m = NULL;
    json_free_t f = NULL;
    json_t *obj;

    scope_enter();
    scope_leave();

    json_get_alloc_funcs(&m, &f);
    if (m != saved_malloc || f != saved_free)
        fail("restore: allocator config not restored after scope");

    /* normal construction/destruction after restore (system allocator) */
    obj = json_pack("{s:[i,i]}", "k", 1, 2);
    if (!obj)
        fail("restore: construction failed after scope leave");

    json_decref(obj);
}

static void run_tests() {
    test_paired_counting();
    test_object_fail_at_3();
    test_array_fail_at_3();
    test_deep_copy_fail_at_3();
    test_fail_sweep();
    test_scope_restore();
}
