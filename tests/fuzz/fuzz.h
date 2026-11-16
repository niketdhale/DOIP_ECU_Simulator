/**
 * @file fuzz.h
 * @brief Shared declarations for the fuzz targets.
 *
 * Every target is a libFuzzer entry point (LLVMFuzzerTestOneInput) plus a
 * small table of seed inputs. With clang the targets link against libFuzzer;
 * with any other compiler tests/fuzz/standalone.c supplies main() and a
 * simple mutator driven by those seeds.
 */
#ifndef FUZZ_H
#define FUZZ_H

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

struct fuzz_seed { const uint8_t *data; size_t len; };
extern const struct fuzz_seed fuzz_seeds[];
extern const size_t           fuzz_seed_count;

#define FUZZ_SEED(...) { (const uint8_t[]){ __VA_ARGS__ }, sizeof((const uint8_t[]){ __VA_ARGS__ }) }

/* A violated invariant must stop the run even without sanitizers */
#define FUZZ_ASSERT(cond) do { if (!(cond)) __builtin_trap(); } while (0)

#endif /* FUZZ_H */
