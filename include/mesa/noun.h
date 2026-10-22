/// @file
///
/// Minimal standalone nouns: refcounted, immutable, single-threaded.
///
/// Ownership follows vere's conventions:
///   - constructors (mesa_cell(), mesa_trel(), ...) TRANSFER their noun
///     arguments; the caller gives up its references.
///   - accessors (mesa_head(), mesa_at(), ...) return BORROWED nouns, valid
///     while the parent is alive; mesa_ref() one to keep it.
///   - everything else (mesa_jam(), mesa_eq(), ...) borrows its arguments.
///
/// Nouns are not thread-safe; don't share them across threads without
/// external locking.

#ifndef MESA_NOUN_H
#define MESA_NOUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint64_t mesa_noun;

/* MESA_NONE: not a noun; returned on failure (e.g. a malformed cue).
*/
#define MESA_NONE ((mesa_noun)UINT64_MAX)

/* memory: strings and buffers libmesa returns (mesa_atom_get_cord(),
**         mesa_jam_bytes(), mesa_ship_render(), ...) are freed with
**         mesa_free(); buffers handed to libmesa to free (a store's get())
**         come from mesa_malloc(). For other languages' FFIs, whose own
**         allocator may not be libmesa's.
*/
void*     mesa_malloc(size_t len);
void      mesa_free(void* ptr);

/* refcounts
*/
mesa_noun mesa_ref(mesa_noun som);
void      mesa_unref(mesa_noun som);

/* atoms (little-endian bytes)
*/
mesa_noun mesa_atom_u64(uint64_t val);
mesa_noun mesa_atom_bytes(size_t len, const uint8_t* buf);
mesa_noun mesa_atom_cord(const char* str);      //  @t from a C string

bool      mesa_is_atom(mesa_noun som);
bool      mesa_is_cell(mesa_noun som);

size_t    mesa_met0(mesa_noun atom);            //  significant bits
size_t    mesa_met3(mesa_noun atom);            //  significant bytes
void      mesa_atom_get_bytes(mesa_noun atom, size_t len, uint8_t* buf);
bool      mesa_atom_get_u64(mesa_noun atom, uint64_t* out);
char*     mesa_atom_get_cord(mesa_noun atom);   //  malloc'd, NUL-terminated

/* cells
*/
mesa_noun mesa_cell(mesa_noun hed, mesa_noun tel);
mesa_noun mesa_trel(mesa_noun a, mesa_noun b, mesa_noun c);
mesa_noun mesa_qual(mesa_noun a, mesa_noun b, mesa_noun c, mesa_noun d);

mesa_noun mesa_head(mesa_noun cell);
mesa_noun mesa_tail(mesa_noun cell);
mesa_noun mesa_at(uint64_t axe, mesa_noun som); //  MESA_NONE if no such axis

/* comparison and hashing
*/
bool      mesa_eq(mesa_noun a, mesa_noun b);
uint32_t  mesa_mug(mesa_noun som);              //  +mug

/* serialization
*/
mesa_noun mesa_jam(mesa_noun som);
uint8_t*  mesa_jam_bytes(mesa_noun som, size_t* len);   //  malloc'd
mesa_noun mesa_cue(mesa_noun atom);                     //  MESA_NONE if bad
mesa_noun mesa_cue_bytes(size_t len, const uint8_t* buf);

#endif /* ifndef MESA_NOUN_H */
