/* Skink coverage runtime — linked into binaries compiled with -cov.
 *
 * The emitter assigns every executable statement a monotonically
 * increasing counter id and records its source position (file, line,
 * col, enclosing function) in the __skink_covmap blob embedded in the
 * program. Each statement calls Skink_cov_hit(id) before its code, so
 * the counter array tracks per-statement execution counts.
 *
 * At exit (atexit, so it also runs for normal returns and panic-free
 * exits) the counts and the embedded map are written to
 * $SKINK_COV_OUT, or <exe-basename>.skinkcov next to the cwd by
 * default. File format:
 *
 *   SKINKCOV1
 *   <covmap blob: N lines of "file\tline\tcol\tfn">
 *   COUNTS
 *   <N decimal counts, one per line>
 *
 * Counters use relaxed atomics so spawned threads do not corrupt the
 * array; relaxed ordering is fine since only totals matter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* Emitted by the compiler into the instrumented program. Declared weak
 * so cov_rt.c can also be linked into uninstrumented binaries (the
 * compiler itself links every file under runtime): the references resolve to
 * NULL when no coverage globals exist and the dump becomes a no-op. */
extern const int __skink_cov_n __attribute__((weak));
extern const int __skink_covmap_len __attribute__((weak));
extern const char __skink_covmap[] __attribute__((weak));

/* Generous static bound; .bss so uninstrumented entries cost nothing.
 * Programs with more probes than this are pathological (>1M source
 * statements). */
#define SKINK_COV_MAX (1u << 20)
static unsigned int skink_cov_counts[SKINK_COV_MAX];

void Skink_cov_hit(int id) {
    if (id >= 0 && (unsigned int)id < SKINK_COV_MAX) {
        __atomic_fetch_add(&skink_cov_counts[id], 1u, __ATOMIC_RELAXED);
    }
}

/* Output path: $SKINK_COV_OUT wins, else "<exe>.skinkcov". */
static const char *skink_cov_outpath(char *buf, size_t buflen) {
    const char *env = getenv("SKINK_COV_OUT");
    if (env && env[0]) return env;
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *base = "program";
    if (n > 0) {
        exe[n] = 0;
        const char *slash = strrchr(exe, '/');
        if (slash && slash[1]) base = slash + 1;
    }
    snprintf(buf, buflen, "%s.skinkcov", base);
    return buf;
}

static void skink_cov_dump(void) {
    /* Uninstrumented binary: weak symbols resolved to NULL. */
    if (!&__skink_cov_n || !&__skink_covmap_len || !__skink_covmap) {
        return;
    }
    int n = __skink_cov_n;
    if (n < 0) n = 0;
    if ((unsigned int)n > SKINK_COV_MAX) n = SKINK_COV_MAX;
    char pathbuf[4096];
    const char *path = skink_cov_outpath(pathbuf, sizeof(pathbuf));
    // O_NOFOLLOW: the coverage file must never clobber a symlink target —
    // in an attacker-writable directory a planted symlink would redirect
    // the dump to an arbitrary file (comp_error.md #153). Fall back to
    // fopen when O_NOFOLLOW is unavailable.
    FILE *f = NULL;
#if defined(O_NOFOLLOW) && !defined(_WIN32)
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
    if (fd >= 0) {
        f = fdopen(fd, "w");
        if (!f)
            close(fd);
    }
#endif
    if (!f)
        f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "skink-cov: cannot write %s\n", path);
        return;
    }
    fprintf(f, "SKINKCOV1\n");
    fwrite(__skink_covmap, 1, (size_t)__skink_covmap_len, f);
    fprintf(f, "COUNTS\n");
    for (int i = 0; i < n; i++) {
        fprintf(f, "%u\n", skink_cov_counts[i]);
    }
    fclose(f);
}

__attribute__((constructor))
static void skink_cov_register(void) {
    atexit(skink_cov_dump);
}
