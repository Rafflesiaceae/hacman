/* hacman - watch a URL, install what changed.
 *
 * One input file describes one project. main() is the fast path and is written
 * to keep the work between exec() and the first decision as small as possible:
 *
 *   read the .siml file (one open/read/close)
 *     -> parse it into a static struct (zero copies, zero allocations)
 *       -> read the state table (one open/read/close)
 *         -> decide from the schedule whether to check at all
 *
 * The HTTP request only happens if the schedule says the project is due, and
 * only an actual change reaches the slow path in install.c. */

#include "hacman.h"

#include <string.h>
#include <time.h>

#include "config.h"

/* Exit codes */
#define HM_EXIT_OK      0
#define HM_EXIT_USAGE   1
#define HM_EXIT_FAILED  2
#define HM_EXIT_CHANGED 10 /* --check-only / --dry-run found changes */

static char       config_buf[HM_CONFIG_MAX];
static hm_project project;
static hm_state   state;

typedef struct {
    const char *file;
    const char *state_path;
    int         check_only;
    int         dry_run;
    int         force;
    int         adopt;
    int         plan;
    int         verbose;
    int         timeout;
} hm_opts;

static const char usage_text[] =
"usage: hacman [OPTIONS] FILE\n"
"\n"
"Checks the URL described by a SIML project file and runs its install script\n"
"when that URL changed. FILE describes exactly one project; \"-\" reads it from\n"
"standard input.\n"
"\n"
"options:\n"
"  -c, --check-only    check only, never install (exit 10 if changes found)\n"
"  -n, --dry-run       report what would be installed, change nothing\n"
"  -f, --force         ignore the schedule and check now\n"
"  -a, --adopt         record the current remote state without installing\n"
"  -p, --plan          print what FILE resolves to, as SIML, and exit\n"
"  -s, --state PATH    state file (default: $XDG_STATE_HOME/hacman/state.tsv)\n"
"  -t, --timeout SECS  per-request timeout (default: 15)\n"
"  -v, --verbose       report an unchanged or skipped project too\n"
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
        } else if (opt_is(a, "-p", "--plan")) {
            o->plan = 1;
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
    hm_opts           o;
    const hm_project *p = &project;
    hm_state_entry   *e;
    hm_check_result   res;
    char              old_mark[HM_MARK_MAX + 1];
    long              len, now, wait = 0;
    int               rc, changed;

    rc = parse_args(argc, argv, &o);
    if (rc != 0) return (rc > 0) ? HM_EXIT_OK : HM_EXIT_USAGE;

    if (o.file == NULL) {
        hm_err("hacman: no project file given (use '-' to read standard input)\n%s",
               usage_text);
        return HM_EXIT_USAGE;
    }

    len = hm_read_all(o.file, config_buf, sizeof(config_buf));
    if (len < 0) return HM_EXIT_USAGE;

    if (hm_config_parse(config_buf, (size_t)len,
                        (strcmp(o.file, "-") != 0) ? o.file : "<stdin>",
                        &project) != 0) {
        return HM_EXIT_USAGE;
    }

    /* --plan answers "what does this file mean?" and stops there: no state, no
     * clock, no network, so its output is reproducible. */
    if (o.plan) {
        hm_plan_print(p);
        hm_out_flush();
        return HM_EXIT_OK;
    }

    if (hm_state_load(&state, o.state_path ? o.state_path : hm_state_default_path()) != 0) {
        return HM_EXIT_USAGE;
    }

    now = (long)time(NULL);

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
        hm_out_flush();
        return HM_EXIT_OK; /* nothing checked, nothing written */
    }

    if (hm_check(p, o.timeout, &res) != 0) {
        /* Deliberately do not touch last_check: a failed request must not push
         * the next attempt into the future. */
        hm_out_flush();
        return HM_EXIT_FAILED;
    }

    e = hm_state_intern(&state, p->name);
    if (e == NULL) {
        hm_err("hacman: state table is full\n");
        return HM_EXIT_FAILED;
    }

    hm_str_copy(old_mark, sizeof(old_mark), (hm_str){ e->mark, strlen(e->mark) });
    /* A project hacman has never seen counts as changed, so a fresh checkout
     * installs on its first run. Use --adopt to record instead. */
    changed = (old_mark[0] == '\0') || strcmp(old_mark, res.mark) != 0;

    if (!o.dry_run && !o.check_only) {
        e->last_check = now;
        state.dirty   = 1;
    }

    if (!changed) {
        if (o.verbose) hm_out("ok       %S (%s)\n", p->name, res.mark);
        rc = (hm_state_save(&state) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK;
        hm_out_flush();
        return rc;
    }

    if (old_mark[0] == '\0') {
        hm_out("new      %S %s\n", p->name, res.mark);
    } else {
        hm_out("changed  %S %s -> %s\n", p->name, old_mark, res.mark);
    }

    if (o.check_only || o.dry_run) {
        hm_out_flush();
        return HM_EXIT_CHANGED;
    }

    if (o.adopt) {
        hm_str_copy(e->mark, sizeof(e->mark), (hm_str){ res.mark, strlen(res.mark) });
        e->last_change = now;
        state.dirty    = 1;
        rc = (hm_state_save(&state) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK;
        hm_out_flush();
        return rc;
    }

    /* --- slow path --------------------------------------------------- */
    hm_out_flush(); /* the install script writes to the same terminal */
    if (hm_install(p, old_mark, res.mark, &res) != 0) {
        /* Neither the mark nor the check time is kept, so the next run retries
         * whatever the schedule says. */
        e->last_check = 0;
        state.dirty   = 1;
        (void)hm_state_save(&state);
        return HM_EXIT_FAILED;
    }

    hm_str_copy(e->mark, sizeof(e->mark), (hm_str){ res.mark, strlen(res.mark) });
    e->last_change = now;
    state.dirty    = 1;

    rc = (hm_state_save(&state) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK;
    hm_out_flush();
    return rc;
}
