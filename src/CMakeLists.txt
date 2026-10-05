# One static library per module. Modules include each other as "<module>/<header>.hpp"
# and must declare dependencies explicitly via target_link_libraries.
add_subdirectory(common)
add_subdirectory(audio)
add_subdirectory(asr)
add_subdirectory(vad)
add_subdirectory(streaming)
add_subdirectory(profiling)
