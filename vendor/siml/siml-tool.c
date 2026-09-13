#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "siml.h"

/* -------------------------------------------------------------------------
 * Streaming file reader — used by verify and dump.
 * Reads one line at a time from an open FILE*, growing its buffer as needed.
 * SIML_TEST_READ_ERROR_AFTER (env) makes it fail after N successful reads,
 * which the test suite uses to exercise I/O-error handling.
 * ------------------------------------------------------------------------- */

struct file_reader {
    FILE   *fp;
    char   *buf;
    size_t  cap;
    size_t  lines_read;
    long    fail_after; /* -1 = never fail */
};

static int file_read_line(void *userdata,
                          const char **out_line,
                          size_t *out_len) {
    struct file_reader *r;
    size_t len;
    int ch;
    size_t new_cap;
    char *new_buf;

    r = (struct file_reader *)userdata;
    if (!r || !out_line || !out_len) return -1;
    if (!r->fp) return -1;
    if (r->fail_after >= 0 && r->lines_read >= (size_t)r->fail_after) return -1;

    if (r->cap == 0) {
        r->cap = 256;
        r->buf = (char *)malloc(r->cap);
        if (!r->buf) return -1;
    }

    len = 0;
    while ((ch = fgetc(r->fp)) != EOF) {
        if (len + 1 >= r->cap) {
            new_cap = r->cap * 2;
            new_buf = (char *)realloc(r->buf, new_cap);
            if (!new_buf) return -1;
            r->buf = new_buf;
            r->cap = new_cap;
        }
        r->buf[len++] = (char)ch;
        if (ch == '\n') break;
    }

    if (ferror(r->fp)) return -1;
    if (len == 0) return 0;

    r->buf[len] = '\0';
    *out_line = r->buf;
    *out_len  = len;
    r->lines_read += 1;
    return 1;
}

static void file_reader_init(struct file_reader *r, FILE *fp) {
    r->fp         = fp;
    r->buf        = NULL;
    r->cap        = 0;
    r->lines_read = 0;
    r->fail_after = -1;

    /* Honour the test-harness env variable when set. */
    {
        const char *env = getenv("SIML_TEST_READ_ERROR_AFTER");
        if (env && env[0] != '\0') {
            r->fail_after = strtol(env, NULL, 10);
            if (r->fail_after < 0) r->fail_after = -1;
        }
    }
}

/* -------------------------------------------------------------------------
 * In-memory reader — used by roundtrip.
 * Slices the already-loaded file buffer without copying.
 * ------------------------------------------------------------------------- */

struct mem_reader {
    const char *data;
    size_t      len;
    size_t      pos;
};

static int mem_read_line(void *userdata,
                         const char **out_line,
                         size_t *out_len) {
    struct mem_reader *r;
    size_t start;
    size_t i;

    r = (struct mem_reader *)userdata;
    if (!r || !out_line || !out_len) return -1;
    if (r->pos >= r->len) return 0;

    start = r->pos;
    i     = start;
    while (i < r->len && r->data[i] != '\n') i += 1;
    if (i < r->len) i += 1; /* include the '\n' */

    *out_line = r->data + start;
    *out_len  = i - start;
    r->pos    = i;
    return 1;
}

/* -------------------------------------------------------------------------
 * Growable byte buffer — used by roundtrip to reconstruct the document.
 * ------------------------------------------------------------------------- */

struct buffer {
    char  *data;
    size_t len;
    size_t cap;
};

static int buf_reserve(struct buffer *b, size_t extra) {
    size_t needed  = b->len + extra;
    size_t new_cap;
    char  *new_data;

    if (needed <= b->cap) return 1;
    new_cap = b->cap ? b->cap : 256;
    while (new_cap < needed) new_cap *= 2;
    new_data = (char *)realloc(b->data, new_cap);
    if (!new_data) return 0;
    b->data = new_data;
    b->cap  = new_cap;
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
    for (i = 0; i < count; ++i) b->data[b->len++] = ' ';
    return 1;
}

/* -------------------------------------------------------------------------
 * Common parse-error reporting.
 * ------------------------------------------------------------------------- */

static void print_error(const siml_event *ev) {
    (void)fprintf(stderr, "SIML error at line %ld: %s\n",
                  ev->line,
                  ev->error_message ? ev->error_message : "parse error");
}

/* =========================================================================
 * verify — parse and validate; silent on success.
 * ========================================================================= */

static int cmd_verify(int argc, char **argv) {
    const char *filename;
    FILE *fp;
    struct file_reader reader;
    siml_parser parser;
    siml_event ev;
    siml_event_type t;
    int rc;

    if (argc != 3) {
        (void)fprintf(stderr, "Usage: %s verify <file.siml>\n", argv[0]);
        return 1;
    }
    filename = argv[2];

    if (strcmp(filename, "-") == 0) {
        fp = stdin;
    } else {
        fp = fopen(filename, "r");
        if (!fp) { perror(filename); return 1; }
    }

    file_reader_init(&reader, fp);
    siml_parser_init(&parser, file_read_line, &reader);

    rc = 0;
    for (;;) {
        t = siml_next(&parser, &ev);
        if (t == SIML_EVENT_ERROR) { print_error(&ev); rc = 1; break; }
        if (t == SIML_EVENT_STREAM_END) break;
    }

    free(reader.buf);
    if (fp != stdin) fclose(fp);
    return rc;
}

/* =========================================================================
 * dump — parse and print a human-readable event trace to stdout.
 * ========================================================================= */

static void print_slice(const siml_slice *s) {
    if (s && s->ptr && s->len > 0)
        (void)fwrite(s->ptr, 1, s->len, stdout);
}

static void print_inline_comment(const siml_event *ev) {
    if (ev->inline_comment.ptr && ev->inline_comment.len > 0) {
        (void)printf("  # (spaces=%u) ", ev->inline_comment_spaces);
        print_slice(&ev->inline_comment);
    }
}

static int cmd_dump(int argc, char **argv) {
    const char *filename;
    FILE *fp;
    struct file_reader reader;
    siml_parser parser;
    siml_event ev;
    int rc;

    if (argc != 3) {
        (void)fprintf(stderr, "Usage: %s dump <file.siml>\n", argv[0]);
        return 1;
    }
    filename = argv[2];

    if (strcmp(filename, "-") == 0) {
        fp = stdin;
        filename = "<stdin>";
    } else {
        fp = fopen(filename, "r");
        if (!fp) { perror(filename); return 1; }
    }

    file_reader_init(&reader, fp);
    siml_parser_init(&parser, file_read_line, &reader);

    rc = 0;
    for (;;) {
        siml_event_type t = siml_next(&parser, &ev);
        if (t == SIML_EVENT_ERROR) { print_error(&ev); rc = 1; break; }
        if (t == SIML_EVENT_STREAM_END) { (void)printf("STREAM_END\n"); break; }

        switch (t) {
        case SIML_EVENT_STREAM_START:
            (void)printf("STREAM_START\n");
            break;
        case SIML_EVENT_DOCUMENT_START:
            (void)printf("DOCUMENT_START\n");
            break;
        case SIML_EVENT_DOCUMENT_END:
            (void)printf("DOCUMENT_END\n");
            break;
        case SIML_EVENT_MAPPING_START:
            (void)printf("MAPPING_START");
            if (ev.key.len > 0) { (void)printf(" key="); print_slice(&ev.key); }
            (void)printf("\n");
            break;
        case SIML_EVENT_MAPPING_END:
            (void)printf("MAPPING_END\n");
            break;
        case SIML_EVENT_SEQUENCE_START:
            (void)printf("SEQUENCE_START");
            (void)printf(ev.seq_style == SIML_SEQ_STYLE_FLOW
                         ? " style=flow" : " style=block");
            if (ev.key.len > 0) { (void)printf(" key="); print_slice(&ev.key); }
            print_inline_comment(&ev);
            (void)printf("\n");
            break;
        case SIML_EVENT_SEQUENCE_END:
            (void)printf("SEQUENCE_END\n");
            break;
        case SIML_EVENT_SCALAR:
            (void)printf("SCALAR");
            if (ev.key.len > 0) { (void)printf(" key="); print_slice(&ev.key); }
            (void)printf(" value='");
            print_slice(&ev.value);
            (void)printf("'");
            print_inline_comment(&ev);
            (void)printf("\n");
            break;
        case SIML_EVENT_BLOCK_SCALAR_START:
            (void)printf("BLOCK_SCALAR_START");
            if (ev.key.len > 0) { (void)printf(" key="); print_slice(&ev.key); }
            print_inline_comment(&ev);
            (void)printf("\n");
            break;
        case SIML_EVENT_BLOCK_SCALAR_LINE:
            (void)printf("BLOCK_SCALAR_LINE '");
            print_slice(&ev.value);
            (void)printf("'\n");
            break;
        case SIML_EVENT_BLOCK_SCALAR_END:
            (void)printf("BLOCK_SCALAR_END\n");
            break;
        case SIML_EVENT_SHEBANG:
            (void)printf("SHEBANG ");
            print_slice(&ev.value);
            (void)printf("\n");
            break;
        case SIML_EVENT_COMMENT:
            (void)printf("COMMENT ");
            print_slice(&ev.value);
            (void)printf("\n");
            break;
        case SIML_EVENT_MAPPING_ENTRY_HEADER:
            (void)printf("MAPPING_ENTRY_HEADER key=");
            print_slice(&ev.key);
            (void)printf("\n");
            break;
        default:
            break;
        }
    }

    free(reader.buf);
    if (fp != stdin) fclose(fp);
    return rc;
}

/* =========================================================================
 * roundtrip — reconstruct the document from parse events and compare
 * byte-for-byte with the original.  Loads the whole file into memory so
 * the comparison is available after the parser finishes.
 * ========================================================================= */

static int emit_inline_comment(struct buffer *b,
                                unsigned int spaces,
                                const siml_slice *comment) {
    if (!comment || comment->len == 0) return 1;
    if (!buf_append_spaces(b, (size_t)spaces)) return 0;
    if (!buf_append(b, "# ", 2)) return 0;
    if (!buf_append(b, comment->ptr, comment->len)) return 0;
    return 1;
}

static int emit_prefix(struct buffer *b, size_t indent,
                        const char *key, size_t key_len,
                        int in_sequence, int has_inline_value,
                        unsigned int inline_prefixes) {
    unsigned int i;
    size_t prefix_indent = (size_t)inline_prefixes * 2;

    if (prefix_indent > indent) return 0;
    if (!buf_append_spaces(b, indent - prefix_indent)) return 0;
    for (i = 0; i < inline_prefixes; ++i) {
        if (!buf_append(b, "- ", 2)) return 0;
    }
    if (in_sequence) {
        if (!buf_append(b, "-", 1)) return 0;
        if (has_inline_value && !buf_append(b, " ", 1)) return 0;
        return 1;
    }
    if (!buf_append(b, key, key_len)) return 0;
    if (has_inline_value) {
        if (!buf_append(b, ": ", 2)) return 0;
    } else {
        if (!buf_append(b, ":", 1)) return 0;
    }
    return 1;
}

/* Recursively render a flow sequence starting after its opening '['. */
static int build_flow_sequence(siml_parser *parser,
                               struct buffer *b,
                               siml_event *ev) {
    int first = 1;

    if (!buf_append_char(b, '[')) return 0;
    for (;;) {
        siml_event_type t = siml_next(parser, ev);
        if (t == SIML_EVENT_ERROR) return 0;
        if (t == SIML_EVENT_SEQUENCE_END) {
            if (!buf_append_char(b, ']')) return 0;
            break;
        }
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
    return 1;
}

static int cmd_roundtrip(int argc, char **argv) {
    const char *filename;
    FILE *fp;
    long file_size;
    size_t read_size;
    char *file_data;
    struct mem_reader reader;
    struct buffer out;
    siml_parser parser;
    siml_event ev;
    int rc;
    size_t stack_indent[SIML_MAX_NESTING];
    siml_container_type stack_type[SIML_MAX_NESTING];
    unsigned int stack_inline_prefixes[SIML_MAX_NESTING];
    size_t depth;
    size_t cur_indent;
    int in_sequence;
    unsigned int inline_prefixes;

    if (argc != 3) {
        (void)fprintf(stderr, "Usage: %s roundtrip <file.siml>\n", argv[0]);
        return 1;
    }
    filename = argv[2];

    fp = fopen(filename, "rb");
    if (!fp) { perror(filename); return 1; }

    /* Determine file size and read the whole file into memory. */
    if (fseek(fp, 0, SEEK_END) != 0) {
        perror(filename); fclose(fp); return 1;
    }
    file_size = ftell(fp);
    if (file_size < 0) {
        perror(filename); fclose(fp); return 1;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        perror(filename); fclose(fp); return 1;
    }

    file_data = (char *)malloc((size_t)file_size);
    if (!file_data) { fclose(fp); return 1; }
    read_size = fread(file_data, 1, (size_t)file_size, fp);
    fclose(fp);
    if (read_size != (size_t)file_size) { free(file_data); return 1; }

    reader.data = file_data;
    reader.len  = read_size;
    reader.pos  = 0;

    out.data = NULL;
    out.len  = 0;
    out.cap  = 0;
    depth    = 0;

    siml_parser_init(&parser, mem_read_line, &reader);

    rc = 0;
    for (;;) {
        siml_event_type t = siml_next(&parser, &ev);
        if (t == SIML_EVENT_ERROR) { print_error(&ev); rc = 1; break; }
        if (t == SIML_EVENT_STREAM_END) break;

        in_sequence    = (depth > 0 && stack_type[depth - 1] == SIML_CONTAINER_SEQ);
        cur_indent     = (depth > 0) ? stack_indent[depth - 1] : 0;
        inline_prefixes = (depth > 0) ? stack_inline_prefixes[depth - 1] : 0;

        switch (t) {
        case SIML_EVENT_STREAM_START:
        case SIML_EVENT_DOCUMENT_START:
            break;
        case SIML_EVENT_DOCUMENT_END:
            if (parser.doc_state == SIML_DOC_BETWEEN) {
                if (!buf_append(&out, "---", 3) || !buf_append_char(&out, '\n'))
                    rc = 1;
            }
            break;
        case SIML_EVENT_SHEBANG:
            if (!buf_append(&out, "#!", 2) ||
                !buf_append(&out, ev.value.ptr, ev.value.len) ||
                !buf_append_char(&out, '\n'))
                rc = 1;
            break;
        case SIML_EVENT_COMMENT:
            if (!buf_append(&out, ev.value.ptr, ev.value.len) ||
                !buf_append_char(&out, '\n'))
                rc = 1;
            break;
        case SIML_EVENT_MAPPING_ENTRY_HEADER:
            if (!emit_prefix(&out, cur_indent,
                             ev.key.ptr, ev.key.len,
                             in_sequence, 0, inline_prefixes) ||
                !buf_append_char(&out, '\n'))
                rc = 1;
            if (inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            break;
        case SIML_EVENT_MAPPING_START:
            if (ev.key.len > 0) {
                if (!emit_prefix(&out, cur_indent,
                                 ev.key.ptr, ev.key.len,
                                 in_sequence, 0, inline_prefixes) ||
                    !buf_append_char(&out, '\n'))
                    rc = 1;
                if (inline_prefixes > 0)
                    stack_inline_prefixes[depth - 1] = 0;
            }
            if (depth >= SIML_MAX_NESTING) { rc = 1; break; }
            stack_type[depth]            = SIML_CONTAINER_MAP;
            stack_indent[depth]          = (depth == 0) ? 0 : (cur_indent + 2);
            stack_inline_prefixes[depth] = in_sequence ? inline_prefixes + 1 : 0;
            if (in_sequence && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            depth += 1;
            break;
        case SIML_EVENT_SEQUENCE_START:
            if (ev.seq_style == SIML_SEQ_STYLE_FLOW) {
                /* Capture key and inline comment before consuming more events. */
                char key_buf[SIML_MAX_KEY_LEN + 1];
                char comment_buf[SIML_MAX_INLINE_COMMENT_TEXT_LEN + 1];
                siml_slice key;
                siml_slice comment;
                unsigned int comment_spaces;
                struct buffer flow;

                key.len = ev.key.len > SIML_MAX_KEY_LEN
                          ? SIML_MAX_KEY_LEN : ev.key.len;
                if (key.len > 0) memcpy(key_buf, ev.key.ptr, key.len);
                key.ptr = key_buf;
                key_buf[key.len] = '\0';

                comment.len = ev.inline_comment.len > SIML_MAX_INLINE_COMMENT_TEXT_LEN
                              ? SIML_MAX_INLINE_COMMENT_TEXT_LEN
                              : ev.inline_comment.len;
                if (comment.len > 0)
                    memcpy(comment_buf, ev.inline_comment.ptr, comment.len);
                comment.ptr = comment_buf;
                comment_buf[comment.len] = '\0';
                comment_spaces = ev.inline_comment_spaces;

                flow.data = NULL; flow.len = 0; flow.cap = 0;
                if (!build_flow_sequence(&parser, &flow, &ev)) {
                    free(flow.data); rc = 1; break;
                }
                /* Re-read depth context: build_flow_sequence consumed events. */
                in_sequence = (depth > 0 &&
                               stack_type[depth - 1] == SIML_CONTAINER_SEQ);
                cur_indent  = (depth > 0) ? stack_indent[depth - 1] : 0;
                if (!emit_prefix(&out, cur_indent,
                                 key.ptr, key.len,
                                 in_sequence, 1, inline_prefixes) ||
                    !buf_append(&out, flow.data, flow.len) ||
                    !emit_inline_comment(&out, comment_spaces, &comment) ||
                    !buf_append_char(&out, '\n')) {
                    free(flow.data); rc = 1; break;
                }
                if (inline_prefixes > 0) stack_inline_prefixes[depth - 1] = 0;
                free(flow.data);
                break;
            }
            if (ev.key.len > 0) {
                if (!emit_prefix(&out, cur_indent,
                                 ev.key.ptr, ev.key.len,
                                 in_sequence, 0, inline_prefixes) ||
                    !buf_append_char(&out, '\n'))
                    rc = 1;
                if (inline_prefixes > 0) stack_inline_prefixes[depth - 1] = 0;
            }
            if (depth >= SIML_MAX_NESTING) { rc = 1; break; }
            stack_type[depth]            = SIML_CONTAINER_SEQ;
            stack_indent[depth]          = (depth == 0) ? 0 : (cur_indent + 2);
            stack_inline_prefixes[depth] = in_sequence ? inline_prefixes + 1 : 0;
            if (in_sequence && inline_prefixes > 0)
                stack_inline_prefixes[depth - 1] = 0;
            depth += 1;
            break;
        case SIML_EVENT_MAPPING_END:
        case SIML_EVENT_SEQUENCE_END:
            if (depth > 0) depth -= 1;
            break;
        case SIML_EVENT_SCALAR:
            if (!emit_prefix(&out, cur_indent,
                             ev.key.ptr, ev.key.len,
                             in_sequence, 1, inline_prefixes) ||
                !buf_append(&out, ev.value.ptr, ev.value.len) ||
                !emit_inline_comment(&out, ev.inline_comment_spaces,
                                     &ev.inline_comment) ||
                !buf_append_char(&out, '\n'))
                rc = 1;
            if (inline_prefixes > 0) stack_inline_prefixes[depth - 1] = 0;
            break;
        case SIML_EVENT_BLOCK_SCALAR_START:
            if (!emit_prefix(&out, cur_indent,
                             ev.key.ptr, ev.key.len,
                             in_sequence, 1, inline_prefixes) ||
                !buf_append(&out, "|", 1) ||
                !emit_inline_comment(&out, ev.inline_comment_spaces,
                                     &ev.inline_comment) ||
                !buf_append_char(&out, '\n'))
                rc = 1;
            if (inline_prefixes > 0) stack_inline_prefixes[depth - 1] = 0;
            break;
        case SIML_EVENT_BLOCK_SCALAR_LINE:
            if (ev.value.len == 0) {
                if (!buf_append_char(&out, '\n')) rc = 1;
            } else {
                if (!buf_append_spaces(&out, cur_indent + 2) ||
                    !buf_append(&out, ev.value.ptr, ev.value.len) ||
                    !buf_append_char(&out, '\n'))
                    rc = 1;
            }
            break;
        case SIML_EVENT_BLOCK_SCALAR_END:
            break;
        case SIML_EVENT_NONE:
        case SIML_EVENT_ERROR:
        default:
            rc = 1;
            break;
        }

        if (rc != 0) break;
    }

    if (rc == 0) {
        /* Strip trailing '\n' added by the reconstructor if the original
         * file did not end with one. */
        if (read_size == 0) {
            if (out.len != 0) rc = 1;
        } else if (file_data[read_size - 1] != '\n' &&
                   out.len > 0 && out.data[out.len - 1] == '\n') {
            out.len -= 1;
        }
        if (out.len != read_size ||
            (out.len > 0 && memcmp(out.data, file_data, out.len) != 0)) {
            (void)fprintf(stderr, "roundtrip mismatch: %s\n", filename);
            rc = 1;
        }
    }

    free(out.data);
    free(file_data);
    return rc;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

static void print_help(const char *prog) {
    (void)printf(
        "Usage: %s [<subcommand>] <file.siml>\n"
        "\n"
        "Subcommands:\n"
        "  verify    <file>  Parse and validate; silent on success, exits 1 on error.\n"
        "  dump      <file>  Parse and print a human-readable event trace to stdout.\n"
        "  roundtrip <file>  Parse, reconstruct, and compare byte-for-byte with input.\n"
        "\n"
        "An existing file may be passed without a subcommand; verify is used.\n"
        "Pass '-' as <file> to read from stdin (verify and dump only).\n",
        prog);
}

static int is_regular_file(const char *path) {
    struct stat path_stat;

    return stat(path, &path_stat) == 0 && S_ISREG(path_stat.st_mode);
}

int main(int argc, char **argv) {
    char *verify_argv[3];

    if (argc < 2) {
        print_help(argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        print_help(argv[0]);
        return 0;
    }
    if (strcmp(argv[1], "verify")    == 0) return cmd_verify(argc, argv);
    if (strcmp(argv[1], "dump")      == 0) return cmd_dump(argc, argv);
    if (strcmp(argv[1], "roundtrip") == 0) return cmd_roundtrip(argc, argv);
    if (argc == 2 && is_regular_file(argv[1])) {
        verify_argv[0] = argv[0];
        verify_argv[1] = "verify";
        verify_argv[2] = argv[1];
        return cmd_verify(3, verify_argv);
    }

    (void)fprintf(stderr, "%s: unknown subcommand '%s'\n"
                          "Run '%s --help' for usage.\n",
                  argv[0], argv[1], argv[0]);
    return 1;
}
