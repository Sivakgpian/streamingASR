#include "common/version.hpp"

#include <gtest/gtest.h>

#include <format>

namespace sasr {
namespace {

TEST(Version, StringMatchesComponents) {
    const Version v = version();
    EXPECT_EQ(version_string(), std::format("{}.{}.{}", v.major, v.minor, v.patch));
}

TEST(Version, ComponentsAreNonNegative) {
    const Version v = version();
    EXPECT_GE(v.major, 0);
    EXPECT_GE(v.minor, 0);
    EXPECT_GE(v.patch, 0);
}

}  // namespace
}  // namespace sasr
