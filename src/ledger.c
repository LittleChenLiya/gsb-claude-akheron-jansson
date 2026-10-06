/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifdef HAVE_CONFIG_H
#include <jansson_private_config.h>
#endif

#include <stdio.h>
#include <string.h>

#include "jansson.h"
#include "jansson_private.h"

/*
 * Debug ledger for the reference API.
 *
 * The ledger observes json_object(), json_object_set() and
 * json_object_set_new().  Each event is keyed by the real object
 * identity (the json_t pointer), so a "retain" and a "steal" of the
 * same value are distinguishable even when their refcounts coincide.
 *
 * The ledger is strictly observational: it never increments or
 * decrements a reference count and never frees an observed object.
 * json_ledger_close() releases only the ledger's own memory.
 */

struct json_ledger {
    json_ledger_event_t *events;
    size_t size;
    size_t capacity;
};

/* At most one ledger records at a time.  Keeping a single global
   pointer makes the observation cost one comparison when disabled. */
static json_ledger_t *active_ledger = NULL;

json_ledger_t *json_ledger_open(void) {
    json_ledger_t *ledger = jsonp_malloc(sizeof(json_ledger_t));
    if (!ledger)
        return NULL;

    ledger->events = NULL;
    ledger->size = 0;
    ledger->capacity = 0;

    active_ledger = ledger;
    return ledger;
}

void json_ledger_close(json_ledger_t *ledger) {
    size_t i;

    if (!ledger)
        return;

    if (active_ledger == ledger)
        active_ledger = NULL;

    /* Release only the ledger's own memory.  The recorded objects are
       never decref'd or freed here. */
    for (i = 0; i < ledger->size; i++)
        jsonp_free((char *)ledger->events[i].key);
    jsonp_free(ledger->events);
    jsonp_free(ledger);
}

size_t json_ledger_size(const json_ledger_t *ledger) {
    if (!ledger)
        return 0;

    return ledger->size;
}

const json_ledger_event_t *json_ledger_get(const json_ledger_t *ledger, size_t index) {
    if (!ledger || index >= ledger->size)
        return NULL;

    return &ledger->events[index];
}

static void ledger_record(json_ledger_event_type_t type, const json_t *object,
                          const char *key, size_t key_len, const json_t *value) {
    json_ledger_t *ledger = active_ledger;
    json_ledger_event_t *event;
    char *key_copy = NULL;

    if (!ledger)
        return;

    /* Copy the key: the caller's buffer may not outlive the event. */
    if (key)
        key_copy = jsonp_strndup(key, key_len);

    if (ledger->size == ledger->capacity) {
        size_t capacity = ledger->capacity ? ledger->capacity * 2 : 16;
        json_ledger_event_t *events = jsonp_malloc(capacity * sizeof(*events));
        if (!events) {
            jsonp_free(key_copy);
            return;
        }
        if (ledger->events) {
            memcpy(events, ledger->events, ledger->size * sizeof(*events));
            jsonp_free(ledger->events);
        }
        ledger->events = events;
        ledger->capacity = capacity;
    }

    event = &ledger->events[ledger->size++];
    event->type = type;
    event->object = object;
    event->value = value;
    event->key = key_copy;
    event->refcount = value ? value->refcount : 0;
}

void jsonp_ledger_record_new(const json_t *object) {
    if (!active_ledger)
        return;

    ledger_record(JSON_LEDGER_NEW, object, NULL, 0, object);
}

void jsonp_ledger_record_set(const json_t *object, const char *key, size_t key_len,
                             const json_t *value, int steal) {
    if (!active_ledger)
        return;

    ledger_record(steal ? JSON_LEDGER_STEAL : JSON_LEDGER_RETAIN, object, key, key_len,
                  value);
}

void json_ledger_dump(const json_ledger_t *ledger, FILE *stream) {
    static const char *const names[] = {"new", "retain", "steal"};
    size_t i;

    if (!ledger || !stream)
        return;

    for (i = 0; i < ledger->size; i++) {
        const json_ledger_event_t *event = &ledger->events[i];
        const char *name =
            event->type <= JSON_LEDGER_STEAL ? names[event->type] : "unknown";

        if (event->key)
            fprintf(stream, "#%lu %s object=%p key=\"%s\" value=%p refcount=%lu\n",
                    (unsigned long)i, name, (const void *)event->object, event->key,
                    (const void *)event->value, (unsigned long)event->refcount);
        else
            fprintf(stream, "#%lu %s object=%p value=%p refcount=%lu\n",
                    (unsigned long)i, name, (const void *)event->object,
                    (const void *)event->value, (unsigned long)event->refcount);
    }
}
