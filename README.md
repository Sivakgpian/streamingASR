# streamingasr

CPU-first streaming ASR engine in C++20. See `CLAUDE.md` for architecture and roadmap.

## Build (WSL2 / Linux)

Requires CMake >= 3.25, Ninja, GCC and/or Clang with C++20 support. Dependencies
(GoogleTest, Google Benchmark) are fetched automatically and pinned by hash.

```sh
cmake --workflow --preset clang-debug     # configure + build + test
```

| Preset          | Compiler | Build type | Sanitizers        |
|-----------------|----------|------------|-------------------|
| `gcc-debug`     | GCC      | Debug      | –                 |
| `gcc-release`   | GCC      | Release    | –                 |
| `gcc-asan`      | GCC      | Debug      | ASan + UBSan      |
| `gcc-tsan`      | GCC      | Debug      | TSan              |
| `clang-debug`   | Clang    | Debug      | –                 |
| `clang-release` | Clang    | Release    | –                 |
| `clang-asan`    | Clang    | Debug      | ASan + UBSan      |
| `clang-tsan`    | Clang    | Debug      | TSan              |

Output goes to `build/<preset>/`. Step by step:

```sh
cmake --preset clang-release
cmake --build --preset clang-release
ctest --preset clang-release
./build/clang-release/benchmarks/sasr_common_bench   # benchmarks: use a release preset
./build/clang-release/examples/sasr_version
```

Sanitizer presets also build *canary* tests (`ctest -L canary`): programs with
deliberate bugs that pass only if the sanitizer reports them.

## Options

| Option                    | Default | Meaning                                         |
|---------------------------|---------|-------------------------------------------------|
| `SASR_WARNINGS_AS_ERRORS` | `ON`    | `-Werror` on project targets (never on deps)    |
| `SASR_SANITIZER`          | empty   | `address,undefined` or `thread`                 |
| `SASR_FRAME_POINTERS`     | `ON`    | `-fno-omit-frame-pointer` for `perf` stacks     |
| `SASR_BUILD_TESTS` / `_BENCHMARKS` / `_EXAMPLES` | `ON` | Toggle subtrees             |

## Adding a module

1. `src/<module>/CMakeLists.txt`: `add_library(sasr_<module>)`, alias `sasr::<module>`,
   include dir `${PROJECT_SOURCE_DIR}/src` (PUBLIC), explicit `target_link_libraries`
   for dependencies, and `sasr_set_warnings(sasr_<module>)`.
2. `add_subdirectory(<module>)` in `src/CMakeLists.txt`.
3. Tests: `tests/<module>/` with `sasr_add_test(...)`; benchmarks: `sasr_add_benchmark(...)`.
