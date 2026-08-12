/* Persisted "what did we last see" records.
 *
 * The state file is a tab-separated table rather than SIML: it is written by
 * hacman only, is read on every single run, and a flat table can be parsed
 * with one read() and a memchr loop. Layout, one line per project:
 *
 *   #hacman-state 1
 *   <name>\t<last_check>\t<last_change>\t<mark>
 */

#include "hacman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>   /* rename() */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HM_STATE_HEADER "#hacman-state 1\n"

static char state_buf[HM_STATE_MAX];
static char state_path_buf[HM_URL_MAX + 1];

/* $HACMAN_STATE, else $XDG_STATE_HOME/hacman/state.tsv, else
 * $HOME/.local/state/hacman/state.tsv. */
const char *hm_state_default_path(void)
{
    const char *env;
    size_t      o = 0;

    env = getenv("HACMAN_STATE");
    if (env != NULL && env[0] != '\0') return env;

    env = getenv("XDG_STATE_HOME");
    if (env == NULL || env[0] == '\0') {
        env = getenv("HOME");
        if (env == NULL || env[0] == '\0') return "./hacman-state.tsv";
        while (*env != '\0' && o + 1 < sizeof(state_path_buf)) state_path_buf[o++] = *env++;
        env = "/.local/state";
    }
    while (*env != '\0' && o + 1 < sizeof(state_path_buf)) state_path_buf[o++] = *env++;

    env = "/hacman/state.tsv";
    while (*env != '\0' && o + 1 < sizeof(state_path_buf)) state_path_buf[o++] = *env++;

    state_path_buf[o] = '\0';
    return state_path_buf;
}

/* Splits off the next field of a state line at `*pos`, up to `sep` or `end`. */
static hm_str next_field(const char **pos, const char *end, char sep)
{
    hm_str      f;
    const char *p = *pos;
    const char *q = p;

    while (q < end && *q != sep) ++q;
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

int hm_state_load(hm_state *st, const char *path)
{
    int         fd;
    size_t      total = 0;
    const char *p, *end;

    memset(st, 0, sizeof(*st));
    st->path = path;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* A missing state file simply means "nothing recorded yet". */
        if (errno == ENOENT) return 0;
        hm_err("hacman: %s: cannot read state file\n", path);
        return -1;
    }

    for (;;) {
        ssize_t n = read(fd, state_buf + total, sizeof(state_buf) - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            hm_err("hacman: %s: read failed\n", path);
            close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total == sizeof(state_buf)) {
            hm_err("hacman: %s: state file too large\n", path);
            close(fd);
            return -1;
        }
    }
    close(fd);

    p   = state_buf;
    end = state_buf + total;
    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        const char *cur = p;
        hm_str      name, checked, changed, mark;

        p = (nl != NULL) ? nl + 1 : end;
        if (line_end == cur || *cur == '#') continue; /* blank or header */

        name    = next_field(&cur, line_end, '\t');
        checked = next_field(&cur, line_end, '\t');
        changed = next_field(&cur, line_end, '\t');
        mark    = next_field(&cur, line_end, '\t');

        if (name.len == 0 || st->count >= HM_MAX_STATE_ENTRIES) continue;

        {
            hm_state_entry *e = &st->ent[st->count++];
            hm_str_copy(e->name, sizeof(e->name), name);
            hm_str_copy(e->mark, sizeof(e->mark), mark);
            e->last_check  = parse_epoch(checked);
            e->last_change = parse_epoch(changed);
        }
    }

    return 0;
}

hm_state_entry *hm_state_find(hm_state *st, hm_str name)
{
    int i;
    for (i = 0; i < st->count; ++i) {
        if (hm_str_eq(name, st->ent[i].name)) return &st->ent[i];
    }
    return NULL;
}

/* Like hm_state_find(), but appends an empty entry when the name is unknown. */
hm_state_entry *hm_state_intern(hm_state *st, hm_str name)
{
    hm_state_entry *e = hm_state_find(st, name);

    if (e != NULL) return e;
    if (st->count >= HM_MAX_STATE_ENTRIES) return NULL;

    e = &st->ent[st->count++];
    memset(e, 0, sizeof(*e));
    hm_str_copy(e->name, sizeof(e->name), name);
    return e;
}

/* Creates every missing directory component of `path`'s parent. */
static void mkdir_parents(const char *path)
{
    char   tmp[HM_URL_MAX + 1];
    size_t i, n = 0;

    while (path[n] != '\0' && n + 1 < sizeof(tmp)) { tmp[n] = path[n]; ++n; }
    tmp[n] = '\0';

    while (n > 0 && tmp[n - 1] != '/') --n; /* strip the file name */
    if (n == 0) return;
    tmp[n - 1] = '\0';

    for (i = 1; tmp[i] != '\0'; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            (void)mkdir(tmp, 0700);
            tmp[i] = '/';
        }
    }
    (void)mkdir(tmp, 0700);
}

static size_t append_raw(char *dst, size_t o, size_t cap, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len && o + 1 < cap; ++i) dst[o++] = s[i];
    return o;
}

/* Appends a field value, neutralising anything that would break the table
 * layout. Separators themselves go through append_raw(). */
static size_t append_field(char *dst, size_t o, size_t cap, const char *s, size_t len)
{
    size_t i;
    for (i = 0; i < len && o + 1 < cap; ++i) {
        char ch = s[i];
        if (ch == '\t' || ch == '\n' || ch == '\r') ch = ' ';
        dst[o++] = ch;
    }
    return o;
}

static size_t append_long(char *dst, size_t o, size_t cap, long v)
{
    char   tmp[24];
    size_t n = 0;
    unsigned long x = (v < 0) ? 0UL : (unsigned long)v;

    do { tmp[n++] = (char)('0' + (x % 10)); x /= 10; } while (x > 0 && n < sizeof(tmp));
    while (n > 0 && o + 1 < cap) dst[o++] = tmp[--n];
    return o;
}

/* Writes the table to a sibling temp file and rename()s it into place, so a
 * crash or a full disk can never leave a half-written state file behind. */
int hm_state_save(const hm_state *st)
{
    char   tmp_path[HM_URL_MAX + 8];
    size_t o = 0, n = 0;
    int    fd, i;

    if (!st->dirty) return 0;

    while (st->path[n] != '\0' && n + 5 < sizeof(tmp_path)) { tmp_path[n] = st->path[n]; ++n; }
    memcpy(tmp_path + n, ".tmp", 5);

    o = append_raw(state_buf, o, sizeof(state_buf),
                   HM_STATE_HEADER, sizeof(HM_STATE_HEADER) - 1);
    for (i = 0; i < st->count; ++i) {
        const hm_state_entry *e = &st->ent[i];
        o = append_field(state_buf, o, sizeof(state_buf), e->name, strlen(e->name));
        o = append_raw(state_buf, o, sizeof(state_buf), "\t", 1);
        o = append_long(state_buf, o, sizeof(state_buf), e->last_check);
        o = append_raw(state_buf, o, sizeof(state_buf), "\t", 1);
        o = append_long(state_buf, o, sizeof(state_buf), e->last_change);
        o = append_raw(state_buf, o, sizeof(state_buf), "\t", 1);
        o = append_field(state_buf, o, sizeof(state_buf), e->mark, strlen(e->mark));
        o = append_raw(state_buf, o, sizeof(state_buf), "\n", 1);
    }

    mkdir_parents(st->path);

    fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        hm_err("hacman: %s: cannot write state file\n", tmp_path);
        return -1;
    }
    {
        size_t written = 0;
        while (written < o) {
            ssize_t w = write(fd, state_buf + written, o - written);
            if (w < 0) {
                if (errno == EINTR) continue;
                hm_err("hacman: %s: write failed\n", tmp_path);
                close(fd);
                return -1;
            }
            written += (size_t)w;
        }
    }
    close(fd);

    if (rename(tmp_path, st->path) != 0) {
        hm_err("hacman: %s: cannot replace state file\n", st->path);
        return -1;
    }
    return 0;
}
