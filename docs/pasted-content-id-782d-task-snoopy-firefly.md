# Streaming ASR: Edge/CPU Architecture Research and Implementation Plan

## Context

The user wants a production-quality, CPU-first, low-latency streaming ASR pipeline in C++20.
- Targets: ARM64 embedded, Snapdragon-class and Jetson-class devices, and x86.
- First model: Whisper Tiny. The backend must be replaceable.
- This document is research and design only. **No repository file was created, modified or deleted.** The only file written is this plan.
- The deliverable is an architecture, a phased plan, and a self-contained prompt for a later implementation session. Implementation waits for explicit approval.

**Legend.** Every claim is tagged so assumptions are not mistaken for facts:
- **[O]** observed in the repository
- **[M]** measured on this machine during this session
- **[A]** assumption or external claim not yet verified here
- **[R]** recommendation

### Preconditions the user must resolve before implementation
1. **[O] Uncommitted work.** M1 changes are uncommitted: the `WavReader` rewrite, `config.hpp`, `run.sh`, `tests/data/en1.wav`, and the CLAUDE.md status edits. The implementation session must start from a commit of this state.
2. **[O] Unpublished test audio.** `en1.wav` hasn't been cleared for publication to the public GitHub repo. Its license/origin is still unknown.
3. **[O] Role conflict.** CLAUDE.md says "the user writes all implementation code; Claude mentors". The final prompt below assumes the user explicitly authorizes Claude to implement. If not, it can be used phase by phase as a teaching plan instead.

---

## A. Existing repository architecture [O]

| Area | Fact |
|---|---|
| Build | CMake ≥ 3.25 + Ninja, `CMakePresets.json` with 8 presets ({gcc,clang}×{debug,release,asan,tsan}) and workflow presets |
| Language | C++20, `CMAKE_CXX_EXTENSIONS OFF` |
| Compilers | GCC 15.2, Clang 21.1.8 (Ubuntu, WSL2) |
| Release flags | `-O3 -DNDEBUG` plus global `-fno-omit-frame-pointer`. **No `-march`, no LTO, no PGO.** Verified in `build/clang-release/CMakeCache.txt` [M] |
| Warnings | `cmake/Warnings.cmake` → `sasr_set_warnings(target)`: strict set + `-Werror`, applied per target |
| Sanitizers | `cmake/Sanitizers.cmake`: global ASan+UBSan or TSan, with canary tests in `tests/sanitizer_canary/` |
| Dependencies | `cmake/Dependencies.cmake`: GoogleTest 1.17.0 and Google Benchmark 1.9.4 via FetchContent, pinned by URL + SHA256 |
| Platforms | Only x86-64 has been built. No ARM64 toolchain or preset. `static_assert(little-endian)` in `wav_reader.cpp` (ARM64 Linux is little-endian, so it's portable) |
| Modules | `src/common` (`version.{hpp,cpp}`, `config.hpp`) and `src/audio` (`wav_reader.{hpp,cpp}`). **`streaming/`, `vad/`, `asr/`, `dsp/`, `runtime/` and `profiling/` don't exist yet** |
| Key types | `sasr::Config {sample_rate=16000, frame_ms=20, frame_samples()}`; `sasr::AudioFrame {std::vector<float> samples; int64_t start_sample}`; `sasr::WavReader(path, Config)` with `std::optional<AudioFrame> read_frame()` and `total_samples()`; `sasr::WavError` |
| Tests | `tests/audio/wav_reader_test.cpp` (9 tests, including real-file `en1.wav`: 255,680 samples, 799 frames) and `tests/common/version_test.cpp` (2). All pass on 8 presets [M, earlier this session] |
| Benchmarks | `benchmarks/common/version_bench.cpp` only, a placeholder |
| Tools | `examples/wav_info.cpp` (`sasr_wav_info`); `run.sh` builds and runs it |
| Design doc | CLAUDE.md specifies the **cumulative-interval** strategy: every `interval`, send all audio of the current utterance to ASR as INTERIM; FINAL on pause ≥ `pause_threshold` or at `max_utterance_duration`; stale INTERIMs may be coalesced; FINAL is never dropped |

**SIMD/math [O]:** no intrinsics, no BLAS, Eigen, oneDNN or XNNPACK, no FFT. The only numeric loop is the int16→float conversion in `WavReader::read_frame` (`wav_reader.cpp:106-108`), which is branch-free and auto-vectorizable.

**Threading [O]:** none. No threads, mutexes, atomics, queues, affinity or priority settings. Everything is single-threaded and synchronous. That's fine for M1, but there's nothing real-time yet: no live source and no pacing.

**Audio [O]:**
- Input: PCM WAV, format 1, mono, 16-bit, 16 kHz. Anything else is rejected, with no resampling.
- Internal format: float32 in [-1, 1) via `/ 32768`.
- Frames: 20 ms (320 samples).
- No ring buffer, normalization or preprocessing. The only source is a file.

**Machine [M]:**
- CPU: i5-11320H (Tiger Lake), 4 cores / 8 threads, AVX2 + FMA + AVX-512F/BW/VL + AVX-512-VNNI. L2 5 MiB, L3 8 MiB, 1 NUMA node.
- RAM: **3.7 GiB visible to WSL2.**
- Missing tools: perf, heaptrack, valgrind, gdb, strace, hyperfine, ffmpeg, pip and numpy are not installed.
- No ASR runtime is installed (no whisper.cpp, ONNX Runtime or sherpa-onnx).

## B. Current data flow [O + M]

```
en1.wav (disk/page cache)
  └─ kernel read → std::filebuf internal buffer (~8 KiB, libstdc++)        copy #1
       └─ ifstream::read 640 B → WavReader::scratch_ (int16, reused)         copy #2
            └─ convert loop → AudioFrame::samples (new vector, 1280 B)       copy #3 + 1 malloc
                 └─ return std::optional<AudioFrame> (move, no copy)
                      └─ caller (wav_info: counts frames; frame destroyed → 1 free)
```

| Stage | Input | Output | Alloc | Copy | Lock | Size | Hot path |
|---|---|---|---|---|---|---|---|
| Header parse (constructor) | file | `WavReader` state | `scratch_` once; `std::string` ids (SSO, no heap) | small | – | ~70 B header | no |
| `ifstream::read` | page cache | filebuf → `scratch_` | 0 | 2 (kernel→filebuf, filebuf→scratch) | – | 640 B/frame | yes |
| int16→float | `scratch_` | `frame.samples` | **1 heap alloc/frame** | 1 (conversion) | – | 1280 B/frame | yes |
| `optional<AudioFrame>` return | frame | caller | 0 | 0 (move) | – | 32 B | yes |
| Consumer | frame | – | **1 free/frame** | – | – | – | yes |

- **Copies:** 3 per frame (2 byte copies + 1 conversion pass).
- **Allocations:** 1 malloc + 1 free per frame = 50/s at 20 ms frames.
- **Locks:** none.
- **Measured [M]:** `sasr_wav_info en1.wav` averages ≈2.07 ms per process over 50 runs, against ≈1.10 ms for `/bin/true`. So the whole 15.98 s file costs **≈1 ms**: WAV-path RTF ≈ 6×10⁻⁵. This is crude wall-clock timing, not a microbenchmark, but it shows the audio path is **not** a bottleneck. ASR will dominate by 3–4 orders of magnitude [A].
- **Memory [M]:** `sizeof(AudioFrame)` = 32 B, heap per frame 1280 B, `sizeof(WavReader)` = 560 B.

## C. Bottlenecks and architectural problems (ranked)

**P0: critical, design-level**
1. **[O+R] Whisper with cumulative-interval does quadratic re-encoding.** Whisper's encoder uses full bidirectional attention over a fixed 30 s, 1500-position window, so each INTERIM re-encodes the whole utterance from scratch. Neither encoder states nor mel can be reused across partials by the model itself.
   - Total encoded audio per utterance of length D at interval I is ≈ D²/(2I) with dynamic `audio_ctx`, and ≈ (D/I)·30 s with default 30 s padding.
   - Example, D = 10 s, I = 1 s: 55 s encoded with dynamic context (5.5×), 300 s with fixed padding (30×).
   - The pipeline must therefore:
     - bound utterances (`max_utterance_ms`);
     - size `audio_ctx` from the window [A: ~3× speedup on short clips reported, with repetition/hallucination risk if pushed too far];
     - coalesce partials;
     - allow partials to be disabled on weak hardware.
2. **[O+R] "Never reprocess confirmed audio" can't hold *inside* an utterance with Whisper.** It holds at utterance/commit granularity: after FINAL, the audio is never sent again. Within an utterance, every partial recomputes. Truly incremental partials need a streaming encoder (see F).
3. **[R] Endpointing dominates final latency.** FINAL can't be emitted before the pause is detected: final latency ≥ `pause_threshold` + final inference. VAD/endpoint tuning matters more than model speed for perceived latency.

**P1: important**
4. **[O]** No VAD, so silence would reach ASR. Whisper hallucinates on silence or noise [A, widely reported].
5. **[O]** No metrics or benchmark harness. RTF and p50–p99 can't be claimed until it exists.
6. **[O]** One heap allocation per frame (`AudioFrame` owns a fresh vector). That's harmless at file speed, but it violates "no allocation in the audio hot path" once a live source runs on a capture thread.
7. **[O]** No ARM64 build preset, so portability is untested.

**P2: optimization opportunities, unproven**
8. **[O]** The filebuf double copy (copy #1→#2). It's negligible (≈1 ms per 16 s of audio [M]); fix only if profiled.
9. **[O]** No `-march` baseline, LTO or PGO. Our own code is <1% of the expected runtime [A], and ggml's kernels pick their own SIMD.

### Phase 3 findings: can components share formats?

None of these components exist yet [O]. Requirements of the candidates [A, from model and runtime documentation]:

| Consumer | Input | Frame/hop |
|---|---|---|
| Whisper (all impls) | 80-bin log-mel (128 for large-v3), 25 ms window, **10 ms hop**, normalized by the **window-global max** (`max(x, max−8)`) | 30 s window |
| Silero VAD v5 | float32 PCM 16 kHz | 512-sample (32 ms) windows |
| Energy VAD | float32 PCM | any; use 10/20 ms |
| Zipformer transducer (sherpa-onnx) | 80-dim Kaldi fbank, 10 ms hop | chunks of 16–64 feature frames |
| Moonshine | **raw PCM** | – |

**[R]:**
- **The shared currency is mono float32 PCM at 16 kHz.** Every candidate consumes 16 kHz, so there is **no internal resampling**. Resample only at the source boundary, when a non-16 kHz source exists (ALSA at 48 kHz in M6).
- **Feature extraction belongs inside the ASR backend, not the shared pipeline.** Whisper mel, Kaldi fbank and Moonshine raw PCM are mutually incompatible, so a shared feature stage would couple the pipeline to Whisper.
- Whisper's per-window normalization means precomputed mel can be cached (the raw log10 values per 10 ms hop), but normalization must be re-applied per window. That's cheap: O(80·frames).
- Keep the engine frame at **20 ms**, a multiple of the 10 ms hop. Silero's 512-sample window is handled by a small preallocated accumulator inside the Silero VAD adapter.

---

## D. Recommended edge architecture

### D.1 Pipeline

```
 Source (WAV / synthetic / later ALSA, network)        [convert to f32 16 kHz at boundary]
   │  push(span<const float>)  ── no AudioSource interface needed: sources are just callers
   ▼
 ┌────────────────── per session ──────────────────────────────────────────────┐
 │ SPSC ring (bounded, prealloc; added in M4 when capture gets its own thread) │
 │   ▼                                                                         │
 │ VAD (energy first; Silero later if the noisy benchmark demands it)          │
 │   ▼                                                                         │
 │ Endpointer state machine (IDLE/SPEECH/SHORT_PAUSE/LONG_PAUSE/FINALIZE)      │
 │   ▼ copy 20 ms frame → utterance buffer (prealloc, contiguous)              │
 │ Utterance buffers ×2 (ping-pong; capacity = preroll + max_utterance)        │
 │   │ schedule: at most 1 job in flight per session; partials coalesced       │
 └───┼─────────────────────────────────────────────────────────────────────────┘
     ▼ job = {session, span<const float> over utterance buffer [0,n), kind}   (zero-copy)
 Inference workers (W threads, each backend call uses T intra-op threads; W·T ≤ cores)
     ▼  AsrStream::accept/decode/finalize   (features + model are backend-internal)
 Result events {session, kind=PARTIAL|FINAL|ERROR, text, audio span, timings}
     ▼  bounded queue → consumer callback
```

### D.2 ASR abstraction (smallest practical) [R]

Whisper is **window-based** (window in, text out). Streaming transducers and Moonshine v2 are **chunk-based** (chunk in, incremental tokens out, with cached state). One stream-shaped interface covers both: the Whisper backend implements `decode()` by re-running its window, and streaming backends implement it natively. The engine never needs to know which kind it has.

```cpp
struct AsrResult { std::string text; bool is_final; /* + timings */ };
class AsrStream {               // per session; owns model state, preallocated
public:
    virtual ~AsrStream() = default;
    virtual void accept(std::span<const float> pcm16k) = 0;  // append; no alloc in steady state
    virtual AsrResult decode() = 0;    // partial over everything accepted so far
    virtual AsrResult finalize() = 0;  // flush, final result
    virtual void reset() = 0;          // next utterance; never re-sees old audio
};
class AsrBackend {              // shared; holds weights once
public:
    virtual ~AsrBackend() = default;
    virtual std::unique_ptr<AsrStream> create_stream() = 0;
    virtual bool is_incremental() const = 0;   // lets the scheduler choose partial cadence
};
```

- Backends: `MockBackend` (tests, deterministic delays), `WhisperCppBackend` (M3), and later `SherpaOnnxBackend`, a QNN-backed backend, and a TensorRT backend.
- The Whisper backend keeps a `span` view into the session's utterance buffer, so `accept` is zero-copy. **[A to verify]** whisper.cpp's context/state split (`whisper_init_*_no_state` + `whisper_init_state` + `whisper_full_with_state`) gives shared weights with per-stream state.
- Text results allocate a `std::string`. That's acceptable: a few per second, off the audio path, and counted in the benchmarks.

### D.3 Per-session state machine [R]

States: `IDLE → SPEECH ⇄ SHORT_PAUSE → LONG_PAUSE → FINALIZE → RESET → IDLE`

| Event | Behavior |
|---|---|
| Speech onset (VAD above threshold for `min_speech_ms`) | IDLE→SPEECH; copy `preroll_ms` of pre-speech audio from a small preroll ring into the utterance buffer so onsets aren't clipped |
| Continuous speech | Append frames. Every `interval_ms`, if new audio exists **and no job is in flight**, submit a PARTIAL. Otherwise coalesce (latest wins) |
| Short pause (silence < `pause_threshold_ms`) | SHORT_PAUSE: keep appending, and keep up to `trailing_silence_ms` |
| Long pause (≥ `pause_threshold_ms`) | LONG_PAUSE→FINALIZE: cancel/await the in-flight partial (whisper.cpp abort callback [A]), submit FINAL (never dropped), swap the ping-pong buffer, RESET→IDLE |
| `max_utterance_ms` reached | Forced FINALIZE (initially at the frame boundary; later at the lowest-VAD-probability frame in the last N ms if the benchmarks show word cuts) |
| EOS during speech | FINALIZE and flush |
| EOS during silence/IDLE | RESET only, **no inference** |
| Silence-only input | Never leaves IDLE, so 0 ASR calls (asserted by test) |
| Model error / inference failure | Emit an ERROR event for that utterance, RESET the session, keep the engine alive, count it in metrics |
| Ring overflow (live) | Drop the newest frames, count `dropped_frames`, mark a timeline gap (`start_sample` discontinuity) |
| Ring overflow (file) | Block the producer (backpressure) |
| Both utterance buffers busy | Live: drop and count. File: block. Memory stays bounded at 2 × buffer per session |
| Session reset (API) | Abort the in-flight job, clear buffers, return to IDLE |

**Stable tokens [R]:**
- Start without LocalAgreement.
- Measure **partial flicker**: characters rewritten per partial, and time-to-stable.
- Only if flicker is high, add LocalAgreement-2-lite: emit the longest common prefix of the last 2 partials as "stable", which is O(tokens), with no buffer trimming.
- Full whisper_streaming-style buffer trimming is only worth considering if the long-continuous-speech benchmark shows forced cuts hurting WER.

---

## E. CPU optimization strategy

| Priority | Item | Rationale |
|---|---|---|
| **High** | Build ggml/whisper.cpp with an explicit ISA baseline per target: x86 `x86-64-v3` (AVX2/FMA), optional AVX-512 variant; ARM64 `armv8.2-a+dotprod+fp16` (+`i8mm` where present). Use `GGML_NATIVE=ON` only for local benchmarking | The model kernels are >95% of compute [A]; portable binaries need explicit flags |
| **High** | Inference threads = physical cores (not SMT siblings), with W·T ≤ cores, leaving a core for capture/engine on ≤ 4-core devices | Oversubscription destroys p99 |
| **High** | Model quantization: compare f16 vs q8_0 vs q5_1 on WER and latency | Smaller weights mean less memory bandwidth, which is the edge bottleneck [A] |
| **High** | Decoding: greedy (beam 1) for partials; **disable temperature fallback for partials** | Fallback re-decodes, creating p99 spikes [A] |
| **High** | Dynamic `audio_ctx` sized from the window (rounded up to a multiple of 64), behind a config flag, with a regression test for repetition | Largest single win for short windows [A: ~3× reported] |
| Optional, measure | AVX-512 vs AVX2 build of ggml on Tiger Lake | Can win or lose (downclocking) |
| Optional | CPU affinity: pin inference to big/prime cores on big.LITTLE (Snapdragon, some Jetsons) | Heterogeneous cores make latency unpredictable without it |
| Optional | `SCHED_FIFO` / high priority for the live capture thread only (M6) | Prevents ALSA xruns; needs rtprio limits |
| Optional | LTO / PGO for our code | Our code is not the hot path; measure first |
| Optional | Incremental mel cache via `whisper_set_mel_with_state` [A: API to verify] | Only if whisper.cpp's reported mel time is >10% of partial latency |
| Unnecessary until profiled | Manual SIMD/NEON, lock-free job queues, custom allocators, NUMA pinning (single-socket targets), `-ffast-math` (breaks NaN checks and determinism; ggml sets its own flags) | Requires evidence |
| Always | SPSC ring indices on separate cache lines (`alignas(64)`) | Avoids false sharing; free |

**x86:** AVX2 baseline; AVX-512/VNNI helps int8 dot products [A]. Concurrency on 4C/8T: 1 worker × 4 threads (latency), or 2 × 2 (throughput).

**ARM64:** NEON is guaranteed. dotprod (ARMv8.2+) and i8mm (ARMv8.6+) matter for quantized matmul [A]. Memory bandwidth is usually the limit, so prefer q5/q8 weights and fewer threads than cores if scaling flattens (measure the thread-scaling curve).

### Hardware tiers (expectations to be verified by benchmark) [A]

| Target | Threads | Model | Streams | Notes |
|---|---|---|---|---|
| 2-core | 1 worker × 2 | tiny q5_1/q8_0 | 1 | interval ≥ 2 s or finals-only; capture shares a core |
| 4-core | 1 × 3–4 | tiny q8_0 | 1–2 | the default dev profile (this machine) |
| 8-core | 2 × 4 or 4 × 2 | tiny/base | 2–4 | choose by p95-final vs throughput |
| ARM64 low-power (A55/A76, RPi-class) | 1 × 4 | tiny q5/q8 | 1 | RPi 5 reported ≈ real-time for tiny/base [A, secondary sources] |
| Snapdragon | big cores pinned | tiny q8 | 1–2 | QNN pays off when CPU or battery is the bottleneck. NPU wants static shapes, which fits streaming Zipformer chunks better than Whisper's dynamic window. sherpa-onnx supports QNN for streaming Zipformer [A: changelog v1.12.18/v1.13.3] |
| Jetson | CPU first; then ggml CUDA (same backend, build flag) | tiny/base | 2–4 | TensorRT only if ggml-CUDA misses targets; per-device engines add build complexity |
| x86 laptop/server | cores/T workers | tiny→small | many | AVX2/AVX-512; scale by sessions per worker |

The design is never GPU-dependent: GPU/NPU are build options of a backend, never pipeline changes.

---

## F. Model/runtime recommendation

| Runtime | CPU | ARM64 | x86 | Streaming | Memory | Quant | Complexity | Embedded |
|---|---|---|---|---|---|---|---|---|
| **whisper.cpp (ggml)** | NEON/AVX/AVX-512 | ✔ | ✔ | Window re-run only (`stream` example is "naive" sliding window [A]) | tiny ≈ 75 MiB on disk (f16), ~273 MB runtime per README [A] | q4/q5/q8 | Low: plain C/C++, no deps, MIT | ✔✔ |
| ONNX Runtime + Whisper ONNX | MLAS (AVX2/512, NEON, KleidiAI SME/i8mm in 1.29/1.30 [A]) | ✔ | ✔ | Window only; you write the decoder loop and KV handling | larger runtime | int8/int4 | Medium–high: you own the encoder/decoder glue | ✔ |
| sherpa-onnx (ORT-based) | via ORT | ✔ | ✔ | **True streaming** for Zipformer transducer/CTC; Whisper offline only | moderate | int8 | Low–medium: full ASR stack | ✔✔ (RPi, Android, RISC-V, NPUs) |
| TensorRT | GPU only | Jetson | dGPU | n/a | – | fp16/int8 | High: per-device engines | Jetson only |
| QNN (Qualcomm) | HTP NPU | Snapdragon | – | static shapes; chunked models fit | – | int8/int16 | High: offline compile per SoC | Snapdragon only |

**Whisper Tiny streaming requirements [R]:**
- VAD-gated utterances.
- Bounded window (≤ 15 s default, hard limit 30 s).
- Dynamic `audio_ctx`.
- Greedy partials, `no_timestamps` + `single_segment` for partials.
- Previous finalized text as the prompt (bounded tokens).
- No temperature fallback on partials.
- One job in flight per session, with coalescing.

**It does recompute for every partial.** That's inherent to the model, not to the implementation.

**Recommendations:**
1. **Initial: whisper.cpp + Whisper `tiny.en`** (`tiny` multilingual via config). No dependencies, MIT, the best CPU/ARM coverage, and GPU backends later via build flags. It also ships Silero VAD (ggml) [A], a possible second VAD with no ONNX dependency.
2. **Future streaming backend: sherpa-onnx streaming Zipformer transducer.** Chunked encoder with cached state means no recomputation, and it has a QNN path. Watch **Moonshine v2 streaming (tiny, 34M, MIT)**: sliding-window encoder, ~50 ms response on M3 [A: paper]. Its HF card says the efficient streaming path isn't in Transformers yet [A], so it isn't production-ready in C++ today.
3. **Hardware-specific:** ggml-CUDA on Jetson first, then TensorRT. On Snapdragon, sherpa-onnx QNN (streaming Zipformer) rather than porting Whisper to QNN.

**Honest latency statement [A, to be benchmarked]:**
- With Whisper Tiny, perceived FINAL latency ≈ `pause_threshold` (~0.5–0.7 s) + one window inference.
- PARTIAL latency ≈ interval/2 + inference.
- RTF < 0.5 for **finals-only** compute is plausible on this i5.
- **Total** RTF including partials grows with utterance length (D/I partials), and may exceed 0.5 on 2-core ARM unless the interval grows or partials are off.
- If sub-300 ms partials are required on low-power ARM, switch to a streaming transducer.

---

## G. Memory and copy analysis (proposed design)

| Buffer | Owner | Lifetime | Size (defaults) | Alloc | Notes |
|---|---|---|---|---|---|
| Source scratch (int16) | source | source lifetime | 640 B | once | existing `WavReader::scratch_` |
| Capture ring (f32) | session | session | 2 s = 128 KiB | once | M4 only; SPSC |
| Preroll ring | session | session | 300 ms = 19 KiB | once | |
| Utterance buffers ×2 | session | session | 2 × (0.3 + 15 s) × 64 KB/s ≈ 1.96 MiB | once | contiguous; zero-copy span to backend |
| VAD state | session | session | < 4 KiB (energy); Silero accumulator 2 KiB | once | |
| Backend stream state | `AsrStream` | session | **unknown; measure** (whisper state = KV cache + compute buffers) | once at create [A] | |
| Model weights | `AsrBackend` | process | tiny q8_0 ≈ 42 MiB, f16 ≈ 75 MiB [A] | once | shared across sessions |
| Result text | event | until consumed | ~100 B | per result (few/s) | the only steady-state allocation, off the audio path |

**Copies per 20 ms frame (audio path):**
1. kernel → scratch (file source; filebuf adds one more until optimized);
2. scratch → convert → ring (or straight into the engine in M2);
3. ring → utterance buffer.

That's **3 copies (~1.3 KB each), 0 heap allocations.** Backend-internal copies (mel, tensors) are measured, not assumed.

**API change for zero allocation:** add `WavReader::read_into(std::span<float> out) -> std::size_t`, and keep `read_frame()` for compatibility.

## H. Threading strategy (simplest with predictable latency) [R]

- **M2:** single thread, synchronous. `engine.push(frame)` runs VAD and the state machine, and calls the `MockBackend` inline. This gives deterministic tests.
- **M4 target:**
  - **Capture thread** per live source (none for files in benchmarks; a pacer simulates real time). Never blocks; writes into the SPSC ring.
  - **Engine thread** (one for all sessions): drains rings, VAD, state machine, buffer copies, scheduling, emitting events. VAD cost is assumed small (energy VAD in µs); measure Silero per session.
  - **Inference workers:** W threads, each owning nothing but borrowing a session's `AsrStream` per job. Job and completion queues are **mutex + condition_variable**, bounded (capacity = sessions + finals backlog). The job rate is about 10/s, so locks are fine; lock-free only on the 50 Hz × N audio ring, and only because the capture thread must never block.
- **Synchronization:** a job carries `{session, n_samples}` published with release/acquire semantics via the queue. Workers read `[0,n)` while the engine appends at `≥ n` (disjoint, no race), and no reset happens while a job is in flight (invariant).
- **Shutdown:** stop the sources, drain, finalize sessions (finals flushed), join the workers.
- **Errors:** exceptions are caught at the worker boundary and turned into ERROR events.

## I. Benchmark and profiling strategy

**Latency definitions** (per event; audio time is mapped to wall time via the pacer clock):
- frame processing (push → state updated)
- VAD per frame
- queue wait (job enqueued → started)
- backend breakdown from whisper.cpp timings (mel/encode/decode)
- **first-partial latency** (speech onset → first PARTIAL)
- **partial latency** (job audio end → event)
- **final latency** (end of speech in audio → FINAL emitted)

**System metrics:**
- **RTF**, reported two ways: final-only, and total including partials.
- CPU %: `getrusage` (user + sys) / wall.
- Peak RSS: `ru_maxrss`.
- Model memory: RSS after load minus baseline.
- Per-stream memory: ΔRSS per added session.
- Allocations/s: counting replacement of `operator new` in the **bench binary only**.
- Copies/frame: by design, documented and asserted via counters in debug.
- Queue depth (max/p95), dropped frames, coalesced partials, partial flicker, WER (needs a labeled set, e.g. a LibriSpeech test-clean subset, CC BY 4.0).

Report **p50/p90/p95/p99**, never only means. Use ≥ 30 runs or ≥ 200 events, with fixed threads, fixed frequency governor where possible, and a warm-up run.

**Matrix:**
- Inputs:
  1. silence
  2. short speech (1–2 s)
  3. continuous (`en1.wav`)
  4. speech + 300 ms pauses
  5. speech + 2 s pauses
  6. long utterance (`en1` × 4, 64 s, no pauses → forced cuts)
  7. noisy (white/pink noise at SNR 20/10/5 dB)
  8. sample rates 8/44.1/48 kHz (needs a boundary resampler, added only when M6 needs it; until then, expect a clean reject)
- Concurrency: × {1, 2, 4, 8} streams.
- Test data is generated by a stdlib-only Python script (no numpy installed [M]) into `build/`, not committed.

**Profiling tools (none installed today [M]):**

| Tool | Question it answers |
|---|---|
| `perf stat` | IPC, cache misses, branch misses: are we compute- or memory-bound? WSL2 may lack some PMU events; confirm on bare metal or the device |
| `perf record` + flamegraph | Where time goes: ggml kernels vs our code |
| heaptrack | Who allocates in steady state, and peak heap |
| valgrind massif/DHAT | Heap timeline; short-lived allocations; bytes copied/accessed |
| ASan/UBSan/TSan (existing presets) | Memory errors, UB, data races (ring, job handoff) |
| `-Rpass=loop-vectorize` / `-fopt-info-vec` | Did the conversion/energy loops vectorize? |
| `perf c2c` | False sharing (only if scaling flattens unexpectedly) |
| simpleperf / Snapdragon Profiler | Android/QNN timing |
| Nsight Systems + tegrastats | Jetson CPU/GPU overlap and power |

---

## J. Files that should change (do not modify now)

| File | Change |
|---|---|
| `src/common/config.hpp` | Add streaming (`interval_ms`, `pause_threshold_ms`, `max_utterance_ms`, `min_speech_ms`, `preroll_ms`, `trailing_silence_ms`, `partials_enabled`), VAD (`kind`, `threshold`), ASR (`backend`, `model_path`, `language`, `threads`, `workers`, `dynamic_audio_ctx`, `beam_size`), queue capacities. Add a `validate()` |
| `src/audio/wav_reader.{hpp,cpp}` | Add `read_into(span<float>)` (allocation-free); keep `read_frame()` |
| `src/vad/energy_vad.{hpp,cpp}` (new) | RMS/energy VAD with hysteresis; concrete class. A `Vad` interface only when Silero arrives |
| `src/streaming/utterance_buffer.hpp` (new) | Preallocated contiguous buffer with a preroll |
| `src/streaming/session.{hpp,cpp}` (new) | State machine (D.3), scheduling, coalescing |
| `src/streaming/engine.{hpp,cpp}` (new) | Owns sessions, the backend, the event sink; `push(session, span)`, `end_of_stream(session)` |
| `src/streaming/spsc_ring.hpp` (new, M4) | Bounded SPSC ring, `alignas(64)` indices |
| `src/asr/asr_backend.hpp` (new) | `AsrBackend` / `AsrStream` / `AsrResult` (D.2) |
| `src/asr/mock_backend.{hpp,cpp}` (new) | Deterministic text + configurable delay, for tests |
| `src/asr/whisper_cpp_backend.{hpp,cpp}` (new, M3) | whisper.cpp context/state, params per D.2/F |
| `src/profiling/metrics.{hpp,cpp}` (new) | Preallocated latency sample arrays, percentiles, counters |
| `cmake/Dependencies.cmake` | `SASR_WITH_WHISPER` option; FetchContent whisper.cpp pinned (URL + SHA256); ggml ISA options |
| `CMakePresets.json` | `clang-profile` (RelWithDebInfo); later an `aarch64` cross preset |
| `scripts/` (new) | `download_model.sh` (pinned URL + sha256), `make_test_audio.py` (stdlib) |
| `tests/streaming/`, `tests/vad/`, `tests/asr/` (new) | Unit and integration tests (see L) |
| `benchmarks/streaming_bench.cpp` (new) | Matrix runner, JSON/CSV output |
| `examples/transcribe.cpp` (new) | File → PARTIAL/FINAL printout; `run.sh` gains an option for it |

## K. Risks and tradeoffs

- **Whisper streaming limits:** quadratic partial cost; hallucination on silence/noise (mitigated by VAD and `no_speech` threshold); `audio_ctx` shrinking can cause repetition loops (needs a regression test); 30 s hard window.
- **Accuracy vs latency:** a shorter `pause_threshold` lowers latency but splits sentences. Smaller windows and greedy decoding lower accuracy. Quantization below q5 may hurt WER, so measure.
- **CPU vs memory:** ping-pong buffers and per-stream whisper state trade RAM for no stalls. 3.7 GiB in WSL limits large concurrency tests.
- **Concurrency:** more workers raise throughput but also per-request latency; the memory-bandwidth ceiling on ARM.
- **Model swapping:** the stream interface hides recompute cost, so the scheduler needs `is_incremental()` to pick the cadence.
- **Portability:** ggml ISA flags per target; there's no ARM hardware in this environment, so ARM numbers stay [A] until someone runs on a device.
- **Complexity:** each optional optimization (LocalAgreement, mel cache, affinity, lock-free) is gated on a benchmark showing need.
- **Dependency risk:** whisper.cpp API churn, so pin the version and wrap it behind `AsrStream`.

## L. Implementation plan (phase-gated; each phase ends with tests green on 8 presets + a short report)

| Phase | Files | Deliverable | Tests | Benchmark |
|---|---|---|---|---|
| 0 Baseline | – | Commit the M1 state; record test status | existing 11 | `sasr_wav_info` timing |
| 1 Config + interfaces | `config.hpp`, `asr/asr_backend.hpp`, `asr/mock_backend.*` | Config fields + `validate()`; mock backend | config validation; mock determinism | – |
| 2 Audio path | `wav_reader.*`, `streaming/utterance_buffer.hpp` | `read_into`, preallocated buffers | `read_into` parity with `read_frame`; buffer bounds/preroll | allocs/frame = 0 (counting new) |
| 3 VAD | `vad/energy_vad.*` | Energy VAD + hysteresis | synthetic tone/silence/noise; onset/offset timing | ns/frame |
| 4 Session + engine (single-thread) | `streaming/session.*`, `streaming/engine.*` | State machine D.3 with mock | **every row of D.3**, silence → 0 ASR calls, EOS cases, max-utterance, error injection, coalescing, FINAL never dropped | – |
| 5 Metrics + harness | `profiling/metrics.*`, `benchmarks/streaming_bench.cpp`, `scripts/make_test_audio.py` | Matrix runner with mock (pipeline overhead baseline) | percentile math | matrix with mock |
| 6 whisper.cpp backend | `Dependencies.cmake`, `asr/whisper_cpp_backend.*`, `scripts/download_model.sh`, `examples/transcribe.cpp` | Real transcription | integration test on `en1.wav` (skipped if no model), silence → no text | matrix 1 stream: RTF (final + total), p50–p99, RSS, model memory |
| 7 Threading | `streaming/spsc_ring.hpp`, engine workers | Capture/engine/worker split, backpressure | TSan stress; overflow policies; shutdown flush | 1/2/4/8 streams |
| 8 Optimize by profile | as indicated | `audio_ctx`, quantization, threads, ISA builds; optional mel cache/LocalAgreement | regression (WER bound, repetition) | before/after table |

## Verification (for the implementation session)
- `cmake --workflow --preset <p>` for all 8 presets: 0 warnings, all tests pass, TSan clean.
- `./run.sh` still works.
- `streaming_bench` reports, per matrix cell: RTF (final/total), first-partial/partial/final latency p50/p90/p95/p99, CPU %, peak RSS, model and per-stream memory, allocations/s, queue depth, dropped frames.
- Silence input: 0 backend calls.
- `en1.wav` produces plausible text, with no repeated-token loops.

---

# FINAL IMPLEMENTATION PROMPT

> You are implementing a CPU-first streaming ASR pipeline in the C++20 repo at `~/streamingASR` (WSL2 Ubuntu; build with `cmake --workflow --preset <name>`, 8 presets {gcc,clang}×{debug,release,asan,tsan}). The user has **explicitly authorized you to write implementation code** for this task, which overrides the mentor-only default in CLAUDE.md. Follow CLAUDE.md for everything else (C++ standards, `-Werror` per-target warnings via `sasr_set_warnings`, one static lib per module `sasr_<module>`/`sasr::<module>`, namespace `sasr`, includes `"<module>/<file>.hpp"`, deps via FetchContent pinned by URL + SHA256). The user prefers **simple code: few files, plain structs, interfaces only where a second implementation exists or tests need one**.
>
> **0. Re-check before editing.** Run `git status` / `git log`, read CLAUDE.md, `src/common/config.hpp`, `src/audio/wav_reader.{hpp,cpp}`, `tests/audio/wav_reader_test.cpp`, `cmake/*.cmake`, `CMakePresets.json`. If M1 work is uncommitted, stop and ask the user to commit. Run all 8 presets and record the baseline (expected: 11 tests + canaries pass). Record `sasr_wav_info tests/data/en1.wav` timing.
>
> **Architecture (approved; do not redesign):**
> - Internal audio = mono float32 16 kHz; sources convert at the boundary; no internal resampling.
> - Engine frame = `Config::frame_ms` (20 ms).
> - Sources call `Engine::push(session_id, std::span<const float>)` and `Engine::end_of_stream(session_id)`. No `AudioSource` interface.
> - Per session: energy VAD → endpointer state machine `IDLE→SPEECH⇄SHORT_PAUSE→LONG_PAUSE→FINALIZE→RESET→IDLE` → two preallocated contiguous utterance buffers (ping-pong, capacity = `preroll_ms` + `max_utterance_ms`) → ASR jobs that pass a zero-copy `span` over `[0, n)`.
> - At most **one in-flight inference per session**. PARTIALs every `interval_ms` only when new audio exists, coalesced (latest wins). **FINAL never dropped.** Silence-only input never calls the backend. EOS in speech finalizes and flushes; EOS in silence only resets. `max_utterance_ms` forces finalize. Backend errors emit an ERROR event and reset the session. Overflow: live sources drop and count, file sources block.
> - ASR abstraction in `src/asr/asr_backend.hpp`: `AsrBackend { create_stream(); is_incremental(); }`, `AsrStream { accept(span<const float>); decode(); finalize(); reset(); }`, `AsrResult { std::string text; bool is_final; timings }`. Feature extraction lives **inside** backends.
> - Backends: `MockBackend` (deterministic, configurable delay; tests) and `WhisperCppBackend`: whisper.cpp pinned via FetchContent behind option `SASR_WITH_WHISPER`, shared context + per-stream state (`whisper_init_state`/`whisper_full_with_state`; verify the API in the pinned version). Defaults: model `tiny.en`, greedy, `single_segment`, `no_timestamps` for partials, no temperature fallback for partials, previous finalized text as a bounded prompt, `dynamic_audio_ctx` config flag (window-sized, multiple of 64).
> - Every tunable lives in `sasr::Config` (`src/common/config.hpp`) with `validate()`. Nothing hard-coded.
>
> **Phases. Do them in order. After each phase: all 8 presets green with 0 warnings, then STOP and report (files changed, tests added, numbers) and wait for approval.**
> 1. Config fields + `validate()`; `asr_backend.hpp`; `MockBackend`. Tests: validation, mock determinism.
> 2. `WavReader::read_into(std::span<float>) -> std::size_t` (allocation-free; keep `read_frame`); `streaming/utterance_buffer.hpp`. Tests: parity, bounds, preroll. Prove 0 allocations per frame with a counting `operator new` in a test/bench binary.
> 3. `vad/energy_vad.{hpp,cpp}` with hysteresis (`min_speech_ms`, threshold). Tests on synthetic tone/silence/noise.
> 4. `streaming/session.{hpp,cpp}`, `streaming/engine.{hpp,cpp}`, **single-threaded and synchronous** with `MockBackend`. A test for every state-machine rule above, including error injection and coalescing.
> 5. `profiling/metrics.{hpp,cpp}` (preallocated samples, p50/p90/p95/p99, counters); `benchmarks/streaming_bench.cpp`; `scripts/make_test_audio.py` (Python stdlib only; writes to `build/testdata/`, not committed) generating silence, short speech, 300 ms / 2 s pause variants, `en1`×4 long, noise at SNR 20/10/5 dB. Report mock-pipeline overhead.
> 6. whisper.cpp backend + `scripts/download_model.sh` (pinned URL + sha256; model not committed) + `examples/transcribe.cpp` (PARTIAL/FINAL printout). Integration test on `tests/data/en1.wav`, skipped when the model is absent. Bench 1 stream: RTF (final-only and total), first-partial/partial/final latency p50–p99, CPU %, peak RSS, model memory, per-stream memory, allocs/s.
> 7. Threading: `streaming/spsc_ring.hpp` (bounded, `alignas(64)` indices) for capture→engine; inference worker pool (W workers × T threads, W·T ≤ physical cores; mutex+condvar bounded job/completion queues are acceptable); clean shutdown that flushes finals. TSan stress tests. Bench 1/2/4/8 streams and report degradation.
> 8. Optimization **only from measurements**: `dynamic_audio_ctx`, quantization (f16/q8_0/q5_1, WER vs latency), thread count, ggml ISA builds (x86-64-v3 vs AVX-512). Optional (only if data justifies): incremental mel cache, LocalAgreement-2-lite stable prefix, affinity. Before/after tables.
>
> **Rules:** smallest reasonable changes; preserve existing behavior and tests; no new dependencies beyond whisper.cpp (and GoogleTest/Benchmark already present); no heap allocation or blocking in the audio path (push/VAD/state/buffer copy); no unnecessary copies (document copies/frame); keep the backend replaceable (the engine must not include whisper headers); no manual SIMD, lock-free structures beyond the SPSC ring, `-ffast-math`, LTO or PGO without a profile showing need; report percentiles, never only averages; don't claim RTF targets you haven't measured. If RTF < 0.5 isn't reached on this machine (i5-11320H, 4C/8T, 3.7 GiB WSL RAM), report why with the breakdown (mel/encode/decode/queue).

---

### Sources (external claims marked [A])
- whisper.cpp README (MIT; NEON/AVX/AVX-512; Silero VAD; tiny ≈ 75 MiB on disk / ~273 MB memory; "naive" stream example): https://github.com/ggml-org/whisper.cpp, https://github.com/ggml-org/whisper.cpp/tree/master/examples/stream
- `audio_ctx` speedup and repetition risk: https://github.com/ggml-org/whisper.cpp/issues/1855, https://github.com/ggml-org/whisper.cpp/discussions/297
- sherpa-onnx QNN (v1.12.18) and streaming Zipformer on QNN (v1.13.3): https://github.com/k2-fsa/sherpa-onnx/blob/master/CHANGELOG.md
- ONNX Runtime KleidiAI/SVE i8mm (1.29/1.30): https://github.com/microsoft/onnxruntime/releases/tag/v1.29.0
- Moonshine v2 streaming: https://arxiv.org/html/2602.12241v1, https://huggingface.co/UsefulSensors/moonshine-streaming-tiny
- RPi 5 tiny/base ≈ real-time (secondary): https://www.openhab.org/addons/voice/whisperstt/
