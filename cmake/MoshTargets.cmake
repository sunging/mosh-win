set(MOSH_UPSTREAM_ROOT "${mosh_upstream_SOURCE_DIR}")
set(MOSH_UPSTREAM_SRC "${MOSH_UPSTREAM_ROOT}/src")
set(MOSH_GENERATED_ROOT "${CMAKE_BINARY_DIR}/generated")
set(MOSH_GENERATED_PROTO_DIR "${MOSH_GENERATED_ROOT}/protobufs")
set(MOSH_GENERATED_INCLUDE_DIR "${MOSH_GENERATED_ROOT}/include")

# Upstream template headers are instantiated by mosh-client and several tests,
# not only by mosh-core.  Apply this after the checkout-wide prefix map so the
# more specific upstream path wins for every native target, including builds
# from a corresponding-source directory outside the checkout.
file(TO_NATIVE_PATH "${MOSH_UPSTREAM_ROOT}" _mosh_upstream_root_native)
target_compile_options(mosh-project-options INTERFACE
  "-ffile-prefix-map=${MOSH_UPSTREAM_ROOT}=third_party/mosh"
  "-fdebug-prefix-map=${MOSH_UPSTREAM_ROOT}=third_party/mosh"
  "-ffile-prefix-map=${_mosh_upstream_root_native}=third_party/mosh"
  "-fdebug-prefix-map=${_mosh_upstream_root_native}=third_party/mosh")

file(MAKE_DIRECTORY "${MOSH_GENERATED_INCLUDE_DIR}")
configure_file("${PROJECT_SOURCE_DIR}/cmake/config.h.in"
  "${MOSH_GENERATED_INCLUDE_DIR}/config.h" @ONLY)

function(_mosh_sources_ready result label)
  set(_missing)
  foreach(_source IN LISTS ARGN)
    if(NOT EXISTS "${_source}")
      list(APPEND _missing "${_source}")
    endif()
  endforeach()

  if(_missing)
    string(REPLACE ";" "\n  " _missing_text "${_missing}")
    if(MOSH_REQUIRE_COMPLETE_PORT)
      message(FATAL_ERROR "${label} sources are incomplete:\n  ${_missing_text}")
    else()
      message(STATUS "${label} target deferred; missing:\n  ${_missing_text}")
    endif()
    set(${result} FALSE PARENT_SCOPE)
  else()
    set(${result} TRUE PARENT_SCOPE)
  endif()
endfunction()

set(_mosh_proto_names userinput hostinput transportinstruction)
set(_mosh_proto_outputs)
foreach(_name IN LISTS _mosh_proto_names)
  set(_proto "${MOSH_UPSTREAM_SRC}/protobufs/${_name}.proto")
  set(_proto_cc "${MOSH_GENERATED_PROTO_DIR}/${_name}.pb.cc")
  set(_proto_h "${MOSH_GENERATED_PROTO_DIR}/${_name}.pb.h")
  add_custom_command(
    OUTPUT "${_proto_cc}" "${_proto_h}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${MOSH_GENERATED_PROTO_DIR}"
    COMMAND "$<TARGET_FILE:protobuf::protoc>"
      "--cpp_out=${MOSH_GENERATED_PROTO_DIR}"
      "--proto_path=${MOSH_UPSTREAM_SRC}/protobufs"
      "${_proto}"
    DEPENDS protobuf::protoc "${_proto}"
    COMMENT "Generating ${_name}.pb.cc"
    VERBATIM)
  list(APPEND _mosh_proto_outputs "${_proto_cc}" "${_proto_h}")
endforeach()

add_library(mosh-protos STATIC ${_mosh_proto_outputs})
target_link_libraries(mosh-protos PUBLIC protobuf::libprotobuf-lite mosh-project-options)
target_include_directories(mosh-protos PUBLIC "${MOSH_GENERATED_PROTO_DIR}")

set(_mosh_platform_sources
  "${PROJECT_SOURCE_DIR}/src/platform/win32_console.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_console.h"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_socket.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_socket.h"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_crypto.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_crypto.h"
  "${PROJECT_SOURCE_DIR}/src/platform/utf8.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/utf8.h")
_mosh_sources_ready(_platform_ready "Win32 platform" ${_mosh_platform_sources})
if(_platform_ready)
  add_library(mosh-win32-platform STATIC ${_mosh_platform_sources})
  target_link_libraries(mosh-win32-platform
    PUBLIC mosh-project-options ws2_32 bcrypt user32)
  target_include_directories(mosh-win32-platform PUBLIC "${PROJECT_SOURCE_DIR}/src")
endif()

set(_mosh_core_sources
  "${PROJECT_SOURCE_DIR}/src/platform/win32_locale.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_swrite.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_terminaldisplayinit.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/win32_timestamp.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/mosh_network_win32.cc"
  "${PROJECT_SOURCE_DIR}/src/platform/mosh_network_win32.h"
  "${MOSH_UPSTREAM_SRC}/crypto/ocb_internal.cc"
  "${MOSH_UPSTREAM_SRC}/crypto/base64.cc"
  "${MOSH_UPSTREAM_SRC}/crypto/crypto.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/parseraction.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/parser.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/parserstate.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminal.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminaldispatcher.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminaldisplay.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminalframebuffer.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminalfunctions.cc"
  "${MOSH_UPSTREAM_SRC}/terminal/terminaluserinput.cc"
  "${MOSH_UPSTREAM_SRC}/network/transportfragment.cc"
  "${MOSH_UPSTREAM_SRC}/network/compressor.cc"
  "${MOSH_UPSTREAM_SRC}/statesync/completeterminal.cc"
  "${MOSH_UPSTREAM_SRC}/statesync/user.cc")
_mosh_sources_ready(_core_sources_ready "Mosh core" ${_mosh_core_sources})
if(_core_sources_ready AND TARGET mosh-win32-platform)
  add_library(mosh-core STATIC ${_mosh_core_sources})
  target_link_libraries(mosh-core
    PUBLIC mosh-project-options mosh-win32-platform mosh-protos ZLIB::ZLIB)
  target_compile_definitions(mosh-core PRIVATE USE_BCRYPT_AES=1)
  target_compile_options(mosh-core PRIVATE
    "-ffile-prefix-map=${MOSH_UPSTREAM_ROOT}=third_party/mosh"
    "-fdebug-prefix-map=${MOSH_UPSTREAM_ROOT}=third_party/mosh")
  target_include_directories(mosh-core PUBLIC
    "${MOSH_GENERATED_INCLUDE_DIR}"
    "${MOSH_GENERATED_PROTO_DIR}"
    "${MOSH_UPSTREAM_SRC}/util"
    "${MOSH_UPSTREAM_SRC}/crypto"
    "${MOSH_UPSTREAM_SRC}/terminal"
    "${MOSH_UPSTREAM_SRC}/network"
    "${MOSH_UPSTREAM_SRC}/statesync"
    "${MOSH_UPSTREAM_SRC}/frontend")
endif()

set(_mosh_client_sources
  "${PROJECT_SOURCE_DIR}/src/client/client_main.cc"
  "${PROJECT_SOURCE_DIR}/src/client/stmclient_win.cc"
  "${PROJECT_SOURCE_DIR}/src/client/stmclient_win.h"
  "${MOSH_UPSTREAM_SRC}/frontend/terminaloverlay.cc")
_mosh_sources_ready(_client_ready "mosh-client" ${_mosh_client_sources})
if(_client_ready AND TARGET mosh-core)
  add_executable(mosh-client ${_mosh_client_sources})
  target_link_libraries(mosh-client PRIVATE mosh-core)
  mosh_win_apply_executable_defaults(mosh-client)
endif()

set(_mosh_launcher_sources
  "${PROJECT_SOURCE_DIR}/src/launcher/main.cc"
  "${PROJECT_SOURCE_DIR}/src/launcher/launcher_core.cc"
  "${PROJECT_SOURCE_DIR}/src/launcher/launcher_core.h"
  "${PROJECT_SOURCE_DIR}/src/launcher/process_windows.cc"
  "${PROJECT_SOURCE_DIR}/src/launcher/process_windows.h")
_mosh_sources_ready(_launcher_ready "mosh launcher" ${_mosh_launcher_sources})
if(_launcher_ready)
  add_executable(mosh-launcher ${_mosh_launcher_sources})
  set_target_properties(mosh-launcher PROPERTIES OUTPUT_NAME mosh)
  target_include_directories(mosh-launcher PRIVATE "${PROJECT_SOURCE_DIR}/src/launcher")
  target_link_libraries(mosh-launcher PRIVATE ws2_32)
  target_link_options(mosh-launcher PRIVATE -municode)
  mosh_win_apply_executable_defaults(mosh-launcher)
endif()
