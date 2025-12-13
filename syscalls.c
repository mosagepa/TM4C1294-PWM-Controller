/* Minimal newlib syscall stubs for bare-metal usage. */

#include <unistd.h>
#include <stddef.h>
#include <stdint.h>
#include <stdalign.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>

/* Provide a portable way to determine a "sane" alignment without requiring C11.
 * If the toolchain supports stdalign.h and max_align_t, that would be ideal,
 * but many embedded Makefiles use -std=gnu99/-std=c99 so we avoid relying on it.
 *
 * Heuristic:
 *   use the larger of sizeof(void*) and sizeof(long double) as a conservative alignment.
 * This will normally be 8 on ARMv7-M toolchains and is safe for malloc/realloc usage.
 */
#ifndef SBRK_ALIGNMENT
#define SBRK_ALIGNMENT ((sizeof(void *) > sizeof(long double)) ? sizeof(void *) : sizeof(long double))
#endif


/* Linker-provided symbols (ensure these exist in your .ld exactly) */
extern char _heap_start;  /* bottom / start */
extern char _heap_end;    /* top / limit */

static char *heap_ptr = NULL;      /* current program break (next free byte) */
volatile unsigned int sbrk_calls = 0; /* debug counter - optional */

void * _sbrk(ptrdiff_t incr)
{
    char *start = &_heap_start;
    char *limit = &_heap_end;

    if (heap_ptr == NULL) {
        heap_ptr = start;
    }

    /* handle zero request: return current break (after alignment) */
    if (incr == 0) {
        /* return aligned current break */
        uintptr_t cur = (uintptr_t)heap_ptr;
        uintptr_t aligned = (cur + (SBRK_ALIGNMENT - 1)) & ~(uintptr_t)(SBRK_ALIGNMENT - 1);
        return (void *)aligned;
    }

    /* compute an aligned base where the returned pointer will point */
    uintptr_t cur = (uintptr_t)heap_ptr;
    uintptr_t aligned = (cur + (SBRK_ALIGNMENT - 1)) & ~(uintptr_t)(SBRK_ALIGNMENT - 1);
    char *aligned_ptr = (char *)aligned;

    if (incr > 0) {
        /* check for overflow in pointer arithmetic */
        if ((uintptr_t)aligned_ptr + (uintptr_t)incr < (uintptr_t)aligned_ptr) {
            errno = ENOMEM;
            return (void *) -1;
        }
        char *new_break = aligned_ptr + incr;
        if (new_break > limit) {
            errno = ENOMEM;
            return (void *) -1;
        }
        char *prev = aligned_ptr;
        heap_ptr = new_break;
        sbrk_calls++;
        return (void *) prev;
    } else { /* incr < 0 -- shrinking request */
        /* Do not allow shrinking below start */
        if ((uintptr_t)aligned_ptr + (intptr_t)incr < (uintptr_t)start) {
            errno = EINVAL;
            return (void *) -1;
        }
        heap_ptr = aligned_ptr + incr;
        sbrk_calls++;
        return (void *) aligned_ptr;
    }
}

int _close(int fd) { (void)fd; errno = EBADF; return -1; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
off_t _lseek(int fd, off_t offset, int whence) { (void)fd; (void)offset; (void)whence; return (off_t)0; }
ssize_t _read(int fd, void *buf, size_t count) { (void)fd; (void)buf; (void)count; errno = EBADF; return -1; }
ssize_t _write(int fd, const void *buf, size_t count) { (void)fd; (void)buf; (void)count; errno = EBADF; return -1; }

void _exit(int status) { (void)status; while (1) { } }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }

