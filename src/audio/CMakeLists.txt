add_library(sasr_audio wav_reader.cpp)
add_library(sasr::audio ALIAS sasr_audio)

target_include_directories(sasr_audio PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(sasr_audio PUBLIC sasr::common)
sasr_set_warnings(sasr_audio)
