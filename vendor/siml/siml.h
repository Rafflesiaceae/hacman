#ifndef SIML_H_INCLUDED
#define SIML_H_INCLUDED

/*
 * SIML reference parser v0.1
 *
 * Header-only, pure ANSI C89 implementation.
 * Include in exactly one translation unit per binary.
 *
 * - No dynamic allocation.
 * - No I/O. The caller provides a line-reading callback.
 * - Pull parser API: the caller repeatedly calls siml_next() to obtain events.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h> /* size_t */

#ifndef SIML_MAX_KEY_LEN
#define SIML_MAX_KEY_LEN 128
#endif

#ifndef SIML_ERROR_BUF_SIZE
#define SIML_ERROR_BUF_SIZE 160
#endif

#ifndef SIML_MAX_NESTING
#define SIML_MAX_NESTING 32
#endif

#ifndef SIML_MAX_LINE_LEN
#define SIML_MAX_LINE_LEN 4608
#endif

#ifndef SIML_MAX_INLINE_VALUE_LEN
#define SIML_MAX_INLINE_VALUE_LEN 2048
#endif

#ifndef SIML_MAX_FLOW_ELEMENT_LEN
#define SIML_MAX_FLOW_ELEMENT_LEN 128
#endif

#ifndef SIML_MAX_COMMENT_TEXT_LEN
#define SIML_MAX_COMMENT_TEXT_LEN 512
#endif

#ifndef SIML_MAX_SHEBANG_TEXT_LEN
#define SIML_MAX_SHEBANG_TEXT_LEN 512
#endif

#ifndef SIML_MAX_INLINE_COMMENT_TEXT_LEN
#define SIML_MAX_INLINE_COMMENT_TEXT_LEN 256
#endif

#ifndef SIML_MAX_INLINE_COMMENT_SPACES
#define SIML_MAX_INLINE_COMMENT_SPACES 255
#endif

#ifndef SIML_MAX_BLOCK_LINE_LEN
#define SIML_MAX_BLOCK_LINE_LEN 4096
#endif

/* Error codes */
typedef enum siml_error_code {
    SIML_ERR_NONE = 0,
    SIML_ERR_IO,
    SIML_ERR_UTF8_BOM,
    SIML_ERR_FINAL_LINE_NO_LF,
    SIML_ERR_CRLF,
    SIML_ERR_CR,
    SIML_ERR_LINE_TOO_LONG,
    SIML_ERR_SHEBANG_EMPTY,
    SIML_ERR_SHEBANG_POSITION,
    SIML_ERR_SHEBANG_TOO_LONG,
    SIML_ERR_BLANK_LINE,
    SIML_ERR_WHITESPACE_ONLY,
    SIML_ERR_TABS,
    SIML_ERR_TRAILING_SPACES,
    SIML_ERR_UNKNOWN_LINE_FORM,
    SIML_ERR_SEPARATOR_FORMAT,
    SIML_ERR_SEPARATOR_INDENT,
    SIML_ERR_SEPARATOR_INLINE_COMMENT,
    SIML_ERR_SEPARATOR_BEFORE_DOC,
    SIML_ERR_SEPARATOR_AFTER_DOC,
    SIML_ERR_DOC_INDENT,
    SIML_ERR_DOC_SCALAR,
    SIML_ERR_INDENT_MULTIPLE,
    SIML_ERR_INDENT_WRONG,
    SIML_ERR_INDENT_NEST_MISMATCH,
    SIML_ERR_NODE_KIND_MIX,
    SIML_ERR_KEY_ILLEGAL,
    SIML_ERR_KEY_TOO_LONG,
    SIML_ERR_EXPECT_SPACE_AFTER_COLON,
    SIML_ERR_HEADER_MAP_INLINE_COMMENT,
    SIML_ERR_HEADER_MAP_NO_NESTED,
    SIML_ERR_EXPECT_SPACE_AFTER_DASH,
    SIML_ERR_HEADER_SEQ_INLINE_COMMENT,
    SIML_ERR_HEADER_SEQ_NO_NESTED,
    SIML_ERR_EMPTY_COMMENT,
    SIML_ERR_COMMENT_INDENT,
    SIML_ERR_COMMENT_TOO_LONG,
    SIML_ERR_INLINE_COMMENT_ALIGN,
    SIML_ERR_INLINE_COMMENT_SPACE,
    SIML_ERR_INLINE_COMMENT_TOO_LONG,
    SIML_ERR_INLINE_VALUE_EMPTY,
    SIML_ERR_INLINE_VALUE_TOO_LONG,
    SIML_ERR_SCALAR_STARTS_WITH_PIPE,
    SIML_ERR_SCALAR_STARTS_WITH_HASH,
    SIML_ERR_FLOW_MULTI_LINE,
    SIML_ERR_FLOW_UNTERMINATED,
    SIML_ERR_FLOW_UNTERMINATED_SAME_LINE,
    SIML_ERR_FLOW_EXCESS_TERM,
    SIML_ERR_FLOW_INLINE_COMMENT,
    SIML_ERR_FLOW_WHITESPACE,
    SIML_ERR_FLOW_EMPTY_ELEM,
    SIML_ERR_FLOW_TRAILING_COMMA,
    SIML_ERR_FLOW_ATOM_TOO_LONG,
    SIML_ERR_FLOW_SCALAR_STARTS_WITH_PIPE,
    SIML_ERR_FLOW_SCALAR_STARTS_WITH_HASH,
    SIML_ERR_BLOCK_EMPTY,
    SIML_ERR_BLOCK_WRONG_INDENT,
    SIML_ERR_BLOCK_LEADING_BLANK,
    SIML_ERR_BLOCK_TRAILING_BLANK,
    SIML_ERR_BLOCK_LINE_TOO_LONG,
    SIML_ERR_BLOCK_WHITESPACE_ONLY,
    SIML_ERR_SEQ_ITEM_REQUIRES_INLINE
} siml_error_code;

/* Event types for the pull parser */
typedef enum siml_event_type {
    SIML_EVENT_NONE = 0,
    SIML_EVENT_STREAM_START,
    SIML_EVENT_DOCUMENT_START,
    SIML_EVENT_MAPPING_START,
    SIML_EVENT_SEQUENCE_START,
    SIML_EVENT_SCALAR,
    SIML_EVENT_BLOCK_SCALAR_START,
    SIML_EVENT_BLOCK_SCALAR_LINE,
    SIML_EVENT_BLOCK_SCALAR_END,
    SIML_EVENT_SEQUENCE_END,
    SIML_EVENT_MAPPING_END,
    SIML_EVENT_DOCUMENT_END,
    SIML_EVENT_STREAM_END,
    SIML_EVENT_SHEBANG,
    SIML_EVENT_COMMENT,
    SIML_EVENT_MAPPING_ENTRY_HEADER,
    SIML_EVENT_ERROR
} siml_event_type;

typedef enum siml_seq_style {
    SIML_SEQ_STYLE_BLOCK = 0,
    SIML_SEQ_STYLE_FLOW  = 1
} siml_seq_style;

/* String slice referencing parser-internal or callback-owned memory.
 * Valid only until the next call to siml_next(). Copy before then if needed.
 */
typedef struct siml_slice_s {
    const char *ptr;
    size_t      len;
} siml_slice;

/* Line-reading callback: must return
 *   1  : data available; out_line and out_len set to raw bytes including the
 *        terminating LF when one is present (LF absent only on the last line
 *        of a stream that ends without one)
 *   0  : end of stream (no bytes; out_line and out_len not meaningful)
 *  <0  : I/O error
 *
 * The returned pointer must remain valid until the next call to the callback.
 * The parser strips the LF (and detects CRLF / missing-LF errors) itself.
 */
typedef int (*siml_read_line_fn)(void *userdata,
                                 const char **out_line,
                                 size_t *out_len);

/* Event structure filled by siml_next().
 *
 *  - key/value are empty slices for stream/document/container events.
 *  - For mapping values, key is the mapping key on the introducing event.
 *  - For sequence items, key is empty.
 *  - For SCALAR, value is the scalar text.
 *  - For BLOCK_SCALAR_LINE, value is the line text (without indent).
 *  - For SHEBANG, value is the exact text after the "#!" prefix.
 *  - For COMMENT, value is the entire comment line (without the trailing LF).
 *  - For MAPPING_ENTRY_HEADER, key is the mapping key from a header-only
 *    "key:\n" line. The following MAPPING_START or SEQUENCE_START will have
 *    an empty key (delivered here). COMMENT events between the header line
 *    and the container start appear between these two events in the stream.
 *  - inline_comment_* are set only on events introduced by inline values.
 */
typedef struct siml_event_s {
    siml_event_type  type;
    siml_slice       key;
    siml_slice       value;
    siml_slice       inline_comment;
    unsigned int     inline_comment_spaces; /* 0 if none */
    siml_seq_style   seq_style;             /* for SEQUENCE_START */
    long             line;                  /* 1-based physical line number */
    siml_error_code  error_code;
    const char      *error_message;         /* static string; never NULL for ERROR */
} siml_event;

/*
 * Internal implementation types — consumers must not use these directly.
 * They appear here only because siml_parser embeds them by value, which
 * requires their definitions to be visible at the point of allocation.
 */
typedef enum siml_mode_e {
    SIML_MODE_NORMAL = 0,
    SIML_MODE_FLOW,
    SIML_MODE_BLOCK
} siml_mode;

typedef enum siml_container_type_e {
    SIML_CONTAINER_MAP = 0,
    SIML_CONTAINER_SEQ = 1
} siml_container_type;

typedef struct siml_container_s {
    siml_container_type type;
    size_t              indent;
    int                 item_count;
} siml_container;

typedef struct siml_flow_frame_s {
    size_t start;
    size_t end;
    size_t pos;
    int    started;
} siml_flow_frame;

typedef enum siml_doc_state_e {
    SIML_DOC_IDLE    = 0, /* before STREAM_START is emitted */
    SIML_DOC_BEFORE  = 1, /* stream started, no document yet */
    SIML_DOC_IN      = 2, /* inside a document */
    SIML_DOC_BETWEEN = 3  /* after --- separator, awaiting next document */
} siml_doc_state;

/* Parser state. Allocate on the stack or statically; never access fields
 * directly — use only siml_parser_init, siml_parser_reset, and siml_next.
 */
typedef struct siml_parser_s {
    /* User-supplied input */
    siml_read_line_fn read_line;
    void             *userdata;

    /* Current physical line */
    const char       *line;
    size_t            line_len;
    long              line_no;
    int               have_line;   /* boolean */
    int               at_eof;      /* boolean */
    siml_error_code   line_cr_code;

    /* High-level document state */
    siml_doc_state    doc_state;

    /* Current mode */
    siml_mode         mode;

    /* Container stack */
    siml_container    stack[SIML_MAX_NESTING];
    int               depth;

    /* Pending header-only value */
    int               pending_map;
    size_t            pending_indent;
    char              pending_key[SIML_MAX_KEY_LEN + 1];
    size_t            pending_key_len;

    /* Nested item introduced on the current sequence item line */
    int               inline_sequence_item;
    size_t            inline_sequence_indent;

    /* Pending end/start events */
    int               pending_close;
    int               target_depth;
    int               pending_doc_end;
    int               pending_doc_start;
    int               pending_container_start;
    siml_container_type pending_container_type;
    siml_seq_style    pending_seq_style;
    size_t            pending_container_key_len;
    int               pending_stream_end;

    /* Flow sequence parsing state */
    int               flow_depth;
    siml_flow_frame   flow_stack[SIML_MAX_NESTING];
    unsigned int      flow_inline_spaces;
    const char       *flow_inline_comment;
    size_t            flow_inline_comment_len;

    /* Block scalar parsing state */
    size_t            block_indent;
    /* mode_key/mode_key_len: key for whichever mode (flow or block) is active */
    char              mode_key[SIML_MAX_KEY_LEN + 1];
    size_t            mode_key_len;
    unsigned int      block_inline_spaces;
    const char       *block_inline_comment;
    size_t            block_inline_comment_len;
    long              block_start_line;
    int               block_seen_content;
    size_t            block_blank_count;
    long              block_blank_start_line;
    int               block_emit_blanks;

    /* Error state */
    siml_error_code   error_code;
    const char       *error_message;
    char              error_buf[SIML_ERROR_BUF_SIZE];
    long              error_line;
} siml_parser;

/* Initialize parser. p may be stack- or statically-allocated.
 * siml_parser_reset() may be called after init to reparse the same stream.
 */
void siml_parser_init(siml_parser *p,
                      siml_read_line_fn read_line,
                      void *userdata);

/* Reset parser to initial state but keep the same read callback and userdata. */
void siml_parser_reset(siml_parser *p);

/* Obtain the next event. Fills *ev and returns ev->type as a
 * convenience for use in loop conditions. Errors yield SIML_EVENT_ERROR.
 */
siml_event_type siml_next(siml_parser *p, siml_event *ev);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---------------- Implementation ---------------- */

#include <string.h> /* memcpy */

/* Internal helpers ------------------------------------------------------ */

static int siml_is_alpha(char c) {
    return ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
}

static int siml_is_digit(char c) {
    return (c >= '0' && c <= '9');
}

static int siml_is_key_char(char c) {
    return siml_is_alpha(c) || siml_is_digit(c) || c == '_' || c == '-' || c == '.' || c == '/';
}

static int siml_is_space_only(const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        if (s[i] != ' ') return 0;
    }
    return 1;
}

static int siml_is_space_or_tab_only(const char *s, size_t len) {
    size_t i;
    for (i = 0; i < len; ++i) {
        if (s[i] != ' ' && s[i] != '\t') return 0;
    }
    return 1;
}

/* Returns 1 and initialises the error state; returns 0 if already errored. */
static int siml_err_begin(siml_parser *p, siml_error_code code) {
    if (p->error_code != SIML_ERR_NONE) return 0;
    p->error_code = code;
    p->error_line = p->line_no;
    return 1;
}

/* Appends string s to buf[0..cap-1] starting at pos; returns new pos. */
static size_t siml_err_append(char *buf, size_t pos, size_t cap, const char *s) {
    while (s && *s != '\0' && pos + 1 < cap)
        buf[pos++] = *s++;
    return pos;
}

static void siml_set_error(siml_parser *p, siml_error_code code, const char *msg) {
    size_t i;
    if (!siml_err_begin(p, code)) return;
    i = siml_err_append(p->error_buf, 0, SIML_ERROR_BUF_SIZE, msg);
    p->error_buf[i] = '\0';
    p->error_message = p->error_buf;
}

static size_t siml_append_ulong(char *buf, size_t pos, size_t cap,
                                unsigned long value) {
    char tmp[32];
    size_t n = 0;
    if (value == 0) {
        tmp[n++] = '0';
    } else {
        while (value > 0 && n < sizeof(tmp)) {
            tmp[n++] = (char)('0' + (value % 10));
            value /= 10;
        }
    }
    while (n > 0 && pos + 1 < cap) {
        buf[pos++] = tmp[--n];
    }
    return pos;
}

static void siml_set_error_one(siml_parser *p, siml_error_code code,
                               const char *prefix, unsigned long value,
                               const char *suffix) {
    size_t i;
    if (!siml_err_begin(p, code)) return;
    i = siml_err_append(p->error_buf, 0, SIML_ERROR_BUF_SIZE, prefix);
    i = siml_append_ulong(p->error_buf, i, SIML_ERROR_BUF_SIZE, value);
    i = siml_err_append(p->error_buf, i, SIML_ERROR_BUF_SIZE, suffix);
    p->error_buf[i] = '\0';
    p->error_message = p->error_buf;
}

static void siml_set_error_two(siml_parser *p, siml_error_code code,
                               const char *prefix, unsigned long a,
                               const char *middle, unsigned long b) {
    size_t i;
    if (!siml_err_begin(p, code)) return;
    i = siml_err_append(p->error_buf, 0, SIML_ERROR_BUF_SIZE, prefix);
    i = siml_append_ulong(p->error_buf, i, SIML_ERROR_BUF_SIZE, a);
    i = siml_err_append(p->error_buf, i, SIML_ERROR_BUF_SIZE, middle);
    i = siml_append_ulong(p->error_buf, i, SIML_ERROR_BUF_SIZE, b);
    p->error_buf[i] = '\0';
    p->error_message = p->error_buf;
}

static void siml_clear_event(siml_event *ev) {
    ev->type                  = SIML_EVENT_NONE;
    ev->key.ptr               = 0;
    ev->key.len               = 0;
    ev->value.ptr             = 0;
    ev->value.len             = 0;
    ev->inline_comment.ptr    = 0;
    ev->inline_comment.len    = 0;
    ev->inline_comment_spaces = 0;
    ev->seq_style             = SIML_SEQ_STYLE_BLOCK;
    ev->line                  = 0;
    ev->error_code            = SIML_ERR_NONE;
    ev->error_message         = 0;
}

static siml_slice siml_make_slice(const char *p, size_t len) {
    siml_slice s;
    s.ptr = p;
    s.len = len;
    return s;
}

/* Fetch next physical line into parser->line/line_len. Returns:
 *   1 on success, 0 on EOF, -1 on error.
 */
static int siml_fetch_line(siml_parser *p) {
    const char *raw;
    size_t len;
    int rc;

    if (p->at_eof) {
        p->have_line = 0;
        return 0;
    }
    rc = p->read_line(p->userdata, &raw, &len);
    if (rc < 0) {
        p->have_line = 0;
        siml_set_error(p, SIML_ERR_IO, "I/O error while reading input");
        return -1;
    }
    if (rc == 0) {
        p->at_eof    = 1;
        p->have_line = 0;
        return 0;
    }
    p->line_no  += 1;
    p->have_line = 1;
    p->line_cr_code = SIML_ERR_NONE;
    if (len > 0 && raw[len - 1] == '\n') {
        len -= 1;
        if (len > 0 && raw[len - 1] == '\r') {
            p->line_cr_code = SIML_ERR_CRLF;
            len -= 1;
        }
    } else {
        p->at_eof = 1;
        if (len > 0) {
            siml_set_error(p, SIML_ERR_FINAL_LINE_NO_LF,
                           "final line without LF");
            return -1;
        }
        p->have_line = 0;
        return 0;
    }
    if (p->line_cr_code == SIML_ERR_NONE) {
        size_t i;
        for (i = 0; i < len; ++i) {
            if (raw[i] == '\r') {
                p->line_cr_code = SIML_ERR_CR;
                break;
            }
        }
    }
    p->line     = raw;
    p->line_len = len;
    return 1;
}

static int siml_check_line_common(siml_parser *p) {
    const char *s = p->line;
    size_t len = p->line_len;

    if (len > SIML_MAX_LINE_LEN) {
        siml_set_error(p, SIML_ERR_LINE_TOO_LONG,
                       "physical line too long (max 4608 bytes)");
        return 0;
    }
    if (p->line_no == 1 && len >= 3) {
        if ((unsigned char)s[0] == 0xEF &&
            (unsigned char)s[1] == 0xBB &&
            (unsigned char)s[2] == 0xBF) {
            siml_set_error(p, SIML_ERR_UTF8_BOM, "UTF-8 BOM is forbidden");
            return 0;
        }
    }
    if (p->line_cr_code == SIML_ERR_CRLF) {
        siml_set_error(p, SIML_ERR_CRLF, "CRLF is forbidden (\\r\\n found)");
        return 0;
    }
    if (p->line_cr_code == SIML_ERR_CR) {
        siml_set_error(p, SIML_ERR_CR, "CR is forbidden (\\r found)");
        return 0;
    }
    return 1;
}

static int siml_check_line_nonblock(siml_parser *p) {
    size_t i;
    const char *s = p->line;
    size_t len = p->line_len;

    if (!siml_check_line_common(p)) return 0;

    if (len == 0) {
        siml_set_error(p, SIML_ERR_BLANK_LINE, "blank lines are not allowed here");
        return 0;
    }
    for (i = 0; i < len; ++i) {
        if (s[i] == '\t') {
            siml_set_error(p, SIML_ERR_TABS, "tabs are not allowed here");
            return 0;
        }
    }
    if (siml_is_space_only(s, len)) {
        siml_set_error(p, SIML_ERR_WHITESPACE_ONLY,
                       "whitespace-only lines are not allowed here");
        return 0;
    }
    return 1;
}

static int siml_count_indent(siml_parser *p, const char *s, size_t len,
                             size_t *out_indent) {
    size_t i = 0;
    while (i < len && s[i] == ' ') {
        ++i;
    }
    if ((i % 2) != 0) {
        siml_set_error(p, SIML_ERR_INDENT_MULTIPLE,
                       "indentation must be a multiple of 2 spaces");
        return 0;
    }
    *out_indent = i;
    return 1;
}

static int siml_parse_comment_line(siml_parser *p, const char *s, size_t len,
                                   size_t *out_indent) {
    size_t indent = 0;
    size_t text_len = 0;

    if (!siml_count_indent(p, s, len, &indent)) return -1;
    *out_indent = indent;

    if (indent >= len) return 0;
    if (s[indent] != '#') return 0;
    if (indent + 1 >= len) {
        siml_set_error(p, SIML_ERR_EMPTY_COMMENT, "empty comment is forbidden");
        return -1;
    }
    if (s[indent + 1] != ' ') {
        siml_set_error(p, SIML_ERR_EMPTY_COMMENT, "empty comment is forbidden");
        return -1;
    }
    if (indent + 2 >= len) {
        siml_set_error(p, SIML_ERR_EMPTY_COMMENT, "empty comment is forbidden");
        return -1;
    }
    text_len = len - (indent + 2);
    if (text_len > SIML_MAX_COMMENT_TEXT_LEN) {
        siml_set_error(p, SIML_ERR_COMMENT_TOO_LONG,
                       "comment text too long (max 512 bytes)");
        return -1;
    }
    return 1;
}

static int siml_parse_inline_comment(siml_parser *p,
                                     const char *s,
                                     size_t start,
                                     size_t len,
                                     size_t *out_value_len,
                                     unsigned int *out_spaces,
                                     const char **out_comment,
                                     size_t *out_comment_len) {
    size_t i;
    size_t hash_pos = (size_t)(-1);
    size_t comment_start = start;

    *out_spaces = 0;
    *out_comment = 0;
    *out_comment_len = 0;

    if (start < len && s[start] == '[') {
        int depth = 0;
        for (i = start; i < len; ++i) {
            if (s[i] == '[') {
                depth += 1;
            } else if (s[i] == ']') {
                depth -= 1;
                if (depth == 0) {
                    comment_start = i + 1;
                    break;
                }
            }
        }
        if (depth != 0) {
            siml_set_error(p, SIML_ERR_FLOW_UNTERMINATED_SAME_LINE,
                           "unterminated flow sequence on the same line");
            return 0;
        }
    }

    for (i = comment_start; i < len; ++i) {
        if (s[i] == '#') {
            if (i > 0 && s[i - 1] == ' ') {
                hash_pos = i;
                break;
            }
        }
    }

    if (hash_pos == (size_t)(-1)) {
        *out_value_len = len - start;
        return 1;
    }

    {
        size_t j = hash_pos;
        size_t value_end;
        while (j > 0 && s[j - 1] == ' ') {
            --j;
        }
        if (hash_pos <= j) {
            siml_set_error(p, SIML_ERR_INLINE_COMMENT_ALIGN,
                           "inline comment alignment out of range (1..255 spaces)");
            return 0;
        }
        if (hash_pos - j > SIML_MAX_INLINE_COMMENT_SPACES) {
            siml_set_error(p, SIML_ERR_INLINE_COMMENT_ALIGN,
                           "inline comment alignment out of range (1..255 spaces)");
            return 0;
        }
        *out_spaces = (unsigned int)(hash_pos - j);
        value_end = hash_pos - *out_spaces;
        if (value_end < start) value_end = start;
        *out_value_len = value_end - start;
    }

    if (hash_pos + 1 >= len || s[hash_pos + 1] != ' ') {
        siml_set_error(p, SIML_ERR_INLINE_COMMENT_SPACE,
                       "inline comment must have exactly 1 space after '#'");
        return 0;
    }
    if (hash_pos + 2 >= len) {
        siml_set_error(p, SIML_ERR_EMPTY_COMMENT, "empty comment is forbidden");
        return 0;
    }

    *out_comment = s + hash_pos + 2;
    *out_comment_len = len - (hash_pos + 2);
    if (*out_comment_len > SIML_MAX_INLINE_COMMENT_TEXT_LEN) {
        siml_set_error(p, SIML_ERR_INLINE_COMMENT_TOO_LONG,
                       "inline comment text too long (max 256 bytes)");
        return 0;
    }

    return 1;
}

static int siml_push_container(siml_parser *p, siml_container_type type,
                               size_t indent) {
    siml_container *c;

    if (p->depth >= SIML_MAX_NESTING) {
        size_t expected = 0;
        if (p->depth > 0) {
            expected = p->stack[p->depth - 1].indent;
        }
        siml_set_error_one(p, SIML_ERR_INDENT_WRONG,
                           "wrong indentation, expected: ",
                           (unsigned long)expected,
                           "");
        return 0;
    }
    c = &p->stack[p->depth++];
    c->type = type;
    c->indent = indent;
    c->item_count = 0;
    return 1;
}

static int siml_request_container_start(siml_parser *p,
                                        siml_container_type type,
                                        siml_seq_style seq_style,
                                        size_t indent,
                                        const char *key,
                                        size_t key_len) {
    if (!siml_push_container(p, type, indent)) return 0;
    p->pending_container_start = 1;
    p->pending_container_type = type;
    p->pending_seq_style = seq_style;
    p->pending_container_key_len = key_len;
    /* key is either p->pending_key (already there) or "" (len == 0) */
    if (key_len > 0 && key != p->pending_key) {
        size_t i;
        for (i = 0; i < key_len && i < SIML_MAX_KEY_LEN; ++i)
            p->pending_key[i] = key[i];
        p->pending_key[key_len] = '\0';
    }
    return 1;
}

static siml_event_type siml_emit_pending_end(siml_parser *p, siml_event *ev) {
    if (p->pending_close) {
        if (p->depth > p->target_depth) {
            siml_container c = p->stack[p->depth - 1];
            p->depth -= 1;
            ev->type = (c.type == SIML_CONTAINER_MAP) ? SIML_EVENT_MAPPING_END
                                                      : SIML_EVENT_SEQUENCE_END;
            ev->line = p->line_no;
            return ev->type;
        }
        p->pending_close = 0;
    }
    if (p->pending_doc_end) {
        p->pending_doc_end = 0;
        p->doc_state = p->pending_stream_end ? SIML_DOC_BEFORE : SIML_DOC_BETWEEN;
        ev->type = SIML_EVENT_DOCUMENT_END;
        ev->line = p->line_no;
        return ev->type;
    }
    if (p->pending_stream_end) {
        p->pending_stream_end = 0;
        ev->type = SIML_EVENT_STREAM_END;
        ev->line = p->line_no;
        return ev->type;
    }
    return SIML_EVENT_NONE;
}

static siml_event_type siml_emit_pending_start(siml_parser *p, siml_event *ev) {
    if (p->pending_doc_start) {
        p->pending_doc_start = 0;
        ev->type = SIML_EVENT_DOCUMENT_START;
        ev->line = p->line_no;
        return ev->type;
    }
    if (p->pending_container_start) {
        p->pending_container_start = 0;
        if (p->pending_container_type == SIML_CONTAINER_MAP) {
            ev->type = SIML_EVENT_MAPPING_START;
        } else {
            ev->type = SIML_EVENT_SEQUENCE_START;
            ev->seq_style = p->pending_seq_style;
        }
        ev->key = siml_make_slice(p->pending_key,
                                  p->pending_container_key_len);
        ev->line = p->line_no;
        return ev->type;
    }
    return SIML_EVENT_NONE;
}

/* Parser public functions ----------------------------------------------- */

void siml_parser_init(siml_parser *p,
                      siml_read_line_fn read_line,
                      void *userdata) {
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->read_line = read_line;
    p->userdata  = userdata;
}

void siml_parser_reset(siml_parser *p) {
    siml_read_line_fn rl;
    void             *ud;
    if (!p) return;
    rl = p->read_line;
    ud = p->userdata;
    memset(p, 0, sizeof(*p));
    p->read_line = rl;
    p->userdata  = ud;
}

/* Forward declarations of internal state handlers */
static siml_event_type siml_next_normal(siml_parser *p, siml_event *ev);
static siml_event_type siml_next_flow(siml_parser *p, siml_event *ev);
static siml_event_type siml_next_block(siml_parser *p, siml_event *ev);

siml_event_type siml_next(siml_parser *p, siml_event *ev) {
    siml_event_type t;

    if (!p || !ev) return SIML_EVENT_ERROR;

    siml_clear_event(ev);

    if (p->error_code != SIML_ERR_NONE) {
        ev->type          = SIML_EVENT_ERROR;
        ev->error_code    = p->error_code;
        ev->error_message = p->error_message;
        ev->line          = p->error_line;
        return ev->type;
    }

    if (p->doc_state == SIML_DOC_IDLE) {
        p->doc_state = SIML_DOC_BEFORE;
        ev->type     = SIML_EVENT_STREAM_START;
        ev->line   = 0;
        return ev->type;
    }

    if (p->mode == SIML_MODE_FLOW) {
        t = siml_next_flow(p, ev);
        if (t == SIML_EVENT_ERROR && p->error_code != SIML_ERR_NONE) {
            ev->error_code    = p->error_code;
            ev->error_message = p->error_message;
            if (ev->line == 0) ev->line = p->error_line;
        }
        return t;
    }

    if (p->mode == SIML_MODE_BLOCK) {
        t = siml_next_block(p, ev);
        if (t == SIML_EVENT_ERROR && p->error_code != SIML_ERR_NONE) {
            ev->error_code    = p->error_code;
            ev->error_message = p->error_message;
            if (ev->line == 0) ev->line = p->error_line;
        }
        return t;
    }

    t = siml_emit_pending_end(p, ev);
    if (t != SIML_EVENT_NONE) return t;

    t = siml_emit_pending_start(p, ev);
    if (t != SIML_EVENT_NONE) return t;

    t = siml_next_normal(p, ev);
    if (t == SIML_EVENT_ERROR && p->error_code != SIML_ERR_NONE) {
        ev->error_code    = p->error_code;
        ev->error_message = p->error_message;
        if (ev->line == 0) ev->line = p->error_line;
    }
    return t;
}

static int siml_parse_mapping_entry(siml_parser *p,
                                    const char *s,
                                    size_t len,
                                    size_t indent,
                                    size_t *out_key_len,
                                    size_t *out_value_start,
                                    size_t *out_value_len,
                                    unsigned int *out_spaces,
                                    const char **out_comment,
                                    size_t *out_comment_len,
                                    int *out_has_inline_value) {
    size_t i;
    int has_colon = 0;

    if (indent >= len) {
        siml_set_error(p, SIML_ERR_UNKNOWN_LINE_FORM, "unknown line form");
        return 0;
    }
    for (i = indent; i < len; ++i) {
        if (s[i] == ':') {
            has_colon = 1;
            break;
        }
    }
    if (!has_colon) {
        siml_set_error(p, SIML_ERR_UNKNOWN_LINE_FORM, "unknown line form");
        return 0;
    }
    if (!siml_is_alpha(s[indent]) && s[indent] != '_') {
        siml_set_error(p, SIML_ERR_KEY_ILLEGAL,
                       "illegal mapping key, must match: [a-zA-Z_][a-zA-Z0-9_./-]*");
        return 0;
    }
    i = indent + 1;
    while (i < len && siml_is_key_char(s[i])) {
        ++i;
    }
    if (i >= len || s[i] != ':') {
        siml_set_error(p, SIML_ERR_KEY_ILLEGAL,
                       "illegal mapping key, must match: [a-zA-Z_][a-zA-Z0-9_./-]*");
        return 0;
    }
    *out_key_len = i - indent;
    if (*out_key_len > SIML_MAX_KEY_LEN) {
        siml_set_error(p, SIML_ERR_KEY_TOO_LONG,
                       "mapping key too long (max 128 bytes)");
        return 0;
    }

    if (i + 1 == len) {
        *out_has_inline_value = 0;
        return 1;
    }
    if (s[i + 1] != ' ' || (i + 2 < len && s[i + 2] == ' ')) {
        siml_set_error(p, SIML_ERR_EXPECT_SPACE_AFTER_COLON,
                       "expected single space after ':'");
        return 0;
    }
    if (i + 2 >= len) {
        siml_set_error(p, SIML_ERR_INLINE_VALUE_EMPTY, "inline value is empty");
        return 0;
    }

    *out_value_start = i + 2;
    if (!siml_parse_inline_comment(p, s, *out_value_start, len,
                                   out_value_len, out_spaces,
                                   out_comment, out_comment_len)) {
        return 0;
    }
    *out_has_inline_value = 1;
    return 1;
}

static int siml_parse_sequence_item(siml_parser *p,
                                    const char *s,
                                    size_t len,
                                    size_t indent,
                                    size_t *out_value_start,
                                    size_t *out_value_len,
                                    unsigned int *out_spaces,
                                    const char **out_comment,
                                    size_t *out_comment_len,
                                    int *out_has_inline_value) {
    size_t dash = indent;

    if (dash >= len || s[dash] != '-') {
        siml_set_error(p, SIML_ERR_EXPECT_SPACE_AFTER_DASH,
                       "expected single space after '-'");
        return 0;
    }
    if (dash + 1 == len) {
        siml_set_error(p, SIML_ERR_SEQ_ITEM_REQUIRES_INLINE,
                       "sequence item must start after '- '");
        return 0;
    }
    if (s[dash + 1] != ' ' || (dash + 2 < len && s[dash + 2] == ' ')) {
        siml_set_error(p, SIML_ERR_EXPECT_SPACE_AFTER_DASH,
                       "expected single space after '-'");
        return 0;
    }
    if (dash + 2 >= len) {
        siml_set_error(p, SIML_ERR_INLINE_VALUE_EMPTY, "inline value is empty");
        return 0;
    }

    *out_value_start = dash + 2;
    if (!siml_parse_inline_comment(p, s, *out_value_start, len,
                                   out_value_len, out_spaces,
                                   out_comment, out_comment_len)) {
        return 0;
    }
    *out_has_inline_value = 1;
    return 1;
}

static int siml_starts_mapping_entry(const char *s,
                                     size_t start,
                                     size_t value_len) {
    size_t i;
    size_t end = start + value_len;

    if (start >= end || (!siml_is_alpha(s[start]) && s[start] != '_')) {
        return 0;
    }
    i = start + 1;
    while (i < end && siml_is_key_char(s[i])) {
        ++i;
    }
    return i < end && s[i] == ':';
}

static int siml_starts_sequence_item(const char *s,
                                     size_t start,
                                     size_t value_len) {
    if (value_len == 0 || s[start] != '-') {
        return 0;
    }
    return value_len == 1 || s[start + 1] == ' ';
}


static siml_event_type siml_start_block(siml_parser *p, siml_event *ev,
                                        const char *key, size_t key_len,
                                        size_t indent,
                                        unsigned int ic_spaces,
                                        const char *ic_ptr, size_t ic_len) {
    p->mode = SIML_MODE_BLOCK;
    p->block_indent = indent;
    if (key_len > 0) {
        memcpy(p->mode_key, key, key_len);
    }
    p->mode_key[key_len] = '\0';
    p->mode_key_len = key_len;
    p->block_inline_spaces = ic_spaces;
    p->block_inline_comment = ic_ptr;
    p->block_inline_comment_len = ic_len;
    p->block_start_line = p->line_no;
    p->block_seen_content = 0;
    p->block_blank_count = 0;
    p->block_blank_start_line = 0;
    p->block_emit_blanks = 0;
    p->have_line = 0;

    ev->type = SIML_EVENT_BLOCK_SCALAR_START;
    if (key_len > 0) {
        ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
    }
    ev->inline_comment_spaces = p->block_inline_spaces;
    ev->inline_comment = siml_make_slice(p->block_inline_comment,
                                         p->block_inline_comment_len);
    ev->line = p->line_no;
    return ev->type;
}

static siml_event_type siml_start_flow(siml_parser *p, siml_event *ev,
                                       const char *key, size_t key_len,
                                       size_t value_start, size_t value_len,
                                       unsigned int ic_spaces,
                                       const char *ic_ptr, size_t ic_len) {
    /* value_len >= 2 guaranteed by siml_parse_inline_comment (matched brackets) */
    p->flow_stack[0].start   = value_start;
    p->flow_stack[0].end     = value_start + value_len - 1;
    p->flow_stack[0].pos     = value_start + 1;
    p->flow_stack[0].started = 0;
    p->flow_depth = 1;
    p->mode = SIML_MODE_FLOW;
    if (key_len > 0) {
        memcpy(p->mode_key, key, key_len);
    }
    p->mode_key[key_len] = '\0';
    p->mode_key_len = key_len;
    p->flow_inline_spaces = ic_spaces;
    p->flow_inline_comment = ic_ptr;
    p->flow_inline_comment_len = ic_len;
    return siml_next_flow(p, ev);
}

static siml_event_type siml_handle_inline_value(siml_parser *p, siml_event *ev,
                                                const char *key, size_t key_len,
                                                size_t indent,
                                                size_t value_start, size_t value_len,
                                                unsigned int ic_spaces,
                                                const char *ic_ptr, size_t ic_len) {
    if (value_len > SIML_MAX_INLINE_VALUE_LEN) {
        siml_set_error(p, SIML_ERR_INLINE_VALUE_TOO_LONG,
                       "inline value too long (max 2048 bytes)");
        return SIML_EVENT_ERROR;
    }
    if (value_len == 0) {
        siml_set_error(p, SIML_ERR_INLINE_VALUE_EMPTY,
                       "inline value is empty");
        return SIML_EVENT_ERROR;
    }

    if (p->line[value_start] == '|') {
        if (value_len != 1) {
            siml_set_error(p, SIML_ERR_SCALAR_STARTS_WITH_PIPE,
                           "scalar must not start with '|'");
            return SIML_EVENT_ERROR;
        }
        return siml_start_block(p, ev, key, key_len, indent,
                                ic_spaces, ic_ptr, ic_len);
    }

    if (p->line[value_start] == '[') {
        return siml_start_flow(p, ev, key, key_len, value_start, value_len,
                               ic_spaces, ic_ptr, ic_len);
    }

    if (p->line[value_start] == '#') {
        siml_set_error(p, SIML_ERR_SCALAR_STARTS_WITH_HASH,
                       "scalar must not start with '#'");
        return SIML_EVENT_ERROR;
    }

    ev->type = SIML_EVENT_SCALAR;
    if (key_len > 0) {
        ev->key = siml_make_slice(key, key_len);
    }
    ev->value = siml_make_slice(p->line + value_start, value_len);
    ev->inline_comment_spaces = ic_spaces;
    ev->inline_comment = siml_make_slice(ic_ptr, ic_len);
    ev->line = p->line_no;
    p->have_line = 0;
    return ev->type;
}

static siml_event_type siml_next_flow(siml_parser *p, siml_event *ev) {
    const char *s = p->line;

    for (;;) {
        int depth = p->flow_depth - 1;
        size_t end;
        size_t pos;

        if (depth < 0) {
            siml_set_error(p, SIML_ERR_FLOW_EXCESS_TERM,
                           "excess non-comment characters after flow sequence termination");
            return SIML_EVENT_ERROR;
        }

        end = p->flow_stack[depth].end;
        pos = p->flow_stack[depth].pos;

        if (!p->flow_stack[depth].started) {
            p->flow_stack[depth].started = 1;
            ev->type = SIML_EVENT_SEQUENCE_START;
            ev->seq_style = SIML_SEQ_STYLE_FLOW;
            if (depth == 0) {
                ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
                ev->inline_comment_spaces = p->flow_inline_spaces;
                ev->inline_comment = siml_make_slice(p->flow_inline_comment,
                                                     p->flow_inline_comment_len);
            }
            ev->line = p->line_no;
            return ev->type;
        }

        if (pos >= end) {
            ev->type = SIML_EVENT_SEQUENCE_END;
            ev->line = p->line_no;
            if (depth == 0) {
                p->mode = SIML_MODE_NORMAL;
                p->have_line = 0;
                p->flow_depth = 0;
            } else {
                p->flow_depth -= 1;
            }
            return ev->type;
        }

        if (s[pos] == ',') {
            siml_set_error(p, SIML_ERR_FLOW_EMPTY_ELEM,
                           "empty flow sequence element");
            return SIML_EVENT_ERROR;
        }

        if (s[pos] == ' ' || s[pos] == '\t') {
            siml_set_error(p, SIML_ERR_FLOW_WHITESPACE,
                           "flow sequence contains whitespace (forbidden)");
            return SIML_EVENT_ERROR;
        }

        if (s[pos] == '[') {
            size_t i = pos;
            int nest = 0;
            size_t match = (size_t)(-1);
            while (i <= end) {
                if (s[i] == '[') nest += 1;
                else if (s[i] == ']') {
                    nest -= 1;
                    if (nest == 0) {
                        match = i;
                        break;
                    }
                }
                ++i;
            }
            if (match == (size_t)(-1) || match > end) {
                siml_set_error(p, SIML_ERR_FLOW_UNTERMINATED,
                               "unterminated flow sequence");
                return SIML_EVENT_ERROR;
            }

            p->flow_stack[depth].pos = match + 1;
            if (p->flow_stack[depth].pos < end) {
                if (s[p->flow_stack[depth].pos] != ',') {
                    siml_set_error(p, SIML_ERR_FLOW_EXCESS_TERM,
                                   "excess non-comment characters after flow sequence termination");
                    return SIML_EVENT_ERROR;
                }
                p->flow_stack[depth].pos += 1;
                if (p->flow_stack[depth].pos == end) {
                    siml_set_error(p, SIML_ERR_FLOW_TRAILING_COMMA,
                                   "trailing comma in flow sequence is forbidden");
                    return SIML_EVENT_ERROR;
                }
            }

            if (p->flow_depth >= SIML_MAX_NESTING) {
                siml_set_error_one(p, SIML_ERR_INDENT_WRONG,
                                   "wrong indentation, expected: ",
                                   0,
                                   "");
                return SIML_EVENT_ERROR;
            }
            p->flow_stack[p->flow_depth].start   = pos;
            p->flow_stack[p->flow_depth].end     = match;
            p->flow_stack[p->flow_depth].pos     = pos + 1;
            p->flow_stack[p->flow_depth].started = 0;
            p->flow_depth += 1;
            continue;
        }

        if (s[pos] == '#') {
            siml_set_error(p, SIML_ERR_FLOW_SCALAR_STARTS_WITH_HASH,
                           "flow-scalar must not start with '#'");
            return SIML_EVENT_ERROR;
        }

        {
            size_t i = pos;
            size_t item_len;
            while (i < end && s[i] != ',') {
                if (s[i] == '[' || s[i] == ']') break;
                ++i;
            }
            if (i <= pos) {
                siml_set_error(p, SIML_ERR_FLOW_EMPTY_ELEM,
                               "empty flow sequence element");
                return SIML_EVENT_ERROR;
            }
            if (i < end && (s[i] == '[' || s[i] == ']')) {
                siml_set_error(p, SIML_ERR_FLOW_EXCESS_TERM,
                               "excess non-comment characters after flow sequence termination");
                return SIML_EVENT_ERROR;
            }
            item_len = i - pos;
            if (item_len > SIML_MAX_FLOW_ELEMENT_LEN) {
                siml_set_error(p, SIML_ERR_FLOW_ATOM_TOO_LONG,
                               "flow-scalar too long (max 128 bytes)");
                return SIML_EVENT_ERROR;
            }
            if (s[pos] == '|') {
                siml_set_error(p, SIML_ERR_FLOW_SCALAR_STARTS_WITH_PIPE,
                               "flow-scalar must not start with '|'");
                return SIML_EVENT_ERROR;
            }

            ev->type = SIML_EVENT_SCALAR;
            ev->value = siml_make_slice(s + pos, item_len);
            ev->line = p->line_no;

            p->flow_stack[depth].pos = pos + item_len;
            if (p->flow_stack[depth].pos < end) {
                if (s[p->flow_stack[depth].pos] != ',') {
                    siml_set_error(p, SIML_ERR_FLOW_EXCESS_TERM,
                                   "excess non-comment characters after flow sequence termination");
                    return SIML_EVENT_ERROR;
                }
                p->flow_stack[depth].pos += 1;
                if (p->flow_stack[depth].pos == end) {
                    siml_set_error(p, SIML_ERR_FLOW_TRAILING_COMMA,
                                   "trailing comma in flow sequence is forbidden");
                    return SIML_EVENT_ERROR;
                }
            }
            return ev->type;
        }
    }
}

static siml_event_type siml_next_block(siml_parser *p, siml_event *ev) {
    int rc;

    for (;;) {
        if (p->block_emit_blanks && p->block_blank_count > 0) {
            ev->type = SIML_EVENT_BLOCK_SCALAR_LINE;
            ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
            ev->value = siml_make_slice("", 0);
            ev->line = p->block_blank_start_line;
            p->block_blank_start_line += 1;
            p->block_blank_count -= 1;
            if (p->block_blank_count == 0) {
                p->block_emit_blanks = 0;
            }
            return ev->type;
        }

        if (!p->have_line) {
            rc = siml_fetch_line(p);
            if (rc < 0) return SIML_EVENT_ERROR;
            if (rc == 0) {
                if (!p->block_seen_content) {
                    siml_set_error(p, SIML_ERR_BLOCK_EMPTY,
                                   "block literal must not be empty");
                    return SIML_EVENT_ERROR;
                }
                if (p->block_blank_count > 0) {
                    siml_set_error(p, SIML_ERR_BLOCK_TRAILING_BLANK,
                                   "block literal has trailing blank line (forbidden)");
                    return SIML_EVENT_ERROR;
                }
                p->mode = SIML_MODE_NORMAL;
                ev->type = SIML_EVENT_BLOCK_SCALAR_END;
                ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
                ev->line = p->block_start_line;
                return ev->type;
            }
        }

        if (!siml_check_line_common(p)) return SIML_EVENT_ERROR;

        if (p->line_len == 0) {
            if (!p->block_seen_content) {
                siml_set_error(p, SIML_ERR_BLOCK_LEADING_BLANK,
                               "block literal has leading blank line (forbidden)");
                return SIML_EVENT_ERROR;
            }
            if (p->block_blank_count == 0) {
                p->block_blank_start_line = p->line_no;
            }
            p->block_blank_count += 1;
            p->have_line = 0;
            continue;
        }

        if (siml_is_space_or_tab_only(p->line, p->line_len)) {
            siml_set_error(p, SIML_ERR_BLOCK_WHITESPACE_ONLY,
                           "whitespace-only lines are forbidden in block literal content");
            return SIML_EVENT_ERROR;
        }

        if (p->line[p->line_len - 1] == ' ') {
            siml_set_error(p, SIML_ERR_TRAILING_SPACES,
                           "trailing spaces are not allowed here");
            return SIML_EVENT_ERROR;
        }

        {
            size_t indent = 0;
            size_t i;
            const char *s = p->line;
            size_t len = p->line_len;

            for (i = 0; i < len && s[i] == ' '; ++i) {
                indent += 1;
            }

            if (indent < p->block_indent + 2) {
                if (!p->block_seen_content) {
                    siml_set_error(p, SIML_ERR_BLOCK_EMPTY,
                                   "block literal must not be empty");
                    return SIML_EVENT_ERROR;
                }
                if (p->block_blank_count > 0) {
                    siml_set_error(p, SIML_ERR_BLOCK_TRAILING_BLANK,
                                   "block literal has trailing blank line (forbidden)");
                    return SIML_EVENT_ERROR;
                }
                p->mode = SIML_MODE_NORMAL;
                ev->type = SIML_EVENT_BLOCK_SCALAR_END;
                ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
                ev->line = p->block_start_line;
                return ev->type;
            }

            if (len - (p->block_indent + 2) > SIML_MAX_BLOCK_LINE_LEN) {
                siml_set_error(p, SIML_ERR_BLOCK_LINE_TOO_LONG,
                               "block literal content line too long (max 4096 bytes)");
                return SIML_EVENT_ERROR;
            }

            if (p->block_blank_count > 0) {
                p->block_emit_blanks = 1;
                continue;
            }

            ev->type = SIML_EVENT_BLOCK_SCALAR_LINE;
            ev->key = siml_make_slice(p->mode_key, p->mode_key_len);
            ev->value = siml_make_slice(s + p->block_indent + 2,
                                        len - (p->block_indent + 2));
            ev->line = p->line_no;
            p->block_seen_content = 1;
            p->have_line = 0;
            return ev->type;
        }
    }
}

/* Returns the container depth to close to before emitting the comment
 * (0 = none), or -1 if the indent is not valid at the current state.
 */
static int siml_comment_check_indent(siml_parser *p, size_t indent) {
    int i;
    if (p->pending_map)
        return (indent == p->pending_indent) ? 0 : -1;
    if (p->depth == 0)
        return (indent == 0) ? 0 : -1;
    if (p->stack[p->depth - 1].indent == indent)
        return 0;
    for (i = p->depth - 2; i >= 0; --i) {
        if (p->stack[i].indent == indent) return i + 1;
    }
    return -1;
}

static siml_event_type siml_next_normal(siml_parser *p, siml_event *ev) {
    int rc;

    for (;;) {
        if (!p->have_line) {
            rc = siml_fetch_line(p);
            if (rc < 0) return SIML_EVENT_ERROR;
            if (rc == 0) {
                if (p->pending_map) {
                    siml_set_error(p, SIML_ERR_HEADER_MAP_NO_NESTED,
                                   "header-only mapping entry must have a nested node");
                    return SIML_EVENT_ERROR;
                }
                if (p->doc_state == SIML_DOC_BETWEEN) {
                    siml_set_error(p, SIML_ERR_SEPARATOR_AFTER_DOC,
                                   "document separator must not appear after the last document");
                    return SIML_EVENT_ERROR;
                }
                if (p->doc_state != SIML_DOC_IN) {
                    p->pending_stream_end = 1;
                    return siml_emit_pending_end(p, ev);
                }
                p->pending_close = 1;
                p->target_depth = 0;
                p->pending_doc_end = 1;
                p->pending_stream_end = 1;
                return siml_emit_pending_end(p, ev);
            }
        }

        if (!siml_check_line_nonblock(p)) return SIML_EVENT_ERROR;

        {
            const char *s = p->line;
            size_t trimmed_len = p->line_len;
            int has_trailing_spaces = 0;
            size_t indent = 0;
            int comment_rc;
            while (trimmed_len > 0 && s[trimmed_len - 1] == ' ') {
                trimmed_len -= 1;
            }
            has_trailing_spaces = (trimmed_len != p->line_len);

            {
                size_t shebang_pos = 0;
                while (shebang_pos < trimmed_len &&
                       s[shebang_pos] == ' ') {
                    shebang_pos += 1;
                }
                if (shebang_pos + 1 < trimmed_len &&
                    s[shebang_pos] == '#' && s[shebang_pos + 1] == '!') {
                    if (p->line_no != 1 || shebang_pos != 0) {
                        siml_set_error(
                            p, SIML_ERR_SHEBANG_POSITION,
                            "shebang is only allowed on the first physical "
                            "line at byte offset 0");
                        return SIML_EVENT_ERROR;
                    }
                    if (has_trailing_spaces) {
                        siml_set_error(p, SIML_ERR_TRAILING_SPACES,
                                       "trailing spaces are not allowed here");
                        return SIML_EVENT_ERROR;
                    }
                    if (trimmed_len == 2) {
                        siml_set_error(p, SIML_ERR_SHEBANG_EMPTY,
                                       "shebang must not be empty");
                        return SIML_EVENT_ERROR;
                    }
                    if (trimmed_len - 2 > SIML_MAX_SHEBANG_TEXT_LEN) {
                        siml_set_error(p, SIML_ERR_SHEBANG_TOO_LONG,
                                       "shebang text too long (max 512 bytes)");
                        return SIML_EVENT_ERROR;
                    }
                    ev->type = SIML_EVENT_SHEBANG;
                    ev->value = siml_make_slice(s + 2, trimmed_len - 2);
                    ev->line = p->line_no;
                    p->have_line = 0;
                    return ev->type;
                }
            }

            comment_rc = siml_parse_comment_line(p, s, trimmed_len, &indent);
            if (comment_rc < 0) return SIML_EVENT_ERROR;
            if (comment_rc > 0) {
                if (has_trailing_spaces) {
                    siml_set_error(p, SIML_ERR_TRAILING_SPACES,
                                   "trailing spaces are not allowed here");
                    return SIML_EVENT_ERROR;
                }
                {
                    int ctarget = siml_comment_check_indent(p, indent);
                    if (ctarget < 0) {
                        siml_set_error(p, SIML_ERR_COMMENT_INDENT,
                                       "comment indentation must match current nesting level");
                        return SIML_EVENT_ERROR;
                    }
                    if (ctarget > 0) {
                        p->pending_close = 1;
                        p->target_depth = ctarget;
                        return siml_emit_pending_end(p, ev);
                    }
                }
                ev->type = SIML_EVENT_COMMENT;
                ev->value = siml_make_slice(p->line, trimmed_len);
                ev->line = p->line_no;
                p->have_line = 0;
                return ev->type;
            }

            {
            size_t len = trimmed_len;
            size_t key_len = 0;
            size_t value_start = 0;
            size_t value_len = 0;
            unsigned int ic_spaces = 0;
            const char *ic_ptr = 0;
            size_t ic_len = 0;
            int has_inline_value = 0;
            int is_mapping = 0;
            int is_sequence = 0;
            siml_container *cur = 0;

            if (p->inline_sequence_item) {
                indent = p->inline_sequence_indent;
                p->inline_sequence_item = 0;
            } else {
                if (!siml_count_indent(p, s, len, &indent)) {
                    return SIML_EVENT_ERROR;
                }
            }

            if (indent == 0 && len >= 3 && s[0] == '-' && s[1] == '-' && s[2] == '-') {
                if (has_trailing_spaces && len == 3) {
                    siml_set_error(p, SIML_ERR_SEPARATOR_FORMAT,
                                   "document separator must be exactly ---");
                    return SIML_EVENT_ERROR;
                }
                if (len == 3) {
                    if (p->pending_map) {
                        siml_set_error(p, SIML_ERR_HEADER_MAP_NO_NESTED,
                                       "header-only mapping entry must have a nested node");
                        return SIML_EVENT_ERROR;
                    }
                    if (p->doc_state != SIML_DOC_IN) {
                        siml_set_error(p, SIML_ERR_SEPARATOR_BEFORE_DOC,
                                       "document separator must not appear before the first document");
                        return SIML_EVENT_ERROR;
                    }
                    p->pending_close = 1;
                    p->target_depth = 0;
                    p->pending_doc_end = 1;
                    p->have_line = 0;
                    return siml_emit_pending_end(p, ev);
                }
                {
                    size_t i;
                    int found_hash = 0;
                    for (i = 3; i < len; ++i) {
                        if (s[i] == '#' && s[i - 1] == ' ') {
                            found_hash = 1;
                            break;
                        }
                    }
                    if (found_hash) {
                        if (i + 1 >= len) {
                            siml_set_error(p, SIML_ERR_EMPTY_COMMENT,
                                           "empty comment is forbidden");
                            return SIML_EVENT_ERROR;
                        }
                        if (s[i + 1] != ' ') {
                            siml_set_error(p, SIML_ERR_INLINE_COMMENT_SPACE,
                                           "inline comment must have exactly 1 space after '#'");
                            return SIML_EVENT_ERROR;
                        }
                        if (i + 2 >= len) {
                            siml_set_error(p, SIML_ERR_EMPTY_COMMENT,
                                           "empty comment is forbidden");
                            return SIML_EVENT_ERROR;
                        }
                        siml_set_error(p, SIML_ERR_SEPARATOR_INLINE_COMMENT,
                                       "document separator must not have inline comments");
                        return SIML_EVENT_ERROR;
                    }
                }
                siml_set_error(p, SIML_ERR_SEPARATOR_FORMAT,
                               "document separator must be exactly ---");
                return SIML_EVENT_ERROR;
            }
            if (indent > 0 && len - indent == 3 &&
                s[indent] == '-' && s[indent + 1] == '-' && s[indent + 2] == '-') {
                siml_set_error(p, SIML_ERR_SEPARATOR_INDENT,
                               "document separator must be at indent 0");
                return SIML_EVENT_ERROR;
            }

            if (indent >= len) {
                siml_set_error(p, SIML_ERR_DOC_SCALAR,
                               "document root must not be a scalar");
                return SIML_EVENT_ERROR;
            }

            if (s[indent] == '-') {
                is_sequence = 1;
            } else {
                is_mapping = 1;
            }

            if (p->pending_map) {
                if (indent != p->pending_indent) {
                    siml_set_error_two(p, SIML_ERR_INDENT_NEST_MISMATCH,
                                       "nested node indentation mismatch, expected ",
                                       (unsigned long)p->pending_indent,
                                       " got ",
                                       (unsigned long)indent);
                    return SIML_EVENT_ERROR;
                }
                if (p->doc_state != SIML_DOC_IN) {
                    siml_set_error(p, SIML_ERR_DOC_SCALAR,
                                   "document root must not be a scalar");
                    return SIML_EVENT_ERROR;
                }
                if (is_mapping) {
                    if (!siml_request_container_start(p, SIML_CONTAINER_MAP,
                                                      SIML_SEQ_STYLE_BLOCK, indent,
                                                      p->pending_key,
                                                      p->pending_key_len)) {
                        return SIML_EVENT_ERROR;
                    }
                } else {
                    if (!siml_request_container_start(p, SIML_CONTAINER_SEQ,
                                                      SIML_SEQ_STYLE_BLOCK, indent,
                                                      p->pending_key,
                                                      p->pending_key_len)) {
                        return SIML_EVENT_ERROR;
                    }
                }
                p->pending_map = 0;
                p->pending_key_len = 0;
                return siml_emit_pending_start(p, ev);
            }

            if (p->doc_state != SIML_DOC_IN) {
                if (indent != 0) {
                    siml_set_error(p, SIML_ERR_DOC_INDENT,
                                   "document must start at indent 0");
                    return SIML_EVENT_ERROR;
                }
                p->doc_state = SIML_DOC_IN;
                p->pending_doc_start = 1;
                if (is_mapping) {
                    if (!siml_request_container_start(p, SIML_CONTAINER_MAP,
                                                      SIML_SEQ_STYLE_BLOCK, indent,
                                                      "", 0)) {
                        return SIML_EVENT_ERROR;
                    }
                } else {
                    if (!siml_request_container_start(p, SIML_CONTAINER_SEQ,
                                                      SIML_SEQ_STYLE_BLOCK, indent,
                                                      "", 0)) {
                        return SIML_EVENT_ERROR;
                    }
                }
                return siml_emit_pending_start(p, ev);
            }

            if (p->depth <= 0) {
                siml_set_error(p, SIML_ERR_DOC_SCALAR,
                               "document root must not be a scalar");
                return SIML_EVENT_ERROR;
            }
            cur = &p->stack[p->depth - 1];

            if (indent > cur->indent) {
                siml_set_error_one(p, SIML_ERR_INDENT_WRONG,
                                   "wrong indentation, expected: ",
                                   (unsigned long)cur->indent,
                                   "");
                return SIML_EVENT_ERROR;
            }

            if (indent < cur->indent) {
                int target;
                int ti;
                for (ti = p->depth - 1; ti >= 0; --ti) {
                    if (p->stack[ti].indent == indent) break;
                }
                target = (ti >= 0) ? ti + 1 : -1;
                if (target < 0) {
                    siml_set_error_one(p, SIML_ERR_INDENT_WRONG,
                                       "wrong indentation, expected: ",
                                       (unsigned long)cur->indent,
                                       "");
                    return SIML_EVENT_ERROR;
                }
                p->pending_close = 1;
                p->target_depth = target;
                return siml_emit_pending_end(p, ev);
            }

            if ((cur->type == SIML_CONTAINER_MAP && !is_mapping) ||
                (cur->type == SIML_CONTAINER_SEQ && !is_sequence)) {
                siml_set_error_one(p, SIML_ERR_NODE_KIND_MIX,
                                   "node kind mixing at indent ",
                                   (unsigned long)indent,
                                   " is forbidden");
                return SIML_EVENT_ERROR;
            }

            if (is_mapping) {
                if (!siml_parse_mapping_entry(p, s, len, indent,
                                              &key_len, &value_start, &value_len,
                                              &ic_spaces, &ic_ptr, &ic_len,
                                              &has_inline_value)) {
                    return SIML_EVENT_ERROR;
                }
                if (has_trailing_spaces) {
                    siml_set_error(p, SIML_ERR_TRAILING_SPACES,
                                   "trailing spaces are not allowed here");
                    return SIML_EVENT_ERROR;
                }
                cur->item_count += 1;

                if (!has_inline_value) {
                    p->pending_map = 1;
                    p->pending_indent = indent + 2;
                    p->pending_key_len = 0;
                    p->have_line = 0;
                    ev->type = SIML_EVENT_MAPPING_ENTRY_HEADER;
                    ev->key  = siml_make_slice(s + indent, key_len);
                    ev->line = p->line_no;
                    return ev->type;
                }

                return siml_handle_inline_value(
                    p, ev,
                    s + indent, key_len, indent,
                    value_start, value_len,
                    ic_spaces, ic_ptr, ic_len);
            }

            if (!siml_parse_sequence_item(p, s, len, indent,
                                          &value_start, &value_len,
                                          &ic_spaces, &ic_ptr, &ic_len,
                                          &has_inline_value)) {
                return SIML_EVENT_ERROR;
            }
            if (has_trailing_spaces) {
                siml_set_error(p, SIML_ERR_TRAILING_SPACES,
                               "trailing spaces are not allowed here");
                return SIML_EVENT_ERROR;
            }
            cur->item_count += 1;

            if (has_inline_value &&
                siml_starts_mapping_entry(s, value_start, value_len)) {
                if (!siml_request_container_start(p, SIML_CONTAINER_MAP,
                                                  SIML_SEQ_STYLE_BLOCK,
                                                  indent + 2, "", 0)) {
                    return SIML_EVENT_ERROR;
                }
                p->inline_sequence_item = 1;
                p->inline_sequence_indent = indent + 2;
                return siml_emit_pending_start(p, ev);
            }

            if (siml_starts_sequence_item(s, value_start, value_len)) {
                if (!siml_request_container_start(p, SIML_CONTAINER_SEQ,
                                                  SIML_SEQ_STYLE_BLOCK,
                                                  indent + 2, "", 0)) {
                    return SIML_EVENT_ERROR;
                }
                p->inline_sequence_item = 1;
                p->inline_sequence_indent = indent + 2;
                return siml_emit_pending_start(p, ev);
            }

            return siml_handle_inline_value(
                p, ev,
                0, 0, indent,
                value_start, value_len,
                ic_spaces, ic_ptr, ic_len);
            }
        }
    }
}

#endif /* SIML_H_INCLUDED */
