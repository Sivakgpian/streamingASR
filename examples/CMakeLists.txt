add_executable(sasr_version version.cpp)
target_link_libraries(sasr_version PRIVATE sasr::common)
sasr_set_warnings(sasr_version)

add_executable(sasr_wav_info wav_info.cpp)
target_link_libraries(sasr_wav_info PRIVATE sasr::audio)
sasr_set_warnings(sasr_wav_info)
