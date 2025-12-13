
/*
 * malloc_simple.c ----------------------------------------------
 *
 * Minimal single-threaded allocator built on _sbrk().
 * - malloc: returns aligned memory (simple header with size).
 * - free:  no-op (does not reclaim).
 * - realloc: allocates new block and copies min(old,new) bytes.
 *
 * Use only in single-threaded embedded contexts as a
 * temporary/diagnostic allocator.
 * --------------------------------------------------------------
 */

/* RATIONALE: ---------------------------------------------------

   When using even the most reduced and default callbacks/ helper
   implementations for _sbrk and its "friend functions", actual
   allocation operations in the user code tend to stall the uC on
   (internal, libc) default calls to functions such as e.g. realloc().

   These stalls are caused by the libc allocator expecting locking
   hooks (or dispatching to runtime code that blocked).

   Providing the no‑op __malloc_lock/__malloc_unlock implementations
   resolved that stall on this single‑threaded bare‑metal system.

   That is a typical and safe fix for single‑threaded firmware:
   newlib may be built with thread-safety features that expect lock
   functions to be provided.

   As we pointed out before, on a single-threaded MCU you can
   implement them as basically no‑ops stubs...

  -------------------------------------------------------------------- */


#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>


/* _sbrk provided by your syscalls file */
extern void * _sbrk(ptrdiff_t incr);

/* alignment: 8 bytes for ARMv7-M hard-float */
#ifndef MALLOC_ALIGN
#define MALLOC_ALIGN 8
#endif


/* block header placed immediately before returned pointer */
typedef struct {
    size_t size;
} ms_header_t;

static inline size_t align_up(size_t v, size_t a) {
    return (v + (a - 1)) & ~(a - 1);
}


/* malloc: request size bytes, return pointer or NULL + errno=ENOMEM */
void * malloc(size_t size)
{
    if (size == 0) size = 1;
    size_t asize = align_up(size, MALLOC_ALIGN);
    size_t total = sizeof(ms_header_t) + asize;
    void *p = _sbrk((ptrdiff_t)total);
    if (p == (void*)-1) {
        errno = ENOMEM;
        return NULL;
    }
    ms_header_t *h = (ms_header_t *)p;
    h->size = asize;
    void *user = (void *)(h + 1);
    return user;
}


/* free: noop in this simple allocator */
void free(void *ptr)
{
    (void)ptr;
}


/* realloc: naive alloc-copy-free */
void * realloc(void *ptr, size_t newsize)
{
    if (!ptr) return malloc(newsize);
    if (newsize == 0) { free(ptr); return NULL; }

    ms_header_t *h = (ms_header_t *)ptr - 1;
    size_t oldsize = h->size;
    void *newptr = malloc(newsize);

    if (!newptr) return NULL;

    size_t copy = (oldsize < newsize) ? oldsize : newsize;

    memcpy(newptr, ptr, copy);

    /* free is noop */
    return newptr;

}

