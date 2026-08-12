/* hacman - watch URLs, install what changed.
 *
 * main() is the fast path and is written to keep the work between exec() and
 * the first decision as small as possible:
 *
 *   read the .siml file (one open/read/close)
 *     -> parse it into static structs (zero copies, zero allocations)
 *       -> read the state table (one open/read/close)
 *         -> for each project, decide from the schedule whether to check
 *
 * Only a project whose schedule says "due" causes an HTTP request, and only a
 * project that actually changed reaches the slow path in install.c. */

#include "hacman.h"

#include <string.h>
#include <time.h>

#include "config.h"

#define HM_MAX_ONLY 16

/* Exit codes */
#define HM_EXIT_OK      0
#define HM_EXIT_USAGE   1
#define HM_EXIT_FAILED  2
#define HM_EXIT_CHANGED 10 /* --check-only / --dry-run found changes */

static char       config_buf[HM_CONFIG_MAX];
static hm_project projects[HM_MAX_PROJECTS];
static hm_state   state;

typedef struct {
    const char *file;
    const char *state_path;
    const char *only[HM_MAX_ONLY];
    int         only_count;
    int         check_only;
    int         dry_run;
    int         force;
    int         adopt;
    int         list;
    int         verbose;
    int         timeout;
} hm_opts;

static const char usage_text[] =
"usage: hacman [OPTIONS] [FILE]\n"
"\n"
"Checks the URLs described by a SIML project list and runs each project's\n"
"install script when its URL changed. FILE defaults to standard input; \"-\"\n"
"reads standard input explicitly.\n"
"\n"
"options:\n"
"  -c, --check-only    check only, never install (exit 10 if changes found)\n"
"  -n, --dry-run       report what would be installed, change nothing\n"
"  -f, --force         ignore schedules and check every project now\n"
"  -a, --adopt         record the current remote state without installing\n"
"  -l, --list          print the parsed project list and exit\n"
"  -o, --only NAME     only act on NAME (repeatable)\n"
"  -s, --state PATH    state file (default: $XDG_STATE_HOME/hacman/state.tsv)\n"
"  -t, --timeout SECS  per-request timeout (default: 15)\n"
"  -v, --verbose       report unchanged and skipped projects too\n"
"  -h, --help          show this help\n"
"  -V, --version       show the version\n"
"\n"
"exit codes: 0 ok, 1 usage/config error, 2 a check or install failed,\n"
"            10 changes found with --check-only/--dry-run\n";

/* "45s", "12m", "3h07m", "5d" - enough to answer "when will it check again?" */
static void fmt_duration(long secs, char *out, size_t cap)
{
    hm_str s;
    char   tmp[32];
    char   unit;
    size_t n = 0;
    long   v;

    if      (secs < 60)    { v = secs;          unit = 's'; }
    else if (secs < 3600)  { v = secs / 60;     unit = 'm'; }
    else if (secs < 86400) { v = secs / 3600;   unit = 'h'; }
    else                   { v = secs / 86400;  unit = 'd'; }

    {
        char   digits[24];
        size_t d = 0;
        unsigned long x = (unsigned long)(v < 0 ? 0 : v);
        do { digits[d++] = (char)('0' + (x % 10)); x /= 10; } while (x > 0 && d < sizeof(digits));
        while (d > 0 && n + 1 < sizeof(tmp)) tmp[n++] = digits[--d];
    }
    if (n + 1 < sizeof(tmp)) tmp[n++] = unit;
    s.ptr = tmp;
    s.len = n;
    hm_str_copy(out, cap, s);
}

static int opt_is(const char *arg, const char *shrt, const char *lng)
{
    return strcmp(arg, shrt) == 0 || strcmp(arg, lng) == 0;
}

static int parse_args(int argc, char **argv, hm_opts *o)
{
    int i;

    memset(o, 0, sizeof(*o));
    o->timeout = 15;

    for (i = 1; i < argc; ++i) {
        const char *a = argv[i];

        if (a[0] != '-' || strcmp(a, "-") == 0) {
            if (o->file != NULL) {
                hm_err("hacman: only one input file may be given\n");
                return -1;
            }
            o->file = a;
        } else if (opt_is(a, "-c", "--check-only")) {
            o->check_only = 1;
        } else if (opt_is(a, "-n", "--dry-run")) {
            o->dry_run = 1;
        } else if (opt_is(a, "-f", "--force")) {
            o->force = 1;
        } else if (opt_is(a, "-a", "--adopt")) {
            o->adopt = 1;
        } else if (opt_is(a, "-l", "--list")) {
            o->list = 1;
        } else if (opt_is(a, "-v", "--verbose")) {
            o->verbose = 1;
        } else if (opt_is(a, "-h", "--help")) {
            hm_out("%s", usage_text);
            hm_out_flush();
            return 1;
        } else if (opt_is(a, "-V", "--version")) {
            hm_out("hacman %s\n", HM_VERSION);
            hm_out_flush();
            return 1;
        } else if (opt_is(a, "-o", "--only")) {
            if (++i >= argc) { hm_err("hacman: --only needs a name\n"); return -1; }
            if (o->only_count >= HM_MAX_ONLY) {
                hm_err("hacman: too many --only names\n");
                return -1;
            }
            o->only[o->only_count++] = argv[i];
        } else if (opt_is(a, "-s", "--state")) {
            if (++i >= argc) { hm_err("hacman: --state needs a path\n"); return -1; }
            o->state_path = argv[i];
        } else if (opt_is(a, "-t", "--timeout")) {
            unsigned long v;
            hm_str        s;
            if (++i >= argc) { hm_err("hacman: --timeout needs a number\n"); return -1; }
            s.ptr = argv[i];
            s.len = strlen(argv[i]);
            if (hm_parse_ulong(s, &v) != 0 || v == 0) {
                hm_err("hacman: invalid timeout '%s'\n", argv[i]);
                return -1;
            }
            o->timeout = (int)v;
        } else {
            hm_err("hacman: unknown option '%s'\n%s", a, usage_text);
            return -1;
        }
    }
    return 0;
}

static int selected(const hm_opts *o, const hm_project *p)
{
    int i;
    if (o->only_count == 0) return 1;
    for (i = 0; i < o->only_count; ++i) {
        if (hm_str_eq(p->name, o->only[i])) return 1;
    }
    return 0;
}

static void list_projects(const hm_project *p, int count)
{
    int i;
    for (i = 0; i < count; ++i) {
        char sched[32];
        hm_sched_describe(&p[i], sched, sizeof(sched));
        hm_out("%S\n  url:      %S\n  check:    %s\n  schedule: %s\n  install:  %s\n",
               p[i].name, p[i].url, hm_check_name(p[i].check), sched,
               p[i].install.len > 0 ? "yes" : "no");
    }
}

/* The scheduling decision - the whole point of the fast path. Returns 1 when
 * the project must be checked now, 0 when its schedule says "not yet". */
static int is_due(const hm_project *p, const hm_state_entry *e, long now,
                  const hm_opts *o, long *wait_out)
{
    long elapsed;

    if (o->force) return 1;
    if (p->sched_kind == HM_SCHED_ALWAYS) return 1;
    if (p->sched_kind == HM_SCHED_NEVER) {
        *wait_out = -1;
        return 0;
    }
    if (e == NULL || e->last_check == 0) return 1; /* never checked */

    elapsed = now - e->last_check;
    if (elapsed < 0) return 1; /* clock moved backwards; check rather than stall */
    if (elapsed >= p->sched_interval) return 1;

    *wait_out = p->sched_interval - elapsed;
    return 0;
}

int main(int argc, char **argv)
{
    hm_opts o;
    long    len;
    int     count, i, rc;
    long    now;
    int     failures = 0, changes = 0;

    rc = parse_args(argc, argv, &o);
    if (rc != 0) return (rc > 0) ? HM_EXIT_OK : HM_EXIT_USAGE;

    len = hm_read_all(o.file, config_buf, sizeof(config_buf));
    if (len < 0) return HM_EXIT_USAGE;

    count = hm_config_parse(config_buf, (size_t)len,
                            (o.file && strcmp(o.file, "-") != 0) ? o.file : "<stdin>",
                            projects, HM_MAX_PROJECTS);
    if (count < 0) return HM_EXIT_USAGE;

    if (o.list) {
        list_projects(projects, count);
        hm_out_flush();
        return HM_EXIT_OK;
    }

    if (hm_state_load(&state, o.state_path ? o.state_path : hm_state_default_path()) != 0) {
        return HM_EXIT_USAGE;
    }

    now = (long)time(NULL);

    for (i = 0; i < count; ++i) {
        const hm_project *p = &projects[i];
        hm_state_entry   *e;
        hm_check_result   res;
        long              wait = 0;
        char              old_mark[HM_MARK_MAX + 1];
        int               changed;

        if (!selected(&o, p)) continue;

        e = hm_state_find(&state, p->name);
        if (!is_due(p, e, now, &o, &wait)) {
            if (o.verbose) {
                char left[32];
                if (wait < 0) {
                    hm_out("skip     %S (schedule: never)\n", p->name);
                } else {
                    fmt_duration(wait, left, sizeof(left));
                    hm_out("skip     %S (next check in %s)\n", p->name, left);
                }
            }
            continue;
        }

        if (hm_check(p, o.timeout, &res) != 0) {
            /* Deliberately do not touch last_check: a failed request must not
             * push the next attempt into the future. */
            failures += 1;
            continue;
        }

        e = hm_state_intern(&state, p->name);
        if (e == NULL) {
            hm_err("hacman: state table is full\n");
            failures += 1;
            continue;
        }

        hm_str_copy(old_mark, sizeof(old_mark),
                    (hm_str){ e->mark, strlen(e->mark) });
        /* An unknown project counts as changed, so a fresh checkout installs
         * everything on its first run. Use --adopt to record instead. */
        changed = (old_mark[0] == '\0') || strcmp(old_mark, res.mark) != 0;

        if (!o.dry_run && !o.check_only) {
            e->last_check = now;
            state.dirty   = 1;
        }

        if (!changed) {
            if (o.verbose) hm_out("ok       %S (%s)\n", p->name, res.mark);
            continue;
        }

        changes += 1;
        if (old_mark[0] == '\0') {
            hm_out("new      %S %s\n", p->name, res.mark);
        } else {
            hm_out("changed  %S %s -> %s\n", p->name, old_mark, res.mark);
        }

        if (o.check_only || o.dry_run) continue;

        if (o.adopt) {
            hm_str_copy(e->mark, sizeof(e->mark),
                        (hm_str){ res.mark, strlen(res.mark) });
            e->last_change = now;
            state.dirty    = 1;
            continue;
        }

        /* --- slow path ------------------------------------------------- */
        hm_out_flush(); /* the install script writes to the same terminal */
        if (hm_install(p, old_mark, res.mark, &res) != 0) {
            /* Neither the mark nor the check time is kept, so the next run
             * retries this project no matter what its schedule says. */
            e->last_check = 0;
            state.dirty   = 1;
            failures     += 1;
            continue;
        }
        hm_str_copy(e->mark, sizeof(e->mark),
                    (hm_str){ res.mark, strlen(res.mark) });
        e->last_change = now;
        state.dirty    = 1;
    }

    /* Nothing was checked? Then nothing is dirty and no file is written. */
    if (hm_state_save(&state) != 0) failures += 1;

    hm_out_flush();

    if (failures > 0) return HM_EXIT_FAILED;
    if (changes > 0 && (o.check_only || o.dry_run)) return HM_EXIT_CHANGED;
    return HM_EXIT_OK;
}
