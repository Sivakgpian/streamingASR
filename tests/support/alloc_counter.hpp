#pragma once

#include <cstddef>

namespace sasr::test {

// Counts calls to the global allocation functions (new/new[]) for the whole
// process. Link tests/support/alloc_counter.cpp into a test binary to
// activate the counting global operator new/delete; linking it overrides
// those operators for the ENTIRE binary (including GoogleTest's own
// allocations), so keep such binaries small and separate from ordinary
// correctness tests. Bracket the call under test with before/after
// snapshots:
//     const auto before = sasr::test::allocation_count();
//     do_the_thing();
//     EXPECT_EQ(sasr::test::allocation_count(), before);
//
// Sanitizer presets (asan/tsan) already replace the global allocation
// functions with their own (ASan to add redzones, TSan via its runtime
// library), and a second replacement in the same binary either mismatches
// ASan's alloc/dealloc bookkeeping or fails to link against TSan's runtime
// (duplicate symbol). So: only link this into test binaries built under
// SASR_SANITIZER == "" (see tests/audio/CMakeLists.txt and
// tests/streaming/CMakeLists.txt for the guard) — plain malloc/free
// counting needs no sanitizer, and the 4 non-sanitized presets are enough
// to prove the no-allocation claim.
[[nodiscard]] std::size_t allocation_count();

}  // namespace sasr::test
