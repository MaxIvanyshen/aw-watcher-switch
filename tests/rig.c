#include "rig.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Rig *rig_new(void) {
  Rig *r = calloc(1, sizeof *r);
  if (!r) abort();
  rig_init(r);
  return r;
}

void rig_del(Rig *r) { rig_free(r); free(r); }

void rig_init(Rig *r) {
  r->crashes = 0;
  r->after_crash = NULL;
  r->user = NULL;
  ffs_init(&r->fs);
  r->pdm = malloc(sizeof *r->pdm);
  if (!r->pdm) abort();
  fp_init(r->pdm);
  fenv_init(&r->env);
  r->env.steady = 1000;
  rig_boot(r);
}

void rig_free(Rig *r) { fenv_free(&r->env); free(r->pdm); r->pdm = NULL; }

void rig_boot(Rig *r) {
  /* Recovery now performs a journal fsync barrier. An armed power cut can occur
   * during init as well as poll; never jump into a previous poll's dead frame. */
  if (setjmp(r->jb) != 0) {
    r->crashes++;
    ffs_clear_plans(&r->fs);
  }
  r->fs.jb = &r->jb;
  r->w = (Watcher *)&r->store;
  memset(&r->store, 0, sizeof r->store);
  if (watcher_size() > sizeof r->store) abort();
  if (watcher_init(r->w, fp_iface(), r->pdm, ffs_iface(), &r->fs, fenv_iface(), &r->env) != 0) abort();
}

int rig_poll(Rig *r) {
  if (setjmp(r->jb) == 0) {
    r->fs.jb = &r->jb;
    watcher_poll(r->w, &r->last_sleep);
    return 0;
  }
  r->crashes++;
  ffs_clear_plans(&r->fs);
  rig_boot(r);
  if (r->after_crash) r->after_crash(r);
  return 1;
}

void rig_settle(Rig *r) {
  for (int i = 0; i < 40; i++) {
    uint32_t before = r->fs.ops;
    int crashed = rig_poll(r);
    if (!crashed && r->fs.ops == before && r->last_sleep == POLL_MS_EXPECT) return;
  }
}

void rig_at(Rig *r, int64_t steady) { r->env.steady = steady; }
void rig_push(Rig *r, RawEvent e) { fp_push(r->pdm, &e); }

void jv_load(Rig *r, Journal_View *v) {
  memset(v, 0, sizeof *v);
  v->line = calloc(RJ_MAX_LINES, sizeof *v->line);
  v->key = calloc(RJ_MAX_LINES, sizeof *v->key);
  FFile *f = ffs_find(&r->fs, AW_PATH_SESSIONS);
  if (!f) return;
  size_t i = 0;
  while (i < f->n) {
    size_t j = i;
    while (j < f->n && f->d[j] != '\n') j++;
    size_t len = j - i;
    if (j == f->n) v->unterminated = 1;
    if (len > 0) {
      v->n_lines++;
      Key k;
      if (jsonl_valid((const char *)f->d + i, len) && jsonl_key((const char *)f->d + i, len, &k) && v->n_valid < RJ_MAX_LINES) {
        memcpy(v->line[v->n_valid], f->d + i, len);
        v->line[v->n_valid][len] = 0;
        v->key[v->n_valid] = k;
        if (rec_is_session(v->line[v->n_valid])) v->sessions++; else v->gaps++;
        v->n_valid++;
      } else v->n_bad++;
    }
    i = j + 1;
  }
}

void jv_free(Journal_View *v) { free(v->line); free(v->key); v->line = NULL; v->key = NULL; }

int jv_keys_increasing(const Journal_View *v) {
  for (int i = 1; i < v->n_valid; i++)
    if (key_cmp(&v->key[i - 1], &v->key[i]) >= 0) return 0;
  return 1;
}

static const char *find_field(const char *line, const char *field) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\":", field);
  const char *p = strstr(line, pat);
  return p ? p + strlen(pat) : NULL;
}

int rec_is_null(const char *line, const char *field) {
  const char *p = find_field(line, field);
  return p && strncmp(p, "null", 4) == 0;
}

int rec_num(const char *line, const char *field, uint64_t *out) {
  const char *p = find_field(line, field);
  if (!p || *p < '0' || *p > '9') return 0;
  *out = strtoull(p, NULL, 10);
  return 1;
}

int rec_str(const char *line, const char *field, char *out, size_t cap) {
  const char *p = find_field(line, field);
  if (!p || *p != '"') return 0;
  p++;
  size_t i = 0;
  while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
  out[i] = 0;
  return 1;
}

int rec_is_session(const char *line) { return strstr(line, "\"t\":\"session\"") != NULL; }

void rig_tick(Rig *r, int64_t to) {
  while (r->env.steady < to) {
    r->env.steady = r->env.steady + 10 < to ? r->env.steady + 10 : to;
    rig_settle(r);
  }
}

int jv_find(const Journal_View *v, int from, const char *needle) {
  for (int i = from; i < v->n_valid; i++)
    if (strstr(v->line[i], needle)) return i;
  return -1;
}

int jv_count(const Journal_View *v, const char *needle) {
  int c = 0;
  for (int i = 0; i < v->n_valid; i++) c += strstr(v->line[i], needle) != NULL;
  return c;
}

void snapshot_state(Rig *r, uint8_t *a, size_t *an, uint8_t *b, size_t *bn) {
  FFile *fa = ffs_find(&r->fs, AW_PATH_STATE_A), *fb = ffs_find(&r->fs, AW_PATH_STATE_B);
  *an = fa ? fa->n : 0; *bn = fb ? fb->n : 0;
  if (fa) memcpy(a, fa->d, fa->n);
  if (fb) memcpy(b, fb->d, fb->n);
}

static int sem_rank(const char *line) {
  char b[8];
  if (!rec_str(line, "sem", b, sizeof b)) return -1;
  return strcmp(b, "ok") == 0 ? 0 : strcmp(b, "exp") == 0 ? 1 : 2;
}

static uint64_t fnum(const char *line, const char *f) { uint64_t v = ~0ull; rec_num(line, f, &v); return v; }

int jv_equivalent(const Journal_View *ref, const Journal_View *got, char *why, size_t cap) {
  if (ref->n_valid != got->n_valid) {
    snprintf(why, cap, "line count %d vs %d", ref->n_valid, got->n_valid);
    return 0;
  }
  for (int i = 0; i < ref->n_valid; i++) {
    const char *a = ref->line[i], *b = got->line[i];
    if (key_cmp(&ref->key[i], &got->key[i]) != 0) { snprintf(why, cap, "line %d key differs:\n %s\n %s", i, a, b); return 0; }
    if (!rec_is_session(a)) {
      if (strcmp(a, b) != 0) { snprintf(why, cap, "gap %d differs:\n %s\n %s", i, a, b); return 0; }
      continue;
    }
    char wa[24], wb[24], pa[24], pb[24];
    rec_str(a, "why", wa, sizeof wa); rec_str(b, "why", wb, sizeof wb);
    rec_str(a, "pid", pa, sizeof pa); rec_str(b, "pid", pb, sizeof pb);
    int ka = !rec_is_null(a, "eu"), kb = !rec_is_null(b, "eu");
    if (strcmp(wa, wb) || strcmp(pa, pb) || fnum(a, "ss") != fnum(b, "ss") || ka != kb || fnum(a, "pol") != fnum(b, "pol")) {
      snprintf(why, cap, "session %d differs:\n %s\n %s", i, a, b);
      return 0;
    }
    if (ka && (fnum(a, "es") != fnum(b, "es") || fnum(a, "eu") != fnum(b, "eu") || fnum(a, "en") != fnum(b, "en") ||
               fnum(a, "su") != fnum(b, "su") || fnum(a, "sn") != fnum(b, "sn") || rec_is_null(a, "dur") != rec_is_null(b, "dur") ||
               (!rec_is_null(a, "dur") && fnum(a, "dur") != fnum(b, "dur")))) {
      snprintf(why, cap, "session %d times differ:\n %s\n %s", i, a, b);
      return 0;
    }
    if (sem_rank(b) < sem_rank(a)) { snprintf(why, cap, "session %d less cautious:\n %s\n %s", i, a, b); return 0; }
  }
  return 1;
}

int jv_invariants(const Journal_View *v, char *why, size_t cap) {
  if (!jv_keys_increasing(v)) { snprintf(why, cap, "keys not strictly increasing"); return 0; }
  uint64_t prev_es = 0;
  uint32_t prev_ep = 0;
  for (int i = 0; i < v->n_valid; i++) {
    const char *l = v->line[i];
    if (v->key[i].ep != prev_ep) { prev_ep = v->key[i].ep; prev_es = 0; }
    if (!rec_is_session(l)) continue;
    char w[24], sem[8];
    rec_str(l, "why", w, sizeof w); rec_str(l, "sem", sem, sizeof sem);
    uint64_t ss = fnum(l, "ss"), es = fnum(l, "es");
    int known = !rec_is_null(l, "eu");
    if (!rec_is_null(l, "dur")) {
      if (es < ss || fnum(l, "dur") != es - ss) { snprintf(why, cap, "dur != es-ss: %s", l); return 0; }
    }
    if (ss < prev_es) { snprintf(why, cap, "sessions overlap: %s", l); return 0; }
    prev_es = es > ss ? es : ss;
    int ok = strcmp(sem, "ok") == 0, exp = strcmp(sem, "exp") == 0;
    int observed = strcmp(w, "out_of_focus") == 0 || strcmp(w, "exit") == 0;
    if (!known && !(strcmp(sem, "unc") == 0)) { snprintf(why, cap, "unobserved end not unc: %s", l); return 0; }
    if (ok && !(observed && known && !rec_is_null(l, "dur"))) { snprintf(why, cap, "ok on an uncertain record: %s", l); return 0; }
    if (exp && strcmp(w, "power") != 0) { snprintf(why, cap, "exp on a non power record: %s", l); return 0; }
    if (strcmp(w, "superseded") == 0 && !strcmp(sem, "ok")) { snprintf(why, cap, "superseded labelled ok: %s", l); return 0; }
    if (rec_is_null(l, "dur") && !strcmp(sem, "ok")) { snprintf(why, cap, "null dur labelled ok: %s", l); return 0; }
  }
  return 1;
}
