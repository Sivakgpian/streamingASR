#pragma once

#include <string_view>

namespace sasr {

struct Version {
    int major;
    int minor;
    int patch;

    friend constexpr bool operator==(const Version&, const Version&) = default;
};

// Version of the library as configured in the top-level CMakeLists.txt.
[[nodiscard]] Version version() noexcept;

// "MAJOR.MINOR.PATCH". The view refers to static storage and never dangles.
[[nodiscard]] std::string_view version_string() noexcept;

}  // namespace sasr
