/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 *
 * Three-object debug ledger example: one small child object is placed
 * into two parent objects, once with json_object_set() (the caller
 * keeps its reference) and once with json_object_set_new() (the callee
 * steals it).  The ledger tells the two ownership kinds apart by the
 * child's real identity, and shows that the child is destroyed exactly
 * once when both parents go away.
 *
 * Build (from the repository root, after building libjansson):
 *   gcc -I src -I <build>/include examples/refledger_three.c -L <build>/lib -ljansson
 */

#include <stdio.h>

#include <jansson.h>
#include <jansson_refledger.h>

static void report(const json_refledger_t *ledger, const void *identity,
                   const char *name) {
    const json_refledger_entry *entry = json_refledger_get(ledger, identity);

    if (!entry) {
        printf("%-12s: no ledger records\n", name);
        return;
    }

    printf("%-12s: created=%zu retained=%zu stolen=%zu destroyed=%zu\n", name,
           entry->created, entry->retained, entry->stolen, entry->destroyed);
}

int main(void) {
    json_refledger_t ledger;
    json_t *parent_a, *parent_b, *child;

    json_refledger_init(&ledger);

    /* three objects: two parents, one shared child */
    parent_a = json_refledger_object_new(&ledger);
    parent_b = json_refledger_object_new(&ledger);
    child = json_object();
    if (!parent_a || !parent_b || !child) {
        fprintf(stderr, "unable to create objects\n");
        json_refledger_close(&ledger);
        return 1;
    }

    /* set: parent_a holds a new reference, we keep ours */
    if (json_refledger_object_set(&ledger, parent_a, "child", child))
        goto oom;

    /* set_new: parent_b takes over our reference (steal) */
    if (json_refledger_object_set_new(&ledger, parent_b, "child", child))
        goto oom;

    printf("after set + set_new of the same child:\n");
    report(&ledger, parent_a, "parent_a");
    report(&ledger, parent_b, "parent_b");
    report(&ledger, child, "child");

    /* releasing both parents destroys the child exactly once */
    json_decref(parent_a);
    json_decref(parent_b);

    printf("\nafter releasing both parents:\n");
    report(&ledger, parent_a, "parent_a");
    report(&ledger, parent_b, "parent_b");
    report(&ledger, child, "child");

    /* closing the ledger frees only the ledger itself */
    json_refledger_close(&ledger);
    return 0;

oom:
    fprintf(stderr, "unable to set values\n");
    json_decref(parent_a);
    json_decref(parent_b);
    json_decref(child);
    json_refledger_close(&ledger);
    return 1;
}
