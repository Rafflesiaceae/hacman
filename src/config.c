/* SIML -> one hm_project, on the fast path.
 *
 * Two properties matter here, both for startup cost:
 *   1. the input is read once into a single buffer (see hm_read_all) and the
 *      line callback below only hands out slices of it - no copying, no I/O
 *      per line, no allocation;
 *   2. every config value is stored as an hm_str pointing back into that same
 *      buffer, including the `install:` block, which is remembered as a raw
 *      region and only de-indented if an install actually happens.
 *
 * An input file describes exactly one project: anything that would introduce a
 * second one - a top-level sequence, a second mapping, a second document - is
 * an error. */

#include "hacman.h"

#include <stdlib.h>  /* getenv() for {{VAR}} expansion */
#include <string.h>

#include "siml.h"

/* Feeds the SIML pull parser from an in-memory buffer. */
typedef struct {
    const char *buf;
    size_t      len;
    size_t      pos;
} hm_line_reader;

static int hm_read_line(void *userdata, const char **out_line, size_t *out_len)
{
    hm_line_reader *r = (hm_line_reader *)userdata;
    const char     *start;
    const char     *nl;

    if (r->pos >= r->len) return 0; /* EOF */

    start = r->buf + r->pos;
    nl    = (const char *)memchr(start, '\n', r->len - r->pos);
    if (nl == NULL) {
        /* Final line without LF: SIML rejects this, signalled by rc == 2. */
        *out_line = start;
        *out_len  = r->len - r->pos;
        r->pos    = r->len;
        return 2;
    }

    *out_line = start;
    *out_len  = (size_t)(nl - start);
    r->pos   += *out_len + 1;
    return 1;
}

/* siml_slice and hm_str are the same idea in two headers; convert explicitly
 * so hacman.h stays independent of the vendored parser. */
static hm_str slice(siml_slice s)
{
    hm_str r;
    r.ptr = s.ptr;
    r.len = s.len;
    return r;
}

/* Parser-wide bookkeeping while walking the event stream. */
typedef struct {
    const char *buf;
    const char *origin;
    hm_project *out;           /* the one project being built            */
    int         started;       /* its mapping has been opened            */
    int         finished;      /* ... and closed again                   */
    hm_project *cur;           /* non-NULL while inside that mapping     */
    int         seen_check;    /* keys that are only valid for one kind  */
    int         seen_version;
    int         seen_install;
    int         seen_workdir;
    int         in_install;    /* inside the `install:` block scalar     */
    const char *block_start;   /* first raw byte of the install block    */
    const char *block_end;     /* one past its last raw byte             */
} hm_cfg;

static void cfg_err(const hm_cfg *c, long line, const char *msg)
{
    hm_err("hacman: %s:%d: %s\n", c->origin, line, msg);
}

const char *hm_check_name(hm_check_kind k)
{
    switch (k) {
    case HM_CHECK_HASH:    return "hash";
    case HM_CHECK_VERSION: return "version";
    case HM_CHECK_ETAG:
    default:               return "etag";
    }
}

/* Renders a schedule back into its config spelling, e.g. "every 6h". */
void hm_sched_describe(const hm_project *p, char *out, size_t cap)
{
    static const struct { const char *unit; long secs; } units[] = {
        { "w", 604800L }, { "d", 86400L }, { "h", 3600L }, { "m", 60L }
    };
    size_t i, o = 0;
    long   v    = p->sched_interval;
    const char *unit = "s";

    if (p->sched_kind == HM_SCHED_ALWAYS) { hm_str_copy(out, cap, (hm_str){"always", 6}); return; }
    if (p->sched_kind == HM_SCHED_NEVER)  { hm_str_copy(out, cap, (hm_str){"never", 5});  return; }

    for (i = 0; i < sizeof(units) / sizeof(units[0]); ++i) {
        if (v % units[i].secs == 0) {
            v    = v / units[i].secs;
            unit = units[i].unit;
            break;
        }
    }
    {
        char tmp[24];
        size_t n = 0;
        long   x = v;
        do { tmp[n++] = (char)('0' + (x % 10)); x /= 10; } while (x > 0 && n < sizeof(tmp));
        while (n > 0 && o + 1 < cap) out[o++] = tmp[--n];
        while (*unit != '\0' && o + 1 < cap) out[o++] = *unit++;
    }
    out[o] = '\0';
}

/* "always" | "never" | "hourly" | "daily" | "weekly" | "monthly" | <n>[smhdw] */
static int parse_schedule(hm_project *p, hm_str v)
{
    unsigned long n;
    long          mul = 1;
    hm_str        num = v;

    if (hm_str_eq(v, "always")) { p->sched_kind = HM_SCHED_ALWAYS; return 0; }
    if (hm_str_eq(v, "never"))  { p->sched_kind = HM_SCHED_NEVER;  return 0; }

    p->sched_kind = HM_SCHED_EVERY;
    if (hm_str_eq(v, "hourly"))  { p->sched_interval = 3600L;    return 0; }
    if (hm_str_eq(v, "daily"))   { p->sched_interval = 86400L;   return 0; }
    if (hm_str_eq(v, "weekly"))  { p->sched_interval = 604800L;  return 0; }
    if (hm_str_eq(v, "monthly")) { p->sched_interval = 2592000L; return 0; }

    if (v.len < 2) return -1;
    switch (v.ptr[v.len - 1]) {
    case 's': mul = 1L;      break;
    case 'm': mul = 60L;     break;
    case 'h': mul = 3600L;   break;
    case 'd': mul = 86400L;  break;
    case 'w': mul = 604800L; break;
    default:  return -1;
    }
    num.len -= 1;
    if (hm_parse_ulong(num, &n) != 0 || n == 0) return -1;

    p->sched_interval = (long)n * mul;
    return 0;
}

static int parse_check(hm_project *p, hm_str v)
{
    if (hm_str_eq(v, "etag"))    { p->check = HM_CHECK_ETAG;    return 0; }
    if (hm_str_eq(v, "hash"))    { p->check = HM_CHECK_HASH;    return 0; }
    if (hm_str_eq(v, "version")) { p->check = HM_CHECK_VERSION; return 0; }
    return -1;
}

/* Applies one `key: value` pair to the project being built. */
static int apply_field(hm_cfg *c, hm_str key, hm_str value, long line)
{
    hm_project *p = c->cur;

    if (hm_str_eq(key, "name")) {
        if (value.len > HM_NAME_MAX) {
            cfg_err(c, line, "name is too long");
            return -1;
        }
        p->name = value;
    } else if (hm_str_eq(key, "url")) {
        if (value.len > HM_URL_MAX) {
            cfg_err(c, line, "url is too long");
            return -1;
        }
        p->url = value;
    } else if (hm_str_eq(key, "command")) {
        if (value.len > HM_COMMAND_MAX) {
            cfg_err(c, line, "command is too long");
            return -1;
        }
        p->command = value;
    } else if (hm_str_eq(key, "bin-path")) {
        if (value.len > HM_PATH_MAX) {
            cfg_err(c, line, "bin-path is too long");
            return -1;
        }
        p->bin = value;
    } else if (hm_str_eq(key, "workdir")) {
        if (value.len > HM_PATH_MAX) {
            cfg_err(c, line, "workdir is too long");
            return -1;
        }
        c->seen_workdir = 1;
        p->workdir      = value;
    } else if (hm_str_eq(key, "check")) {
        c->seen_check = 1;
        if (parse_check(p, value) != 0) {
            cfg_err(c, line, "check must be one of: etag, hash, version");
            return -1;
        }
    } else if (hm_str_eq(key, "schedule")) {
        if (parse_schedule(p, value) != 0) {
            cfg_err(c, line, "schedule must be always, never, hourly, daily, "
                             "weekly, monthly or <n>[smhdw]");
            return -1;
        }
    } else if (hm_str_eq(key, "version-prefix")) {
        c->seen_version   = 1;
        p->version_prefix = value;
    } else if (hm_str_eq(key, "version-suffix")) {
        c->seen_version   = 1;
        p->version_suffix = value;
    } else if (hm_str_eq(key, "install")) {
        c->seen_install   = 1;
        /* Inline form: `install: make install`. The block form is handled by
         * the BLOCK_SCALAR_* events. */
        p->install        = value;
        p->install_indent = 0;
    } else {
        hm_err("hacman: %s:%d: unknown key '%S'\n", c->origin, line, key);
        return -1;
    }
    return 0;
}

static int begin_project(hm_cfg *c, long line)
{
    hm_project *p = c->out;

    if (c->started) {
        cfg_err(c, line, "input must describe exactly one project");
        return -1;
    }
    c->started = 1;
    memset(p, 0, sizeof(*p));
    p->check      = HM_CHECK_ETAG;
    p->sched_kind = HM_SCHED_EVERY;
    /* Nothing hacman watches is worth asking about more than once a day, so
     * the default schedule is a full 24h; anything shorter is opt-in. */
    p->sched_interval = 86400L;
    p->line       = line;
    c->cur        = p;
    return 0;
}

/* Expands {{VAR}} references from the environment.
 *
 * This is the one place a config value is not simply borrowed from the input
 * buffer: a working directory has to be a real path before it can be entered,
 * and before it can identify a cache record. */
static int expand_template(hm_cfg *c, hm_str tpl, const char *what,
                           char *out, size_t cap, long line)
{
    size_t i = 0, o = 0;

    while (i < tpl.len) {
        if (i + 1 < tpl.len && tpl.ptr[i] == '{' && tpl.ptr[i + 1] == '{') {
            char        name[64];
            size_t      n = 0;
            const char *value;

            i += 2;
            while (i < tpl.len && tpl.ptr[i] != '}' && n + 1 < sizeof(name)) {
                name[n++] = tpl.ptr[i++];
            }
            name[n] = '\0';
            if (i + 1 >= tpl.len || tpl.ptr[i] != '}' || tpl.ptr[i + 1] != '}') {
                hm_err("hacman: %s:%d: unterminated '{{' in %s\n",
                       c->origin, line, what);
                return -1;
            }
            i += 2;

            value = getenv(name);
            if (value == NULL || value[0] == '\0') {
                hm_err("hacman: %s:%d: %s refers to {{%s}}, which is not set "
                       "in the environment\n", c->origin, line, what, name);
                return -1;
            }
            while (*value != '\0' && o + 1 < cap) out[o++] = *value++;
            continue;
        }
        if (o + 1 < cap) out[o++] = tpl.ptr[i];
        ++i;
    }
    out[o] = '\0';

    /* A relative path would mean something different per caller, while the
     * cache record it identifies - or the program it names - would not. */
    if (out[0] != '/') {
        hm_err("hacman: %s:%d: %s must expand to an absolute path\n",
               c->origin, line, what);
        return -1;
    }
    return 0;
}

/* Clamps a name defaulted from a url or command: it is display text, so
 * shortening it beats rejecting the project over it. */
static hm_str default_name(hm_str from)
{
    if (from.len > HM_NAME_MAX) from.len = HM_NAME_MAX;
    return from;
}

static int finish_project(hm_cfg *c)
{
    hm_project *p = c->cur;

    if (p->url.len == 0 && p->command.len == 0) {
        cfg_err(c, p->line, "project needs either 'url' or 'command'");
        return -1;
    }
    if (p->url.len > 0 && p->command.len > 0) {
        cfg_err(c, p->line, "'url' and 'command' are mutually exclusive");
        return -1;
    }

    if (p->command.len > 0) {
        /* A command project runs something on a schedule: there is nothing to
         * compare, and the command is the work, so there is nothing to install
         * afterwards either. */
        p->kind = HM_KIND_COMMAND;
        if (c->seen_check || c->seen_version) {
            cfg_err(c, p->line, "'check' and 'version-*' only apply to a 'url'");
            return -1;
        }
        if (c->seen_install) {
            cfg_err(c, p->line,
                    "a 'command' project has no 'install': the command is the work");
            return -1;
        }
        if (p->name.len == 0) p->name = default_name(p->command);

        /* Without a working directory, run where the user lives. */
        if (p->workdir.len == 0) {
            p->workdir.ptr = "{{HOME}}";
            p->workdir.len = 8;
        }
        if (expand_template(c, p->workdir, "workdir", p->workdir_path,
                            sizeof(p->workdir_path), p->line) != 0) {
            return -1;
        }
    } else {
        p->kind = HM_KIND_URL;
        if (c->seen_workdir) {
            cfg_err(c, p->line, "'workdir' only applies to a 'command'");
            return -1;
        }
        if (p->name.len == 0) p->name = default_name(p->url);
        if (p->check == HM_CHECK_VERSION && p->version_prefix.len == 0) {
            cfg_err(c, p->line, "check: version requires 'version-prefix'");
            return -1;
        }
    }

    /* The program this project sets up, if it names one. */
    if (p->bin.len > 0 &&
        expand_template(c, p->bin, "bin-path", p->bin_path,
                        sizeof(p->bin_path), p->line) != 0) {
        return -1;
    }

    c->finished = 1;
    c->cur      = NULL;
    return 0;
}

/* Records the raw extent of an install block instead of copying it.
 *
 * SIML strips the block's own indentation from every emitted line, so the
 * first content line tells us how many columns to strip later: scanning back
 * to the preceding LF yields the line start, and the distance to the event's
 * value is exactly that indentation. */
static void block_line(hm_cfg *c, const siml_event *ev)
{
    if (ev->value.len == 0 || ev->value.ptr == NULL) return; /* blank line */

    if (c->block_start == NULL) {
        const char *line_start = ev->value.ptr;
        while (line_start > c->buf && line_start[-1] != '\n') --line_start;
        c->block_start            = line_start;
        c->cur->install_indent    = (size_t)(ev->value.ptr - line_start);
    }
    c->block_end = ev->value.ptr + ev->value.len;
}

int hm_config_parse(const char *buf, size_t len, const char *origin,
                    hm_project *out)
{
    hm_line_reader reader;
    siml_parser    parser;
    siml_event     ev;
    hm_cfg         c;

    memset(&c, 0, sizeof(c));
    c.buf    = buf;
    c.origin = origin;
    c.out    = out;

    reader.buf = buf;
    reader.len = len;
    reader.pos = 0;

    siml_parser_init(&parser, hm_read_line, &reader);

    for (;;) {
        siml_event_type t = siml_next(&parser, &ev);

        switch (t) {
        case SIML_EVENT_STREAM_START:
        case SIML_EVENT_DOCUMENT_END:
        case SIML_EVENT_COMMENT:
            break;

        case SIML_EVENT_DOCUMENT_START:
            /* A second document would be a second project. */
            if (c.started) {
                cfg_err(&c, ev.line, "input must describe exactly one project");
                return -1;
            }
            break;

        case SIML_EVENT_SEQUENCE_START:
            if (c.cur != NULL) {
                cfg_err(&c, ev.line, "sequences are not supported inside a project");
            } else {
                cfg_err(&c, ev.line,
                        "input must describe exactly one project, written as a "
                        "plain mapping (no leading '- ')");
            }
            return -1;

        case SIML_EVENT_SEQUENCE_END:
            break;

        case SIML_EVENT_MAPPING_START:
            if (c.cur != NULL) {
                cfg_err(&c, ev.line, "nested mappings are not supported");
                return -1;
            }
            if (begin_project(&c, ev.line) != 0) return -1;
            break;

        case SIML_EVENT_MAPPING_END:
            if (finish_project(&c) != 0) return -1;
            break;

        case SIML_EVENT_SCALAR:
            if (c.cur == NULL) {
                cfg_err(&c, ev.line, "expected a mapping of project fields");
                return -1;
            }
            if (ev.key.len == 0) {
                cfg_err(&c, ev.line, "bare sequence items are not supported");
                return -1;
            }
            if (apply_field(&c, slice(ev.key), slice(ev.value), ev.line) != 0) {
                return -1;
            }
            break;

        case SIML_EVENT_BLOCK_SCALAR_START:
            if (c.cur == NULL || !hm_str_eq(slice(ev.key), "install")) {
                cfg_err(&c, ev.line, "only 'install' may be a block scalar");
                return -1;
            }
            c.in_install  = 1;
            c.block_start = NULL;
            c.block_end   = NULL;
            break;

        case SIML_EVENT_BLOCK_SCALAR_LINE:
            if (c.in_install) block_line(&c, &ev);
            break;

        case SIML_EVENT_BLOCK_SCALAR_END:
            if (c.in_install && c.block_start != NULL) {
                c.cur->install.ptr = c.block_start;
                c.cur->install.len = (size_t)(c.block_end - c.block_start);
            }
            c.in_install = 0;
            break;

        case SIML_EVENT_STREAM_END:
            if (!c.finished) {
                hm_err("hacman: %s: input describes no project\n", origin);
                return -1;
            }
            return 0;

        case SIML_EVENT_ERROR:
        default:
            hm_err("hacman: %s:%d: %s\n", origin, ev.line,
                   ev.error_message ? ev.error_message : "parse error");
            return -1;
        }
    }
}

/* Yields the install script one line at a time, with the block indentation
 * that SIML stripped from the raw region put back to use: `*cursor` starts at
 * p->install.ptr and walks to the end of the block.
 *
 * Both the plan writer and the installer go through this, so the script they
 * describe and the script that runs can never drift apart. */
int hm_install_next_line(const hm_project *p, const char **cursor, hm_str *out)
{
    const char *end = p->install.ptr + p->install.len;
    const char *cur = *cursor;
    const char *nl;
    const char *eol;
    size_t      strip = 0;

    if (p->install.len == 0 || cur == NULL || cur >= end) return 0;

    nl  = (const char *)memchr(cur, '\n', (size_t)(end - cur));
    eol = (nl != NULL) ? nl : end;

    while (strip < p->install_indent && cur + strip < eol && cur[strip] == ' ') ++strip;

    out->ptr = cur + strip;
    out->len = (size_t)(eol - (cur + strip));
    *cursor  = (nl != NULL) ? nl + 1 : end;
    return 1;
}
