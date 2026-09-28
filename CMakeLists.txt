cmake_minimum_required(VERSION 3.28)

project(mosh-win
  VERSION 1.4.0
  DESCRIPTION "Native Windows client port of Mosh"
  HOMEPAGE_URL "https://mosh.org/"
  LANGUAGES C CXX)

if(NOT WIN32 OR NOT MINGW)
  message(FATAL_ERROR "mosh-win supports the MinGW-w64/UCRT Windows toolchain only")
endif()

if(CYGWIN OR MSYS)
  message(FATAL_ERROR "Cygwin and MSYS runtime builds are intentionally unsupported")
endif()

if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR
   NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL "15.1.0" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "15.1.0")
  message(FATAL_ERROR
    "mosh-win requires the pinned MinGW-w64 GCC 15.1.0 toolchain; found "
    "C ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}, "
    "C++ ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR
    "mosh-win requires an x64 MinGW-w64 toolchain; pointer size is "
    "${CMAKE_SIZEOF_VOID_P}")
endif()

include(CheckCSourceCompiles)
check_c_source_compiles([[
  #include <_mingw.h>
  #if !defined(__MINGW64__) || !defined(_WIN64)
  #error "not an x64 MinGW-w64 compiler"
  #endif
  #if !defined(_UCRT)
  #error "MinGW-w64 is not targeting UCRT"
  #endif
  int main(void) { return 0; }
]] MOSH_TOOLCHAIN_IS_X64_UCRT)
if(NOT MOSH_TOOLCHAIN_IS_X64_UCRT)
  message(FATAL_ERROR
    "mosh-win requires the x64 UCRT variant of MinGW-w64")
endif()

option(MOSH_REQUIRE_COMPLETE_PORT
  "Fail configuration when any native client source is missing" OFF)
option(MOSH_BUILD_TESTS "Build the native Windows test suite" ON)
option(MOSH_APPLY_UPSTREAM_PATCHES "Apply patches/series to the fetched Mosh tree" ON)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Keep diagnostics, assertion strings, and debug metadata independent of the
# absolute checkout/build directory.  These options must be directory-wide:
# FetchContent dependencies such as protobuf contain __FILE__ strings too and
# do not link against mosh-project-options.
file(TO_NATIVE_PATH "${PROJECT_SOURCE_DIR}" _mosh_source_dir_native)
file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" _mosh_binary_dir_native)
add_compile_options(
  "-ffile-prefix-map=${PROJECT_SOURCE_DIR}=."
  "-fdebug-prefix-map=${PROJECT_SOURCE_DIR}=."
  "-ffile-prefix-map=${_mosh_source_dir_native}=."
  "-fdebug-prefix-map=${_mosh_source_dir_native}=."
  # GCC applies the last matching prefix map, so the more-specific build
  # directory mapping must follow the checkout-wide mapping above.
  "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build"
  "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=build"
  "-ffile-prefix-map=${_mosh_binary_dir_native}=build"
  "-fdebug-prefix-map=${_mosh_binary_dir_native}=build")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ProjectOptions)
include(Dependencies)
include(MoshTargets)

if(MOSH_BUILD_TESTS)
  include(CTest)
  enable_testing()
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
  else()
    message(STATUS "tests/CMakeLists.txt is not present; test targets are deferred")
  endif()
endif()

include(GNUInstallDirs)
foreach(_runtime IN ITEMS mosh-launcher mosh-client)
  if(TARGET "${_runtime}")
    install(TARGETS "${_runtime}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endforeach()
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION "${CMAKE_INSTALL_DOCDIR}")

set(CPACK_PACKAGE_NAME "mosh-win")
set(CPACK_PACKAGE_VERSION "1.4.0-win1")
set(CPACK_GENERATOR "ZIP")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
include(CPack)
