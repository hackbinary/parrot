/*
Copyright (C) 2001-2010, Parrot Foundation.
$Id$

=head1 NAME

src/gc/gc_gms.c - Generational M&S

=head1 DESCRIPTION

Generational, non-compacting, mark and sweep GC.

Objects are stored in N (up to 8) different lists; one for each generation.
Collection with lower number is younger.  PObj_GC_generation_0_FLAG,
PObj_GC_generation_1_FLAG, PObj_GC_generation_2_FLAG determine which
generation an object is in.  PObj_GC_on_dirty_list_FLAG is set by Write
Barrier.

"Dirty" objects are moved into "dirty_list" out of C<generation> list.

Invariant: new reference from old-to-new generation can be introduced either:
    - from objects already in "dirty_list"
    - or with moving referent object into "dirty_list"

Corollary: objects in "dirty_list" either:
    - not referenced at all
    - referenced from same or younger generation
    - their referents are on "dirty_list" too

(Proof is very simple and left as exercise for reader)

Notation 
1. "*A1:B2" is a live object A in generation 1 referenced by an object B in generation 2.
2. "B0" is a non-live object B in generation 0 not referenced by anything.
3. gn: generation n.
4. R: root set.
5. D: dirty list.

GC algorithm WILL be:

0. Pre-requirements:
    a) Invariant described above to maintain dirty_list.
    b) Maintain invariant by
        i) "sealing" all objects in old generations with Write Barriers.
        ii) Write Barrier will:
            - Unseal object
            - Set "on_dirty_list" flag
            - Move object into "dirty_list"
    c) gc_gms_mark_pmc_header ignores:
        i) objects from generation older than C<self->gen_to_collect>.
        ii) objects with on_dirty_list flag set.
        iii) move objects to "work_list" for fully mark objects without recursion.

1. Trigger GC (based on phase of the Moon for simplicity. We can tune Moon later).

2. Choose K - how many collections we want to collect. Collections [0..K] will
be collected. Remember K in C<self->gen_to_collect>.

3. Move all objects from collections younger K from dirty_list
back to original lists. Reason for this is "corollary of invariant". We can
either collect such objects or they will be marked by referents from
"dirty_list".

4. Trace root objects. According to "0. Pre-requirements" we will ignore all
"old" objects. All relevant objects are moved into "work_list".

5. Iterate over "dirty_set" calling VTABLE_mark on it. It will move all
children into "work_list".

6. Iterate over "work_list" calling VTABLE_mark on it.

7. Sweep generations starting from K:
    - Destroy all dead objects
    - Move live objects into generation max(K+1, N)
    - Paint them white.

8. ...

9. Profit!

We are not cleaning "dirty_list" after this process to rescan it again on next
iteration. It allows us to keep track of old-to-new inter-generations
references between iterations. Objects from "dirty_list" which is ready to be
collected handled by "Step 3".


Pictures of GC steps.
TBD

=cut

*/

#include "parrot/parrot.h"
#include "parrot/gc_api.h"
#include "parrot/pointer_array.h"
#include "gc_private.h"
#include "fixed_allocator.h"

#define PANIC_OUT_OF_MEM(size) failed_allocation(__LINE__, (size))

/* Maximum number of collections */
#define MAX_GENERATIONS     8

/* We allocate additional space in front of PObj* to store additional pointer */
typedef struct pmc_alloc_struct {
    void *ptr;
    PMC   pmc;   /* NB: Value! */
} pmc_alloc_struct;

typedef struct string_alloc_struct {
    void    *ptr;
    STRING   str;   /* NB: Value! */
} string_alloc_struct;

#define PMC2PAC(p) ((pmc_alloc_struct *)((char*)(p) - sizeof (void *)))
#define STR2PAC(p) ((string_alloc_struct *)((char*)(p) - sizeof (void *)))


/* Get generation from PObj->flags */
#define POBJ2GEN(pobj)                                          \
    (                                                           \
        (((pobj)->flags & PObj_GC_generation_0_FLAG) ? 1 : 0)   \
        + (((pobj)->flags) & PObj_GC_generation_1_FLAG ? 2 : 0) \
        + (((pobj)->flags) & PObj_GC_generation_2_FLAG ? 4 : 0) \
    )

/* Get flags for generation number */
#define GEN2FLAGS(gen)                                  \
    (                                                   \
        ((gen) & 1 ? PObj_GC_generation_0_FLAG : 0)     \
        | ((gen) & 2 ? PObj_GC_generation_1_FLAG : 0)   \
        | ((gen) & 4 ? PObj_GC_generation_2_FLAG : 0)   \
    )

/* Private information */
typedef struct MarkSweep_GC {
    /* Allocator for PMC headers */
    struct Pool_Allocator  *pmc_allocator;

    /* During M&S gather new live objects in this list */
    struct Parrot_Pointer_Array     *work_list;

    /* During M&S gather new live objects in this list */
    struct Parrot_Pointer_Array     *dirty_list;

    /* Currently allocate objects. */
    struct Parrot_Pointer_Array     *objects[MAX_GENERATIONS];

    /* Allocator for strings */
    struct Pool_Allocator           *string_allocator;

    /* MAX_GENERATIONS generations of strings */
    struct Parrot_Pointer_Array     *strings[MAX_GENERATIONS];

    /* Fixed-size allocator */
    struct Fixed_Allocator *fixed_size_allocator;

    /* String GC */
    struct String_GC        string_gc;

    /* Amount of allocated memory before trigger gc */
    size_t                  gc_threshold;

    /* During GC phase - which generation we are collecting */
    size_t                  gen_to_collect;

    /* GC blocking */
    UINTVAL gc_mark_block_level;  /* How many outstanding GC block
                                     requests are there? */
    UINTVAL gc_sweep_block_level; /* How many outstanding GC block
                                     requests are there? */

    UINTVAL num_early_gc_PMCs;    /* how many PMCs want immediate destruction */

} MarkSweep_GC;

/* Callback to destroy PMC or free string storage */
typedef void (*sweep_cb)(PARROT_INTERP, PObj *obj);

/* HEADERIZER HFILE: src/gc/gc_private.h */

/* HEADERIZER BEGIN: static */
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */

PARROT_DOES_NOT_RETURN
static void failed_allocation(unsigned int line, unsigned long size);

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static Buffer* gc_gms_allocate_buffer_header(PARROT_INTERP,
    SHIM(size_t size))
        __attribute__nonnull__(1);

static void gc_gms_allocate_buffer_storage(PARROT_INTERP,
    ARGIN(Buffer *str),
    size_t size)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

PARROT_CAN_RETURN_NULL
static void* gc_gms_allocate_fixed_size_storage(PARROT_INTERP, size_t size)
        __attribute__nonnull__(1);

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void * gc_gms_allocate_memory_chunk(SHIM_INTERP, size_t size);

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void * gc_gms_allocate_memory_chunk_zeroed(SHIM_INTERP, size_t size);

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static void* gc_gms_allocate_pmc_attributes(PARROT_INTERP, ARGMOD(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        FUNC_MODIFIES(*pmc);

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static PMC* gc_gms_allocate_pmc_header(PARROT_INTERP, UINTVAL flags)
        __attribute__nonnull__(1);

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static STRING* gc_gms_allocate_string_header(PARROT_INTERP,
    SHIM(UINTVAL flags))
        __attribute__nonnull__(1);

static void gc_gms_allocate_string_storage(PARROT_INTERP,
    ARGIN(STRING *str),
    size_t size)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_block_GC_mark(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_block_GC_sweep(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_check_sanity(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_cleanup_dirty_list(PARROT_INTERP,
    ARGIN(MarkSweep_GC *self),
    ARGIN(Parrot_Pointer_Array *dirty_list),
    size_t gen)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        __attribute__nonnull__(3);

static void gc_gms_compact_memory_pool(PARROT_INTERP)
        __attribute__nonnull__(1);

static size_t gc_gms_count_used_pmc_memory(PARROT_INTERP,
    ARGIN(Parrot_Pointer_Array *list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static size_t gc_gms_count_used_string_memory(PARROT_INTERP,
    ARGIN(Parrot_Pointer_Array *list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_ensure_flags(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_ensure_flags_in_list(PARROT_INTERP,
    ARGIN(Parrot_Pointer_Array* list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_finalize(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_free_buffer_header(PARROT_INTERP,
    ARGFREE(Buffer *s),
    SHIM(size_t size))
        __attribute__nonnull__(1);

static void gc_gms_free_fixed_size_storage(PARROT_INTERP,
    size_t size,
    ARGMOD(void *data))
        __attribute__nonnull__(1)
        __attribute__nonnull__(3)
        FUNC_MODIFIES(*data);

static void gc_gms_free_memory_chunk(SHIM_INTERP, ARGFREE(void *data));
static void gc_gms_free_pmc_attributes(PARROT_INTERP, ARGMOD(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        FUNC_MODIFIES(*pmc);

static void gc_gms_free_pmc_header(PARROT_INTERP, ARGFREE(PMC *pmc))
        __attribute__nonnull__(1);

static void gc_gms_free_string_header(PARROT_INTERP, ARGFREE(STRING *s))
        __attribute__nonnull__(1);

static size_t gc_gms_get_gc_info(PARROT_INTERP, Interpinfo_enum which)
        __attribute__nonnull__(1);

static unsigned int gc_gms_is_blocked_GC_mark(PARROT_INTERP)
        __attribute__nonnull__(1);

static unsigned int gc_gms_is_blocked_GC_sweep(PARROT_INTERP)
        __attribute__nonnull__(1);

static int gc_gms_is_pmc_ptr(PARROT_INTERP, ARGIN_NULLOK(void *ptr))
        __attribute__nonnull__(1);

static int gc_gms_is_ptr_owned(PARROT_INTERP,
    ARGIN_NULLOK(void *ptr),
    ARGIN(Pool_Allocator *pool),
    ARGIN(Parrot_Pointer_Array *list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(3)
        __attribute__nonnull__(4);

static int gc_gms_is_string_ptr(PARROT_INTERP, ARGIN_NULLOK(void *ptr))
        __attribute__nonnull__(1);

static void gc_gms_iterate_live_strings(PARROT_INTERP,
    string_iterator_callback callback,
    ARGIN_NULLOK(void *data))
        __attribute__nonnull__(1);

static void gc_gms_mark_and_sweep(PARROT_INTERP, UINTVAL flags)
        __attribute__nonnull__(1);

static void gc_gms_mark_pmc_header(PARROT_INTERP, ARGIN(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_mark_str_header(PARROT_INTERP, ARGIN_NULLOK(STRING *str))
        __attribute__nonnull__(1);

static void gc_gms_maybe_mark_and_sweep(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_pmc_needs_early_collection(PARROT_INTERP,
    ARGMOD(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        FUNC_MODIFIES(*pmc);

static void gc_gms_print_stats(PARROT_INTERP,
    ARGIN(const char* header),
    int gen)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_process_dirty_list(PARROT_INTERP,
    ARGIN(MarkSweep_GC *self),
    ARGIN(Parrot_Pointer_Array *dirty_list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        __attribute__nonnull__(3);

static void gc_gms_process_work_list(PARROT_INTERP,
    ARGIN(MarkSweep_GC *self),
    ARGIN(Parrot_Pointer_Array *work_list))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2)
        __attribute__nonnull__(3);

static void gc_gms_reallocate_buffer_storage(PARROT_INTERP,
    ARGIN(Buffer *str),
    size_t size)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void * gc_gms_reallocate_memory_chunk(SHIM_INTERP,
    ARGFREE(void *from),
    size_t size);

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void * gc_gms_reallocate_memory_chunk_zeroed(SHIM_INTERP,
    ARGFREE(void *data),
    size_t newsize,
    size_t oldsize);

static void gc_gms_reallocate_string_storage(PARROT_INTERP,
    ARGIN(STRING *str),
    size_t size)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_seal_object(PARROT_INTERP, ARGIN(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static size_t gc_gms_select_generation_to_collect(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_set_gen_flags(PARROT_INTERP, ARGIN(PMC *pmc), int gen)
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_swap_vtables(ARGIN(PMC *pmc))
        __attribute__nonnull__(1);

static void gc_gms_sweep_pmc_cb(PARROT_INTERP, ARGIN(PObj *obj))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_sweep_pools(PARROT_INTERP, ARGIN(MarkSweep_GC *self))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_sweep_string_cb(PARROT_INTERP, ARGIN(PObj *obj))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_unblock_GC_mark(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_unblock_GC_sweep(PARROT_INTERP)
        __attribute__nonnull__(1);

static void gc_gms_unseal_object(PARROT_INTERP, ARGIN(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

static void gc_gms_write_barrier(PARROT_INTERP, ARGIN(PMC *pmc))
        __attribute__nonnull__(1)
        __attribute__nonnull__(2);

#define ASSERT_ARGS_failed_allocation __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_allocate_buffer_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_allocate_buffer_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(str))
#define ASSERT_ARGS_gc_gms_allocate_fixed_size_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_allocate_memory_chunk __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_allocate_memory_chunk_zeroed \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_allocate_pmc_attributes \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_allocate_pmc_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_allocate_string_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_allocate_string_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(str))
#define ASSERT_ARGS_gc_gms_block_GC_mark __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_block_GC_sweep __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_check_sanity __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_cleanup_dirty_list __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(self) \
    , PARROT_ASSERT_ARG(dirty_list))
#define ASSERT_ARGS_gc_gms_compact_memory_pool __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_count_used_pmc_memory __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(list))
#define ASSERT_ARGS_gc_gms_count_used_string_memory \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(list))
#define ASSERT_ARGS_gc_gms_ensure_flags __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_ensure_flags_in_list __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(list))
#define ASSERT_ARGS_gc_gms_finalize __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_free_buffer_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_free_fixed_size_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(data))
#define ASSERT_ARGS_gc_gms_free_memory_chunk __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_free_pmc_attributes __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_free_pmc_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_free_string_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_get_gc_info __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_is_blocked_GC_mark __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_is_blocked_GC_sweep __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_is_pmc_ptr __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_is_ptr_owned __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pool) \
    , PARROT_ASSERT_ARG(list))
#define ASSERT_ARGS_gc_gms_is_string_ptr __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_iterate_live_strings __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_mark_and_sweep __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_mark_pmc_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_mark_str_header __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_maybe_mark_and_sweep __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_pmc_needs_early_collection \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_print_stats __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(header))
#define ASSERT_ARGS_gc_gms_process_dirty_list __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(self) \
    , PARROT_ASSERT_ARG(dirty_list))
#define ASSERT_ARGS_gc_gms_process_work_list __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(self) \
    , PARROT_ASSERT_ARG(work_list))
#define ASSERT_ARGS_gc_gms_reallocate_buffer_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(str))
#define ASSERT_ARGS_gc_gms_reallocate_memory_chunk \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_reallocate_memory_chunk_zeroed \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (0)
#define ASSERT_ARGS_gc_gms_reallocate_string_storage \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(str))
#define ASSERT_ARGS_gc_gms_seal_object __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_select_generation_to_collect \
     __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_set_gen_flags __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_swap_vtables __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_sweep_pmc_cb __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(obj))
#define ASSERT_ARGS_gc_gms_sweep_pools __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(self))
#define ASSERT_ARGS_gc_gms_sweep_string_cb __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(obj))
#define ASSERT_ARGS_gc_gms_unblock_GC_mark __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_unblock_GC_sweep __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp))
#define ASSERT_ARGS_gc_gms_unseal_object __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
#define ASSERT_ARGS_gc_gms_write_barrier __attribute__unused__ int _ASSERT_ARGS_CHECK = (\
       PARROT_ASSERT_ARG(interp) \
    , PARROT_ASSERT_ARG(pmc))
/* Don't modify between HEADERIZER BEGIN / HEADERIZER END.  Your changes will be lost. */
/* HEADERIZER END: static */

/*

=head1 Functions

=over 4

=item C<static void gc_gms_mark_and_sweep(PARROT_INTERP, UINTVAL flags)>

This function is called from the GC API function C<Parrot_gc_mark_and_sweep>.

Flags can be a combination of these values:

  GC_finish_FLAG
  GC_lazy_FLAG
  GC_trace_stack_FLAG

=cut

*/


/*

=item C<void Parrot_gc_gms_init(PARROT_INTERP)>

Initializes the generational collector

=cut

*/

void
Parrot_gc_gms_init(PARROT_INTERP)
{
    ASSERT_ARGS(Parrot_gc_gms_init)
    struct MarkSweep_GC *self;

    /* We have to transfer ownership of memory to parent interp in threaded parrot */
    interp->gc_sys->finalize_gc_system = NULL; /* gc_gms_finalize; */

    interp->gc_sys->do_gc_mark                  = gc_gms_mark_and_sweep;
    interp->gc_sys->compact_string_pool         = gc_gms_compact_memory_pool;

    /*
    interp->gc_sys->mark_special                = gc_gms_mark_special;
    */
    interp->gc_sys->pmc_needs_early_collection  = gc_gms_pmc_needs_early_collection;

    interp->gc_sys->allocate_pmc_header         = gc_gms_allocate_pmc_header;
    interp->gc_sys->free_pmc_header             = gc_gms_free_pmc_header;

    interp->gc_sys->allocate_string_header      = gc_gms_allocate_string_header;
    interp->gc_sys->free_string_header          = gc_gms_free_string_header;

    interp->gc_sys->allocate_bufferlike_header  = gc_gms_allocate_buffer_header;
    interp->gc_sys->free_bufferlike_header      = gc_gms_free_buffer_header;

    interp->gc_sys->allocate_pmc_attributes     = gc_gms_allocate_pmc_attributes;
    interp->gc_sys->free_pmc_attributes         = gc_gms_free_pmc_attributes;

    interp->gc_sys->is_pmc_ptr                  = gc_gms_is_pmc_ptr;
    interp->gc_sys->is_string_ptr               = gc_gms_is_string_ptr;
    interp->gc_sys->mark_pmc_header             = gc_gms_mark_pmc_header;
    interp->gc_sys->mark_str_header             = gc_gms_mark_str_header;

    interp->gc_sys->block_mark                  = gc_gms_block_GC_mark;
    interp->gc_sys->unblock_mark                = gc_gms_unblock_GC_mark;
    interp->gc_sys->is_blocked_mark             = gc_gms_is_blocked_GC_mark;

    interp->gc_sys->block_sweep                 = gc_gms_block_GC_sweep;
    interp->gc_sys->unblock_sweep               = gc_gms_unblock_GC_sweep;
    interp->gc_sys->is_blocked_sweep            = gc_gms_is_blocked_GC_sweep;

    interp->gc_sys->allocate_string_storage     = gc_gms_allocate_string_storage;
    interp->gc_sys->reallocate_string_storage   = gc_gms_reallocate_string_storage;

    interp->gc_sys->allocate_buffer_storage     = gc_gms_allocate_buffer_storage;
    interp->gc_sys->reallocate_buffer_storage   = gc_gms_reallocate_buffer_storage;

    interp->gc_sys->allocate_fixed_size_storage = gc_gms_allocate_fixed_size_storage;
    interp->gc_sys->free_fixed_size_storage     = gc_gms_free_fixed_size_storage;

    /* We don't distinguish between chunk and chunk_with_pointers */
    interp->gc_sys->allocate_memory_chunk       = gc_gms_allocate_memory_chunk;
    interp->gc_sys->reallocate_memory_chunk     = gc_gms_reallocate_memory_chunk;
    interp->gc_sys->allocate_memory_chunk_with_interior_pointers
                = gc_gms_allocate_memory_chunk_zeroed;
    interp->gc_sys->reallocate_memory_chunk_with_interior_pointers
                = gc_gms_reallocate_memory_chunk_zeroed;
    interp->gc_sys->free_memory_chunk           = gc_gms_free_memory_chunk;

    interp->gc_sys->iterate_live_strings        = gc_gms_iterate_live_strings;
    interp->gc_sys->write_barrier               = gc_gms_write_barrier;

    interp->gc_sys->get_gc_info                 = gc_gms_get_gc_info;

    if (interp->parent_interpreter && interp->parent_interpreter->gc_sys) {
        /* This is a "child" interpreter. Just reuse parent one */
        self = (MarkSweep_GC*)interp->parent_interpreter->gc_sys->gc_private;
    }
    else {
        size_t i;

        self = mem_allocate_zeroed_typed(MarkSweep_GC);

        self->pmc_allocator = Parrot_gc_pool_new(interp,
            sizeof (pmc_alloc_struct));

        self->string_allocator = Parrot_gc_pool_new(interp,
            sizeof (string_alloc_struct));

        /* Allocate list for gray objects */
        self->work_list  = Parrot_pa_new(interp);
        self->dirty_list = Parrot_pa_new(interp);

        for (i = 0; i < MAX_GENERATIONS; i++) {
            self->objects[i] = Parrot_pa_new(interp);
            self->strings[i] = Parrot_pa_new(interp);
        }

        self->fixed_size_allocator = Parrot_gc_fixed_allocator_new(interp);

        /* Collect every nM allocated. */
        /* Hardcode for now. Will be configured via CLI */
        self->gc_threshold = 1 * 1024 * 1024;

        Parrot_gc_str_initialize(interp, &self->string_gc);
    }

    interp->gc_sys->gc_private = self;

}

static void
gc_gms_mark_and_sweep(PARROT_INTERP, UINTVAL flags)
{
    ASSERT_ARGS(gc_gms_mark_and_sweep)
    MarkSweep_GC      *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_Pointer_Array      *list;
    int               i, gen = -1;

    UNUSED(flags);

    /* GC is blocked */
    if (self->gc_mark_block_level)
        return;

    /* Ignore it. Will cleanup in gc_gms_finalize */
    if (flags & GC_finish_FLAG)
        return;

    /* Ignore calls from String GC. We know better when to trigger GC */
    if (flags & GC_strings_cb_FLAG)
        return;

    /* Block further GC calls */
    ++self->gc_mark_block_level;

    interp->gc_sys->stats.gc_mark_runs++;

#if 1
    gc_gms_print_stats(interp, "Before", gen);
#endif

    gc_gms_check_sanity(interp);
    /*
    2. Choose K - how many collections we want to collect. Collections [0..K] will
    be collected. Remember K in C<self->gen_to_collect>.
    */
    self->gen_to_collect = gen = gc_gms_select_generation_to_collect(interp);

    /*
    3. Move all objects from collections younger K from dirty_list
    back to original lists. Reason for this is "corollary of invariant". We can
    either collect such objects or they will be marked by referents from
    "dirty_list".
    */
    gc_gms_cleanup_dirty_list(interp, self, self->dirty_list, gen);

    /*
    4. Trace root objects. According to "0. Pre-requirements" we will ignore all
    "old" objects. All relevant objects are moved into "work_list".
    */
    gc_gms_mark_pmc_header(interp, PMCNULL);
    Parrot_gc_trace_root(interp, NULL, GC_TRACE_FULL);
    if (interp->pdb && interp->pdb->debugger) {
        Parrot_gc_trace_root(interp->pdb->debugger, NULL, (Parrot_gc_trace_type)0);
    }
    gc_gms_check_sanity(interp);

    /*
    5. Iterate over "dirty_set" calling VTABLE_mark on it. It will move all
    children into "work_list".
    */
    gc_gms_process_dirty_list(interp, self, self->dirty_list);
    gc_gms_check_sanity(interp);

    /*
    6. Iterate over "work_list" calling VTABLE_mark on it.
    */
    gc_gms_process_work_list(interp, self, self->work_list);
    gc_gms_check_sanity(interp);

    /*
    7. Sweep generations starting from K:
        - Destroy all dead objects
        - Move live objects into generation max(K+1, N)
        - Paint them white.
    */
    gc_gms_sweep_pools(interp, self);
    gc_gms_check_sanity(interp);


    /* Update some stats */
    interp->gc_sys->stats.header_allocs_since_last_collect = 0;
    interp->gc_sys->stats.mem_used_last_collect            = 0;
    self->gc_mark_block_level--;

    /* We swept all dead objects */
    self->num_early_gc_PMCs                      = 0;

    //gc_gms_compact_memory_pool(interp);
    gc_gms_check_sanity(interp);

    gc_gms_ensure_flags(interp);

#if 1 //def DETAIL_MEMORY_DEBUG
    gc_gms_print_stats(interp, "After", gen);
#endif
}

/*

=item C<static size_t gc_gms_select_generation_to_collect(PARROT_INTERP)>

Select how many generations we do want to collect.

=cut

*/
static size_t
gc_gms_select_generation_to_collect(PARROT_INTERP)
{
    /* TODO Use less naive approach. E.g. count amount of allocated memory in
     * older generations */
    size_t ret  = (size_t)log10(interp->gc_sys->stats.gc_mark_runs);
    return ret < MAX_GENERATIONS ? ret : MAX_GENERATIONS;
}

/*

=item C<gc_gms_cleanup_dirty_list()>

Move all objects from collections younger K from dirty_list
back to original lists. Reason for this is "corollary of invariant". We can
either collect such objects or they will be marked by referents from
"dirty_list".

=cut

*/
static void
gc_gms_cleanup_dirty_list(PARROT_INTERP,
        ARGIN(MarkSweep_GC *self),
        ARGIN(Parrot_Pointer_Array *dirty_list),
        size_t gen)
{
    POINTER_ARRAY_ITER(dirty_list,
        pmc_alloc_struct *item = (pmc_alloc_struct *)ptr;
        PMC              *pmc  = &(item->pmc);
        size_t            gen  = POBJ2GEN(pmc);
        if (gen <= self->gen_to_collect) {
            Parrot_pa_remove(interp, dirty_list, ptr);
            item->ptr = Parrot_pa_insert(interp, self->objects[gen], item);
        });
}

/*

=item C<gc_gms_process_dirty_list()>

Iterate over "dirty_set" calling VTABLE_mark on it. It will move all
children into "work_list".

=cut

*/
static void
gc_gms_process_dirty_list(PARROT_INTERP,
        ARGIN(MarkSweep_GC *self),
        ARGIN(Parrot_Pointer_Array *dirty_list))
{
    POINTER_ARRAY_ITER(dirty_list,
        PMC *pmc = &((pmc_alloc_struct *)ptr)->pmc;

        if (PObj_custom_mark_TEST(pmc))
            VTABLE_mark(interp, pmc);

        if (PMC_metadata(pmc))
            Parrot_gc_mark_PMC_alive(interp, PMC_metadata(pmc)););
}

static void
gc_gms_process_work_list(PARROT_INTERP,
        ARGIN(MarkSweep_GC *self),
        ARGIN(Parrot_Pointer_Array *work_list))
{
    POINTER_ARRAY_ITER(work_list,
        PMC *pmc = &((pmc_alloc_struct *)ptr)->pmc;

        if (PObj_custom_mark_TEST(pmc))
            VTABLE_mark(interp, pmc);

        if (PMC_metadata(pmc))
            Parrot_gc_mark_PMC_alive(interp, PMC_metadata(pmc)););
}

/*

=item C<gc_gms_sweep_pools()>

Sweep generations starting from K:
    - Destroy all dead objects
    - Move live objects into generation max(K+1, N)
    - Paint them white.

=cut

*/
static void
gc_gms_sweep_pools(PARROT_INTERP,
        ARGIN(MarkSweep_GC *self))
{
    ASSERT_ARGS(gc_gms_sweep_pools)

    size_t i;

    for (i = 0; i < self->gen_to_collect; i++) {
        /* Don't move to generation beyond last */
        int move_to_old = (i + 1) != MAX_GENERATIONS;

        POINTER_ARRAY_ITER(self->objects[i],
            pmc_alloc_struct *item = (pmc_alloc_struct *)ptr;
            PMC              *pmc  = &(item->pmc);

            /* Paint live objects white */
            if (PObj_live_TEST(pmc)) {
                PObj_live_CLEAR(pmc);

                if (move_to_old)
                    item->ptr = Parrot_pa_insert(interp, self->objects[i + 1], item);
            }
            else if (!PObj_constant_TEST(pmc)) {
                Parrot_pa_remove(interp, self->objects[i], PMC2PAC(pmc)->ptr);

                /* this is manual inlining of Parrot_pmc_destroy() */
                if (PObj_custom_destroy_TEST(pmc))
                    VTABLE_destroy(interp, pmc);

                if (pmc->vtable->attr_size && PMC_data(pmc))
                    Parrot_gc_free_pmc_attributes(interp, pmc);
                PMC_data(pmc) = NULL;

                PObj_on_free_list_SET(pmc);
                PObj_gc_CLEAR(pmc);

                Parrot_gc_pool_free(interp, self->pmc_allocator, ptr);
            });
    }
}


/*

=item C<static void gc_gms_mark_pmc_header(PARROT_INTERP, PMC *pmc)>

mark as grey

=cut

*/

static void
gc_gms_mark_pmc_header(PARROT_INTERP, ARGIN(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_mark_pmc_header)
    MarkSweep_GC      *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    pmc_alloc_struct  *item = PMC2PAC(pmc);
    size_t             gen  = POBJ2GEN(pmc);

    /* Object was already marked as grey. Or live. Or dead. Skip it */
    if (PObj_is_live_or_free_TESTALL(pmc) || PObj_constant_TEST(pmc))
        return;

    /* mark it live. Even if it's from older generation we can have link
     * _from_ young object which should keep it alive */
    PObj_live_SET(pmc);

    /* If object too old - skip it */
    if (gen > self->gen_to_collect)
        return;

    /* Object is on dirty_list. */
    if (pmc->flags & PObj_GC_on_dirty_list_FLAG)
        return;

    Parrot_pa_remove(interp, self->objects[gen], item);
    item->ptr = Parrot_pa_insert(interp, self->work_list, item);
}

/*

=item C<static void gc_gms_mark_str_header(PARROT_INTERP, STRING *str)>

Mark String

=cut

*/

static void
gc_gms_mark_str_header(PARROT_INTERP, ARGIN_NULLOK(STRING *str))
{
    ASSERT_ARGS(gc_gms_mark_str_header)
    MarkSweep_GC      *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    if (str)
        PObj_live_SET(str);
}


/*

=item C<static void gc_gms_compact_memory_pool(PARROT_INTERP)>

Stub for compacting memory pools.

=cut

*/
static void
gc_gms_compact_memory_pool(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_compact_memory_pool)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_gc_str_compact_pool(interp, &self->string_gc);
}

/*

=item C<static PMC* gc_gms_allocate_pmc_header(PARROT_INTERP, UINTVAL flags)>

=item C<static void gc_gms_free_pmc_header(PARROT_INTERP, PMC *pmc)>

=item C<static STRING* gc_gms_allocate_string_header(PARROT_INTERP, UINTVAL
flags)>

=item C<static void gc_gms_free_string_header(PARROT_INTERP, STRING *s)>

=item C<static void* gc_gms_allocate_pmc_attributes(PARROT_INTERP, PMC *pmc)>

=item C<static void gc_gms_free_pmc_attributes(PARROT_INTERP, PMC *pmc)>

=item C<static void gc_gms_allocate_string_storage(PARROT_INTERP, STRING *str,
size_t size)>

=item C<static void gc_gms_reallocate_string_storage(PARROT_INTERP, STRING *str,
size_t size)>

=item C<static void gc_gms_allocate_buffer_storage(PARROT_INTERP, Buffer *str,
size_t size)>

=item C<static void gc_gms_reallocate_buffer_storage(PARROT_INTERP, Buffer *str,
size_t size)>

=item C<static void* gc_gms_allocate_fixed_size_storage(PARROT_INTERP, size_t
size)>

=item C<static void gc_gms_free_fixed_size_storage(PARROT_INTERP, size_t size,
void *data)>

Functions for allocating/deallocating various objects.

*/


PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static void*
gc_gms_allocate_pmc_attributes(PARROT_INTERP, ARGMOD(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_allocate_pmc_attributes)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    const size_t  attr_size = pmc->vtable->attr_size;
    PMC_data(pmc) = Parrot_gc_fixed_allocator_allocate(interp,
                        self->fixed_size_allocator, attr_size);
    memset(PMC_data(pmc), 0, attr_size);

    interp->gc_sys->stats.mem_used_last_collect += attr_size;

    return PMC_data(pmc);
}

static void
gc_gms_free_pmc_attributes(PARROT_INTERP, ARGMOD(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_free_pmc_attributes)
    if (PMC_data(pmc)) {
        MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
        Parrot_gc_fixed_allocator_free(interp, self->fixed_size_allocator,
                PMC_data(pmc), pmc->vtable->attr_size);

        interp->gc_sys->stats.mem_used_last_collect -= pmc->vtable->attr_size;
    }
}

PARROT_CAN_RETURN_NULL
static void*
gc_gms_allocate_fixed_size_storage(PARROT_INTERP, size_t size)
{
    ASSERT_ARGS(gc_gms_allocate_fixed_size_storage)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

    interp->gc_sys->stats.memory_allocated      += size;
    interp->gc_sys->stats.mem_used_last_collect += size;

    return Parrot_gc_fixed_allocator_allocate(interp, self->fixed_size_allocator, size);
}

static void
gc_gms_free_fixed_size_storage(PARROT_INTERP, size_t size, ARGMOD(void *data))
{
    ASSERT_ARGS(gc_gms_free_fixed_size_storage)
    if (data) {
        MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

        interp->gc_sys->stats.memory_allocated      -= size;
        interp->gc_sys->stats.mem_used_last_collect -= size;

        Parrot_gc_fixed_allocator_free(interp, self->fixed_size_allocator, data, size);
    }
}

/*

=item C<static size_t gc_gms_get_gc_info(PARROT_INTERP, Interpinfo_enum which)>

GC introspection function.
gets stats based on enum which

=cut

*/
static size_t
gc_gms_get_gc_info(PARROT_INTERP, Interpinfo_enum which)
{
    ASSERT_ARGS(gc_gms_get_gc_info)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

    if (which == IMPATIENT_PMCS)
        return self->num_early_gc_PMCs;

    return Parrot_gc_get_info(interp, which, &interp->gc_sys->stats);
}



/*
=item C<static void gc_gms_finalize(PARROT_INTERP)>

Finalize GC subsystem.

=cut
*/
static void
gc_gms_finalize(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_finalize)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t        i;

    Parrot_gc_str_finalize(interp, &self->string_gc);

    for (i = 0; i < MAX_GENERATIONS; i++) {
        Parrot_pa_destroy(interp, self->objects[0]);
        Parrot_pa_destroy(interp, self->strings[0]);
    }

    Parrot_gc_pool_destroy(interp, self->pmc_allocator);
    Parrot_gc_pool_destroy(interp, self->string_allocator);
    Parrot_gc_fixed_allocator_destroy(interp, self->fixed_size_allocator);
}

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static PMC*
gc_gms_allocate_pmc_header(PARROT_INTERP, UINTVAL flags)
{
    ASSERT_ARGS(gc_gms_allocate_pmc_header)
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Pool_Allocator   * const pool = self->pmc_allocator;
    pmc_alloc_struct *ptr;

    gc_gms_maybe_mark_and_sweep(interp);

    /* Increase used memory. Not precisely accurate due Pool_Allocator paging */
    ++interp->gc_sys->stats.header_allocs_since_last_collect;

    interp->gc_sys->stats.memory_allocated      += sizeof (PMC);
    interp->gc_sys->stats.mem_used_last_collect += sizeof (PMC);

    ptr         = (pmc_alloc_struct *)Parrot_gc_pool_allocate(interp, pool);
    ptr->ptr    = Parrot_pa_insert(interp, self->objects[0], ptr);

    return &ptr->pmc;
}

static void
gc_gms_free_pmc_header(PARROT_INTERP, ARGFREE(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_free_pmc_header)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

    if (pmc) {
        size_t gen = POBJ2GEN(pmc);

        /* We should never free objects from dirty list directly! */
        PARROT_ASSERT(!(pmc->flags & PObj_GC_on_dirty_list_FLAG));

        if (PObj_on_free_list_TEST(pmc))
            return;

        Parrot_pa_remove(interp, self->objects[gen], PMC2PAC(pmc)->ptr);
        PObj_on_free_list_SET(pmc);

        Parrot_pmc_destroy(interp, pmc);

        Parrot_gc_pool_free(interp, self->pmc_allocator, PMC2PAC(pmc));

        --interp->gc_sys->stats.header_allocs_since_last_collect;
        interp->gc_sys->stats.memory_allocated      -= sizeof (PMC);
        interp->gc_sys->stats.mem_used_last_collect -= sizeof (PMC);
    }
}

/*

=item C<static int gc_gms_is_pmc_ptr(PARROT_INTERP, void *ptr)>

establish if *ptr is.owned

=cut

*/

static int
gc_gms_is_pmc_ptr(PARROT_INTERP, ARGIN_NULLOK(void *ptr))
{
    ASSERT_ARGS(gc_gms_is_pmc_ptr)
    MarkSweep_GC      *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t             i;
    for (i = 0; i < MAX_GENERATIONS; i++) {
        if (gc_gms_is_ptr_owned(interp, ptr, self->pmc_allocator, self->objects[i]))
            return 1;
    }
    return 0;
}

/*

=item C<static void gc_gms_sweep_pmc_cb(PARROT_INTERP, PObj *obj)>

destroy pmc *obj

=cut

*/

static void
gc_gms_sweep_pmc_cb(PARROT_INTERP, ARGIN(PObj *obj))
{
    ASSERT_ARGS(gc_gms_sweep_pmc_cb)
    PMC *pmc = (PMC *)obj;
    Parrot_pmc_destroy(interp, pmc);
}

/*

=item C<gc_gms_allocate_string_header(PARROT_INTERP, STRING *str)>

=item C<gc_gms_free_string_header(PARROT_INTERP, STRING *s)>

=item C<static Buffer* gc_gms_allocate_buffer_header(PARROT_INTERP, size_t
size)>

=item C<static void gc_gms_free_buffer_header(PARROT_INTERP, Buffer *s, size_t
size)>

Allocate/free string/buffer headers.

*/

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static STRING*
gc_gms_allocate_string_header(PARROT_INTERP, SHIM(UINTVAL flags))
{
    ASSERT_ARGS(gc_gms_allocate_string_header)
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Pool_Allocator   *pool = self->string_allocator;
    string_alloc_struct *ptr;
    STRING           *ret;

    gc_gms_maybe_mark_and_sweep(interp);

    /* Increase used memory. Not precisely accurate due Pool_Allocator paging */
    ++interp->gc_sys->stats.header_allocs_since_last_collect;
    interp->gc_sys->stats.memory_allocated      += sizeof (STRING);
    interp->gc_sys->stats.mem_used_last_collect += sizeof (STRING);

    ptr = (string_alloc_struct *)Parrot_gc_pool_allocate(interp, pool);
    ptr->ptr = Parrot_pa_insert(interp, self->strings[0], ptr);

    ret = &ptr->str;
    memset(ret, 0, sizeof (STRING));
    return ret;
}

static void
gc_gms_free_string_header(PARROT_INTERP, ARGFREE(STRING *s))
{
    ASSERT_ARGS(gc_gms_free_string_header)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

    if (s
    && !PObj_on_free_list_TEST(s)) {
        MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
        size_t        gen  = POBJ2GEN(s);

        Parrot_pa_remove(interp, self->strings[gen], STR2PAC(s)->ptr);

        if (Buffer_bufstart(s) && !PObj_external_TEST(s))
            Parrot_gc_str_free_buffer_storage(interp,
                &self->string_gc, (Buffer *)s);

        PObj_on_free_list_SET(s);

        Parrot_gc_pool_free(interp, self->string_allocator, STR2PAC(s));

        --interp->gc_sys->stats.header_allocs_since_last_collect;
        interp->gc_sys->stats.memory_allocated      -= sizeof (STRING);
        interp->gc_sys->stats.mem_used_last_collect -= sizeof (STRING);
    }
}

PARROT_MALLOC
PARROT_CAN_RETURN_NULL
static Buffer*
gc_gms_allocate_buffer_header(PARROT_INTERP, SHIM(size_t size))
{
    ASSERT_ARGS(gc_gms_allocate_buffer_header)
    return (Buffer*)gc_gms_allocate_string_header(interp, 0);
}

static void
gc_gms_free_buffer_header(PARROT_INTERP, ARGFREE(Buffer *s), SHIM(size_t size))
{
    ASSERT_ARGS(gc_gms_free_buffer_header)
    gc_gms_free_string_header(interp, (STRING*)s);
}

/*

=item C<static int gc_gms_is_string_ptr(PARROT_INTERP, void *ptr)>

establish if string *ptr is owned

=cut

*/

static int
gc_gms_is_string_ptr(PARROT_INTERP, ARGIN_NULLOK(void *ptr))
{
    ASSERT_ARGS(gc_gms_is_string_ptr)
    MarkSweep_GC      *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t             i;
    for (i = 0; i < MAX_GENERATIONS; i++) {
        if (gc_gms_is_ptr_owned(interp, ptr, self->string_allocator, self->objects[i]))
            return 1;
    }
    return 0;
}

/*

item C<void gc_gms_allocate_string_storage(PARROT_INTERP, STRING *str, size_t
size)>

=item C<void gc_gms_reallocate_string_storage(PARROT_INTERP, STRING *str, size_t
size)>

=item C<void gc_gms_allocate_buffer_storage(PARROT_INTERP, Buffer *str, size_t
size)>

=item C<void gc_gms_reallocate_buffer_storage(PARROT_INTERP, Buffer *str, size_t
size)>

Functions for allocating strings/buffers storage.

=cut
*/

static void
gc_gms_allocate_string_storage(PARROT_INTERP, ARGIN(STRING *str), size_t size)
{
    ASSERT_ARGS(gc_gms_allocate_string_storage)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_gc_str_allocate_string_storage(interp, &self->string_gc, str, size);
    interp->gc_sys->stats.mem_used_last_collect += size;
}

static void
gc_gms_reallocate_string_storage(PARROT_INTERP, ARGIN(STRING *str), size_t size)
{
    ASSERT_ARGS(gc_gms_reallocate_string_storage)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_gc_str_reallocate_string_storage(interp, &self->string_gc, str, size);
    interp->gc_sys->stats.mem_used_last_collect += size;
}

static void
gc_gms_allocate_buffer_storage(PARROT_INTERP, ARGIN(Buffer *str), size_t size)
{
    ASSERT_ARGS(gc_gms_allocate_buffer_storage)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_gc_str_allocate_buffer_storage(interp, &self->string_gc, str, size);
    interp->gc_sys->stats.mem_used_last_collect += size;
}

static void
gc_gms_reallocate_buffer_storage(PARROT_INTERP, ARGIN(Buffer *str), size_t size)
{
    ASSERT_ARGS(gc_gms_reallocate_buffer_storage)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Parrot_gc_str_reallocate_buffer_storage(interp, &self->string_gc, str, size);
    interp->gc_sys->stats.mem_used_last_collect += size;
}

/*

=item C<static void gc_gms_sweep_string_cb(PARROT_INTERP, PObj *obj)>

destroy string *obj

=cut

*/

static void
gc_gms_sweep_string_cb(PARROT_INTERP, ARGIN(PObj *obj))
{
    ASSERT_ARGS(gc_gms_sweep_string_cb)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    Buffer       *str  = (Buffer *)obj;
    /* Compact string pool here. Or get rid of "shared buffers" and just free storage */
    if (Buffer_bufstart(str) && !PObj_external_TEST(str))
        Parrot_gc_str_free_buffer_storage(interp, &self->string_gc, str);
}


/*

=item C<static void gc_gms_iterate_live_strings(PARROT_INTERP,
string_iterator_callback callback, void *data)>

Iterate over live string invoking callback for each of them. Used during
compacting of string pool.

=cut
*/
static void
gc_gms_iterate_live_strings(PARROT_INTERP,
        string_iterator_callback callback,
        ARGIN_NULLOK(void *data))
{
    ASSERT_ARGS(gc_gms_iterate_live_strings)

    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t             i;
    for (i = 0; i < MAX_GENERATIONS; i++) {
        // FIXME
    }
}




/*
=item C<static int gc_gms_is_ptr_owned(PARROT_INTERP, void *ptr, Pool_Allocator
*pool, Parrot_Pointer_Array *list)>

Helper function to check that we own PObj

=cut
*/

static int
gc_gms_is_ptr_owned(PARROT_INTERP, ARGIN_NULLOK(void *ptr),
    ARGIN(Pool_Allocator *pool), ARGIN(Parrot_Pointer_Array *list))
{
    ASSERT_ARGS(gc_gms_is_ptr_owned)
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    PObj             *obj  = (PObj *)ptr;
    pmc_alloc_struct *item = PMC2PAC(ptr);

    if (!obj || !item)
        return 0;

    if (!Parrot_gc_pool_is_owned(interp, pool, item))
        return 0;

    /* black or white objects marked already. */
    if (PObj_is_live_or_free_TESTALL(obj))
        return 0;

    /* Pool.is_owned isn't precise enough (yet) */
    return Parrot_pa_is_owned(interp, list, item, item->ptr);
}


/*

=item C<static void gc_gms_block_GC_mark(PARROT_INTERP)>

Blocks the GC from performing its mark phase.

=item C<static void gc_gms_unblock_GC_mark(PARROT_INTERP)>

Unblocks the GC mark.

=item C<static void gc_gms_block_GC_sweep(PARROT_INTERP)>

Blocks the GC from performing its sweep phase.

=item C<static void gc_gms_unblock_GC_sweep(PARROT_INTERP)>

Unblocks GC sweep.

=item C<static unsigned int gc_gms_is_blocked_GC_mark(PARROT_INTERP)>

Determines if the GC mark is currently blocked.

=item C<static unsigned int gc_gms_is_blocked_GC_sweep(PARROT_INTERP)>

Determines if the GC sweep is currently blocked.

=cut

*/

static void
gc_gms_block_GC_mark(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_block_GC_mark)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    ++self->gc_mark_block_level;
    Parrot_shared_gc_block(interp);
}

static void
gc_gms_unblock_GC_mark(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_unblock_GC_mark)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    if (self->gc_mark_block_level) {
        --self->gc_mark_block_level;
        Parrot_shared_gc_unblock(interp);
    }
}

static void
gc_gms_block_GC_sweep(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_block_GC_sweep)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    ++self->gc_sweep_block_level;
}

static void
gc_gms_unblock_GC_sweep(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_unblock_GC_sweep)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    if (self->gc_sweep_block_level)
        --self->gc_sweep_block_level;
}

static unsigned int
gc_gms_is_blocked_GC_mark(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_is_blocked_GC_mark)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    return self->gc_mark_block_level;
}

static unsigned int
gc_gms_is_blocked_GC_sweep(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_is_blocked_GC_sweep)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    return self->gc_sweep_block_level;
}



/*

=item C<static void * gc_gms_allocate_memory_chunk(PARROT_INTERP, size_t size)>

=item C<static void * gc_gms_reallocate_memory_chunk(PARROT_INTERP, void *from,
size_t size)>

=item C<static void * gc_gms_allocate_memory_chunk_zeroed(PARROT_INTERP, size_t
size)>

=item C<static void * gc_gms_reallocate_memory_chunk_zeroed(PARROT_INTERP, void
*data, size_t newsize, size_t oldsize)>

=item C<static void gc_gms_free_memory_chunk(PARROT_INTERP, void *data)>

=item C<static void failed_allocation(unsigned int line, unsigned long size)>

TODO Write docu.

*/

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void *
gc_gms_allocate_memory_chunk(SHIM_INTERP, size_t size)
{
    ASSERT_ARGS(gc_gms_allocate_memory_chunk)
    void * const ptr = malloc(size);
#ifdef DETAIL_MEMORY_DEBUG
    fprintf(stderr, "Allocated %i at %p\n", size, ptr);
#endif
    if (!ptr)
        PANIC_OUT_OF_MEM(size);
    return ptr;
}

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void *
gc_gms_reallocate_memory_chunk(SHIM_INTERP, ARGFREE(void *from), size_t size)
{
    ASSERT_ARGS(gc_gms_reallocate_memory_chunk)
    void *ptr;
#ifdef DETAIL_MEMORY_DEBUG
    fprintf(stderr, "Freed %p (realloc -- %i bytes)\n", from, size);
#endif
    if (from)
        ptr = realloc(from, size);
    else
        ptr = calloc(1, size);
#ifdef DETAIL_MEMORY_DEBUG
    fprintf(stderr, "Allocated %i at %p\n", size, ptr);
#endif
    if (!ptr)
        PANIC_OUT_OF_MEM(size);
    return ptr;
}

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void *
gc_gms_allocate_memory_chunk_zeroed(SHIM_INTERP, size_t size)
{
    ASSERT_ARGS(gc_gms_allocate_memory_chunk_zeroed)
    void * const ptr = calloc(1, (size_t)size);
#ifdef DETAIL_MEMORY_DEBUG
    fprintf(stderr, "Allocated %i at %p\n", size, ptr);
#endif
    if (!ptr)
        PANIC_OUT_OF_MEM(size);
    return ptr;
}

PARROT_MALLOC
PARROT_CANNOT_RETURN_NULL
static void *
gc_gms_reallocate_memory_chunk_zeroed(SHIM_INTERP, ARGFREE(void *data),
        size_t newsize, size_t oldsize)
{
    ASSERT_ARGS(gc_gms_reallocate_memory_chunk_zeroed)
    void * const ptr = realloc(data, newsize);
    if (newsize > oldsize)
        memset((char*)ptr + oldsize, 0, newsize - oldsize);
    return ptr;
}

static void
gc_gms_free_memory_chunk(SHIM_INTERP, ARGFREE(void *data))
{
    ASSERT_ARGS(gc_gms_free_memory_chunk)
#ifdef DETAIL_MEMORY_DEBUG
    fprintf(stderr, "Freed %p\n", data);
#endif
    if (data)
        free(data);
}

PARROT_DOES_NOT_RETURN
static void
failed_allocation(unsigned int line, unsigned long size)
{
    ASSERT_ARGS(failed_allocation)
    fprintf(stderr, "Failed allocation of %lu bytes\n", size);
    do_panic(NULL, "Out of mem", __FILE__, line);
}


/*

=item C<static void gc_gms_pmc_needs_early_collection(PARROT_INTERP, PMC *pmc)>

Mark a PMC as needing timely destruction

=cut

*/

static void
gc_gms_pmc_needs_early_collection(PARROT_INTERP, ARGMOD(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_pmc_needs_early_collection)
    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    ++self->num_early_gc_PMCs;
}

/*

=item C<static void gc_gms_maybe_mark_and_sweep(PARROT_INTERP)>

Maybe M&S. Depends on total allocated memory, memory allocated since last alloc
and phase of the Moon.

=cut

*/
static void
gc_gms_maybe_mark_and_sweep(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_maybe_mark_and_sweep)

    MarkSweep_GC *self = (MarkSweep_GC *)interp->gc_sys->gc_private;

    /* Collect every 256M. Hardcode for now. Will be configured via CLI */
    if (interp->gc_sys->stats.mem_used_last_collect > self->gc_threshold) {
        gc_gms_mark_and_sweep(interp, 0);
    }
}

/*
=item C<static void gc_gms_write_barrier(PARROT_INTERP, PMC *pmc)>

WriteBarrier for PMC. Add to root_objects list for mandatory next collecting.

=cut
*/
static void
gc_gms_write_barrier(PARROT_INTERP, ARGIN(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_write_barrier)
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t            gen  = POBJ2GEN(pmc);
    pmc_alloc_struct *item = PMC2PAC(pmc);

    if (pmc->flags & PObj_GC_wb_triggered_FLAG)
        return;
    if (!gen)
        return;

    Parrot_pa_remove(interp, self->objects[gen], item);
    item->ptr = Parrot_pa_insert(interp, self->dirty_list, item);

    pmc->flags |= PObj_GC_on_dirty_list_FLAG;

    /* We don't need it anymore */
    gc_gms_unseal_object(interp, pmc);
}

/*

=item C<static size_t gc_gms_count_used_string_memory(PARROT_INTERP,
Parrot_Pointer_Array *list)>

find amount of used string memory

=cut

*/

static size_t
gc_gms_count_used_string_memory(PARROT_INTERP, ARGIN(Parrot_Pointer_Array *list))
{
    ASSERT_ARGS(gc_gms_count_used_string_memory)

    size_t total_amount = 0;
#if 0
    List_Item_Header *tmp = list->first;
    while (tmp) {
        List_Item_Header *next = tmp->next;
        PObj             *obj  = LLH2Obj_typed(tmp, PObj);
        STRING           *str  = (STRING*)obj;

        /* Header size */
        total_amount += sizeof (List_Item_Header)
                        + sizeof (STRING*);
        total_amount += str->bufused;

        tmp = next;
    }
#endif
    return total_amount;
}

/*

=item C<static size_t gc_gms_count_used_pmc_memory(PARROT_INTERP,
Parrot_Pointer_Array *list)>

find amount of used pmc memory

=cut

*/

static size_t
gc_gms_count_used_pmc_memory(PARROT_INTERP, ARGIN(Parrot_Pointer_Array *list))
{
    ASSERT_ARGS(gc_gms_count_used_pmc_memory)

    size_t total_amount = 0;
#if 0
    List_Item_Header *tmp = list->first;
    while (tmp) {
        List_Item_Header *next = tmp->next;
        PMC              *obj  = LLH2Obj_typed(tmp, PMC);

        /* Header size */
        total_amount += sizeof (List_Item_Header)
                        + sizeof (PMC*);
        total_amount += obj->vtable->attr_size;

        tmp = next;
    }
#endif
    return total_amount;
}

/*
=item C<static void gc_gms_seal_object(PARROT_INTERP, PMC *pmc)>

Seal object with write barrier.

=cut
*/
static void
gc_gms_seal_object(PARROT_INTERP, ARGIN(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_seal_object)
    /* "Seal" object with write barrier */
    VTABLE  *t   = pmc->vtable;

    PARROT_ASSERT(!(t->flags & VTABLE_IS_WRITE_BARRIER_FLAG));
    gc_gms_swap_vtables(pmc);
}

static void
gc_gms_unseal_object(PARROT_INTERP, ARGIN(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_seal_object)
    /* "Seal" object with write barrier */
    VTABLE  *t   = pmc->vtable;

    PARROT_ASSERT(t->flags & VTABLE_IS_WRITE_BARRIER_FLAG);
    gc_gms_swap_vtables(pmc);
}

static void
gc_gms_swap_vtables(ARGIN(PMC *pmc))
{
    ASSERT_ARGS(gc_gms_swap_vtables)

    VTABLE  *t   = pmc->vtable;

    PARROT_ASSERT(pmc->vtable);
    PARROT_ASSERT(pmc->vtable->wb_variant_vtable);

    pmc->vtable = pmc->vtable->wb_variant_vtable;
    pmc->vtable->wb_variant_vtable = t;

    PARROT_ASSERT(pmc->vtable != pmc->vtable->wb_variant_vtable);
    PARROT_ASSERT(pmc->vtable != pmc->vtable->ro_variant_vtable);
}

/*
=item C<static void gc_gms_set_gen_flags(PARROT_INTERP, PMC *pmc, int gen)>

Set flags for generation.

=cut
*/
static void
gc_gms_set_gen_flags(PARROT_INTERP, ARGIN(PMC *pmc), int gen)
{
    ASSERT_ARGS(gc_gms_set_gen_flags)
    pmc->flags &= ~(PObj_GC_generation_0_FLAG
        | PObj_GC_generation_1_FLAG
        | PObj_GC_generation_2_FLAG);
    pmc->flags |= GEN2FLAGS(gen);
}

/*

=item C<static void gc_gms_check_sanity(PARROT_INTERP)>

sanity check

=cut

*/

static void
gc_gms_check_sanity(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_check_sanity)
#if 0
#ifndef NDEBUG
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    List_Item_Header *tmp;
    int gen;

    for (gen=0; gen < 2; gen++) {
        tmp = self->objects[gen]->first;
        while (tmp) {
            PMC * pmc = LLH2Obj_typed(tmp, PMC);

            PARROT_ASSERT(pobj2gen((PObj *)pmc) == gen);

            tmp = tmp->next;
        }
    }
#endif
#endif
}

/*

=item C<static void gc_gms_print_stats(PARROT_INTERP, const char* header, int
gen)>

debug function

=cut

*/

static void
gc_gms_print_stats(PARROT_INTERP, ARGIN(const char* header), int gen)
{
    ASSERT_ARGS(gc_gms_print_stats)
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    size_t            i;

    fprintf(stderr, "%s\ngen: %d\n", header, gen);

#ifndef NDEBUG
    for (i = 0; i < MAX_GENERATIONS; i++)
        fprintf(stderr, "%d: %d %d\n",
                i, self->objects[i]->count, self->strings[i]->count);
#endif

    fprintf(stderr, "PMC: %d\n", Parrot_gc_pool_allocated_size(interp, self->pmc_allocator));
    fprintf(stderr, "STRING: %d\n", Parrot_gc_pool_allocated_size(interp, self->string_allocator));

    fprintf(stderr, "buffers: %d\n", self->string_gc.memory_pool->total_allocated);
    fprintf(stderr, "const buffers: %d\n", self->string_gc.constant_string_pool->total_allocated);

    fprintf(stderr, "attrs: %d\n", Parrot_gc_fixed_allocator_allocated_memory(interp,
                                                                      self->fixed_size_allocator));

    fprintf(stderr, "\n");

}

/*

=item C<static void gc_gms_ensure_flags_in_list(PARROT_INTERP,
Parrot_Pointer_Array* list)>

Make sure the flags are in the list

=cut

*/

static void
gc_gms_ensure_flags_in_list(PARROT_INTERP, ARGIN(Parrot_Pointer_Array* list))
{
    ASSERT_ARGS(gc_gms_ensure_flags_in_list)
#if 0
    List_Item_Header *tmp = list->first;
    while (tmp) {
        PObj *obj = LLH2Obj_typed(tmp, PObj);
        PARROT_ASSERT(!(obj->flags & b_PObj_live_FLAG));
        tmp = tmp->next;
    }
#endif
}

/*

=item C<static void gc_gms_ensure_flags(PARROT_INTERP)>

debug function to check for flags

=cut

*/

static void
gc_gms_ensure_flags(PARROT_INTERP)
{
    ASSERT_ARGS(gc_gms_ensure_flags)
#ifndef NDEBUG
    MarkSweep_GC     *self = (MarkSweep_GC *)interp->gc_sys->gc_private;
    gc_gms_ensure_flags_in_list(interp, self->objects[0]);
    gc_gms_ensure_flags_in_list(interp, self->objects[1]);
    gc_gms_ensure_flags_in_list(interp, self->objects[2]);
    gc_gms_ensure_flags_in_list(interp, self->strings[0]);
    gc_gms_ensure_flags_in_list(interp, self->strings[1]);
    gc_gms_ensure_flags_in_list(interp, self->strings[2]);
#endif
}

/*

=back

=cut

*/

/*
 * Local variables:
 *   c-file-style: "parrot"
 * End:
 * vim: expandtab shiftwidth=4:
 */