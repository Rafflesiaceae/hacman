/* Small, dependency-free helpers for hacman's fast path.
 *
 * Output goes through a hand-rolled formatter on top of write(2) instead of
 * <stdio.h>: the fast path then touches no stdio buffers, no locale and no
 * allocator. The slow (install) path is free to use stdio. */

#include "hacman.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

#include "xxhash.h"

#define HM_OUT_CAP 4096
/* Longest single hm_out()/hm_err() call. Formatting truncates rather than
 * overflowing, so this has to stay above the longest message (the usage
 * text). */
#define HM_LINE_CAP 4096

static char   out_buf[HM_OUT_CAP];
static size_t out_len;
static int    out_fd = 1;
static int    out_prefix_stderr;
static int    stderr_line_start = 1;
static const char stderr_prefix[] = "hacman: ";

/* write(2) with EINTR/short-write handling. */
static void hm_write_all(int fd, const char *s, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return; /* nothing useful left to do about a failing stdout */
        }
        s += (size_t)n;
        len -= (size_t)n;
    }
}

static size_t fmt_ulong(char *dst, size_t cap, unsigned long v)
{
    char   tmp[24];
    size_t n = 0, i = 0;

    do {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v > 0 && n < sizeof(tmp));

    while (n > 0 && i < cap) dst[i++] = tmp[--n];
    return i;
}

/* Renders `fmt` into `dst`, truncating instead of overflowing.
 * Supported conversions: %s %S %u %d %c %% (see hacman.h). */
static size_t hm_vformat(char *dst, size_t cap, const char *fmt, va_list ap)
{
    size_t o = 0;

    for (; *fmt != '\0' && o < cap; ++fmt) {
        if (*fmt != '%') {
            dst[o++] = *fmt;
            continue;
        }
        ++fmt;
        switch (*fmt) {
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            while (*s != '\0' && o < cap) dst[o++] = *s++;
            break;
        }
        case 'S': {
            hm_str s = va_arg(ap, hm_str);
            size_t i;
            for (i = 0; i < s.len && o < cap; ++i) dst[o++] = s.ptr[i];
            break;
        }
        case 'u':
            o += fmt_ulong(dst + o, cap - o, va_arg(ap, unsigned long));
            break;
        case 'd': {
            long v = va_arg(ap, long);
            if (v < 0) {
                dst[o++] = '-';
                if (o < cap) o += fmt_ulong(dst + o, cap - o, (unsigned long)-v);
            } else {
                o += fmt_ulong(dst + o, cap - o, (unsigned long)v);
            }
            break;
        }
        case 'c':
            dst[o++] = (char)va_arg(ap, int);
            break;
        case '%':
            dst[o++] = '%';
            break;
        case '\0':
            return o;
        default:
            dst[o++] = *fmt;
            break;
        }
    }
    return o;
}

void hm_out_target(int fd)
{
    hm_out_flush();
    out_fd            = fd;
    out_prefix_stderr = (fd == STDERR_FILENO);
    stderr_line_start = 1;
}

/* Prefixes stderr lines while allowing messages that already carry hacman's
 * marker to pass through once. This also handles multi-line usage text. */
static void hm_write_prefixed_stderr(const char *s, size_t len, int keep_existing_prefix)
{
    size_t offset = 0;

    while (offset < len) {
        const char *newline;
        size_t      part;

        if (stderr_line_start) {
            if (!keep_existing_prefix ||
                len - offset < sizeof(stderr_prefix) - 1 ||
                memcmp(s + offset, stderr_prefix, sizeof(stderr_prefix) - 1) != 0) {
                hm_write_all(STDERR_FILENO, stderr_prefix, sizeof(stderr_prefix) - 1);
            }
            stderr_line_start = 0;
        }
        newline = (const char *)memchr(s + offset, '\n', len - offset);
        part    = newline != NULL ? (size_t)(newline - (s + offset)) + 1 : len - offset;
        hm_write_all(STDERR_FILENO, s + offset, part);
        offset += part;
        if (s[offset - 1] == '\n') stderr_line_start = 1;
    }
}

void hm_err_bytes(const char *data, size_t len)
{
    hm_out_flush();
    hm_write_prefixed_stderr(data, len, 1);
}

void hm_err_stream_end(void)
{
    if (!stderr_line_start) {
        hm_write_all(STDERR_FILENO, "\n", 1);
        stderr_line_start = 1;
    }
}

void hm_out_flush(void)
{
    if (out_len > 0) {
        if (out_prefix_stderr) {
            hm_write_prefixed_stderr(out_buf, out_len, 0);
        } else {
            hm_write_all(out_fd, out_buf, out_len);
        }
        out_len = 0;
    }
}

void hm_out(const char *fmt, ...)
{
    char    line[HM_LINE_CAP];
    size_t  n;
    va_list ap;

    va_start(ap, fmt);
    n = hm_vformat(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n > HM_OUT_CAP - out_len) hm_out_flush();
    if (n > HM_OUT_CAP) {
        if (out_prefix_stderr) {
            hm_write_prefixed_stderr(line, n, 0);
        } else {
            hm_write_all(out_fd, line, n);
        }
        return;
    }
    memcpy(out_buf + out_len, line, n);
    out_len += n;
}

void hm_err(const char *fmt, ...)
{
    char    line[HM_LINE_CAP];
    size_t  n;
    va_list ap;

    hm_out_flush(); /* keep stdout/stderr ordering intact */

    va_start(ap, fmt);
    n = hm_vformat(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* A previous streamed diagnostic may have ended without a newline. Keep
     * the next hacman message on its own clearly marked line. */
    hm_err_stream_end();
    hm_write_prefixed_stderr(line, n, 1);
}

int hm_str_eq(hm_str a, const char *lit)
{
    size_t i;
    for (i = 0; i < a.len; ++i) {
        if (lit[i] == '\0' || lit[i] != a.ptr[i]) return 0;
    }
    return lit[a.len] == '\0';
}

int hm_str_eq_str(hm_str a, hm_str b)
{
    return a.len == b.len && (a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0);
}

/* Copies `s` into `dst` as a NUL-terminated string. Returns the copied length,
 * which is smaller than s.len if it did not fit. */
size_t hm_str_copy(char *dst, size_t cap, hm_str s)
{
    size_t n = s.len;
    if (cap == 0) return 0;
    if (n > cap - 1) n = cap - 1;
    if (n > 0) memcpy(dst, s.ptr, n);
    dst[n] = '\0';
    return n;
}

/* XXH3 provides one stable digest representation for response bodies and
 * cache identities. */
void hm_hash_hex(const char *data, size_t len, char *out17)
{
    static const char hex[] = "0123456789abcdef";
    XXH64_hash_t      h     = XXH3_64bits(data, len);
    size_t            i;

    for (i = 0; i < 16; ++i) out17[i] = hex[(h >> (60 - 4 * i)) & 0xF];
    out17[16] = '\0';
}

int hm_parse_ulong(hm_str s, unsigned long *out)
{
    unsigned long v = 0;
    size_t        i;

    if (s.len == 0) return -1;
    for (i = 0; i < s.len; ++i) {
        if (s.ptr[i] < '0' || s.ptr[i] > '9') return -1;
        v = v * 10 + (unsigned long)(s.ptr[i] - '0');
    }
    *out = v;
    return 0;
}

/* Slurps a whole file in as few syscalls as the kernel allows: one open(),
 * read() until EOF, one close(). No stdio, no per-line allocation - the SIML
 * parser is then fed slices of this single buffer.
 *
 * `path` is a file name, or "-" for standard input. */
long hm_read_all(const char *path, char *buf, size_t cap)
{
    int    fd       = 0;
    int    close_fd = 0;
    size_t total    = 0;

    if (strcmp(path, "-") != 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            hm_err("hacman: %s: cannot open file\n", path);
            return -1;
        }
        close_fd = 1;
    }

    for (;;) {
        ssize_t n = read(fd, buf + total, cap - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            hm_err("hacman: %s: read failed\n", path);
            if (close_fd) close(fd);
            return -1;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total == cap) {
            hm_err("hacman: %s: input too large (max %u bytes)\n", path, (unsigned long)cap);
            if (close_fd) close(fd);
            return -1;
        }
    }

    if (close_fd) close(fd);
    return (long)total;
}
