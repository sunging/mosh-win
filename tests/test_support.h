/* Shared helpers for the standalone core test executables. */
#pragma once

#include <cstdlib>
#include <iostream>

/* tests/CMakeLists.txt defines MOSH_TEST_NAME for every core test target. */
#ifndef MOSH_TEST_NAME
#error "MOSH_TEST_NAME must be defined to the test executable's name"
#endif

namespace mosh::test {

/* Fails the whole test executable with a diagnostic when |condition| is
   false.  Tests run under CTest, which reports the non-zero exit status. */
inline void expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << MOSH_TEST_NAME ": " << message << '\n';
    std::exit(1);
  }
}

} // namespace mosh::test
