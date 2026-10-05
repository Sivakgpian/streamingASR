add_executable(sasr_version version.cpp)
target_link_libraries(sasr_version PRIVATE sasr::common)
sasr_set_warnings(sasr_version)

add_executable(sasr_wav_info wav_info.cpp)
target_link_libraries(sasr_wav_info PRIVATE sasr::audio)
sasr_set_warnings(sasr_wav_info)

if(SASR_WITH_WHISPER)
    add_executable(sasr_transcribe transcribe.cpp)
    target_link_libraries(sasr_transcribe PRIVATE sasr::asr sasr::audio sasr::streaming)
    sasr_set_warnings(sasr_transcribe)
endif()
