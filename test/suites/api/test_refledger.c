/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#include "util.h"
#include <jansson.h>
#include <jansson_refledger.h>
#include <string.h>

/* The same small object handed to json_object_set() (caller keeps its
   reference) and to json_object_set_new() (callee steals it) must get
   distinct ownership records, keyed by the object's real identity. */
static void test_set_vs_set_new() {
    json_refledger_t ledger;
    const json_refledger_entry *entry;
    json_t *parent1, *parent2, *child;

    json_refledger_init(&ledger);

    parent1 = json_refledger_object_new(&ledger);
    parent2 = json_refledger_object_new(&ledger);
    child = json_object();
    if (!parent1 || !parent2 || !child)
        fail("unable to create objects");

    /* set: caller keeps its reference, so the refcount grows */
    if (json_refledger_object_set(&ledger, parent1, "k", child))
        fail("unable to set value");
    if (child->refcount != 2)
        fail("json_object_set did not retain the value");

    /* set_new: callee steals the caller's reference, refcount unchanged */
    if (json_refledger_object_set_new(&ledger, parent2, "k", child))
        fail("unable to set_new value");
    if (child->refcount != 2)
        fail("json_object_set_new did not steal the value");

    /* one identity, two different ownership records */
    entry = json_refledger_get(&ledger, child);
    if (!entry)
        fail("ledger has no record for the child identity");
    if (entry->identity != (const void *)child)
        fail("ledger record is not keyed by the real object identity");
    if (entry->retained != 1)
        fail("json_object_set was not recorded as a retained reference");
    if (entry->stolen != 1)
        fail("json_object_set_new was not recorded as a stolen reference");
    if (entry->created != 0 || entry->destroyed != 0)
        fail("unexpected create/destroy records for the child");

    /* the two parents have their own distinct records */
    entry = json_refledger_get(&ledger, parent1);
    if (!entry || entry->created != 1 || entry->retained != 0 || entry->stolen != 0)
        fail("bad ledger record for parent1");
    entry = json_refledger_get(&ledger, parent2);
    if (!entry || entry->created != 1 || entry->retained != 0 || entry->stolen != 0)
        fail("bad ledger record for parent2");

    /* destroying both parents destroys the child exactly once */
    json_decref(parent1);
    json_decref(parent2);

    entry = json_refledger_get(&ledger, child);
    if (!entry || entry->destroyed != 1)
        fail("child was not destroyed exactly once");
    entry = json_refledger_get(&ledger, parent1);
    if (!entry || entry->destroyed != 1)
        fail("parent1 was not destroyed exactly once");
    entry = json_refledger_get(&ledger, parent2);
    if (!entry || entry->destroyed != 1)
        fail("parent2 was not destroyed exactly once");

    json_refledger_close(&ledger);
}

/* A single object created and released the normal way must be observed
   as created once and destroyed once -- the ledger itself must not
   keep it alive or free it early. */
static void test_destroy_only_once() {
    json_refledger_t ledger;
    const json_refledger_entry *entry;
    json_t *object;

    json_refledger_init(&ledger);

    object = json_refledger_object_new(&ledger);
    if (!object)
        fail("unable to create object");

    entry = json_refledger_get(&ledger, object);
    if (!entry || entry->created != 1 || entry->destroyed != 0)
        fail("bad ledger record after create");

    json_decref(object);

    entry = json_refledger_get(&ledger, object);
    if (!entry || entry->destroyed != 1)
        fail("object was not destroyed exactly once");

    json_refledger_close(&ledger);
}

/* Closing the ledger must not release the objects it observed. */
static void test_close_does_not_free() {
    json_refledger_t ledger;
    json_t *survivor, *value;

    json_refledger_init(&ledger);

    survivor = json_refledger_object_new(&ledger);
    value = json_integer(42);
    if (!survivor || !value)
        fail("unable to create objects");
    if (json_refledger_object_set_new(&ledger, survivor, "n", value))
        fail("unable to set_new value");

    json_refledger_close(&ledger);

    /* the survivor must still be a fully usable object */
    if (!json_is_object(survivor))
        fail("closing the ledger damaged the object");
    if (json_object_size(survivor) != 1)
        fail("closing the ledger lost object members");
    if (json_integer_value(json_object_get(survivor, "n")) != 42)
        fail("closing the ledger corrupted object members");

    /* after close, destroys are no longer observed (observer removed),
       and the application releases its objects as usual */
    json_decref(survivor);
}

/* With no ledger open at all, the plain reference API must behave
   exactly as before. */
static void test_default_behaviour_without_ledger() {
    json_t *object, *value;

    object = json_object();
    value = json_integer(7);
    if (!object || !value)
        fail("unable to create objects");

    if (json_object_set(object, "a", value))
        fail("json_object_set failed");
    if (value->refcount != 2)
        fail("json_object_set did not retain");
    if (json_object_set_new(object, "b", value))
        fail("json_object_set_new failed");
    if (value->refcount != 2)
        fail("json_object_set_new did not steal");

    json_decref(object);
}

static void run_tests() {
    test_set_vs_set_new();
    test_destroy_only_once();
    test_close_does_not_free();
    test_default_behaviour_without_ledger();
}
