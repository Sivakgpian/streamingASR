// Transcribes a WAV file through the full Engine/Session/VAD pipeline,
// printing PARTIAL and FINAL results as they're produced.
// Usage: sasr_transcribe <model.bin> <file.wav>
#include "asr/whisper_cpp_backend.hpp"
#include "audio/wav_reader.hpp"
#include "common/config.hpp"
#include "streaming/engine.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <model.bin> <file.wav>\n";
        return 2;
    }

    try {
        sasr::Config config;
        config.model_path = argv[1];
        config.asr_backend = sasr::Config::AsrBackendKind::kWhisperCpp;
        config.validate();

        sasr::WhisperCppBackend backend(config);
        sasr::Engine engine(config, backend);

        constexpr sasr::SessionId kSession = 1;
        engine.open_session(kSession, [](const sasr::SessionEvent& event) {
            switch (event.kind) {
                case sasr::SessionEventKind::kPartial:
                    std::cout << "[PARTIAL] " << event.text << '\n';
                    break;
                case sasr::SessionEventKind::kFinal:
                    std::cout << "[FINAL]   " << event.text << '\n';
                    break;
                case sasr::SessionEventKind::kError:
                    std::cerr << "[ERROR]   " << event.error << '\n';
                    break;
            }
        });

        sasr::WavReader reader(argv[2], config);
        std::vector<float> chunk(config.frame_samples());
        while (true) {
            const std::size_t n = reader.read_into(chunk);
            if (n == 0) {
                break;
            }
            engine.push(kSession, std::span<const float>(chunk).first(n));
        }
        engine.end_of_stream(kSession);
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
