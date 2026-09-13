/* `hacman --plan` - what this input file resolves to.
 *
 * The plan is the parsed project with every default filled in, plus the steps
 * that would follow from it: the request that would be made or the command
 * that would run, what would be compared, which cache record answers "was this
 * done already", and the script that would run with which environment.
 *
 * It is derived from the input file and the environment alone - it never reads
 * the cache, the clock or the network - so the same file always plans to the
 * same bytes, which is what makes it usable as a golden-file fixture (see
 * tests/).
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
        hm_out("compare: an XXH3 hash of the whole response body\n");
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

static void print_files(const hm_project *p, const hm_cache *c)
{
    size_t i;

    if (p->file_count == 0) return;
    hm_out("files-dir: %s\n", c->workdir);
    hm_out("files-written: before command execution\n");
    hm_out("files:\n");
    for (i = 0; i < p->file_count; ++i) {
        const hm_embedded_file *file   = &p->files[i];
        const char             *cursor = file->content.ptr;
        hm_str                  line;

        hm_out("  %S: |\n", file->path);
        while (hm_file_next_line(file, &cursor, &line)) {
            if (line.len == 0) hm_out("\n");
            else hm_out("    %S\n", line);
        }
    }
}

void hm_plan_print(const hm_project *p, const hm_cache *c)
{
    char sched[32];

    hm_sched_describe(p, sched, sizeof(sched));

    hm_out("name: %S\n", p->name);
    if (p->kind == HM_KIND_COMMAND) {
        hm_out("command: %S\n", p->command);
        hm_out("workdir: %s\n", (p->file_count > 0) ? c->workdir : p->workdir_path);
        hm_out("run: %s -c <command>, in workdir\n", HM_SHELL);
        hm_out("record-when: the command exits 0\n");
        print_files(p, c);
    } else {
        hm_out("url: %S\n", p->url);
        print_check(p);
    }
    hm_out("schedule: %s\n", sched);
    switch (p->sched_kind) {
    case HM_SCHED_ALWAYS:
        hm_out("check-when: on every invocation\n");
        break;
    case HM_SCHED_NEVER:
        hm_out("check-when: only with --force\n");
        break;
    case HM_SCHED_EVERY:
        hm_out("check-when: %u seconds after the last run\n", (unsigned long)p->sched_interval);
        break;
    }

    /* Which record decides "already done", and what it stands for: two files
     * that plan to the same identity share one last-run. */
    hm_out("cache-identity: %s\n", c->identity);
    hm_out("cache-file: %s\n", c->path);

    if (p->bin_path[0] != '\0') {
        hm_out("bin-path: %s\n", p->bin_path);
        hm_out("exec: bin-path, with every argument that followed FILE\n");
    }

    if (p->kind == HM_KIND_URL) print_install(p);
}
