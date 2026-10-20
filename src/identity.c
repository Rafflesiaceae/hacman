/* The project identity: what a cache record stands for.
 *
 * A record is only trusted while the identity stored in it equals the identity
 * of the project being run. Whenever they differ, hacman treats the project as
 * never set up and starts again from an empty work directory. So the identity
 * decides exactly when previously completed setup stops counting.
 *
 * The identity is
 *
 *   siml:<xxh3-64 of every byte of the project file> hacman:<version>
 *
 * - The whole file, byte for byte. Nothing is parsed, normalised or left out:
 *   comments, whitespace, `name`, `install`, `bin-path`, sandbox settings and
 *   embedded files all count. Any edit to the project therefore starts setup
 *   again, and no field can be forgotten when one is added to the format.
 *   Deliberately cheap edits (a comment, a rename) cost one re-setup; that is
 *   the price of never running stale setup.
 *
 * - The version of the hacman that executes it (hm_version(), which includes
 *   a digest of the binary itself). Setup is the product of both the project
 *   and the program interpreting it, so a build that differs in any byte never
 *   inherits state from another one, even for an unchanged file, while a
 *   rebuild that reproduces the same binary keeps it.
 *
 * What is not part of it: the file's path (the cache address in cache.c
 * covers that), its timestamps, and anything outside the file such as the
 * environment, the network or other files that a command reads. Those change
 * what a run does without changing what the project is; the schedule and the
 * `check` mechanism exist to notice them.
 *
 * The string is a fixed-size digest rather than the file text, so it stays
 * short whatever the project contains and never needs truncating. It is
 * written as the first line of each record, which makes `head -1` on a record
 * enough to compare it with `hacman --plan`'s `cache-identity:` line. */

#include "hacman.h"

#include <string.h>

void hm_identity_build(const char *siml, size_t len, char *out, size_t cap)
{
    static const char prefix[]  = "siml:";
    static const char infix[]   = " hacman:";
    const char       *version   = hm_version();
    size_t            version_n = strlen(version);
    char              hash[17];
    size_t            o = 0;

    hm_hash_hex(siml, len, hash);

    /* The pieces are fixed or bounded by the build, so a buffer of
     * HM_IDENTITY_MAX always fits them; the guard keeps a smaller `cap` from
     * overrunning rather than silently producing a shorter, ambiguous string
     * (a truncated identity could compare equal for two different projects). */
    if (cap < sizeof(prefix) - 1 + 16 + sizeof(infix) - 1 + version_n + 1) {
        out[0] = '\0';
        return;
    }
    memcpy(out + o, prefix, sizeof(prefix) - 1);
    o += sizeof(prefix) - 1;
    memcpy(out + o, hash, 16);
    o += 16;
    memcpy(out + o, infix, sizeof(infix) - 1);
    o += sizeof(infix) - 1;
    memcpy(out + o, version, version_n + 1); /* includes the NUL */
}
