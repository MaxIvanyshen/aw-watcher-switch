#include "diag.h"

#include <string.h>

#include "util.h"

void diag_init(Diag *d, const Env *env, void *eu) {
  memset(d, 0, sizeof *d);
  d->env = env;
  d->eu = eu;
}

void diag_begin(Diag *d) { d->on = !d->dead && d->env->diag_enabled(d->eu) != 0; }

int diag_active(const Diag *d) { return d->on && !d->dead; }

static void emit(Diag *d, SBuf *s) {
  if (!diag_active(d)) return;
  sb_str(s, "}\n");
  if (s->ovf) return;
  if (d->bytes + s->len > DIAG_CAP_BYTES) {
    static const char t[] = "{\"d\":\"trunc\",\"cap\":2097152}\n";
    d->env->diag_write(d->eu, t, sizeof t - 1);
    d->dead = 1;
    return;
  }
  if (d->env->diag_write(d->eu, s->p, s->len) != 0) { d->dead = 1; return; }
  d->bytes += (uint32_t)s->len;
}

static void head(Diag *d, SBuf *s, const char *kind) {
  sb_init(s, d->line, sizeof d->line);
  sb_str(s, "{\"d\":\""); sb_str(s, kind); sb_ch(s, '"');
}

static void num(SBuf *s, const char *name, int64_t v) {
  sb_str(s, ",\""); sb_str(s, name); sb_str(s, "\":"); sb_i64(s, v);
}

static void str(SBuf *s, const char *name, const char *v) {
  sb_str(s, ",\""); sb_str(s, name); sb_str(s, "\":\""); sb_str(s, v); sb_ch(s, '"');
}

void diag_boot(Diag *d, const char *mode, uint32_t ep, int32_t cursor) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "boot");
  num(&s, "fmt", 1); str(&s, "mode", mode); num(&s, "ep", ep); num(&s, "cur", cursor);
  emit(d, &s);
}

void diag_range(Diag *d, int rc, int32_t total, int32_t start, int32_t end) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "range");
  num(&s, "rc", rc); num(&s, "total", total); num(&s, "start", start); num(&s, "end", end);
  emit(d, &s);
}

void diag_probe(Diag *d, int32_t idx, int rc, int32_t n) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "probe");
  num(&s, "i", idx); num(&s, "rc", rc); num(&s, "n", n);
  emit(d, &s);
}

void diag_raw(Diag *d, const char *kind, int32_t idx, const RawEvent *r, const Event *e, const TrackOut *o) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "raw");
  str(&s, "k", kind); num(&s, "i", idx);
  sb_str(&s, ",\"hex\":\""); sb_hexbytes(&s, r->b, PDM_EVENT_SIZE); sb_ch(&s, '"');
  num(&s, "pt", e->play_type); num(&s, "ap", e->applet_id); num(&s, "pol", e->log_policy); num(&s, "et", e->applet_event);
  sb_str(&s, ",\"pid\":\"0x"); sb_hex(&s, e->pid, 16); sb_ch(&s, '"');
  sb_str(&s, ",\"us\":"); sb_u64(&s, e->ts_user);
  sb_str(&s, ",\"ns\":"); sb_u64(&s, e->ts_net);
  sb_str(&s, ",\"ss\":"); sb_u64(&s, e->ts_steady);
  if (o) {
    str(&s, "act", note_name(o->note));
    if (o->has_rec) { str(&s, "why", close_why_name(o->rec.why)); str(&s, "sem", sem_name(o->rec.sem)); }
    if (o->bump) num(&s, "bump", 1);
  }
  emit(d, &s);
}

void diag_tr(Diag *d, const char *what, const char *why, uint32_t ep, int32_t a, int32_t b) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "tr");
  str(&s, "what", what); str(&s, "why", why); num(&s, "ep", ep); num(&s, "a", a); num(&s, "b", b);
  emit(d, &s);
}

void diag_io(Diag *d, const char *op, int rc, uint32_t backoff_ms) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "io");
  str(&s, "op", op); num(&s, "rc", rc); num(&s, "backoff", backoff_ms);
  emit(d, &s);
}

void diag_stats(Diag *d, const DiagStats *st) {
  if (!diag_active(d)) return;
  SBuf s; head(d, &s, "stats");
  num(&s, "events", st->events); num(&s, "ignored", st->ignored); num(&s, "dup_focus", st->dup_focus);
  num(&s, "io_err", st->io_err); num(&s, "epochs", st->epochs);
  emit(d, &s);
}
