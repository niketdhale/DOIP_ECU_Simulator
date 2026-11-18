/**
 * @file standalone.c
 * @brief main() for the fuzz targets on toolchains without libFuzzer.
 *
 *   ./fuzz-uds                 FUZZ_RUNS (default 200000) mutated inputs;
 *                              deterministic for a given FUZZ_SEED (default 1)
 *   ./fuzz-uds crash.bin ...   replay inputs, e.g. a saved crash
 *
 * It is a blind mutator: no coverage feedback. Build the same targets with
 * `make fuzz FUZZ_ENGINE=libfuzzer` (clang) for coverage-guided fuzzing.
 * When a sanitizer aborts, the offending input is written to
 * crash-<program>.bin so it can be replayed.
 */
#include "fuzz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INPUT 8192

static uint8_t     s_input[MAX_INPUT];
static size_t      s_len;
static const char *s_prog = "fuzz";

/* Provided by the sanitizer runtime when there is one */
__attribute__((weak)) void __sanitizer_set_death_callback(void (*callback)(void));

static void save_crash_input(void)
{
    char path[256];
    snprintf(path, sizeof(path), "crash-%s.bin", s_prog);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(s_input, 1, s_len, f);
    fclose(f);
    fprintf(stderr, "\nfuzz: input saved to %s (replay with: ./%s %s)\n", path, s_prog, path);
}

/* xorshift64*: small, fast, and identical on every platform */
static uint64_t s_rng;
static uint32_t rnd(void)
{
    s_rng ^= s_rng >> 12; s_rng ^= s_rng << 25; s_rng ^= s_rng >> 27;
    return (uint32_t)((s_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

static void mutate(void)
{
    static const uint8_t interesting[] = { 0x00, 0x01, 0x02, 0x03, 0x07, 0x08, 0x0E, 0x10, 0x20, 0x30,
                                           0x3E, 0x7E, 0x7F, 0x80, 0x81, 0xF1, 0xFC, 0xFD, 0xFE, 0xFF };
    size_t pos = s_len ? rnd() % s_len : 0;
    switch (rnd() % 7) {
        case 0: if (s_len) s_input[pos] ^= (uint8_t)(1U << (rnd() % 8)); break;
        case 1: if (s_len) s_input[pos] = (uint8_t)rnd(); break;
        case 2: if (s_len) s_input[pos] = interesting[rnd() % sizeof(interesting)]; break;
        case 3: s_len = pos; break;                                        /* truncate */
        case 4: {                                                          /* insert random bytes */
            size_t n = 1 + rnd() % 8;
            if (s_len + n > MAX_INPUT) break;
            memmove(s_input + pos + n, s_input + pos, s_len - pos);
            for (size_t i = 0; i < n; i++) s_input[pos + i] = (uint8_t)rnd();
            s_len += n;
            break;
        }
        case 5: {                                                          /* repeat a slice at the end */
            if (!s_len) break;
            size_t n = 1 + rnd() % (s_len - pos);
            if (s_len + n > MAX_INPUT) break;
            memmove(s_input + s_len, s_input + pos, n);
            s_len += n;
            break;
        }
        default: {                                                         /* append another seed */
            if (!fuzz_seed_count) break;
            const struct fuzz_seed *s = &fuzz_seeds[rnd() % fuzz_seed_count];
            if (s_len + s->len > MAX_INPUT) break;
            memcpy(s_input + s_len, s->data, s->len);
            s_len += s->len;
            break;
        }
    }
}

int main(int argc, char **argv)
{
    const char *slash = strrchr(argv[0], '/');
    s_prog = slash ? slash + 1 : argv[0];
    if (__sanitizer_set_death_callback) __sanitizer_set_death_callback(save_crash_input);

    if (argc > 1) {                                   /* replay mode */
        for (int i = 1; i < argc; i++) {
            FILE *f = fopen(argv[i], "rb");
            if (!f) { perror(argv[i]); return 1; }
            s_len = fread(s_input, 1, sizeof(s_input), f);
            fclose(f);
            LLVMFuzzerTestOneInput(s_input, s_len);
            printf("%s: %s (%zu bytes) ok\n", s_prog, argv[i], s_len);
        }
        return 0;
    }

    const char *env  = getenv("FUZZ_RUNS");
    unsigned long runs = env ? strtoul(env, NULL, 10) : 200000UL;
    env   = getenv("FUZZ_SEED");
    s_rng = (env ? strtoull(env, NULL, 10) : 1ULL) * 0x9E3779B97F4A7C15ULL + 1;

    for (size_t i = 0; i < fuzz_seed_count; i++)      /* every seed once, unmodified */
        LLVMFuzzerTestOneInput(fuzz_seeds[i].data, fuzz_seeds[i].len);

    for (unsigned long i = 0; i < runs; i++) {
        if (fuzz_seed_count && rnd() % 16 != 0) {     /* start from a seed... */
            const struct fuzz_seed *s = &fuzz_seeds[rnd() % fuzz_seed_count];
            s_len = s->len < MAX_INPUT ? s->len : MAX_INPUT;
            memcpy(s_input, s->data, s_len);
        } else {                                      /* ...or from noise */
            s_len = rnd() % 64;
            for (size_t k = 0; k < s_len; k++) s_input[k] = (uint8_t)rnd();
        }
        for (unsigned n = 1 + rnd() % 6; n > 0; n--) mutate();
        LLVMFuzzerTestOneInput(s_input, s_len);
    }
    printf("%s: %lu inputs, no crash\n", s_prog, runs);
    return 0;
}
