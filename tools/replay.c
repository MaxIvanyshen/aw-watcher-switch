/* Offline replay of diag.jsonl "raw" lines through the real decoder and tracker.
 *
 *   replay [--src live|dump|auto] [--watched|--unwatched] [FILE]
 *
 * Prints the session and gap records the watcher would have produced, in journal format, on stdout.
 * A summary goes to stderr. Reads stdin when no file is given.
 *
 * live lines were written while the watcher was running and are replayed as continuously observed
 * (--watched), so sem matches what the watcher wrote. dump lines (dump_all) cover time the watcher may
 * not have observed, so they replay unwatched: any session longer than ALIVE_GAP_MAX between events is
 * labelled unc. Initialize is never treated as observed (R8).
 *
 * diag.jsonl contains raw log entries. Account entries include account ids: keep the file private. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonl.h"
#include "util.h"

typedef enum { SRC_AUTO, SRC_LIVE, SRC_DUMP } Src;

typedef struct {
  Src src;
  int watched;      /* -1 = decide from the source */
} ReplayOpts;

typedef struct { int32_t idx; uint32_t crc; int used; } Seen;

typedef struct {
  Tracker t;
  uint32_t ep;
  int32_t last_idx;
  int have_last;
  Seen seen[256];
  unsigned raw_used, sessions, gaps, bad_lines, dup_replays;
  unsigned notes[NOTE_COUNT];
} ReplayState;

static const char *field(const char *line, const char *name) {
  char pat[24];
  snprintf(pat, sizeof pat, "\"%s\":", name);
  const char *p = strstr(line, pat);
  return p ? p + strlen(pat) : NULL;
}

static int parse_hex_event(const char *hex, RawEvent *r) {
  if (strlen(hex) < 2 * PDM_EVENT_SIZE + 1) return 0;
  memset(r, 0, sizeof *r);
  for (int i = 0; i < PDM_EVENT_SIZE; i++) {
    int v = 0;
    for (int k = 0; k < 2; k++) {
      char c = hex[2 * i + k];
      int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
      if (d < 0) return 0;
      v = v << 4 | d;
    }
    r->b[i] = (uint8_t)v;
  }
  return hex[2 * PDM_EVENT_SIZE] == '"';
}

static void put_session(ReplayState *s, FILE *out, int32_t idx, const SessionRec *r) {
  char b[JSONL_MAX];
  Key k = {s->ep, idx, SUB_SESSION};
  if (jsonl_session(b, sizeof b, &k, r)) fputs(b, out);
  s->sessions++;
}

static void put_gap(ReplayState *s, FILE *out, int32_t idx, GapWhy why) {
  char b[JSONL_MAX];
  Key k = {s->ep, idx, SUB_GAP};
  if (jsonl_gap(b, sizeof b, &k, why, idx, idx)) fputs(b, out);
  s->gaps++;
}

static void observe(ReplayState *s, const Event *e) {
  if (e->ts_steady == 0 || !s->t.open || e->play_type == PT_INIT) return;
  uint64_t a = s->t.alive;
  for (int i = 0; i < 1000000 && a + ALIVE_GAP_MAX < e->ts_steady; i++) { a += ALIVE_GAP_MAX; tracker_touch(&s->t, a); }
}

static void feed(ReplayState *s, FILE *out, int32_t idx, const RawEvent *raw, int watched) {
  uint32_t crc = crc32_of(raw->b, PDM_EVENT_SIZE);
  Seen *slot = &s->seen[(uint32_t)idx % 256];
  if (slot->used && slot->idx == idx && slot->crc == crc && s->have_last && idx <= s->last_idx) { s->dup_replays++; return; }
  if (s->have_last && idx <= s->last_idx) {
    SessionRec r;
    if (tracker_close_unc(&s->t, WHY_LOST, &r)) {
      /* The lower triggering index belongs to the new stream, not the old
       * session. Close after its last consumed index to preserve key order. */
      if (s->last_idx == INT32_MAX) { s->ep++; put_session(s, out, idx, &r); }
      else put_session(s, out, s->last_idx + 1, &r);
    }
    tracker_reset(&s->t);
    s->ep++;
    put_gap(s, out, idx, GAP_LOG_RESET);
    memset(s->seen, 0, sizeof s->seen);
  }
  slot = &s->seen[(uint32_t)idx % 256];
  slot->used = 1; slot->idx = idx; slot->crc = crc;
  s->last_idx = idx; s->have_last = 1;
  s->raw_used++;

  Event e;
  TrackOut o;
  decode_event(raw, &e);
  if (watched) observe(s, &e);
  tracker_step(&s->t, &e, &o);
  s->notes[o.note]++;
  if (o.has_rec) put_session(s, out, idx, &o.rec);
  if (o.bump) { s->ep++; put_gap(s, out, idx, GAP_STEADY_RESET); }
}

static int pass(FILE *in, FILE *out, ReplayState *s, const char *want_kind, int watched) {
  char *line = malloc(4096);
  if (!line) return 1;
  rewind(in);
  while (fgets(line, 4096, in)) {
    if (!strstr(line, "\"d\":\"raw\"")) continue;
    const char *k = field(line, "k"), *i = field(line, "i"), *h = field(line, "hex");
    RawEvent raw;
    if (!k || !i || !h || h[0] != '"' || !parse_hex_event(h + 1, &raw)) { s->bad_lines++; continue; }
    if (strncmp(k + 1, want_kind, strlen(want_kind)) != 0) continue;
    feed(s, out, (int32_t)strtol(i, NULL, 10), &raw, watched);
  }
  free(line);
  return 0;
}

static int has_kind(FILE *in, const char *kind) {
  char pat[32];
  snprintf(pat, sizeof pat, "\"k\":\"%s\"", kind);
  char *line = malloc(4096);
  int found = 0;
  rewind(in);
  while (line && !found && fgets(line, 4096, in)) found = strstr(line, "\"d\":\"raw\"") && strstr(line, pat);
  free(line);
  return found;
}

/* Replays one seekable stream. Returns 0 on success. */
int replay_run(FILE *in, FILE *out, FILE *err, const ReplayOpts *o) {
  Src src = o->src;
  if (src == SRC_AUTO) src = has_kind(in, "live") ? SRC_LIVE : SRC_DUMP;
  const char *kind = src == SRC_LIVE ? "live" : "dump";
  int watched = o->watched >= 0 ? o->watched : src == SRC_LIVE;
  ReplayState *s = calloc(1, sizeof *s);
  if (!s) return 1;
  s->ep = 1;
  int rc = pass(in, out, s, kind, watched);
  if (err) {
    fprintf(err, "replay: source=%s %s, %u events, %u sessions, %u gaps, %u repeated events skipped, %u bad raw lines\n", kind,
            watched ? "watched" : "unwatched", s->raw_used, s->sessions, s->gaps, s->dup_replays, s->bad_lines);
    for (int n = 0; n < NOTE_COUNT; n++)
      if (s->notes[n]) fprintf(err, "  %-14s %u\n", note_name((Note)n), s->notes[n]);
  }
  free(s);
  return rc;
}

#ifndef REPLAY_NO_MAIN
int main(int argc, char **argv) {
  ReplayOpts o = {SRC_AUTO, -1};
  const char *file = NULL;
  int rc = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--src") && i + 1 < argc) {
      i++;
      o.src = !strcmp(argv[i], "live") ? SRC_LIVE : !strcmp(argv[i], "dump") ? SRC_DUMP : SRC_AUTO;
    } else if (!strcmp(argv[i], "--watched")) o.watched = 1;
    else if (!strcmp(argv[i], "--unwatched")) o.watched = 0;
    else if (argv[i][0] == '-' && argv[i][1]) { fprintf(stderr, "usage: replay [--src live|dump|auto] [--watched|--unwatched] [FILE]\n"); return 2; }
    else if (file) { fprintf(stderr, "replay: only one input file is supported; process captures separately\n"); return 2; }
    else file = argv[i];
  }
  if (file) {
    FILE *f = fopen(file, "r");
    if (!f) { perror(file); return 1; }
    rc = replay_run(f, stdout, stderr, &o);
    fclose(f);
  } else {
    FILE *f = tmpfile();   /* stdin is not seekable */
    char buf[4096];
    size_t n;
    if (!f) return 1;
    while ((n = fread(buf, 1, sizeof buf, stdin)) > 0) fwrite(buf, 1, n, f);
    rc = replay_run(f, stdout, stderr, &o);
    fclose(f);
  }
  return rc;
}
#endif
