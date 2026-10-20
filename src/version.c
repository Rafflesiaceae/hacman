/* The hacman version: the Meson project version plus the binary's own digest.
 *
 *   <project version>+<xxh3-64 of the binary, 16 hex digits>
 *
 * The digest is not known while compiling; tools/stamp.c writes it into the
 * slot below after linking (see selfhash.h). Nothing in the binary depends on
 * Git, the clock or the build machine, so the same source built with the same
 * toolchain is the same file byte for byte, and its version changes exactly
 * when the program does. The project identity embeds this version, so a cache
 * record stays valid across rebuilds that produce an identical binary. */

#include "hacman.h"

#include <stdint.h>
#include <string.h>

#include "config.h"
#include "selfhash.h"

/* The slot hm-stamp fills in.
 * - volatile: the initializer is a constant 0, so without it the compiler
 *   folds every read to 0 and the stamped value would never be seen;
 * - section: see HM_SELFHASH_SECTION;
 * - used: keeps the object even if a future build stops reading it. */
__attribute__((section(HM_SELFHASH_SECTION), used, aligned(8))) static const volatile uint64_t
    hm_self_hash = 0;

const char *hm_version(void)
{
    static const char hex[] = "0123456789abcdef";
    static char       version[sizeof(HM_PROJECT_VERSION) + 1 + 16];
    uint64_t          h;
    size_t            o;
    int               i;

    /* Formatted once; hacman is single-threaded. */
    if (version[0] != '\0') return version;

    h = hm_self_hash;
    o = sizeof(HM_PROJECT_VERSION) - 1;
    memcpy(version, HM_PROJECT_VERSION, o);
    version[o++] = '+';
    for (i = 0; i < 16; ++i) version[o++] = hex[(h >> (60 - 4 * i)) & 0xF];
    version[o] = '\0';
    return version;
}
