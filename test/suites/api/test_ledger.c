/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

/* Directed tests for the reference API debug ledger.  The ledger must
   only observe json_object(), json_object_set() and
   json_object_set_new(); it must never change reference counts or
   free observed objects. */

#include "util.h"
#include <jansson.h>
#include <string.h>

/* Counting allocator with a watch list: records how many times each
   watched pointer is freed, so the test can prove that every object
   is destroyed exactly once and that closing the ledger frees
   nothing. */
#define MAX_WATCHED 16

static const void *watched_ptrs[MAX_WATCHED];
static int watched_frees[MAX_WATCHED];
static size_t watched_count;

static void watch_reset(void) {
    size_t i;
    for (i = 0; i < MAX_WATCHED; i++) {
        watched_ptrs[i] = NULL;
        watched_frees[i] = 0;
    }
    watched_count = 0;
}

static void watch(const void *ptr) {
    if (watched_count < MAX_WATCHED) {
        watched_ptrs[watched_count] = ptr;
        watched_frees[watched_count] = 0;
        watched_count++;
    }
}

static int frees_of(const void *ptr) {
    size_t i;
    for (i = 0; i < watched_count; i++) {
        if (watched_ptrs[i] == ptr)
            return watched_frees[i];
    }
    return 0;
}

static void *counting_malloc(size_t size) {
    return malloc(size);
}

static void counting_free(void *ptr) {
    size_t i;
    for (i = 0; i < watched_count; i++) {
        if (watched_ptrs[i] == ptr) {
            watched_frees[i]++;
            break;
        }
    }
    free(ptr);
}

/* The same small object goes through json_object_set() (retain) and
   json_object_set_new() (steal); the ledger must record different
   ownership for the two, keyed by object identity. */
static void test_retain_vs_steal(void) {
    json_ledger_t *ledger;
    const json_ledger_event_t *event;
    json_t *object, *small;
    char keybuf[16];

    ledger = json_ledger_open();
    if (!ledger)
        fail("unable to open ledger");

    object = json_object();
    small = json_integer(42);
    if (!object || !small)
        fail("unable to create values");

    watch_reset();
    watch(object);
    watch(small);

    /* event 0: creation of the object */
    if (json_ledger_size(ledger) != 1)
        fail("ledger did not record json_object()");
    event = json_ledger_get(ledger, 0);
    if (event->type != JSON_LEDGER_NEW)
        fail("creation was not recorded as new");
    if (event->object != object || event->value != object)
        fail("new event has wrong identity");
    if (event->key != NULL)
        fail("new event must not have a key");

    /* event 1: retain; the caller keeps its own reference.  Use a
       mutable key buffer to prove the ledger copies the key. */
    strcpy(keybuf, "retained");
    if (json_object_set(object, keybuf, small))
        fail("json_object_set failed");
    keybuf[0] = 'X';

    if (small->refcount != 2)
        fail("json_object_set did not take a new reference");

    /* event 2: steal; the caller gives up its own reference */
    if (json_object_set_new(object, "stolen", small))
        fail("json_object_set_new failed");

    if (small->refcount != 2)
        fail("json_object_set_new changed the reference count");

    if (json_ledger_size(ledger) != 3)
        fail("ledger did not record both sets");

    event = json_ledger_get(ledger, 1);
    if (event->type != JSON_LEDGER_RETAIN)
        fail("json_object_set was not recorded as retain");
    if (event->object != object || event->value != small)
        fail("retain event has wrong identity");
    if (!event->key || strcmp(event->key, "retained") != 0)
        fail("retain event did not copy the key");
    if (event->refcount != 2)
        fail("retain event observed wrong refcount");

    event = json_ledger_get(ledger, 2);
    if (event->type != JSON_LEDGER_STEAL)
        fail("json_object_set_new was not recorded as steal");
    if (event->object != object || event->value != small)
        fail("steal event has wrong identity");
    if (!event->key || strcmp(event->key, "stolen") != 0)
        fail("steal event has wrong key");

    /* same small object, different ownership records */
    if (json_ledger_get(ledger, 1)->type == json_ledger_get(ledger, 2)->type)
        fail("retain and steal were recorded identically");

    /* Closing the ledger releases only the ledger's own memory. */
    json_ledger_close(ledger);

    if (frees_of(object) != 0 || frees_of(small) != 0)
        fail("closing the ledger freed an observed object");
    if (json_object_get(object, "retained") != small)
        fail("object was damaged by closing the ledger");
    if (json_integer_value(small) != 42)
        fail("value was damaged by closing the ledger");
    if (small->refcount != 2)
        fail("closing the ledger changed a reference count");

    /* Normal destruction: object holds both references to small, so
       decref'ing the object destroys each value exactly once. */
    json_decref(object);

    if (frees_of(object) != 1)
        fail("object was not destroyed exactly once");
    if (frees_of(small) != 1)
        fail("small was not destroyed exactly once");
}

/* A failed set must not be recorded, and the default API behavior
   (refcount unchanged on failure) must be preserved. */
static void test_failed_set_not_recorded(void) {
    json_ledger_t *ledger;
    json_t *object, *small;

    ledger = json_ledger_open();
    if (!ledger)
        fail("unable to open ledger");

    object = json_object();
    small = json_integer(1);
    if (!object || !small)
        fail("unable to create values");

    watch_reset();
    watch(object);
    watch(small);

    /* target is not an object: fails, the extra reference is consumed */
    if (json_object_set(small, "bad", small) != -1)
        fail("json_object_set on non-object did not fail");
    if (small->refcount != 1)
        fail("failed json_object_set changed the reference count");

    /* NULL key: fails */
    if (json_object_set(object, NULL, small) != -1)
        fail("json_object_set with NULL key did not fail");
    if (small->refcount != 1)
        fail("failed json_object_set changed the reference count");

    /* only the json_object() creation may be recorded */
    if (json_ledger_size(ledger) != 1)
        fail("failed set was recorded");

    json_ledger_close(ledger);

    json_decref(object);
    json_decref(small);

    if (frees_of(object) != 1 || frees_of(small) != 1)
        fail("values were not destroyed exactly once");
}

/* With no ledger open the reference API behaves as before, and a
   ledger opened later starts empty. */
static void test_no_ledger_no_observation(void) {
    json_ledger_t *ledger;
    json_t *object, *small;

    object = json_object();
    small = json_integer(7);
    if (!object || !small)
        fail("unable to create values");

    watch_reset();
    watch(object);
    watch(small);

    if (json_object_set(object, "a", small))
        fail("json_object_set failed");
    if (json_object_set_new(object, "b", small))
        fail("json_object_set_new failed");

    ledger = json_ledger_open();
    if (!ledger)
        fail("unable to open ledger");
    if (json_ledger_size(ledger) != 0)
        fail("ledger recorded events while closed");
    json_ledger_close(ledger);

    if (small->refcount != 2)
        fail("reference count wrong without ledger");

    json_decref(object);

    if (frees_of(object) != 1 || frees_of(small) != 1)
        fail("values were not destroyed exactly once");
}

static void run_tests() {
    json_malloc_t m;
    json_free_t f;

    json_get_alloc_funcs(&m, &f);
    json_set_alloc_funcs(counting_malloc, counting_free);

    test_retain_vs_steal();
    test_failed_set_not_recorded();
    test_no_ledger_no_observation();

    json_set_alloc_funcs(m, f);
}
