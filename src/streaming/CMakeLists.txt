# Header-only for now (utterance_buffer.hpp). Gains a .cpp (session.cpp,
# engine.cpp) once that lands, becoming an ordinary static library like
# the other modules.
add_library(sasr_streaming INTERFACE)
add_library(sasr::streaming ALIAS sasr_streaming)
target_include_directories(sasr_streaming INTERFACE "${PROJECT_SOURCE_DIR}/src")
