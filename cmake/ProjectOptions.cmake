# SPDX-License-Identifier: GPL-3.0-or-later
# mosh_prefix_map_flags(<out-var> <from> <to>)
#
# Produce -ffile-prefix-map/-fdebug-prefix-map options that rewrite <from> to
# <to> in __FILE__ strings and debug information.  Both the CMake (forward
# slash) and the native (backslash) spelling of <from> are mapped, because
# GCC sees either form depending on how a path reached the command line.
function(mosh_prefix_map_flags out_var from to)
  file(TO_NATIVE_PATH "${from}" _from_native)
  set(${out_var}
    "-ffile-prefix-map=${from}=${to}"
    "-fdebug-prefix-map=${from}=${to}"
    "-ffile-prefix-map=${_from_native}=${to}"
    "-fdebug-prefix-map=${_from_native}=${to}"
    PARENT_SCOPE)
endfunction()

# Usage requirements shared by every native target of this project.
add_library(mosh-project-options INTERFACE)
target_compile_definitions(mosh-project-options INTERFACE
  NOMINMAX
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000000
  "PACKAGE_NAME=\"mosh\""
  "PACKAGE_VERSION=\"${MOSH_UPSTREAM_VERSION}\""
  "PACKAGE_STRING=\"mosh ${MOSH_WIN_VERSION}\""
  "BUILD_VERSION=\"windows-native\""
  "MOSH_WIN_VERSION=\"${MOSH_WIN_VERSION}\"")

target_compile_options(mosh-project-options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wformat=2
  -Wshadow
  -fstack-protector-strong
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>)

# Static, hardened and timestamp-free linking for every shipped or test
# executable.
function(mosh_win_apply_executable_defaults target)
  target_link_libraries(${target} PRIVATE mosh-project-options)
  target_link_options(${target} PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
    -Wl,--dynamicbase
    -Wl,--high-entropy-va
    -Wl,--nxcompat
    -Wl,--no-insert-timestamp)
endfunction()
