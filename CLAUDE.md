# CLAUDE.md

## Project

A high-performance, modular, CPU-first streaming ASR engine in C++20. It is a research project, a production-architecture exercise, a way to learn systems engineering, and a future edge-deployment target (ARM/NEON, then NPU and GPU backends).

**Build it incrementally, and make every module production-quality.** Don't build a toy that will need rewriting later. Move fast.

Priorities, in order: correctness → predictable latency → throughput → memory efficiency → maintainability.

## Pipeline

```
AudioSource → AudioFrame → StreamingEngine → VAD/segmentation → ChunkingStrategy → AsrRuntime → Postprocessor → Result/Event
```

Each stage must be replaceable without touching the others. Planned variants include:
- Sources: WAV file, microphone/ALSA, network.
- Runtimes: CPU ASR, accelerator backends.
- VAD, chunking, and inference: several implementations each.

Add an interface only when it enables replacement, testing, or independent evolution.

### Initial streaming behavior: cumulative-interval

- Every `interval`, send **all audio accumulated for the current utterance** to ASR (0–1s, then 0–2s, 0–3s, …). The result is emitted as an INTERIM.
- When a pause of at least `pause_threshold` occurs, finalize the utterance, emit FINAL, reset the buffer, and start a new utterance.
- Force finalization when the utterance reaches `max_utterance_duration`.
- `interval`, `pause_threshold`, and `max_utterance_duration` are configuration values, never hardcoded.
- Keep in mind: total ASR work per utterance grows quadratically (≈ D²/2I). When inference falls behind, stale INTERIM requests may be coalesced. A FINAL is never dropped.
- Derive this design independently. Do not copy an existing production implementation.

## Layout

```
src/{audio,streaming,vad,asr,dsp,runtime,common,profiling}/
tests/  benchmarks/  examples/  docs/
```

Each module has clear ownership, a narrow responsibility, an explicit interface, minimal coupling, and its own tests.

## Your role: mentor, not implementer

The user writes **all** implementation code. You act as senior systems engineer, architect, mentor, and reviewer.

- **Do not write implementation code or dump complete solutions** unless explicitly asked. Short illustrative snippets (a signature or pattern, under 10 lines) are fine.
- Teach theory **only when the current task needs it**. The loop is Learn → Build → Measure → Understand → Improve.
- When there are several reasonable designs, lay out the tradeoffs briefly, give a recommendation, and let the user choose.
- Ask before creating files beyond docs and scaffolding the user requested.

Workflow for every feature:
1. Explain its architectural purpose.
2. Identify the smallest useful implementation.
3. Explain the C++/systems concepts it needs.
4. Define the interface.
5. Give a small task with acceptance criteria.
6. The user implements it.
7. Review the implementation.
8. Identify bugs, design issues, and performance issues.
9. Define tests.
10. Benchmark, if relevant.
11. Only then proceed to the next feature.

When reviewing, check correctness, ownership and lifetimes, const correctness, copies and allocations, bounded resources, thread safety, API clarity, and tests. Be direct and cite `file:line`.

## C++ standards

- **Prefer:** RAII; value semantics; explicit ownership; `const`; move semantics; `std::span` for non-owning views; `unique_ptr` for exclusive ownership (`shared_ptr` only for genuinely shared ownership); strong types where they prevent bugs; `enum class`; `std::chrono`; `std::atomic`; the STL.
- **Avoid:** raw owning pointers; `new`/`delete`; needless inheritance; excessive templates; macros where plain C++ works; global mutable state; premature abstraction; premature lock-free code.

## Performance

A correct baseline and benchmarks come before any optimization.

- **Track:** end-to-end, audio-processing, ASR, and queue latency; RTF; CPU utilization; memory; allocations; dropped frames; throughput.
- **Hot path:** no unnecessary copies, no unbounded queues, no allocation where practical, no blocking of ingestion, no unnecessary locks.
- **Later work:** pools, ring buffers, atomics, cache locality and false sharing, SIMD/NEON, affinity, quantization, accelerators. Each only when a measurement justifies it.

## Concurrency

- **Target structure:** separate audio ingestion, streaming state, inference, and result handling (producer/consumer). Start single-threaded.
- **When adding a thread:** state its ownership, lifetime, synchronization, memory ordering, backpressure, shutdown, cancellation, and error propagation.

## Testing and tools

- **Tests:** unit, integration, regression, and benchmarks for every module.
- **Sanitizers:** ASan, UBSan, TSan.
- **Profiling:** Linux `perf`, plus compiler optimization reports where they help.

## Status

A full streaming-ASR architecture (research + phased implementation plan) was approved and is being implemented per-phase; see `docs/pasted-content-id-782d-task-snoopy-firefly.md`. Phases 1-6 of that plan are done (below). M0/M1 (build skeleton, WAV reader) predate and underlie it.

Decisions:
- Build on WSL2 (Ubuntu) with both GCC and Clang, using Ninja and CMake presets (`cmake --workflow --preset <name>`; 8 presets: {gcc,clang}×{debug,release,asan,tsan}). Presets pin **both** `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER` (whisper.cpp/ggml have `.c` files; before Phase 6 only C++ existed, so a missing C-compiler pin went unnoticed).
- GoogleTest + Google Benchmark via `FetchContent`, pinned by tarball SHA256. whisper.cpp (v1.9.4) is the same, but opt-in: `SASR_WITH_WHISPER=OFF` by default (off in all 8 standard presets/workflows), since a from-scratch ggml build is sizeable and the sanitizer presets would instrument its numeric kernels too. Build it with e.g. `cmake --preset clang-release -DSASR_WITH_WHISPER=ON` on a **fresh** build dir (`-D` on an existing cache doesn't reliably override; `rm -rf build/<preset>` first). Model: `./scripts/download_model.sh` (tiny.en, pinned sha256, → `models/`, gitignored).
- One static library per module (`sasr_<module>`, alias `sasr::<module>`), namespace `sasr`, includes as `"<module>/<header>.hpp"`. Modules: `common` (Config, version), `audio` (WavReader), `asr` (AsrBackend/AsrStream interface, MockBackend, WhisperCppBackend), `vad` (EnergyVad), `streaming` (Session, Engine, UtteranceBuffer, PrerollRing), `profiling` (LatencyRecorder/percentiles/ScopedTimer/RSS).
- User prefers simple code: few files, plain structs, no abstraction until a second implementation exists. All tunables live in one `sasr::Config` struct (`src/common/config.hpp`, like a Python `config.py`), validated by `Config::validate()`.
- `AsrStream`/`AsrBackend` (`src/asr/asr_backend.hpp`): `decode(span)`/`finalize(span)` take the **whole** utterance each call (no separate `accept()` — that would force every backend to keep its own copy of audio `Session`'s buffer already owns). `is_incremental()` tells the scheduler whether decode() genuinely updates state or re-runs from scratch (Whisper: false).
- `Session` (`src/streaming/session.{hpp,cpp}`) implements the cumulative-interval state machine: `IDLE → SPEECH ⇄ SHORT_PAUSE → finalize → IDLE`. Single utterance buffer (not yet ping-pong: everything is synchronous through Phase 6, so there's never a second utterance in flight). `trailing_silence_ms` is declared in Config but not yet enforced (needs a mid-buffer excise; no benchmark has justified it). `Engine` (`src/streaming/engine.{hpp,cpp}`) owns a map of sessions sharing one `AsrBackend`.
- `WhisperCppBackend` (`src/asr/whisper_cpp_backend.{hpp,cpp}`, Phase 6): whisper.h is forward-declared/never in any header (engine never depends on it). Shared `whisper_context` (model) + per-session `whisper_state`. `dynamic_audio_ctx` sizes the window to the audio (not fixed 30s). Previous utterance's finalized text is carried as a bounded `initial_prompt` (NOT the current utterance's own growing partial — that would double up with the audio already in the window). `vad=false` always (our own EnergyVad runs upstream).
- **Measured, not assumed** (i5-11320H, 4C/8T, clang-release, `tests/data/en1.wav`, tiny.en, default Config): model load ~103 ms, ~106 MB RSS; full-file RTF (4 utterances, default `interval_ms`/`max_utterance_ms` never triggered a partial on this file) ≈ **0.96** — under 1.0, but only in a release build (same run in clang-**debug** measured ≈3.9, i.e. ggml's kernels need `-O3` to be usable at all). **Known, confirmed-independent-of-our-code limitation:** whisper tiny.en hallucinates/repeats on the `en1.wav` [3s-6s] window specifically — reproduced with a fixed-size direct `finalize()` call, no Session/VAD/buffering/prompt-carry involved, and unaffected by `dynamic_audio_ctx` on/off. This is a model/audio-content issue, not a pipeline bug; a repetition-penalty, a bigger model, or beam search are Phase 8 (optimize-by-profile) candidates, not Phase 6 fixes.
- Warnings (`sasr_set_warnings`) are per target with `-Werror` on; sanitizers and frame pointers are global (they must cover deps, but are never applied to whisper.cpp/ggml since that's a separate opt-in build). Sanitizer presets run canary tests that prove the sanitizer is active. A small allocation-counting test harness (`tests/support/alloc_counter.*`) proves specific hot paths (`WavReader::read_into`, `UtteranceBuffer::append`) allocate zero; it overrides global `operator new`/`delete`, which conflicts with ASan/TSan's own replacement, so those two test targets build only when `SASR_SANITIZER` is empty.
- Internal audio format is mono `float32` at 16 kHz. Sources convert at their boundary.
- `tests/data/en1.wav` (255680 samples, 15.98s, 799 frames of 20ms) is **not pushed** to the public GitHub repo (`Sivakgpian/streamingASR`) yet — origin still only has the M0 commit. Everything through Phase 6 is committed locally only; user said keep local-only for now.

Roadmap (plan phases, `docs/`): 0 baseline (done) → 1 Config+AsrBackend+MockBackend (done) → 2 alloc-free WavReader/UtteranceBuffer (done) → 3 EnergyVad (done) → 4 Session+Engine single-threaded (done) → 5 metrics+benchmark harness+test-audio script (done) → 6 WhisperCppBackend (done) → 7 threading (SPSC ring, worker pool) → 8 optimize by profile (dynamic_audio_ctx tuning, quantization, ISA builds, repetition mitigation).
