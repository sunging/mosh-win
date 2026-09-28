include(FetchContent)

cmake_policy(SET CMP0135 NEW)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(mosh_upstream
  URL "https://github.com/mobile-shell/mosh/archive/refs/tags/mosh-1.4.0.tar.gz"
  URL_HASH "SHA256=ae581fbddf038730af9eee4d319a483288395a0722d0c94c7efb7fdbdbb0dbac"
  EXCLUDE_FROM_ALL
  SYSTEM)

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
  URL "https://zlib.net/fossils/zlib-1.3.1.tar.gz"
  URL_HASH "SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23"
  EXCLUDE_FROM_ALL
  SYSTEM)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_CONFORMANCE OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)
FetchContent_Declare(protobuf
  URL "https://github.com/protocolbuffers/protobuf/releases/download/v21.12/protobuf-cpp-3.21.12.tar.gz"
  URL_HASH "SHA256=4eab9b524aa5913c6fffb20b2a8abf5ef7f95a80bc0701f3a6dbb4c607f73460"
  EXCLUDE_FROM_ALL
  SYSTEM)

FetchContent_MakeAvailable(mosh_upstream zlib protobuf)

# Corresponding-source bundles can place dependency trees outside both the
# checkout and the build directory.  Normalize those paths too, so diagnostics
# and __FILE__ strings remain identical to a normal FetchContent build and do
# not disclose the directory where the source bundle was unpacked.
function(_mosh_normalize_dependency_source target source_dir stable_dir)
  if(NOT TARGET "${target}")
    return()
  endif()
  mosh_prefix_map_flags(_flags "${source_dir}" "${stable_dir}")
  target_compile_options("${target}" PRIVATE ${_flags})
endfunction()

foreach(_protobuf_target IN ITEMS
    libprotobuf-lite libprotobuf libprotoc protoc)
  _mosh_normalize_dependency_source(
    "${_protobuf_target}" "${protobuf_SOURCE_DIR}"
    "build/_deps/protobuf-src")
endforeach()
foreach(_zlib_target IN ITEMS zlib zlibstatic)
  _mosh_normalize_dependency_source(
    "${_zlib_target}" "${zlib_SOURCE_DIR}" "build/_deps/zlib-src")
endforeach()

if(MOSH_APPLY_UPSTREAM_PATCHES AND EXISTS "${PROJECT_SOURCE_DIR}/patches/series")
  include(ApplyMoshPatches)
  mosh_apply_patch_series(
    "${mosh_upstream_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/patches"
    "${PROJECT_SOURCE_DIR}/patches/series")
endif()

if(TARGET zlibstatic AND NOT TARGET ZLIB::ZLIB)
  add_library(ZLIB::ZLIB ALIAS zlibstatic)
endif()
