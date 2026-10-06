.. _alloc-failure-injection:

=====================================
Allocation failure injection contract
=====================================

Jansson guarantees that an allocation failure in the middle of a
composite operation never leaks and never produces half-built JSON.
This contract is pinned down by the deterministic test
``test/suites/api/test_alloc_fail.c``.

Contract
========

1. **Scoped binding.**  A custom allocator is bound to a single test
   scope (or a single instance of an operation): the real allocator
   configuration is saved with :func:`json_get_alloc_funcs` on scope
   entry and restored with :func:`json_set_alloc_funcs` on scope exit.
   The real (system) allocator configuration is never modified
   permanently, and the normal allocator interface
   (:func:`json_set_alloc_funcs`, :func:`json_set_alloc_funcs2`,
   :func:`json_get_alloc_funcs`, :func:`json_get_alloc_funcs2`) is
   preserved unchanged.

2. **Paired counting of real allocations.**  The injector counts only
   allocations that actually happen: every successful ``malloc`` is
   paired with exactly one ``free``.  (When no ``realloc`` function is
   installed, jansson emulates it with ``malloc`` + ``free``, so the
   pairing stays exact.)  Counts are observed from the allocator
   callbacks; the operation is never re-run just to compute how many
   allocations it needs.

3. **Mid-failure returns everything.**  If the Nth allocation of an
   operation fails (the acceptance point uses N = 3), the operation
   releases every resource it acquired during that call: *live
   allocations* (``allocs - frees``) return to zero and the alloc/free
   counters are pairwise equal.

4. **Old containers survive.**  A failed ``json_array_append_*`` or
   ``json_object_set_*`` leaves the old container's content unchanged
   and the container fully usable; a failed :func:`json_deep_copy`
   returns ``NULL`` instead of a half-built value; the next legal
   construction (injection disabled, or the restored normal allocator)
   works normally.

Injected failure locations
==========================

The third-allocation failure lands at a different, well-defined place
in each scenario:

===================  ==============================  ==============================
Scenario             Allocation sequence             Failing allocation
===================  ==============================  ==============================
array append         string buffer, string struct,   array table grow
                     **table grow**                   (``json_array_grow()``,
                                                     ``src/value.c``)
object set           string buffer, string struct,   hashtable pair
                     **hashtable pair**              (``init_pair()``,
                                                     ``src/hashtable.c``)
deep copy            parents-set buckets, result     result object buckets
                     object struct, **object         (``json_object()`` /
                     buckets**                       ``hashtable_init()``,
                                                     ``src/value.c``)
===================  ==============================  ==============================

Recursive deep-copy cleanup unwinds through ``do_deep_copy()``
(``src/value.c``), which decrefs the partially built result on any
failure, releasing every value constructed so far.

Running the acceptance test
===========================

Serial (``-j1``) build and run of just this test::

    cmake -S . -B build -DJANSSON_BUILD_DOCS=OFF
    cmake --build build -j1 --target test_alloc_fail
    ctest --test-dir build -R '^test_alloc_fail$' -j1 --output-on-failure

Each injected failure is reported on stderr with its location, e.g.::

    inject-fail #3 at json_array_append_new -> json_array_grow (src/value.c)
