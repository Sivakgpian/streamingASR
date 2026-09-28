# One static library per module. Modules include each other as "<module>/<header>.hpp"
# and must declare dependencies explicitly via target_link_libraries.
add_subdirectory(common)
