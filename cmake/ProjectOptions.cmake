add_library(mosh-project-options INTERFACE)
target_compile_definitions(mosh-project-options INTERFACE
  NOMINMAX
  UNICODE
  _UNICODE
  _WIN32_WINNT=0x0A00
  NTDDI_VERSION=0x0A000000
  PACKAGE_NAME="mosh"
  PACKAGE_VERSION="1.4.0"
  PACKAGE_STRING="mosh\ 1.4.0-win1"
  BUILD_VERSION="windows-native")

target_compile_options(mosh-project-options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wformat=2
  -Wshadow
  -fstack-protector-strong
  $<$<CONFIG:Release>:-O2>
  $<$<CONFIG:Release>:-D_FORTIFY_SOURCE=2>
  -ffile-prefix-map=${PROJECT_SOURCE_DIR}=.
  -fdebug-prefix-map=${PROJECT_SOURCE_DIR}=.
  -ffile-prefix-map=${CMAKE_BINARY_DIR}=build
  -fdebug-prefix-map=${CMAKE_BINARY_DIR}=build)

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
