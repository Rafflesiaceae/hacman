/* hm-stamp - write a binary's own digest into its self-hash slot.
 *
 *   hm-stamp IN OUT       copy IN to OUT with the slot filled in
 *   hm-stamp --check FILE verify that FILE's slot holds FILE's digest
 *
 * The slot is the ELF section HM_SELFHASH_SECTION (see src/selfhash.h). Its
 * digest is XXH3-64 over the whole file while the slot is all zeros, stored
 * in the file's own byte order so the program reads it back as a plain
 * uint64_t. Stamping refuses input whose slot is not zero, which guarantees
 * that the stored value is always the digest of the zeroed file and can be
 * re-verified later.
 *
 * Runs on the build machine; the ELF file may be of either class and byte
 * order, so header fields are decoded by hand rather than through the host's
 * struct layout. */

/* Project arguments do not reach build-machine targets when cross-compiling,
 * and -std=c99 alone hides chmod() and struct stat. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <elf.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "selfhash.h"
#include "xxhash.h"

/* A file loaded into memory, plus the decoding state for its ELF headers. */
typedef struct {
    unsigned char *data;
    size_t         len;
    int            is64; /* ELFCLASS64 rather than ELFCLASS32 */
    int            msb;  /* ELFDATA2MSB rather than ELFDATA2LSB */
} elf_file;

/* Reads an unsigned `n`-byte field at `off` in the file's byte order. The
 * caller has already checked that the field lies within the file. */
static uint64_t rd(const elf_file *f, size_t off, size_t n)
{
    uint64_t v = 0;
    size_t   i;

    for (i = 0; i < n; ++i) {
        size_t b = f->msb ? i : n - 1 - i;
        v        = (v << 8) | f->data[off + b];
    }
    return v;
}

/* Writes `v` as an 8-byte field at `off` in the file's byte order. */
static void wr64(elf_file *f, size_t off, uint64_t v)
{
    size_t i;

    for (i = 0; i < 8; ++i) {
        size_t b         = f->msb ? 7 - i : i;
        f->data[off + b] = (unsigned char)(v >> (8 * i));
    }
}

/* Reads a whole file into `f`. Returns 0 on success. */
static int load(const char *path, elf_file *f)
{
    FILE *fp = fopen(path, "rb");
    long  size;

    if (!fp) {
        fprintf(stderr, "hm-stamp: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (size = ftell(fp)) < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fprintf(stderr, "hm-stamp: cannot size %s: %s\n", path, strerror(errno));
        fclose(fp);
        return -1;
    }
    f->len  = (size_t)size;
    f->data = malloc(f->len ? f->len : 1);
    if (!f->data || fread(f->data, 1, f->len, fp) != f->len) {
        fprintf(stderr, "hm-stamp: cannot read %s\n", path);
        fclose(fp);
        return -1;
    }
    fclose(fp);
    return 0;
}

/* Finds the file offset of the slot. Returns 0 on success; anything that is
 * not exactly one well-formed slot section is an error, so a damaged or
 * foreign file is never stamped at a guessed location. */
static int find_slot(elf_file *f, const char *path, size_t *slot)
{
    uint64_t shoff, shentsize, shnum, shstrndx, str_off, str_size, i;
    size_t   name_len = sizeof(HM_SELFHASH_SECTION);
    int      found    = 0;

    if (f->len < EI_NIDENT || memcmp(f->data, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "hm-stamp: %s is not an ELF file\n", path);
        return -1;
    }
    f->is64 = f->data[EI_CLASS] == ELFCLASS64;
    f->msb  = f->data[EI_DATA] == ELFDATA2MSB;
    if ((!f->is64 && f->data[EI_CLASS] != ELFCLASS32) ||
        (!f->msb && f->data[EI_DATA] != ELFDATA2LSB) ||
        f->len < (f->is64 ? sizeof(Elf64_Ehdr) : sizeof(Elf32_Ehdr))) {
        fprintf(stderr, "hm-stamp: %s has an unsupported ELF header\n", path);
        return -1;
    }

/* Field offsets and sizes come from <elf.h> so they are not magic numbers. */
#define EHDR(field)                                                                                \
    (f->is64 ? rd(f, offsetof(Elf64_Ehdr, field), sizeof(((Elf64_Ehdr *)0)->field))                \
             : rd(f, offsetof(Elf32_Ehdr, field), sizeof(((Elf32_Ehdr *)0)->field)))
#define SHDR(base, field)                                                                          \
    (f->is64 ? rd(f, (base) + offsetof(Elf64_Shdr, field), sizeof(((Elf64_Shdr *)0)->field))       \
             : rd(f, (base) + offsetof(Elf32_Shdr, field), sizeof(((Elf32_Shdr *)0)->field)))

    shoff     = EHDR(e_shoff);
    shentsize = EHDR(e_shentsize);
    shnum     = EHDR(e_shnum);
    shstrndx  = EHDR(e_shstrndx);

    /* Extended numbering (shnum 0, SHN_XINDEX) never occurs at this size. */
    if (shentsize != (f->is64 ? sizeof(Elf64_Shdr) : sizeof(Elf32_Shdr)) || shnum == 0 ||
        shstrndx >= shnum || shoff > f->len || shnum > (f->len - shoff) / shentsize) {
        fprintf(stderr, "hm-stamp: %s has no usable section table\n", path);
        return -1;
    }

    str_off  = SHDR(shoff + shstrndx * shentsize, sh_offset);
    str_size = SHDR(shoff + shstrndx * shentsize, sh_size);
    if (str_off > f->len || str_size > f->len - str_off) {
        fprintf(stderr, "hm-stamp: %s has a damaged section name table\n", path);
        return -1;
    }

    for (i = 0; i < shnum; ++i) {
        size_t   base = (size_t)(shoff + i * shentsize);
        uint64_t name = SHDR(base, sh_name);
        uint64_t off  = SHDR(base, sh_offset);
        uint64_t size = SHDR(base, sh_size);

        /* Compare the name including its NUL, within the name table. */
        if (name >= str_size || str_size - name < name_len ||
            memcmp(f->data + str_off + name, HM_SELFHASH_SECTION, name_len) != 0)
            continue;
        if (SHDR(base, sh_type) != SHT_PROGBITS || size != HM_SELFHASH_SIZE || off > f->len ||
            f->len - off < HM_SELFHASH_SIZE) {
            fprintf(stderr, "hm-stamp: %s has a malformed %s section\n", path,
                    HM_SELFHASH_SECTION);
            return -1;
        }
        *slot = (size_t)off;
        ++found;
    }
#undef EHDR
#undef SHDR

    if (found != 1) {
        fprintf(stderr, "hm-stamp: %s has %d %s sections, expected 1\n", path, found,
                HM_SELFHASH_SECTION);
        return -1;
    }
    return 0;
}

/* Writes `f` to `path` through a temporary file and rename(), so a running
 * copy of the old binary (ETXTBSY) or an interrupted build never leaves a
 * truncated executable behind. `mode` is the input's permission bits. */
static int save(const elf_file *f, const char *path, mode_t mode)
{
    size_t tmp_len = strlen(path) + sizeof(".tmp");
    char  *tmp     = malloc(tmp_len);
    FILE  *fp;
    int    ok;

    if (!tmp) return -1;
    snprintf(tmp, tmp_len, "%s.tmp", path);
    fp = fopen(tmp, "wb");
    if (!fp) {
        fprintf(stderr, "hm-stamp: cannot create %s: %s\n", tmp, strerror(errno));
        free(tmp);
        return -1;
    }
    ok = fwrite(f->data, 1, f->len, fp) == f->len;
    ok = (fclose(fp) == 0) && ok;
    ok = ok && chmod(tmp, mode) == 0 && rename(tmp, path) == 0;
    if (!ok) {
        fprintf(stderr, "hm-stamp: cannot write %s: %s\n", path, strerror(errno));
        remove(tmp);
    }
    free(tmp);
    return ok ? 0 : -1;
}

/* Copies `in` to `out` with the slot set to the digest of `in`. */
static int stamp(const char *in, const char *out)
{
    elf_file    f = {0};
    struct stat st;
    size_t      slot;
    size_t      i;

    if (load(in, &f) != 0 || find_slot(&f, in, &slot) != 0) return 1;
    for (i = 0; i < HM_SELFHASH_SIZE; ++i) {
        if (f.data[slot + i] != 0) {
            fprintf(stderr, "hm-stamp: %s is already stamped\n", in);
            return 1;
        }
    }
    if (stat(in, &st) != 0) {
        fprintf(stderr, "hm-stamp: cannot stat %s: %s\n", in, strerror(errno));
        return 1;
    }
    /* The digest covers the file exactly as linked, with the slot zero. */
    wr64(&f, slot, XXH3_64bits(f.data, f.len));
    return save(&f, out, st.st_mode & 07777) == 0 ? 0 : 1;
}

/* Verifies that the slot in `path` holds the digest of the zeroed file. */
static int check(const char *path)
{
    elf_file f = {0};
    size_t   slot;
    uint64_t stored, actual;

    if (load(path, &f) != 0 || find_slot(&f, path, &slot) != 0) return 1;
    stored = rd(&f, slot, HM_SELFHASH_SIZE);
    wr64(&f, slot, 0);
    actual = XXH3_64bits(f.data, f.len);
    if (stored != actual) {
        fprintf(stderr, "hm-stamp: %s: stored %016llx, file hashes to %016llx\n", path,
                (unsigned long long)stored, (unsigned long long)actual);
        return 1;
    }
    printf("%016llx\n", (unsigned long long)stored);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "--check") == 0) return check(argv[2]);
    if (argc == 3 && argv[1][0] != '-') return stamp(argv[1], argv[2]);
    fprintf(stderr, "usage: hm-stamp IN OUT\n"
                    "       hm-stamp --check FILE\n");
    return 2;
}
