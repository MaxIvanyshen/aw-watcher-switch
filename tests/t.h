#ifndef T_H
#define T_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_fail, t_checks;

#define CHECK(c) do { t_checks++; if (!(c)) { t_fail++; fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); t_checks++; \
  if (va_ != vb_) { t_fail++; fprintf(stderr, "%s:%d: %s == %s failed: %lld vs %lld\n", __FILE__, __LINE__, #a, #b, va_, vb_); } } while (0)
#define CHECK_STR(a, b) do { const char *sa_ = (a), *sb_ = (b); t_checks++; \
  if (strcmp(sa_, sb_) != 0) { t_fail++; fprintf(stderr, "%s:%d: %s == %s failed:\n  %s\n  %s\n", __FILE__, __LINE__, #a, #b, sa_, sb_); } } while (0)

#define RUN(fn) do { int before_ = t_fail; fn(); printf("  %-40s %s\n", #fn, t_fail == before_ ? "ok" : "FAIL"); } while (0)

static int t_done(const char *name) {
  printf("%s: %d checks, %d failed\n", name, t_checks, t_fail);
  return t_fail ? 1 : 0;
}

#endif
