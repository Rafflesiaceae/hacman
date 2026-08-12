#ifndef HACMAN_H_INCLUDED
#define HACMAN_H_INCLUDED

#include <stddef.h>

/* Compile-time limits.
 *
 * Every buffer hacman uses on its fast path lives in BSS or on the stack: the
 * check path never calls malloc(), so there is no allocator to warm up and no
 * failure mode to handle. BSS pages are faulted in lazily by the kernel, so
 * generous limits cost nothing at startup. */
#define HM_MAX_PROJECTS 128
#define HM_CONFIG_MAX   (1024u * 1024u)       /* .siml input                  */
#define HM_STATE_MAX    (256u * 1024u)        /* state file                   */
#define HM_BODY_MAX     (4u * 1024u * 1024u)  /* HTTP response body           */
#define HM_MARK_MAX     255                   /* etag / hash / version string */
#define HM_NAME_MAX     127
#define HM_URL_MAX      1023

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

/* How "changed" is decided for a URL. */
typedef enum {
    HM_CHECK_ETAG = 0,  /* HEAD, compare ETag (or Last-Modified)          */
    HM_CHECK_HASH,      /* GET, compare a hash of the whole body          */
    HM_CHECK_VERSION    /* GET, compare a version substring from the body */
} hm_check_kind;

typedef struct {
    hm_str        name;            /* defaults to url when not given       */
    hm_str        url;
    hm_check_kind check;
    hm_sched_kind sched_kind;
    long          sched_interval;  /* seconds, for HM_SCHED_EVERY          */
    hm_str        version_prefix;  /* HM_CHECK_VERSION: text before value  */
    hm_str        version_suffix;  /* HM_CHECK_VERSION: text after value   */
    hm_str        install;         /* raw block-scalar region, still indented */
    size_t        install_indent;  /* columns to strip from install lines  */
    long          line;            /* line the entry started on            */
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

/* Reads a whole file (or fd 0 when path is NULL or "-") into `buf`.
 * Returns the byte count, or -1 on error (message already printed). */
long hm_read_all(const char *path, char *buf, size_t cap);

/* --- config.c ----------------------------------------------------------- */

/* Parses `len` bytes of SIML into `out`. All resulting slices point into
 * `buf`, which must stay alive for as long as the projects are used.
 * Returns the project count, or -1 on error (message already printed). */
int hm_config_parse(const char *buf, size_t len, const char *origin,
                    hm_project *out, int max);

const char *hm_check_name(hm_check_kind k);
void        hm_sched_describe(const hm_project *p, char *out, size_t cap);

/* --- state.c ------------------------------------------------------------ */

typedef struct {
    char name[HM_NAME_MAX + 1];
    char mark[HM_MARK_MAX + 1];  /* last observed etag/hash/version, "" = none */
    long last_check;             /* epoch seconds, 0 = never checked           */
    long last_change;            /* epoch seconds, 0 = never changed           */
} hm_state_entry;

typedef struct {
    hm_state_entry ent[HM_MAX_PROJECTS];
    int            count;
    int            dirty;
    const char    *path;
} hm_state;

int             hm_state_load(hm_state *st, const char *path);
hm_state_entry *hm_state_find(hm_state *st, hm_str name);
hm_state_entry *hm_state_intern(hm_state *st, hm_str name);
int             hm_state_save(const hm_state *st);
const char     *hm_state_default_path(void);

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

#endif /* HACMAN_H_INCLUDED */
