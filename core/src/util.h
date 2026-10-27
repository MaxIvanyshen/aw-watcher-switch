#ifndef AW_UTIL_H
#define AW_UTIL_H

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_update(uint32_t crc, const void *p, size_t n);
uint32_t crc32_of(const void *p, size_t n);

/* Bounded writer. On overflow it sets ovf and keeps the buffer NUL terminated. */
typedef struct { char *p; size_t cap, len; int ovf; } SBuf;

void sb_init(SBuf *s, char *p, size_t cap);
void sb_str(SBuf *s, const char *z);
void sb_ch(SBuf *s, char c);
void sb_u64(SBuf *s, uint64_t v);
void sb_i64(SBuf *s, int64_t v);
void sb_hex(SBuf *s, uint64_t v, int digits);
void sb_hexbytes(SBuf *s, const uint8_t *b, size_t n);

#endif
