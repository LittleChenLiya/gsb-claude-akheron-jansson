/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 *
 * Debug reference ledger: observing-only bookkeeping for the object
 * reference API.  See jansson_refledger.h for the contract.  This
 * module deliberately uses the plain C allocator (not jsonp_malloc)
 * so that it stays independent of the library's memory hooks, and it
 * never touches reference counts or frees JSON objects.
 */

#ifdef HAVE_CONFIG_H
#include <jansson_private_config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "jansson_private.h"
#include "jansson_refledger.h"

/* Hook called from json_delete() in value.c.  NULL by default, so the
   reference API keeps its default behaviour unless a ledger is open. */
void (*jsonp_refledger_delete_observer)(json_t *) = NULL;

/* The ledger currently holding the destroy observer (at most one). */
static json_refledger_t *active_ledger;

enum refledger_event {
    REFLEDGER_CREATED,
    REFLEDGER_RETAINED,
    REFLEDGER_STOLEN,
    REFLEDGER_DESTROYED
};

static json_refledger_entry *refledger_find(json_refledger_t *ledger,
                                            const void *identity) {
    size_t i;

    for (i = 0; i < ledger->size; i++) {
        if (ledger->entries[i].identity == identity)
            return &ledger->entries[i];
    }

    return NULL;
}

static json_refledger_entry *refledger_add(json_refledger_t *ledger,
                                           const void *identity) {
    json_refledger_entry *entries;
    size_t capacity;

    if (ledger->size == ledger->capacity) {
        capacity = ledger->capacity ? ledger->capacity * 2 : 8;
        entries = realloc(ledger->entries, capacity * sizeof(json_refledger_entry));
        if (!entries)
            return NULL;
        ledger->entries = entries;
        ledger->capacity = capacity;
    }

    entries = &ledger->entries[ledger->size++];
    memset(entries, 0, sizeof(*entries));
    entries->identity = identity;
    return entries;
}

static void refledger_record(json_refledger_t *ledger, const void *identity,
                             enum refledger_event event) {
    json_refledger_entry *entry;

    if (!ledger || !identity)
        return;

    entry = refledger_find(ledger, identity);
    if (!entry)
        entry = refledger_add(ledger, identity);
    if (!entry)
        return; /* out of memory: observation is best-effort, API result stands */

    switch (event) {
        case REFLEDGER_CREATED:
            entry->created++;
            break;
        case REFLEDGER_RETAINED:
            entry->retained++;
            break;
        case REFLEDGER_STOLEN:
            entry->stolen++;
            break;
        case REFLEDGER_DESTROYED:
            entry->destroyed++;
            break;
    }
}

static void refledger_observe_delete(json_t *json) {
    /* Observing only: must not incref/decref or free anything here. */
    if (active_ledger)
        refledger_record(active_ledger, json, REFLEDGER_DESTROYED);
}

void json_refledger_init(json_refledger_t *ledger) {
    if (!ledger)
        return;

    ledger->entries = NULL;
    ledger->size = 0;
    ledger->capacity = 0;
    ledger->watching = 1;

    active_ledger = ledger;
    jsonp_refledger_delete_observer = refledger_observe_delete;
}

void json_refledger_close(json_refledger_t *ledger) {
    if (!ledger)
        return;

    if (ledger->watching && active_ledger == ledger) {
        jsonp_refledger_delete_observer = NULL;
        active_ledger = NULL;
    }

    /* Free only the ledger's own memory; tracked JSON objects are
       left untouched for the application to release. */
    free(ledger->entries);
    ledger->entries = NULL;
    ledger->size = 0;
    ledger->capacity = 0;
    ledger->watching = 0;
}

json_t *json_refledger_object_new(json_refledger_t *ledger) {
    json_t *object = json_object();

    if (object)
        refledger_record(ledger, object, REFLEDGER_CREATED);

    return object;
}

int json_refledger_object_set(json_refledger_t *ledger, json_t *object,
                              const char *key, json_t *value) {
    int result = json_object_set(object, key, value);

    if (result == 0 && value)
        refledger_record(ledger, value, REFLEDGER_RETAINED);

    return result;
}

int json_refledger_object_set_new(json_refledger_t *ledger, json_t *object,
                                  const char *key, json_t *value) {
    int result = json_object_set_new(object, key, value);

    if (result == 0 && value)
        refledger_record(ledger, value, REFLEDGER_STOLEN);

    return result;
}

const json_refledger_entry *json_refledger_get(const json_refledger_t *ledger,
                                               const void *identity) {
    size_t i;

    if (!ledger || !identity)
        return NULL;

    for (i = 0; i < ledger->size; i++) {
        if (ledger->entries[i].identity == identity)
            return &ledger->entries[i];
    }

    return NULL;
}
