#ifndef HACMAN_SELFHASH_H_INCLUDED
#define HACMAN_SELFHASH_H_INCLUDED

/* The binary's self digest, shared by the program (src/version.c) and the
 * build step that fills it in (tools/stamp.c).
 *
 * hacman is linked with an 8-byte, all-zero slot in a section of its own.
 * After linking, hm-stamp hashes the complete file with XXH3-64 (the hash the
 * project identity uses), while the slot is still zero, and writes the result
 * into the slot in the target's byte order. The digest therefore describes
 * every byte of the program: source, compiler, flags and Meson options alike.
 *
 * To verify a binary, zero the slot again and hash the file: the result must
 * equal the value that was stored there (`hm-stamp --check FILE`). */

/* Name of the ELF section that holds nothing but the 8-byte slot. A section
 * of its own keeps the slot in the file (never .bss), lets the stamp step find
 * it without symbols and survives `strip`. */
#define HM_SELFHASH_SECTION ".hm_selfhash"

/* Size of the slot in bytes: one XXH3-64 digest. */
#define HM_SELFHASH_SIZE 8

#endif
