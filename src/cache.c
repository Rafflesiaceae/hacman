/* The cache: what hacman last saw, one tiny file per watched thing.
 *
 * Below ~/.cache/hacman by default. A record is two lines:
 *
 *   #hacman-cache 1 <identity>
 *   <last_check>\t<last_change>\t<mark>
 *
 * The identity line protects every cache lookup: a record whose identity does
 * not match is ignored rather than trusted (identity.c defines what it is made
 * of). Most file names hash that identity; embedded projects instead hash their
 * input path so edits reuse one record and work directory.
 *
 * One file per record keeps the fast path down to a single small read. Slow
 * paths additionally take a per-record lock, so unrelated projects proceed
 * independently while concurrent updates of one project serialize. */

#include "hacman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h> /* rename() */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HM_CACHE_MAGIC "#hacman-cache 1 "

static char record_buf[HM_RECORD_MAX];
static char cache_dir_buf[HM_PATH_MAX + 1];

/* Appends `s`, replacing anything that would break the record layout. */
static size_t append_clean(char *dst, size_t o, size_t cap, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len && o + 1 < cap; ++i) {
        char ch = s[i];
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        dst[o++] = ch;
    }
    return o;
}

static size_t append_str(char *dst, size_t o, size_t cap, const char *s)
{
    while (*s != '\0' && o + 1 < cap) dst[o++] = *s++;
    return o;
}

static size_t append_long(char *dst, size_t o, size_t cap, long v)
{
    char          tmp[24];
    size_t        n = 0;
    unsigned long x = (v < 0) ? 0UL : (unsigned long)v;

    do {
        tmp[n++] = (char)('0' + (x % 10));
        x /= 10;
    } while (x > 0 && n < sizeof(tmp));
    while (n > 0 && o + 1 < cap) dst[o++] = tmp[--n];
    return o;
}

/* Anchors a relative cache directory at the current directory.
 *
 * Setup children chdir() into their work directory before the sandbox opens
 * its writable paths, and nested hacman runs start from wherever the setup
 * happens to be, so every cache-derived path must stay valid regardless of
 * the cwd. Absolute paths, the normal case, are returned untouched without a
 * syscall. If the cwd cannot be determined the relative path is kept. */
static const char *absolute_cache_dir(const char *dir)
{
    static char abs_buf[HM_PATH_MAX + 1];
    char        cwd[HM_PATH_MAX + 1];
    size_t      o;

    if (dir[0] == '/') return dir;
    if (getcwd(cwd, sizeof(cwd)) == NULL) return dir;
    if (strlen(cwd) + 1 + strlen(dir) > HM_PATH_MAX) return dir;

    o          = append_str(abs_buf, 0, sizeof(abs_buf), cwd);
    o          = append_str(abs_buf, o, sizeof(abs_buf), "/");
    o          = append_str(abs_buf, o, sizeof(abs_buf), dir);
    abs_buf[o] = '\0';
    return abs_buf;
}

const char *hm_cache_dir(const char *override)
{
    const hm_env *startup;
    const char   *env;
    size_t        o = 0;

    if (override != NULL && override[0] != '\0') return absolute_cache_dir(override);

    startup = hm_env_get();
    env = startup->cache;
    if (env != NULL && env[0] != '\0') return absolute_cache_dir(env);

    /* Keep every default cache artifact under one predictable user-owned
     * tree. Falling back to the caller's cwd would leak cache directories
     * into arbitrary projects. */
    env = startup->home;
    if (env == NULL || env[0] == '\0') return NULL;
    o                = append_str(cache_dir_buf, o, sizeof(cache_dir_buf), env);
    o                = append_str(cache_dir_buf, o, sizeof(cache_dir_buf), "/.cache/hacman");
    cache_dir_buf[o] = '\0';
    return absolute_cache_dir(cache_dir_buf);
}

void hm_cache_init(hm_cache *c, const hm_project *p, const char *dir, const char *source_path,
                   const char *identity)
{
    char   hash[17];
    size_t o = 0;

    c->mark[0]     = '\0';
    c->last_check  = 0;
    c->last_change = 0;
    c->known       = 0;
    c->workdir[0]  = '\0';

    o              = append_str(c->identity, 0, sizeof(c->identity), identity);
    c->identity[o] = '\0';
    o              = 0;
    /* A file-backed embedded project owns one stable cache location. Its
     * changing identity remains inside that record and invalidates it on the
     * next load. Standard input has no stable path, so it keeps the
     * content-addressed fallback. */
    if (p->file_count > 0 && source_path != NULL) {
        hm_hash_hex(source_path, strlen(source_path), hash);
    } else {
        hm_hash_hex(c->identity, strlen(c->identity), hash);
    }

    o = append_str(c->key, 0, sizeof(c->key), (p->kind == HM_KIND_COMMAND) ? "cmd-" : "url-");
    o = append_str(c->key, o, sizeof(c->key), hash);
    c->key[o] = '\0';

    o          = append_str(c->path, 0, sizeof(c->path), dir);
    o          = append_str(c->path, o, sizeof(c->path), "/");
    o          = append_str(c->path, o, sizeof(c->path), c->key);
    c->path[o] = '\0';

    /* Every project owns a writable sandbox directory. Embedded commands also
     * use it as their source/build directory; other project kinds leave it
     * empty until setup code actually needs to run. */
    o             = append_str(c->workdir, 0, sizeof(c->workdir), dir);
    o             = append_str(c->workdir, o, sizeof(c->workdir), "/");
    o             = append_str(c->workdir, o, sizeof(c->workdir), c->key);
    o             = append_str(c->workdir, o, sizeof(c->workdir), ".work");
    c->workdir[o] = '\0';
}

/* Splits off the next tab-separated field of a record line. */
static hm_str next_field(const char **pos, const char *end)
{
    hm_str      f;
    const char *p = *pos;
    const char *q = p;

    while (q < end && *q != '\t') ++q;
    f.ptr = p;
    f.len = (size_t)(q - p);
    *pos  = (q < end) ? q + 1 : end;
    return f;
}

static long parse_epoch(hm_str s)
{
    unsigned long v;
    if (hm_parse_ulong(s, &v) != 0) return 0;
    return (long)v;
}

int hm_cache_load(hm_cache *c)
{
    int         fd;
    size_t      total = 0;
    const char *p, *end, *nl;

    /* A caller may reload after waiting for another process. Clear only the
     * mutable record state so a disappeared or replaced record cannot leave
     * stale values from the first read behind. */
    c->mark[0]     = '\0';
    c->last_check  = 0;
    c->last_change = 0;
    c->known       = 0;

    fd = open(c->path, O_RDONLY);
    if (fd < 0) {
        /* No record yet is the normal state of a fresh cache. */
        if (errno == ENOENT) return 0;
        hm_err("hacman: %s: cannot read cache record: %s\n", c->path, strerror(errno));
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, record_buf + total, sizeof(record_buf) - total - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            hm_err("hacman: %s: read failed: %s\n", c->path, strerror(errno));
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total + 1 >= sizeof(record_buf)) break;
        /* Atomic cache writes contain two LF-terminated lines. Once both are
         * present, an EOF read cannot contribute anything the parser uses.
         * Incomplete/short reads still continue until the record or EOF. */
        nl = (const char *)memchr(record_buf, '\n', total);
        if (nl != NULL && memchr(nl + 1, '\n', total - (size_t)(nl + 1 - record_buf)) != NULL) {
            break;
        }
    }
    close(fd);

    p   = record_buf;
    end = record_buf + total;

    /* Line 1: the magic plus the identity this record belongs to. */
    nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (nl == NULL) return 0;
    {
        size_t magic_len = sizeof(HM_CACHE_MAGIC) - 1;
        size_t line_len  = (size_t)(nl - p);
        size_t id_len    = strlen(c->identity);

        if (line_len < magic_len || memcmp(p, HM_CACHE_MAGIC, magic_len) != 0) {
            return 0; /* not ours: treat as absent, it will be overwritten */
        }
        if (line_len - magic_len != id_len || memcmp(p + magic_len, c->identity, id_len) != 0) {
            /* Same file name, different meaning: ignore rather than trust. */
            return 0;
        }
    }
    p = nl + 1;

    /* Line 2: last check, last change, marker. */
    nl = (const char *)memchr(p, '\n', (size_t)(end - p));
    if (nl == NULL) nl = end;
    {
        const char *cur     = p;
        hm_str      checked = next_field(&cur, nl);
        hm_str      changed = next_field(&cur, nl);
        hm_str      mark    = next_field(&cur, nl);

        c->last_check  = parse_epoch(checked);
        c->last_change = parse_epoch(changed);
        hm_str_copy(c->mark, sizeof(c->mark), mark);
        c->known = 1;
    }
    return 0;
}

/* Creates `path` and every missing component above it. */
static void mkdir_p(const char *path)
{
    char   tmp[HM_PATH_MAX + 1];
    size_t i, n = 0;

    while (path[n] != '\0' && n + 1 < sizeof(tmp)) {
        tmp[n] = path[n];
        ++n;
    }
    tmp[n] = '\0';

    for (i = 1; tmp[i] != '\0'; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

/* Copies the directory portion of a cache-record path. */
static void cache_parent(const hm_cache *c, char out[HM_PATH_MAX + 1])
{
    size_t i = 0;
    size_t n = strlen(c->path);

    while (n > 0 && c->path[n - 1] != '/') --n;
    while (i + 1 < n && i + 1 < HM_PATH_MAX + 1) {
        out[i] = c->path[i];
        ++i;
    }
    out[i] = '\0';
}

#ifdef __linux__
/* Reads the parent of `pid` from /proc/<pid>/stat, or returns -1. The comm
 * field may itself contain spaces and parentheses, so parsing starts after
 * its last ')': " <state> <ppid> ...". */
static long parent_pid(long pid)
{
    char        path[48], buf[512];
    const char *p;
    size_t      o;
    ssize_t     n;
    long        ppid = 0;
    int         fd;

    o       = append_str(path, 0, sizeof(path), "/proc/");
    o       = append_long(path, o, sizeof(path), pid);
    o       = append_str(path, o, sizeof(path), "/stat");
    path[o] = '\0';

    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    p = strrchr(buf, ')');
    if (p == NULL || p[1] != ' ' || p[2] == '\0' || p[3] != ' ') return -1;
    for (p += 4; *p >= '0' && *p <= '9'; ++p) ppid = ppid * 10 + (*p - '0');
    return ppid;
}

/* Whether `pid` is one of this process's ancestors. The walk is bounded so a
 * vanishing or recycled process cannot keep it going. */
static int is_ancestor(long pid)
{
    long cur = (long)getppid();
    int  depth;

    for (depth = 0; cur > 0 && depth < 256; ++depth) {
        if (cur == pid) return 1;
        if (cur == 1) break;
        cur = parent_pid(cur);
    }
    return 0;
}
#endif

/* A setup step that runs a tool whose own setup is in progress further up the
 * process tree (a shim invoking itself, or A -> B -> A) would block forever:
 * the holder is waiting for this very process to exit. Detect that before
 * blocking. Returns 1 if the lock on `fd` is held by an ancestor. */
static int held_by_ancestor(int fd, const struct flock *want, long *holder)
{
#ifdef __linux__
    struct flock probe = *want;

    if (fcntl(fd, F_GETLK, &probe) != 0 || probe.l_type == F_UNLCK) return 0;
    *holder = (long)probe.l_pid;
    return is_ancestor(*holder);
#else
    (void)fd;
    (void)want;
    (void)holder;
    return 0;
#endif
}

int hm_cache_lock(const hm_cache *c)
{
    char         lock_path[HM_PATH_MAX + 6];
    char         dir[HM_PATH_MAX + 1];
    size_t       o;
    int          fd;
    long         holder = 0;
    struct flock lock;

    /* Lock files are permanent siblings of records. Keeping the inode stable
     * avoids the unlink/recreate race inherent in transient lock files. */
    o            = append_str(lock_path, 0, sizeof(lock_path), c->path);
    o            = append_str(lock_path, o, sizeof(lock_path), ".lock");
    lock_path[o] = '\0';

    fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0 && errno == ENOENT) {
        /* Existing projects avoid all mkdir calls; only a fresh cache pays
         * for creating its directory hierarchy before retrying the open. */
        cache_parent(c, dir);
        mkdir_p(dir);
        fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    }
    if (fd < 0) {
        hm_err("hacman: %s: cannot open update lock: %s\n", lock_path, strerror(errno));
        return -1;
    }

    memset(&lock, 0, sizeof(lock));
    lock.l_type   = F_WRLCK;
    lock.l_whence = SEEK_SET;
    /* The uncontended case takes the lock without probing anything. Only a
     * busy lock pays for the ancestry check before waiting for its holder. */
    if (fcntl(fd, F_SETLK, &lock) == 0) return fd;
    if ((errno == EACCES || errno == EAGAIN) && held_by_ancestor(fd, &lock, &holder)) {
        hm_err("hacman: %s: update lock is held by parent process %d, whose setup "
               "re-entered this project; refusing to wait for it\n",
               lock_path, holder);
        close(fd);
        return -1;
    }
    while (fcntl(fd, F_SETLKW, &lock) != 0) {
        if (errno == EINTR) continue;
        hm_err("hacman: %s: cannot acquire update lock: %s\n", lock_path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

void hm_cache_unlock(int lock_fd)
{
    /* close() releases a process-owned fcntl lock atomically; O_CLOEXEC is a
     * second line of defence if a future error path reaches exec first. */
    if (lock_fd >= 0) close(lock_fd);
}

int hm_cache_save(const hm_cache *c)
{
    char   tmp_path[HM_PATH_MAX + 32];
    char   dir[HM_PATH_MAX + 1];
    size_t o = 0, n;
    int    fd;

    /* Sibling temp file, named after this process so that parallel runs cannot
     * step on each other, then rename() into place. */
    o           = append_str(tmp_path, 0, sizeof(tmp_path), c->path);
    o           = append_str(tmp_path, o, sizeof(tmp_path), ".tmp.");
    o           = append_long(tmp_path, o, sizeof(tmp_path), (long)getpid());
    tmp_path[o] = '\0';

    n = strlen(c->path);
    cache_parent(c, dir);
    if (n > 1) mkdir_p(dir);

    o = append_str(record_buf, 0, sizeof(record_buf), HM_CACHE_MAGIC);
    o = append_str(record_buf, o, sizeof(record_buf), c->identity);
    o = append_str(record_buf, o, sizeof(record_buf), "\n");
    o = append_long(record_buf, o, sizeof(record_buf), c->last_check);
    o = append_str(record_buf, o, sizeof(record_buf), "\t");
    o = append_long(record_buf, o, sizeof(record_buf), c->last_change);
    o = append_str(record_buf, o, sizeof(record_buf), "\t");
    o = append_clean(record_buf, o, sizeof(record_buf), c->mark, strlen(c->mark));
    o = append_str(record_buf, o, sizeof(record_buf), "\n");

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        hm_err("hacman: %s: cannot write cache record: %s\n", tmp_path, strerror(errno));
        return -1;
    }
    {
        size_t written = 0;
        while (written < o) {
            ssize_t w = write(fd, record_buf + written, o - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                hm_err("hacman: %s: write failed: %s\n", tmp_path, strerror(errno));
                close(fd);
                (void)unlink(tmp_path);
                return -1;
            }
            written += (size_t)w;
        }
    }
    close(fd);

    if (rename(tmp_path, c->path) != 0) {
        hm_err("hacman: %s: cannot replace cache record: %s\n", c->path, strerror(errno));
        (void)unlink(tmp_path);
        return -1;
    }
    return 0;
}
