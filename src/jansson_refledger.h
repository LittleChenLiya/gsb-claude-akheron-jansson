/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 *
 *
 * Debug reference ledger for the object reference API.
 *
 * The ledger is a pure observer for json_object(), json_object_set()
 * and json_object_set_new().  It records ownership events keyed by the
 * real object identity (its address) -- never by refcount numbers --
 * so a reference kept by the caller (json_object_set()) and a
 * reference stolen by the callee (json_object_set_new()) show up as
 * distinct records even for the same small object.
 *
 * The ledger only watches: it never calls json_incref()/json_decref()
 * on the objects it tracks, it never frees application objects, and
 * closing the ledger releases only the ledger's own bookkeeping
 * memory.  When no ledger is in use the reference API keeps its
 * default behaviour.
 *
 * Out of scope by design: cycle/container (deep) diagnostics and
 * cross-allocator checking.
 */

#ifndef JANSSON_REFLEDGER_H
#define JANSSON_REFLEDGER_H

#include <stddef.h>

#include "jansson.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One observed identity (a real object address) and the ownership
   events recorded for it. */
typedef struct {
    const void *identity; /* real object address */
    size_t created;       /* births via json_refledger_object_new() */
    size_t retained;      /* times handed to json_refledger_object_set():
                             json_object_set() keeps the caller's reference */
    size_t stolen;        /* times handed to json_refledger_object_set_new():
                             json_object_set_new() steals the caller's reference */
    size_t destroyed;     /* json_delete() runs observed for this identity */
} json_refledger_entry;

typedef struct {
    json_refledger_entry *entries;
    size_t size;
    size_t capacity;
    int watching; /* nonzero while this ledger holds the destroy observer */
} json_refledger_t;

/* Initialise the ledger and install the destroy observer.  At most one
   ledger observes destroys at a time; initialising another ledger takes
   over the observer.  Does not touch any JSON object. */
void json_refledger_init(json_refledger_t *ledger);

/* Close the ledger: uninstall the destroy observer (if held) and free
   the ledger's own bookkeeping memory.  Never changes reference counts
   and never frees JSON objects; objects created while the ledger was
   open stay alive and must be released by the application as usual. */
void json_refledger_close(json_refledger_t *ledger);

/* Observed wrappers around the reference API.  Each performs the real
   call with unchanged semantics and, on success, records one event
   keyed by the real identity of the object involved. */
json_t *json_refledger_object_new(json_refledger_t *ledger);
int json_refledger_object_set(json_refledger_t *ledger, json_t *object,
                              const char *key, json_t *value);
int json_refledger_object_set_new(json_refledger_t *ledger, json_t *object,
                                  const char *key, json_t *value);

/* Return the record for a given object identity, or NULL if the ledger
   has observed no events for it.  The pointer is valid until the next
   ledger operation that records an event or until json_refledger_close(). */
const json_refledger_entry *json_refledger_get(const json_refledger_t *ledger,
                                               const void *identity);

#ifdef __cplusplus
}
#endif

#endif /* JANSSON_REFLEDGER_H */
