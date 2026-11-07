/* Checks and scenarios for the host tests. Each scenario runs in a child
 * process of its own, so what one leaves behind (the runtime's statics, the
 * shared memory) cannot change the next. */
#ifndef HOST_CHECK_H
#define HOST_CHECK_H

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_checks, g_failed;

#define CHECK(c)                                                             \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(c)) {                                                              \
      g_failed++;                                                            \
      printf("  %s:%d: FAIL: %s\n", __FILE__, __LINE__, #c);                 \
    }                                                                        \
  } while (0)

/* As CHECK, printing two numbers that should be equal. */
#define CHECK_EQ(a, b)                                                       \
  do {                                                                       \
    long long a_ = (long long)(a), b_ = (long long)(b);                      \
    g_checks++;                                                              \
    if (a_ != b_) {                                                          \
      g_failed++;                                                            \
      printf("  %s:%d: FAIL: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, \
             #a, #b, a_, b_);                                                \
    }                                                                        \
  } while (0)

typedef struct {
  const char *name;
  void (*fn)(void);
} Scenario;

static int run_scenarios(const char *suite, const Scenario *s, int n) {
  int checks = 0, failed = 0, bad = 0;
  for (int i = 0; i < n; i++) {
    int fd[2], st = 0, got[2] = {0, 0};
    pid_t pid;
    fflush(stdout);
    if (pipe(fd) || (pid = fork()) < 0) {
      printf("%s: cannot fork\n", suite);
      return 1;
    }
    if (pid == 0) {
      close(fd[0]);
      g_checks = g_failed = 0;
      s[i].fn();
      got[0] = g_checks;
      got[1] = g_failed;
      if (write(fd[1], got, sizeof(got)) != sizeof(got))
        _exit(4);
      fflush(stdout);
      _exit(0);
    }
    close(fd[1]);
    if (read(fd[0], got, sizeof(got)) != sizeof(got))
      got[0] = got[1] = 0;
    close(fd[0]);
    waitpid(pid, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
      printf("  %s: stopped (status %d)\n", s[i].name, st);
      bad++;
    } else if (got[1]) {
      printf("  %s: %d of %d checks failed\n", s[i].name, got[1], got[0]);
    }
    checks += got[0];
    failed += got[1];
  }
  printf("%s: %d scenarios, %d checks, %d failed%s\n", suite, n, checks,
         failed, bad ? ", some stopped early" : "");
  return failed || bad;
}

#define SCENARIOS(suite, ...)                                                \
  int main(void) {                                                           \
    static const Scenario list_[] = {__VA_ARGS__};                           \
    setvbuf(stdout, NULL, _IONBF, 0);                                        \
    return run_scenarios(suite, list_, sizeof(list_) / sizeof(list_[0]));    \
  }

#define S(f) {#f, f}

#endif
