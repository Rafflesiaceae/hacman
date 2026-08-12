/* `hacman --plan` - what this input file resolves to.
 *
 * The plan is the parsed project with every default filled in, plus the steps
 * that would follow from it: the request that would be made, what would be
 * compared, and the script that would run with which environment. It is
 * derived from the input file alone - it never reads the state file, the clock
 * or the network - so the same file always plans to the same bytes, which is
 * what makes it usable as a golden-file fixture (see tests/).
 *
 * The output is itself SIML, so it can be read back by anything that already
 * speaks the input format. */

#include "hacman.h"

#include "config.h"

/* The variables install.c exports, in the order it sets them. */
static void print_env(const hm_project *p)
{
    hm_out("install-env: [HACMAN_NAME,HACMAN_URL,HACMAN_CHECK,HACMAN_VERSION,"
           "HACMAN_PREVIOUS");
    if (p->check != HM_CHECK_ETAG) hm_out(",HACMAN_RESPONSE");
    hm_out("]\n");
}

static void print_check(const hm_project *p)
{
    hm_out("check: %s\n", hm_check_name(p->check));

    switch (p->check) {
    case HM_CHECK_ETAG:
        hm_out("request: HEAD %S\n", p->url);
        hm_out("compare: the ETag header, or Last-Modified when absent\n");
        break;

    case HM_CHECK_HASH:
        hm_out("request: GET %S\n", p->url);
        hm_out("compare: an FNV-1a hash of the whole response body\n");
        break;

    case HM_CHECK_VERSION:
        hm_out("version-prefix: %S\n", p->version_prefix);
        if (p->version_suffix.len > 0) {
            hm_out("version-suffix: %S\n", p->version_suffix);
        }
        hm_out("request: GET %S\n", p->url);
        if (p->version_suffix.len > 0) {
            hm_out("compare: the text between version-prefix and version-suffix\n");
        } else {
            hm_out("compare: the text from version-prefix to the end of that line\n");
        }
        break;
    }
}

static void print_install(const hm_project *p)
{
    const char *cursor = p->install.ptr;
    hm_str      line;

    if (p->install.len == 0) {
        hm_out("install: (none - a change would only be reported)\n");
        return;
    }

    hm_out("install-shell: %s -e <script>\n", HM_SHELL);
    print_env(p);

    /* Block scalar, indented by the two spaces SIML mandates. */
    hm_out("install: |\n");
    while (hm_install_next_line(p, &cursor, &line)) {
        if (line.len == 0) {
            hm_out("\n");
        } else {
            hm_out("  %S\n", line);
        }
    }
}

void hm_plan_print(const hm_project *p)
{
    char sched[32];

    hm_sched_describe(p, sched, sizeof(sched));

    hm_out("name: %S\n", p->name);
    hm_out("url: %S\n", p->url);
    print_check(p);
    hm_out("schedule: %s\n", sched);
    switch (p->sched_kind) {
    case HM_SCHED_ALWAYS:
        hm_out("check-when: on every run\n");
        break;
    case HM_SCHED_NEVER:
        hm_out("check-when: only with --force\n");
        break;
    case HM_SCHED_EVERY:
        hm_out("check-when: %u seconds after the last check\n",
               (unsigned long)p->sched_interval);
        break;
    }
    print_install(p);
}
