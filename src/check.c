/* "Did the URL change?" - the second half of the fast path.
 *
 * HTTP is delegated to curl(1) rather than linked in: hacman ships as a static
 * musl binary, and statically linking a TLS stack would dwarf the program and
 * drag a CA bundle along with it. The cost of one fork+exec is irrelevant next
 * to the network round trip, and it is only paid when a project is actually
 * due for a check - a run where every schedule says "not yet" spawns nothing
 * at all. */

#include "hacman.h"

#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config.h"

static char body_buf[HM_BODY_MAX];

/* Runs curl and captures its stdout. Returns 0 on success, -1 otherwise. */
static int run_curl(const char *url, int head_only, int timeout_secs,
                    size_t *out_len)
{
    char  timeout_arg[16];
    const char *argv[16];
    int   argc = 0;
    int   fds[2];
    pid_t pid;
    size_t total = 0;

    {   /* timeout as a decimal string, without stdio */
        size_t n = 0, i = 0;
        char   tmp[16];
        unsigned long v = (unsigned long)(timeout_secs > 0 ? timeout_secs : 15);
        do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v > 0 && n < sizeof(tmp));
        while (n > 0) timeout_arg[i++] = tmp[--n];
        timeout_arg[i] = '\0';
    }

    argv[argc++] = HM_CURL;
    argv[argc++] = "--silent";
    argv[argc++] = "--show-error";
    argv[argc++] = "--location";     /* follow redirects                    */
    argv[argc++] = "--fail";         /* HTTP >= 400 is a failure            */
    argv[argc++] = "--max-time";
    argv[argc++] = timeout_arg;
    if (head_only) argv[argc++] = "--head";
    argv[argc++] = "--";
    argv[argc++] = url;
    argv[argc]   = NULL;

    if (pipe(fds) != 0) {
        hm_err("hacman: pipe() failed\n");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        hm_err("hacman: fork() failed\n");
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) _exit(127);
        close(fds[1]);
        execvp(HM_CURL, (char *const *)argv);
        _exit(127);
    }

    close(fds[1]);
    for (;;) {
        ssize_t n = read(fds[0], body_buf + total, sizeof(body_buf) - total - 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        total += (size_t)n;
        if (total + 1 >= sizeof(body_buf)) break; /* truncate oversized bodies */
    }
    close(fds[0]);
    body_buf[total] = '\0';

    {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                hm_err("hacman: waitpid() failed\n");
                return -1;
            }
        }
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
                hm_err("hacman: cannot execute '%s' (is curl installed?)\n", HM_CURL);
            }
            return -1;
        }
    }

    *out_len = total;
    return 0;
}

static int ci_prefix(const char *line, size_t len, const char *name)
{
    size_t i;
    for (i = 0; name[i] != '\0'; ++i) {
        char c;
        if (i >= len) return 0;
        c = line[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != name[i]) return 0;
    }
    return 1;
}

/* Extracts the last occurrence of `name:` from a header block. Redirect chains
 * print one block per hop, and only the final hop describes the resource. */
static int last_header(const char *buf, size_t len, const char *name, hm_str *out)
{
    const char *p     = buf;
    const char *end   = buf + len;
    int         found = 0;
    size_t      name_len = strlen(name);

    while (p < end) {
        const char *nl = (const char *)memchr(p, '\n', (size_t)(end - p));
        const char *line_end = nl ? nl : end;
        size_t      line_len = (size_t)(line_end - p);

        while (line_len > 0 && p[line_len - 1] == '\r') --line_len;

        if (ci_prefix(p, line_len, name)) {
            const char *v = p + name_len;
            const char *w = p + line_len;
            while (v < w && (*v == ' ' || *v == '\t')) ++v;
            while (w > v && (w[-1] == ' ' || w[-1] == '\t')) --w;
            out->ptr = v;
            out->len = (size_t)(w - v);
            found    = 1;
        }
        p = nl ? nl + 1 : end;
    }
    return found;
}

static const char *find_sub(const char *hay, size_t hay_len, hm_str needle)
{
    size_t i;

    if (needle.len == 0 || needle.len > hay_len) return NULL;
    for (i = 0; i + needle.len <= hay_len; ++i) {
        if (hay[i] == needle.ptr[0] && memcmp(hay + i, needle.ptr, needle.len) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

/* Pulls the text between `version-prefix` and `version-suffix` out of the
 * body. An empty suffix means "up to the end of the line". */
static int extract_version(const hm_project *p, const char *body, size_t len,
                           char *out, size_t cap)
{
    const char *start = find_sub(body, len, p->version_prefix);
    const char *end;

    if (start == NULL) {
        hm_err("hacman: %S: version-prefix not found in response\n", p->name);
        return -1;
    }
    start += p->version_prefix.len;

    if (p->version_suffix.len > 0) {
        end = find_sub(start, len - (size_t)(start - body), p->version_suffix);
        if (end == NULL) {
            hm_err("hacman: %S: version-suffix not found after version-prefix\n", p->name);
            return -1;
        }
    } else {
        end = (const char *)memchr(start, '\n', len - (size_t)(start - body));
        if (end == NULL) end = body + len;
    }

    if (end <= start) {
        hm_err("hacman: %S: extracted version is empty\n", p->name);
        return -1;
    }
    {
        /* Surrounding whitespace is trimmed: SIML cannot express a value with
         * leading or trailing spaces, so `version-prefix: version` has to be
         * able to match "version 1.2.3". */
        hm_str v;
        v.ptr = start;
        v.len = (size_t)(end - start);
        while (v.len > 0 && (*v.ptr == ' ' || *v.ptr == '\t')) { ++v.ptr; --v.len; }
        while (v.len > 0 && (v.ptr[v.len - 1] == '\r' ||
                             v.ptr[v.len - 1] == ' '  ||
                             v.ptr[v.len - 1] == '\t')) --v.len;
        if (v.len == 0) {
            hm_err("hacman: %S: extracted version is empty\n", p->name);
            return -1;
        }
        hm_str_copy(out, cap, v);
    }
    return 0;
}

int hm_check(const hm_project *p, int timeout_secs, hm_check_result *res)
{
    char   url[HM_URL_MAX + 1];
    size_t len = 0;

    if (p->url.len > HM_URL_MAX) {
        hm_err("hacman: %S: url is too long\n", p->name);
        return -1;
    }
    hm_str_copy(url, sizeof(url), p->url);

    res->mark[0]  = '\0';
    res->body     = "";
    res->body_len = 0;

    if (run_curl(url, p->check == HM_CHECK_ETAG, timeout_secs, &len) != 0) {
        hm_err("hacman: %S: request failed: %s\n", p->name, url);
        return -1;
    }

    switch (p->check) {
    case HM_CHECK_ETAG: {
        hm_str v;
        if (last_header(body_buf, len, "etag:", &v) ||
            last_header(body_buf, len, "last-modified:", &v)) {
            hm_str_copy(res->mark, sizeof(res->mark), v);
            return 0;
        }
        hm_err("hacman: %S: response has no ETag or Last-Modified header; "
               "use 'check: hash' or 'check: version'\n", p->name);
        return -1;
    }
    case HM_CHECK_HASH:
        hm_hash_hex(body_buf, len, res->mark);
        res->body     = body_buf;
        res->body_len = len;
        return 0;

    case HM_CHECK_VERSION:
        if (extract_version(p, body_buf, len, res->mark, sizeof(res->mark)) != 0) {
            return -1;
        }
        res->body     = body_buf;
        res->body_len = len;
        return 0;
    }

    return -1;
}
