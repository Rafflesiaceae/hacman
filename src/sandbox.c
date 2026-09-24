/* Filesystem confinement for project-controlled setup code.
 *
 * Landlock composes with the caller's normal permissions and follows the
 * process across exec. We grant read/execute access from the filesystem root
 * and add the mutation rights only for one cache-owned work directory, plus
 * the cache directory as a whole so a setup script that shells out to
 * another hacman-managed tool (hacman is meant to be self-hosted this way -
 * see examples/meson.siml, tig.siml, bazelisk.siml) can still let that
 * nested hacman invocation write its own cache record. */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "hacman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#include <sys/syscall.h>

/* Keep the stable Landlock UAPI subset local: minimal musl sysroots expose
 * the syscall numbers without installing Linux's optional UAPI headers. */
struct landlock_ruleset_attr {
    uint64_t handled_access_fs;
};

struct landlock_path_beneath_attr {
    uint64_t allowed_access;
    int32_t  parent_fd;
} __attribute__((packed));

#define LANDLOCK_CREATE_RULESET_VERSION (1U << 0)
#define LANDLOCK_RULE_PATH_BENEATH      1
#define LANDLOCK_ACCESS_FS_EXECUTE      (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE   (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE    (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR     (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR   (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE  (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR    (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR     (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG     (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK    (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO    (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK   (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM     (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER        (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE     (1ULL << 14)
#define LANDLOCK_ACCESS_FS_IOCTL_DEV    (1ULL << 15)
#define LANDLOCK_ACCESS_FS_RESOLVE_UNIX (1ULL << 16)

/* ABI 3 is the first version that mediates truncation as well as opens and
 * directory mutations. Earlier ABIs cannot implement a read-only host. */
#define HM_LANDLOCK_MIN_ABI 3

static unsigned long long handled_access(int abi)
{
    unsigned long long access =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR | LANDLOCK_ACCESS_FS_REMOVE_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_FILE | LANDLOCK_ACCESS_FS_MAKE_CHAR |
        LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK | LANDLOCK_ACCESS_FS_MAKE_SYM;

    if (abi >= 2) access |= LANDLOCK_ACCESS_FS_REFER;
    if (abi >= 3) access |= LANDLOCK_ACCESS_FS_TRUNCATE;
    if (abi >= 5) access |= LANDLOCK_ACCESS_FS_IOCTL_DEV;
    if (abi >= 9) access |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
    return access;
}

static unsigned long long readonly_access(int abi)
{
    unsigned long long access =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;

    /* Resolving a pathname socket does not mutate its filesystem entry and is
     * needed by otherwise read-only clients of host services. */
    if (abi >= 9) access |= LANDLOCK_ACCESS_FS_RESOLVE_UNIX;
    return access;
}

static int add_path_rule(int ruleset_fd, const char *path, unsigned long long access, int directory)
{
    struct landlock_path_beneath_attr rule;
    int                               path_fd;
    int                               rc;

    /* O_NOFOLLOW keeps a replaced final entry from redirecting a rule;
     * directory rules additionally reject non-directory objects. */
    path_fd = open(path, O_PATH | O_CLOEXEC | O_NOFOLLOW | (directory ? O_DIRECTORY : 0));
    if (path_fd < 0) {
        fprintf(stderr, "hacman: sandbox: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    memset(&rule, 0, sizeof(rule));
    rule.allowed_access = access;
    rule.parent_fd      = path_fd;
    rc = (int)syscall(SYS_landlock_add_rule, ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    if (rc != 0) {
        fprintf(stderr, "hacman: sandbox: cannot allow %s: %s\n", path, strerror(errno));
    }
    close(path_fd);
    return rc;
}

int hm_sandbox_enter(const char *workdir, const char *cache_dir)
{
    struct landlock_ruleset_attr ruleset;
    unsigned long long           access;
    int                          abi;
    int                          ruleset_fd;
    int                          rc = -1;

    abi = (int)syscall(SYS_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < HM_LANDLOCK_MIN_ABI) {
        if (abi < 0) {
            fprintf(stderr, "hacman: sandbox: Landlock is unavailable: %s\n", strerror(errno));
        } else {
            fprintf(stderr, "hacman: sandbox: Landlock ABI %d is too old (need %d)\n", abi,
                    HM_LANDLOCK_MIN_ABI);
        }
        return -1;
    }

    access = handled_access(abi);
    memset(&ruleset, 0, sizeof(ruleset));
    ruleset.handled_access_fs = access;
    /* Only the ABI-1 prefix is passed, allowing this binary to run against
     * older kernels even when built with a newer growing UAPI structure. */
    ruleset_fd =
        (int)syscall(SYS_landlock_create_ruleset, &ruleset, sizeof(ruleset.handled_access_fs), 0);
    if (ruleset_fd < 0) {
        fprintf(stderr, "hacman: sandbox: cannot create Landlock ruleset: %s\n", strerror(errno));
        return -1;
    }

    if (add_path_rule(ruleset_fd, "/", readonly_access(abi), 1) != 0) goto cleanup;
    if (add_path_rule(ruleset_fd, workdir, access, 1) != 0) goto cleanup;
    /* `cache_dir` already contains `workdir`, so this rule alone would be
     * enough; the workdir rule above stays so a caller passing a workdir
     * outside cache_dir still gets it. Nested hacman tools (meson, tig, ...
     * found on PATH) need this to lock and rewrite their own cache record,
     * which lives directly in cache_dir rather than under this project's
     * workdir. */
    if (add_path_rule(ruleset_fd, cache_dir, access, 1) != 0) goto cleanup;
    /* Discarding output is ubiquitous shell behavior; grant only the write
     * right on this device node, not on its /dev parent hierarchy. */
    if (add_path_rule(ruleset_fd, "/dev/null", LANDLOCK_ACCESS_FS_WRITE_FILE, 0) != 0) {
        goto cleanup;
    }

    /* Unprivileged callers must prevent privilege gains before restricting
     * themselves, which also keeps set-ID executables from escaping policy. */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        fprintf(stderr, "hacman: sandbox: cannot set no_new_privs: %s\n", strerror(errno));
        goto cleanup;
    }
    if (syscall(SYS_landlock_restrict_self, ruleset_fd, 0) != 0) {
        fprintf(stderr, "hacman: sandbox: cannot enforce Landlock ruleset: %s\n", strerror(errno));
        goto cleanup;
    }
    rc = 0;

cleanup:
    close(ruleset_fd);
    return rc;
}

#else

int hm_sandbox_enter(const char *workdir, const char *cache_dir)
{
    (void)workdir;
    (void)cache_dir;
    fprintf(stderr,
            "hacman: sandbox: Landlock requires Linux; use 'sandboxed: false' to opt out\n");
    return -1;
}

#endif
