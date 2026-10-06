/*
 * Copyright (c) 2009-2016 Petri Lehtinen <petri@digip.org>
 *
 * Jansson is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 */

/* Three-object example for the reference API debug ledger.
 *
 * Three objects are created: a catalog, a book and a pen.  The book
 * is added to the catalog with json_object_set() (the caller keeps
 * its own reference) and the pen with json_object_set_new() (the
 * catalog steals the caller's reference).  The ledger records the two
 * transfers with different ownership even though both values end up
 * with the same refcount.
 *
 * The ledger only observes: closing it frees nothing, and all objects
 * are destroyed exactly once through the normal reference API.
 */

#include <jansson.h>
#include <stdio.h>

int main(void) {
    json_ledger_t *ledger;
    json_t *catalog, *book, *pen;

    ledger = json_ledger_open();
    if (!ledger) {
        fprintf(stderr, "unable to open ledger\n");
        return 1;
    }

    catalog = json_object();
    book = json_object();
    pen = json_object();
    if (!catalog || !book || !pen) {
        fprintf(stderr, "unable to create objects\n");
        return 1;
    }

    json_object_set_new(book, "title", json_string("C Programming"));
    json_object_set_new(pen, "color", json_string("black"));

    /* retain: the caller keeps its own reference to book */
    if (json_object_set(catalog, "book", book)) {
        fprintf(stderr, "json_object_set failed\n");
        return 1;
    }

    /* steal: the caller gives up its own reference to pen */
    if (json_object_set_new(catalog, "pen", pen)) {
        fprintf(stderr, "json_object_set_new failed\n");
        return 1;
    }

    /* the same distinction is visible in the ledger: "book" is a
       retain, "pen" is a steal */
    json_ledger_dump(ledger, stdout);

    /* the ledger only observes; closing it frees nothing */
    json_ledger_close(ledger);

    /* the caller still owns its reference to book ... */
    json_decref(book);
    /* ... while the catalog owns pen outright, plus its own
       reference to book; each object is destroyed exactly once */
    json_decref(catalog);

    return 0;
}
