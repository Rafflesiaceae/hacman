/*
 * siml-fuzzer.c — libFuzzer harness for the SIML reference parser.
 *
 * Checks two invariants on every fuzz input:
 *   1. The parser does not crash, overflow a buffer, or corrupt memory
 *      (AddressSanitizer catches these automatically).
 *   2. Roundtrip: if the parser accepts the input, reconstructing the
 *      document from parse events must reproduce the original bytes
 *      exactly.  A mismatch means the parser and the reconstructor have
 *      diverged — a correctness bug — and the harness calls abort() so
 *      libFuzzer saves the reproducer.
 *
 * Build and run:
 *   python3 fuzzer.py               # compile if needed, seed corpus, run 15 min
 *   python3 fuzzer.py --forever     # run until Ctrl-C
 *   python3 fuzzer.py -- -max_total_time=300  # pass flags to libFuzzer
 *   meson compile fuzz -C build     # same as above via meson
 *
 * Or manually:
 *   clang -fsanitize=fuzzer,address -O1 -o fuzzer fuzzer.c
 *   ./fuzzer .cache/fuzzer/ testcases/
 *
 * Requires clang with libFuzzer support (Linux and macOS).
 */

#include "siml.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- input reader -------------------------------------------------------- */

struct fuzz_reader {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
};

static int fuzz_read_line(void *userdata, const char **out_line, size_t *out_len) {
    struct fuzz_reader *r = (struct fuzz_reader *)userdata;
    size_t start, i;
    if (r->pos >= r->len) return 0;
    start = r->pos;
    i = start;
    while (i < r->len && r->data[i] != '\n') i++;
    if (i < r->len) i++;
    *out_line = (const char *)(r->data + start);
    *out_len = i - start;
    r->pos = i;
    return 1;
}

/* ---- growable byte buffer ------------------------------------------------ */

struct buffer {
    char  *data;
    size_t len;
    size_t cap;
};

static int buf_reserve(struct buffer *b, size_t extra) {
    size_t needed = b->len + extra;
    size_t new_cap;
    char *p;
    if (needed <= b->cap) return 1;
    new_cap = b->cap ? b->cap : 256;
    while (new_cap < needed) new_cap *= 2;
    p = (char *)realloc(b->data, new_cap);
    if (!p) return 0;
    b->data = p;
    b->cap = new_cap;
    return 1;
}

static int buf_append(struct buffer *b, const char *s, size_t len) {
    if (!buf_reserve(b, len)) return 0;
    memcpy(b->data + b->len, s, len);
    b->len += len;
    return 1;
}

static int buf_append_char(struct buffer *b, char c) {
    if (!buf_reserve(b, 1)) return 0;
    b->data[b->len++] = c;
    return 1;
}

static int buf_append_spaces(struct buffer *b, size_t count) {
    size_t i;
    if (!buf_reserve(b, count)) return 0;
    for (i = 0; i < count; i++) b->data[b->len++] = ' ';
    return 1;
}

/* ---- reconstruction helpers (mirrors siml-tool.c) ------------------------ */

static int emit_inline_comment(struct buffer *b, unsigned int spaces,
                                const siml_slice *comment) {
    if (!comment || comment->len == 0) return 1;
    if (!buf_append_spaces(b, (size_t)spaces)) return 0;
    if (!buf_append(b, "# ", 2)) return 0;
    return buf_append(b, comment->ptr, comment->len);
}

static int emit_prefix(struct buffer *b, size_t indent,
                        const char *key, size_t key_len,
                        int in_sequence, int has_inline_value,
                        unsigned int inline_prefixes) {
    unsigned int i;
    size_t prefix_indent = (size_t)inline_prefixes * 2;
    if (prefix_indent > indent) return 0;
    if (!buf_append_spaces(b, indent - prefix_indent)) return 0;
    for (i = 0; i < inline_prefixes; i++) {
        if (!buf_append(b, "- ", 2)) return 0;
    }
    if (in_sequence) {
        if (!buf_append(b, "-", 1)) return 0;
        if (has_inline_value && !buf_append(b, " ", 1)) return 0;
        return 1;
    }
    if (!buf_append(b, key, key_len)) return 0;
    if (has_inline_value)
        return buf_append(b, ": ", 2);
    return buf_append(b, ":", 1);
}

static int build_flow_sequence(siml_parser *parser, struct buffer *b,
                                siml_event *ev) {
    int first = 1;
    if (!buf_append_char(b, '[')) return 0;
    for (;;) {
        siml_event_type t = siml_next(parser, ev);
        if (t == SIML_EVENT_ERROR) return 0;
        if (t == SIML_EVENT_SEQUENCE_END) return buf_append_char(b, ']');
        if (!first && !buf_append_char(b, ',')) return 0;
        first = 0;
        if (t == SIML_EVENT_SEQUENCE_START) {
            if (!build_flow_sequence(parser, b, ev)) return 0;
        } else if (t == SIML_EVENT_SCALAR) {
            if (!buf_append(b, ev->value.ptr, ev->value.len)) return 0;
        } else {
            return 0;
        }
    }
}

/*
 * Reconstruct the document from parse events and compare against the original
 * input bytes.
 *
 * Returns:
 *   1  parse succeeded and reconstruction matches input exactly
 *   0  parse succeeded but reconstruction does not match — roundtrip bug
 *  -1  parse error or allocation failure (inconclusive, not a bug)
 */
static int run_roundtrip(const uint8_t *data, size_t size) {
    struct fuzz_reader reader;
    siml_parser        parser;
    siml_event         ev;
    struct buffer      out;
    size_t             stack_indent[SIML_MAX_NESTING];
    siml_container_type stack_type[SIML_MAX_NESTING];
    unsigned int       stack_inline_prefixes[SIML_MAX_NESTING];
    size_t             depth;
    size_t             cur_indent;
    int                in_sequence;
    unsigned int       inline_prefixes;
    int                parsed_ok;
    int                ok;

    reader.data = data;
    reader.len  = size;
    reader.pos  = 0;
    out.data    = NULL;
    out.len     = 0;
    out.cap     = 0;
    depth       = 0;
    parsed_ok   = 0;
    ok          = 1;

    siml_parser_init(&parser, fuzz_read_line, &reader);

    for (;;) {
        siml_event_type t = siml_next(&parser, &ev);
        if (t == SIML_EVENT_ERROR) break;
        if (t == SIML_EVENT_STREAM_END) { parsed_ok = 1; break; }

        in_sequence     = (depth > 0 && stack_type[depth - 1] == SIML_CONTAINER_SEQ);
        cur_indent      = (depth > 0) ? stack_indent[depth - 1] : 0;
        inline_prefixes = (depth > 0) ? stack_inline_prefixes[depth - 1] : 0;

        switch (t) {
        case SIML_EVENT_STREAM_START:
        case SIML_EVENT_DOCUMENT_START:
            break;

        case SIML_EVENT_DOCUMENT_END:
            if (parser.doc_state == SIML_DOC_BETWEEN)
                ok = buf_append(&out, "---", 3) && buf_append_char(&out, '\n');
            break;

        case SIML_EVENT_SHEBANG:
            ok = buf_append(&out, "#!", 2) &&
                 buf_append(&out, ev.value.ptr, ev.value.len) &&
                 buf_append_char(&out, '\n');
            break;

        case SIML_EVENT_COMMENT:
            ok = buf_append(&out, ev.value.ptr, ev.value.len) &&
                 buf_append_char(&out, '\n');
            break;

        case SIML_EVENT_MAPPING_START:
            if (ev.key.len > 0) {
                ok = emit_prefix(&out, cur_indent, ev.key.ptr, ev.key.len,
                                 in_sequence, 0, inline_prefixes) &&
                     buf_append_char(&out, '\n');
                if (ok && inline_prefixes > 0)
                    stack_inline_prefixes[depth - 1] = 0;
            }
            if (!ok || depth >= SIML_MAX_NESTING) { ok = 0; break; }
            stack_type[depth]            = SIML_CONTAINER_MAP;
            stack_indent[depth]          = (depth == 0) ? 0 : (cur_indent + 2);
            stack_inline_prefixes[depth] = in_sequence ? inline_prefixes + 1 : 0;
            if (in_sequence && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            depth++;
            break;

        case SIML_EVENT_SEQUENCE_START:
            if (ev.seq_style == SIML_SEQ_STYLE_FLOW) {
                char         key_buf[SIML_MAX_KEY_LEN + 1];
                char         cmt_buf[SIML_MAX_INLINE_COMMENT_TEXT_LEN + 1];
                siml_slice   key, cmt;
                unsigned int cmt_spaces;
                struct buffer flow;

                key.len = ev.key.len > SIML_MAX_KEY_LEN
                          ? SIML_MAX_KEY_LEN : ev.key.len;
                if (key.len > 0) memcpy(key_buf, ev.key.ptr, key.len);
                key.ptr = key_buf;
                key_buf[key.len] = '\0';

                cmt.len = ev.inline_comment.len > SIML_MAX_INLINE_COMMENT_TEXT_LEN
                          ? SIML_MAX_INLINE_COMMENT_TEXT_LEN
                          : ev.inline_comment.len;
                if (cmt.len > 0)
                    memcpy(cmt_buf, ev.inline_comment.ptr, cmt.len);
                cmt.ptr    = cmt_buf;
                cmt_buf[cmt.len] = '\0';
                cmt_spaces = ev.inline_comment_spaces;

                flow.data = NULL; flow.len = 0; flow.cap = 0;
                if (!build_flow_sequence(&parser, &flow, &ev)) {
                    free(flow.data); ok = 0; break;
                }
                /* Re-read after build_flow_sequence consumed events. */
                in_sequence = (depth > 0 &&
                               stack_type[depth - 1] == SIML_CONTAINER_SEQ);
                cur_indent  = (depth > 0) ? stack_indent[depth - 1] : 0;
                ok = emit_prefix(&out, cur_indent, key.ptr, key.len,
                                 in_sequence, 1, inline_prefixes) &&
                     buf_append(&out, flow.data, flow.len) &&
                     emit_inline_comment(&out, cmt_spaces, &cmt) &&
                     buf_append_char(&out, '\n');
                free(flow.data);
                if (ok && inline_prefixes > 0)
                    stack_inline_prefixes[depth - 1] = 0;
                break;
            }
            if (ev.key.len > 0) {
                ok = emit_prefix(&out, cur_indent, ev.key.ptr, ev.key.len,
                                 in_sequence, 0, inline_prefixes) &&
                     buf_append_char(&out, '\n');
                if (ok && inline_prefixes > 0)
                    stack_inline_prefixes[depth - 1] = 0;
            }
            if (!ok || depth >= SIML_MAX_NESTING) { ok = 0; break; }
            stack_type[depth]            = SIML_CONTAINER_SEQ;
            stack_indent[depth]          = (depth == 0) ? 0 : (cur_indent + 2);
            stack_inline_prefixes[depth] = in_sequence ? inline_prefixes + 1 : 0;
            if (in_sequence && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            depth++;
            break;

        case SIML_EVENT_MAPPING_END:
        case SIML_EVENT_SEQUENCE_END:
            if (depth > 0) depth--;
            break;

        case SIML_EVENT_SCALAR:
            ok = emit_prefix(&out, cur_indent, ev.key.ptr, ev.key.len,
                             in_sequence, 1, inline_prefixes) &&
                 buf_append(&out, ev.value.ptr, ev.value.len) &&
                 emit_inline_comment(&out, ev.inline_comment_spaces,
                                     &ev.inline_comment) &&
                 buf_append_char(&out, '\n');
            if (ok && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            break;

        case SIML_EVENT_BLOCK_SCALAR_START:
            ok = emit_prefix(&out, cur_indent, ev.key.ptr, ev.key.len,
                             in_sequence, 1, inline_prefixes) &&
                 buf_append(&out, "|", 1) &&
                 emit_inline_comment(&out, ev.inline_comment_spaces,
                                     &ev.inline_comment) &&
                 buf_append_char(&out, '\n');
            if (ok && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            break;

        case SIML_EVENT_BLOCK_SCALAR_LINE:
            if (ev.value.len == 0) {
                ok = buf_append_char(&out, '\n');
            } else {
                ok = buf_append_spaces(&out, cur_indent + 2) &&
                     buf_append(&out, ev.value.ptr, ev.value.len) &&
                     buf_append_char(&out, '\n');
            }
            break;

        case SIML_EVENT_BLOCK_SCALAR_END:
            break;

        case SIML_EVENT_MAPPING_ENTRY_HEADER:
            ok = emit_prefix(&out, cur_indent,
                             ev.key.ptr, ev.key.len,
                             in_sequence, 0, inline_prefixes) &&
                 buf_append_char(&out, '\n');
            if (ok && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            break;

        default:
            ok = 0;
            break;
        }

        if (!ok) break;
    }

    if (!parsed_ok || !ok) {
        free(out.data);
        return -1;
    }

    /* Strip trailing '\n' the reconstructor adds if the original didn't end
     * with one — mirrors the same adjustment in cmd_roundtrip. */
    if (size == 0) {
        if (out.len != 0) { free(out.data); return 0; }
    } else {
        if (((const char *)data)[size - 1] != '\n' &&
            out.len > 0 && out.data[out.len - 1] == '\n')
            out.len--;
        if (out.len != size ||
            (out.len > 0 && memcmp(out.data, data, out.len) != 0)) {
            free(out.data);
            return 0;
        }
    }

    free(out.data);
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    int rc = run_roundtrip(data, size);
    /*
     * rc == 1:  parse ok, reconstruction matches — all good
     * rc == -1: parse error or OOM — expected for most fuzz inputs
     * rc == 0:  parse ok but reconstruction does not match — roundtrip bug
     */
    if (rc == 0) abort();
    return 0;
}
