#pragma once

#include "asr/asr_backend.hpp"
#include "common/config.hpp"

#include <memory>

// Forward-declared, never included here: whisper.h stays out of every
// header so consumers of AsrBackend (the streaming engine) never need to
// know this backend exists, let alone compile against whisper.cpp's API
// (CLAUDE.md: the backend must be replaceable). Only
// whisper_cpp_backend.cpp includes <whisper.h>.
//
// This class is only compiled in when SASR_WITH_WHISPER=ON (see
// cmake/Dependencies.cmake and src/asr/CMakeLists.txt). The header is
// always present so it always documents the backend; using it without
// that option gives a link error, not a compile error.
struct whisper_context;

namespace sasr {

// ASR backend using whisper.cpp. Not incremental (AsrBackend::
// is_incremental() returns false): whisper's encoder re-runs over the
// whole audio window on every decode()/finalize() call, by design of
// the model, not of this wrapper -- see CLAUDE.md / docs/ for why that
// makes utterance length and the partial interval matter for cost.
//
// One shared whisper_context (the loaded model weights) backs many
// per-session whisper_states, matching whisper.cpp's own context/state
// split: create_stream() calls whisper_init_state() against this
// backend's context, so model weights are loaded exactly once no matter
// how many sessions are open.
class WhisperCppBackend final : public AsrBackend {
public:
    // Loads the model at config.model_path (CPU only; GPU backends are a
    // later, explicit per-build choice, not wired up here). Throws
    // std::runtime_error if the file is missing or whisper.cpp fails to
    // load it. `config` is kept by reference inside every stream this
    // backend creates, so it must outlive this backend, which must in
    // turn outlive every stream it creates (same requirement as any
    // AsrBackend).
    explicit WhisperCppBackend(const Config& config);
    ~WhisperCppBackend() override;

    WhisperCppBackend(const WhisperCppBackend&) = delete;
    WhisperCppBackend& operator=(const WhisperCppBackend&) = delete;
    WhisperCppBackend(WhisperCppBackend&&) = delete;
    WhisperCppBackend& operator=(WhisperCppBackend&&) = delete;

    [[nodiscard]] std::unique_ptr<AsrStream> create_stream() override;
    [[nodiscard]] bool is_incremental() const noexcept override { return false; }

private:
    struct ContextDeleter {
        void operator()(whisper_context* ctx) const noexcept;
    };

    const Config& config_;
    std::unique_ptr<whisper_context, ContextDeleter> ctx_;
};

}  // namespace sasr
