/* hacman - set a program up, then get out of the way.
 *
 * hacman is a shim: `hacman [OPTIONS] FILE [ARGS...]` makes sure the thing FILE
 * describes is set up - a URL checked and installed, or a command run - and
 * then execs the program named by bin-path with everything that followed FILE.
 * Options therefore have to come *before* FILE; every argument after it belongs
 * to the program, including the ones that look like hacman's own.
 *
 * One input file describes one project. main() is the fast path and is written
 * to keep the work between exec() and the first decision as small as possible:
 *
 *   read the .siml file (one open/read/close)
 *     -> parse it into a static struct (zero copies, zero allocations)
 *       -> read this project's cache record (one open/read/close)
 *         -> decide from the schedule whether to do anything at all
 *
 * Nothing beyond that happens unless the schedule says the project is due, and
 * nothing is recorded unless the work that followed actually succeeded. */

#include "hacman.h"

#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"

/* Exit codes */
#define HM_EXIT_OK      0
#define HM_EXIT_USAGE   1
#define HM_EXIT_FAILED  2
#define HM_EXIT_CHANGED 10 /* --check-only / --dry-run found changes */

static char       config_buf[HM_CONFIG_MAX];
static char       resolved_bin_path[HM_PATH_MAX + 1];
static hm_project project;
static hm_cache   cache;

typedef struct {
    const char *file;
    char      **prog_argv; /* &argv[FILE], reused as the program's argv */
    const char *cache_dir;
    int         check_only;
    int         dry_run;
    int         force;
    int         adopt;
    int         plan;
    int         verbose;
    int         timeout;
} hm_opts;

static const char usage_text[] =
    "usage: hacman [OPTIONS] FILE [ARGS...]\n"
    "\n"
    "Checks the URL described by a SIML project file and runs its install script\n"
    "when that URL changed, or - for a file that describes a command instead -\n"
    "runs that command no more often than its schedule allows. FILE describes\n"
    "exactly one project; \"-\" reads it from standard input.\n"
    "\n"
    "When the project names a bin-path, hacman execs it once the setup is done and\n"
    "forwards ARGS to it. Options must therefore come before FILE - everything\n"
    "after FILE belongs to the program.\n"
    "\n"
    "options:\n"
    "  -c, --check-only    check only, never install or run (exit 10 if due)\n"
    "  -n, --dry-run       report what would happen, change nothing\n"
    "  -f, --force         ignore the schedule and act now\n"
    "  -a, --adopt         record the current state without installing or running\n"
    "  -p, --plan          print what FILE resolves to, as SIML, and exit\n"
    "      --cache DIR     cache directory (default: ~/.cache/hacman)\n"
    "  -t, --timeout SECS  per-request timeout (default: 15)\n"
    "  -v, --verbose       report an unchanged or skipped project too\n"
    "  -h, --help          show this help\n"
    "  -V, --version       show the version\n"
    "\n"
    "exit codes: 0 ok, 1 usage/config error, 2 a check, install or command failed,\n"
    "            10 work is pending with --check-only/--dry-run\n";

/* "45s", "12m", "3h07m", "5d" - enough to answer "when will it check again?" */
static void fmt_duration(long secs, char *out, size_t cap)
{
    hm_str s;
    char   tmp[32];
    char   unit;
    size_t n = 0;
    long   v;

    if (secs < 60) {
        v    = secs;
        unit = 's';
    } else if (secs < 3600) {
        v    = secs / 60;
        unit = 'm';
    } else if (secs < 86400) {
        v    = secs / 3600;
        unit = 'h';
    } else {
        v    = secs / 86400;
        unit = 'd';
    }

    {
        char          digits[24];
        size_t        d = 0;
        unsigned long x = (unsigned long)(v < 0 ? 0 : v);
        do {
            digits[d++] = (char)('0' + (x % 10));
            x /= 10;
        } while (x > 0 && d < sizeof(digits));
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

        /* The first non-option is FILE, and it ends hacman's own arguments:
         * the rest is the program's, however it is spelled. */
        if (a[0] != '-' || strcmp(a, "-") == 0) {
            o->file      = a;
            o->prog_argv = &argv[i];
            return 0;
        }

        if (opt_is(a, "-c", "--check-only")) {
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
        } else if (strcmp(a, "--cache") == 0) {
            if (++i >= argc) {
                hm_err("hacman: --cache needs a directory\n");
                return -1;
            }
            o->cache_dir = argv[i];
        } else if (opt_is(a, "-t", "--timeout")) {
            unsigned long v;
            hm_str        s;
            if (++i >= argc) {
                hm_err("hacman: --timeout needs a number\n");
                return -1;
            }
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
 * the project must be acted on now, 0 when its schedule says "not yet". */
static int is_due(const hm_project *p, const hm_cache *c, long now, const hm_opts *o,
                  long *wait_out)
{
    long elapsed;

    if (o->force) return 1;

    /* Embedded projects are immutable, content-addressed builds: their cache
     * identity already changes with the command, file paths, or file contents.
     * A successful record therefore means there is no setup work to repeat.
     * Rebuild a missing executable instead of handing an invalid cache entry
     * to exec(), while projects without bin-path retain useful --adopt
     * semantics based on the record alone. */
    if (p->file_count > 0) {
        if (!c->known) return 1;
        if (p->bin_path[0] != '\0' && access(p->bin_path, X_OK) != 0) return 1;
        return 0;
    }

    if (p->sched_kind == HM_SCHED_ALWAYS) return 1;
    if (p->sched_kind == HM_SCHED_NEVER) {
        *wait_out = -1;
        return 0;
    }
    if (c->last_check == 0) return 1; /* never done */

    elapsed = now - c->last_check;
    if (elapsed < 0) return 1; /* clock moved backwards; act rather than stall */
    if (elapsed >= p->sched_interval) return 1;

    *wait_out = p->sched_interval - elapsed;
    return 0;
}

/* Last step of every path: flush what hacman had to say and, if the project
 * names a program, become it.
 *
 * Only a run that got as far as "the program is set up" execs: a failure would
 * hand over a program that may be missing or half-updated, and the inspection
 * modes are meant to report rather than to act. */
static int finish(const hm_project *p, const hm_opts *o, int rc)
{
    hm_out_flush();

    if (rc != HM_EXIT_OK || p->bin_path[0] == '\0') return rc;
    if (o->plan || o->check_only || o->dry_run || o->adopt) return rc;

    return hm_exec_bin(p, o->prog_argv); /* only returns if exec failed */
}

/* An embedded project does not know its cache work directory until its cache
 * identity has been built. Resolve its relative bin-path once that directory
 * is available, before either the plan or the eventual exec sees it. */
static int resolve_embedded_bin(hm_project *p, const hm_cache *c, const char *file)
{
    size_t base_len, bin_len;

    if (p->bin_path[0] == '\0' || p->bin_path[0] == '/') return 0;

    base_len = strlen(c->workdir);
    bin_len  = strlen(p->bin_path);
    if (base_len + 1 + bin_len > HM_PATH_MAX) {
        hm_err("hacman: %s: resolved bin-path is too long\n", file);
        return -1;
    }

    memcpy(resolved_bin_path, c->workdir, base_len);
    resolved_bin_path[base_len] = '/';
    memcpy(resolved_bin_path + base_len + 1, p->bin_path, bin_len + 1);
    memcpy(p->bin_path, resolved_bin_path, base_len + 1 + bin_len + 1);
    return 0;
}

/* A command project: run it, and remember that only if it succeeded. */
static int run_command_project(const hm_project *p, hm_opts *o, long now)
{
    const char *workdir = (p->file_count > 0) ? cache.workdir : p->workdir_path;

    if (o->check_only || o->dry_run) {
        hm_out("due      %S\n", p->name);
        hm_out_flush();
        return HM_EXIT_CHANGED;
    }

    if (!o->adopt) {
        if (o->verbose) hm_out("run      %S\n", p->name);
        hm_out_flush();
        if (p->file_count > 0 && hm_materialize_files(p, workdir) != 0) {
            return HM_EXIT_FAILED;
        }
        if (hm_command_run(p, workdir) != 0) {
            /* Nothing is written: the last run stays whatever it was, so the
             * next invocation tries again. */
            return HM_EXIT_FAILED;
        }
    }

    cache.last_check  = now;
    cache.last_change = now;
    return (hm_cache_save(&cache) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK;
}

int main(int argc, char **argv)
{
    hm_opts           o;
    const hm_project *p = &project;
    const char       *cache_dir;
    hm_check_result   res;
    char              old_mark[HM_MARK_MAX + 1];
    long              len, now, wait = 0;
    int               rc, changed;

    rc = parse_args(argc, argv, &o);
    if (rc != 0) return (rc > 0) ? HM_EXIT_OK : HM_EXIT_USAGE;

    if (o.file == NULL) {
        hm_err("hacman: no project file given (use '-' to read standard input)\n%s", usage_text);
        return HM_EXIT_USAGE;
    }

    len = hm_read_all(o.file, config_buf, sizeof(config_buf));
    if (len < 0) return HM_EXIT_USAGE;

    if (hm_config_parse(config_buf, (size_t)len, (strcmp(o.file, "-") != 0) ? o.file : "<stdin>",
                        &project) != 0) {
        return HM_EXIT_USAGE;
    }

    if (project.bin_path[0] != '\0') {
        /* As a shim, hacman must leave stdout to the program it execs. */
        hm_out_target(2);
    } else if (o.prog_argv[1] != NULL) {
        hm_err("hacman: %s: arguments after FILE need a 'bin-path' to forward "
               "them to\n",
               o.file);
        return HM_EXIT_USAGE;
    }

    /* The cache record is addressed by what the project *is*, so this also
     * settles which record two different files share. */
    cache_dir = hm_cache_dir(o.cache_dir);
    if (cache_dir == NULL) {
        hm_err("hacman: HOME is not set; use --cache or HACMAN_CACHE\n");
        return HM_EXIT_USAGE;
    }
    hm_cache_init(&cache, p, cache_dir);
    if (resolve_embedded_bin(&project, &cache, o.file) != 0) {
        return HM_EXIT_USAGE;
    }

    /* --plan answers "what does this file mean?" and stops there: no reads, no
     * clock, no network, so its output is reproducible. */
    if (o.plan) {
        hm_out_target(1);
        hm_plan_print(p, &cache);
        hm_out_flush();
        return HM_EXIT_OK;
    }

    if (hm_cache_load(&cache) != 0) return HM_EXIT_USAGE;

    now = (long)time(NULL);

    if (!is_due(p, &cache, now, &o, &wait)) {
        if (o.verbose) {
            char left[32];
            if (p->file_count > 0) {
                hm_out("skip     %S (embedded inputs unchanged)\n", p->name);
            } else if (wait < 0) {
                hm_out("skip     %S (schedule: never)\n", p->name);
            } else {
                fmt_duration(wait, left, sizeof(left));
                hm_out("skip     %S (next run in %s)\n", p->name, left);
            }
        }
        /* Nothing done, nothing written - and straight on to the program,
         * which is the common case for a shim. */
        return finish(p, &o, HM_EXIT_OK);
    }

    if (p->kind == HM_KIND_COMMAND) {
        return finish(p, &o, run_command_project(p, &o, now));
    }

    if (hm_check(p, o.timeout, &res) != 0) {
        /* Nothing is recorded: a failed request must not push the next attempt
         * into the future. */
        return finish(p, &o, HM_EXIT_FAILED);
    }

    hm_str_copy(old_mark, sizeof(old_mark), (hm_str){cache.mark, strlen(cache.mark)});
    /* A project hacman has never seen counts as changed, so a fresh checkout
     * installs on its first run. Use --adopt to record instead. */
    changed = !cache.known || strcmp(old_mark, res.mark) != 0;

    if (!changed) {
        if (o.verbose) hm_out("ok       %S (%s)\n", p->name, res.mark);
        cache.last_check = now;
        return finish(p, &o, (hm_cache_save(&cache) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK);
    }

    if (!cache.known) {
        hm_out("new      %S %s\n", p->name, res.mark);
    } else {
        hm_out("changed  %S %s -> %s\n", p->name, old_mark, res.mark);
    }

    if (o.check_only || o.dry_run) return finish(p, &o, HM_EXIT_CHANGED);

    if (!o.adopt) {
        /* --- slow path ----------------------------------------------- */
        hm_out_flush(); /* the install script writes to the same terminal */
        if (hm_install(p, old_mark, res.mark, &res) != 0) {
            /* Nothing is written, so the next run repeats check and install. */
            return finish(p, &o, HM_EXIT_FAILED);
        }
    }

    hm_str_copy(cache.mark, sizeof(cache.mark), (hm_str){res.mark, strlen(res.mark)});
    cache.last_check  = now;
    cache.last_change = now;

    return finish(p, &o, (hm_cache_save(&cache) != 0) ? HM_EXIT_FAILED : HM_EXIT_OK);
}
