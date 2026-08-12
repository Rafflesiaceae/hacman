#ifndef HACMAN_H_INCLUDED
#define HACMAN_H_INCLUDED

#include <stddef.h>

/* Compile-time limits.
 *
 * Every buffer hacman uses on its fast path lives in BSS or on the stack: the
 * check path never calls malloc(), so there is no allocator to warm up and no
 * failure mode to handle. BSS pages are faulted in lazily by the kernel, so
 * generous limits cost nothing at startup. */
#define HM_CONFIG_MAX   (1024u * 1024u)       /* .siml input                  */
#define HM_BODY_MAX     (4u * 1024u * 1024u)  /* HTTP response body           */
#define HM_RECORD_MAX   4096                  /* one cache record             */
#define HM_MARK_MAX     255                   /* etag / hash / version string */
#define HM_NAME_MAX     127                   /* display name only           */
#define HM_URL_MAX      1023
#define HM_COMMAND_MAX  2047
#define HM_PATH_MAX     1023                  /* working directory, cache dir */
#define HM_KEY_MAX      31                    /* cache file name             */
#define HM_IDENTITY_MAX 4095                  /* what a cache key stands for  */

/* A borrowed, non-NUL-terminated string. Config values are slices into the
 * single buffer the .siml input was read into; nothing is ever copied. */
typedef struct {
    const char *ptr;
    size_t      len;
} hm_str;

/* When to talk to the network at all. */
typedef enum {
    HM_SCHED_ALWAYS = 0,  /* check on every run                          */
    HM_SCHED_EVERY,       /* check if `interval` seconds have passed     */
    HM_SCHED_NEVER        /* only check when --force is given            */
} hm_sched_kind;

/* What an input file describes. */
typedef enum {
    HM_KIND_URL = 0,  /* watch a URL, install when it changed */
    HM_KIND_COMMAND   /* run a shell command, no more often than the schedule */
} hm_project_kind;

/* How "changed" is decided for a URL. */
typedef enum {
    HM_CHECK_ETAG = 0,  /* HEAD, compare ETag (or Last-Modified)          */
    HM_CHECK_HASH,      /* GET, compare a hash of the whole body          */
    HM_CHECK_VERSION    /* GET, compare a version substring from the body */
} hm_check_kind;

/* One project - and an input file describes exactly one of them. */
typedef struct {
    hm_project_kind kind;
    hm_str        name;            /* display only; defaults to url/command */
    hm_str        url;             /* HM_KIND_URL                           */
    hm_str        command;         /* HM_KIND_COMMAND                       */
    hm_str        workdir;         /* HM_KIND_COMMAND: raw {{VAR}} template */
    /* The one value that is not a slice of the input: expanding {{VAR}} has
     * to write somewhere. Defaults to the expansion of "{{HOME}}". */
    char          workdir_path[HM_PATH_MAX + 1];
    hm_check_kind check;
    hm_sched_kind sched_kind;
    long          sched_interval;  /* seconds, for HM_SCHED_EVERY          */
    hm_str        version_prefix;  /* HM_CHECK_VERSION: text before value  */
    hm_str        version_suffix;  /* HM_CHECK_VERSION: text after value   */
    hm_str        install;         /* raw block-scalar region, still indented */
    size_t        install_indent;  /* columns to strip from install lines  */
    long          line;            /* line the project started on          */
} hm_project;

/* --- util.c ------------------------------------------------------------- */

/* Minimal formatter used instead of <stdio.h> on the fast path.
 * Conversions: %s (const char *), %S (hm_str), %u (unsigned long),
 *              %d (long), %c (int), %% */
void hm_out(const char *fmt, ...);   /* buffered stdout            */
void hm_err(const char *fmt, ...);   /* unbuffered stderr          */
void hm_out_flush(void);

int    hm_str_eq(hm_str a, const char *lit);
int    hm_str_eq_str(hm_str a, hm_str b);
size_t hm_str_copy(char *dst, size_t cap, hm_str s);
int    hm_parse_ulong(hm_str s, unsigned long *out);

/* FNV-1a over `data`, written to `out17` as 16 hex digits plus NUL. */
void   hm_hash_hex(const char *data, size_t len, char *out17);

/* Reads a whole file (or standard input when path is "-") into `buf`.
 * Returns the byte count, or -1 on error (message already printed). */
long hm_read_all(const char *path, char *buf, size_t cap);

/* --- config.c ----------------------------------------------------------- */

/* Parses `len` bytes of SIML describing exactly one project into `out`. All
 * resulting slices point into `buf`, which must stay alive for as long as the
 * project is used. Returns 0, or -1 on error (message already printed). */
int hm_config_parse(const char *buf, size_t len, const char *origin,
                    hm_project *out);

const char *hm_check_name(hm_check_kind k);
void        hm_sched_describe(const hm_project *p, char *out, size_t cap);

/* Walks the install script one de-indented line at a time. Initialise
 * `*cursor` to p->install.ptr; returns 0 once the block is exhausted. */
int hm_install_next_line(const hm_project *p, const char **cursor, hm_str *out);

/* --- plan.c ------------------------------------------------------------- */

struct hm_cache_s;

/* Serialises what hacman would do with this project, as SIML. The plan
 * depends on the input file and the environment alone - never on the cache
 * contents, the clock or the network - so it is stable enough to diff against
 * a golden file. */
void hm_plan_print(const hm_project *p, const struct hm_cache_s *c);

/* --- cache.c ------------------------------------------------------------ */

/* The cache is a directory of tiny records, one per watched thing, below
 * ~/.cache/hacman by default. One file per record instead of a single table
 * means the fast path reads exactly the bytes it needs, and two hacman runs
 * started in parallel - one per project file - cannot lose each other's
 * updates.
 *
 * A record is addressed by what it stands for, never by which file mentioned
 * it: the identity of a URL project is its URL and check scheme, and the
 * identity of a command project is its command and working directory. Two
 * input files that describe the same command in the same directory therefore
 * share one last-run, however they are named. */
typedef struct hm_cache_s {
    char key[HM_KEY_MAX + 1];            /* file name within the cache dir  */
    char identity[HM_IDENTITY_MAX + 1];  /* what that name stands for       */
    char path[HM_PATH_MAX + 1];          /* dir + "/" + key                 */
    char mark[HM_MARK_MAX + 1];          /* last observed etag/hash/version */
    long last_check;                     /* epoch seconds, 0 = never        */
    long last_change;                    /* epoch seconds, 0 = never        */
    int  known;                          /* a record for this identity existed */
} hm_cache;

/* ${HACMAN_CACHE}, else ${XDG_CACHE_HOME:-$HOME/.cache}/hacman. */
const char *hm_cache_dir(const char *override);

/* Derives identity, key and file path. Touches no files. */
void hm_cache_init(hm_cache *c, const hm_project *p, const char *dir);

/* Reads the record, if any. Returns 0 on success (also when absent, with
 * known == 0), -1 on an unreadable file. */
int hm_cache_load(hm_cache *c);

/* Writes the record atomically, creating the cache directory as needed. */
int hm_cache_save(const hm_cache *c);

/* --- check.c ------------------------------------------------------------ */

typedef struct {
    char        mark[HM_MARK_MAX + 1];  /* freshly observed marker      */
    const char *body;                   /* response body, "" for HEAD   */
    size_t      body_len;
} hm_check_result;

/* Performs the HTTP request for `p` and fills `res`.
 * Returns 0 on success, -1 on failure (message already printed). */
int hm_check(const hm_project *p, int timeout_secs, hm_check_result *res);

/* --- install.c ---------------------------------------------------------- */

/* Slow path: runs the project's install script. Clarity beats speed here.
 * Returns 0 on success, -1 if the script failed or could not be run. */
int hm_install(const hm_project *p, const char *old_mark, const char *new_mark,
               const hm_check_result *res);

/* Slow path for HM_KIND_COMMAND: runs `command` with the shell, in `workdir`.
 * Returns 0 on success, -1 if the command failed or could not be run. */
int hm_command_run(const hm_project *p);

#endif /* HACMAN_H_INCLUDED */
