#include "common/version.hpp"

namespace sasr {

Version version() noexcept {
    return Version{SASR_VERSION_MAJOR, SASR_VERSION_MINOR, SASR_VERSION_PATCH};
}

std::string_view version_string() noexcept { return SASR_VERSION_STRING; }

}  // namespace sasr
