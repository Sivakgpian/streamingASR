find_package(Threads REQUIRED)

add_library(sasr_streaming session.cpp engine.cpp job_runner.cpp threaded_engine.cpp)
add_library(sasr::streaming ALIAS sasr_streaming)

target_include_directories(sasr_streaming PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(sasr_streaming PUBLIC sasr::common sasr::asr sasr::vad Threads::Threads)
sasr_set_warnings(sasr_streaming)
