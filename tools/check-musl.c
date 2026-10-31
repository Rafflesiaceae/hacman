/* musl has no public identification macro. Its sys/types.h requests size_t from
 * bits/alltypes.h, which records the definition with __DEFINED_size_t. Check
 * the actual headers rather than compiler names or target triples: musl-gcc
 * can report the underlying GCC's glibc target triple. */
#include <features.h>
#include <sys/types.h>

#if defined(__GLIBC__) || !defined(__DEFINED_size_t)
#error "hacman requires musl libc"
#endif

int main(void) { return 0; }
