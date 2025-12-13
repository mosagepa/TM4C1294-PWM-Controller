
/* malloc_lock_stubs.c - no-op stubs for newlib malloc locking (single-threaded)
 *
 * Provide the reentrant lock/unlock used by some builds of newlib:
 *   void __malloc_lock(struct _reent *re);
 *   void __malloc_unlock(struct _reent *re);
 *
 * Also provide common no-argument variants that some toolchains may call:
 *   void __malloc_lock_noarg(void);
 *   void __malloc_unlock_noarg(void);
 *
 * The no-arg variants just call the reentrant versions with NULL.
 */

#include <stdlib.h>
#include <reent.h>

/* Reentrant versions (called by newlib when configured for thread-safety) */
void __malloc_lock(struct _reent *re)  { (void)re; }
void __malloc_unlock(struct _reent *re){ (void)re; }

/* No-arg wrappers that forward to the reentrant versions.
   Some newlib variants call the no-arg names; providing these
   avoids undefined references and also avoids alias/type warnings. */
void __malloc_lock_noarg(void)  { __malloc_lock(NULL); }
void __malloc_unlock_noarg(void){ __malloc_unlock(NULL); }

/* Some versions expect names without '_noarg' suffix — provide those too. */
void __malloc_lock_no_arg(void)  { __malloc_lock(NULL); } /* alternate spelling if referenced */
void __malloc_unlock_no_arg(void){ __malloc_unlock(NULL); }
