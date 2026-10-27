#ifndef AW_JOURNAL_H
#define AW_JOURNAL_H

#include "jsonl.h"
#include "watcher.h"

#define JOURNAL_TAIL_BYTES 4096

/* Append-only sessions.jsonl with key based idempotence (D10, D11). */
typedef struct {
  const Fs *fs;
  void *fu;
  Key last;          /* highest key known to be in the file */
  uint8_t last_ok;
  uint8_t exists;    /* file has at least one byte */
  uint8_t dirty;     /* the tail is unknown after a failed append: re-read before the next one */
  uint8_t fence;     /* the file may end in a torn line: start the next append with '\n' */
  uint32_t suppressed;
  char tail[JOURNAL_TAIL_BYTES];
} Journal;

void journal_init(Journal *j, const Fs *fs, void *fu);
int journal_sync(Journal *j); /* re-read the tail. 0 or errno */

/* line[0] is reserved for a fence byte, the record is line[1..n]. Returns 0 when committed or already present. */
int journal_emit(Journal *j, const Key *k, char *line, size_t n);

#endif
