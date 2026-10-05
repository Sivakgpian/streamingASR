// Reads a WAV file frame by frame and prints its length and memory use.
// Usage: sasr_wav_info <file.wav>
#include "audio/wav_reader.hpp"
#include "common/config.hpp"

#include <exception>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: sasr_wav_info <file.wav>\n";
        return 2;
    }
    try {
        const sasr::Config config;
        sasr::WavReader reader(argv[1], config);

        int frames = 0;
        std::size_t frame_heap_bytes = 0;
        while (const auto frame = reader.read_frame()) {
            ++frames;
            frame_heap_bytes = frame->samples.capacity() * sizeof(float);
        }

        std::cout << "samples            : " << reader.total_samples() << '\n'
                  << "duration           : " << reader.total_samples() * 1000 / config.sample_rate
                  << " ms\n"
                  << "frames (" << config.frame_ms << " ms)     : " << frames << '\n'
                  << "sizeof(AudioFrame) : " << sizeof(sasr::AudioFrame) << " B\n"
                  << "frame heap         : " << frame_heap_bytes << " B\n"
                  << "sizeof(WavReader)  : " << sizeof(sasr::WavReader) << " B\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
