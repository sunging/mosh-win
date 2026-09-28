set(_launcher_source_dir "${PROJECT_SOURCE_DIR}/src/launcher")

add_executable(mosh-test-fake-ssh launcher/fake_ssh.cc)
set_target_properties(mosh-test-fake-ssh PROPERTIES
  OUTPUT_NAME "fake-ssh")
target_link_options(mosh-test-fake-ssh PRIVATE -municode)
mosh_win_apply_executable_defaults(mosh-test-fake-ssh)

add_executable(mosh-launcher-tests
  launcher/launcher_tests.cc
  "${_launcher_source_dir}/launcher_core.cc"
  "${_launcher_source_dir}/process_windows.cc")
target_include_directories(mosh-launcher-tests PRIVATE
  "${_launcher_source_dir}")
target_link_libraries(mosh-launcher-tests PRIVATE ws2_32)
target_link_options(mosh-launcher-tests PRIVATE -municode)
mosh_win_apply_executable_defaults(mosh-launcher-tests)

add_test(NAME launcher.unit
  COMMAND mosh-launcher-tests "$<TARGET_FILE:mosh-test-fake-ssh>")
set_tests_properties(launcher.unit PROPERTIES
  LABELS "launcher;unit"
  TIMEOUT 20)

if(TARGET mosh-launcher)
  # Exercise the complete launcher boundary.  The same fixture acts as SSH
  # during bootstrap and as mosh-client after the launcher injects MOSH_KEY.
  # It rejects a key on argv and checks that prediction/no-init settings are
  # confined to the child environment.
  add_test(NAME launcher.fake-ssh-e2e
    COMMAND "$<TARGET_FILE:mosh-launcher>"
      "--ssh-path=$<TARGET_FILE:mosh-test-fake-ssh>"
      "--client=$<TARGET_FILE:mosh-test-fake-ssh>"
      --predict=always
      -o
      --no-init
      fake-host)
  set_tests_properties(launcher.fake-ssh-e2e PROPERTIES
    ENVIRONMENT "MOSH_FAKE_EXPECT_FULL_ENV=1"
    FAIL_REGULAR_EXPRESSION "UNSAFE"
    PASS_REGULAR_EXPRESSION "SAFE"
    LABELS "launcher;integration"
    TIMEOUT 20)

  add_test(NAME launcher.post-connect-ssh-failure
    COMMAND "$<TARGET_FILE:mosh-launcher>"
      "--ssh-path=$<TARGET_FILE:mosh-test-fake-ssh>"
      "--client=$<TARGET_FILE:mosh-test-fake-ssh>"
      fake-host)
  set_tests_properties(launcher.post-connect-ssh-failure PROPERTIES
    ENVIRONMENT "MOSH_FAKE_SSH_MODE=post-connect-failure"
    FAIL_REGULAR_EXPRESSION "UNSAFE"
    PASS_REGULAR_EXPRESSION "SAFE"
    LABELS "launcher;integration;regression"
    TIMEOUT 20)

  add_test(NAME launcher.malformed-bootstrap
    COMMAND "${CMAKE_COMMAND}"
      "-DLAUNCHER=$<TARGET_FILE:mosh-launcher>"
      "-DFAKE_SSH=$<TARGET_FILE:mosh-test-fake-ssh>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/integration/assert_malformed_bootstrap.cmake")
  set_tests_properties(launcher.malformed-bootstrap PROPERTIES
    LABELS "launcher;integration;negative"
    TIMEOUT 20)
endif()

# These sources are added by the core port in independently runnable pieces.
# Keep the mapping explicit because the tests intentionally have different
# dependency surfaces (for example UTF-8 must not accidentally require the
# much larger protobuf/terminal core).
function(_mosh_add_core_test test_stem timeout)
  set(_source "${CMAKE_CURRENT_SOURCE_DIR}/core/${test_stem}.cc")
  if(NOT EXISTS "${_source}")
    return()
  endif()

  set(_target "mosh-${test_stem}")
  add_executable("${_target}" "${_source}")
  target_include_directories("${_target}" PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries("${_target}" PRIVATE ${ARGN})
  mosh_win_apply_executable_defaults("${_target}")

  add_test(NAME "core.${test_stem}" COMMAND "${_target}")
  set_tests_properties("core.${test_stem}" PROPERTIES
    LABELS "core;unit"
    TIMEOUT "${timeout}")
endfunction()

if(TARGET mosh-win32-platform)
  _mosh_add_core_test(utf8_test 30 mosh-win32-platform)
  _mosh_add_core_test(socket_test 90 mosh-win32-platform ws2_32)
  _mosh_add_core_test(console_test 30 mosh-win32-platform)
endif()
if(TARGET mosh-core)
  _mosh_add_core_test(crypto_test 60 mosh-core mosh-win32-platform)
  _mosh_add_core_test(state_sync_test 60 mosh-core)
  _mosh_add_core_test(terminal_test 60 mosh-core)

  # Compile the Win32 Connection implementation with a test-owned virtual
  # clock. This exercises the real Winsock and crypto path while advancing a
  # 15-second outage instantly and without adding test hooks to production.
  add_executable(mosh-network-resilience-test
    core/network_resilience_test.cc
    "${PROJECT_SOURCE_DIR}/src/platform/mosh_network_win32.cc")
  target_include_directories(mosh-network-resilience-test PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(mosh-network-resilience-test PRIVATE
    mosh-core mosh-win32-platform)
  mosh_win_apply_executable_defaults(mosh-network-resilience-test)
  add_test(NAME core.network_resilience_test
    COMMAND mosh-network-resilience-test)
  set_tests_properties(core.network_resilience_test PROPERTIES
    LABELS "core;network;resilience;unit"
    TIMEOUT 20)

  # Compile the official Mosh 1.4.0 OCB vector/iterative/tamper suite against
  # the exact CNG-backed crypto object linked into mosh-client.exe.
  set(_upstream_tests "${MOSH_UPSTREAM_SRC}/tests")
  if(EXISTS "${_upstream_tests}/ocb-aes.cc" AND
     EXISTS "${_upstream_tests}/test_utils.cc")
    add_executable(mosh-upstream-ocb-test
      "${_upstream_tests}/ocb-aes.cc"
      "${_upstream_tests}/test_utils.cc")
    target_include_directories(mosh-upstream-ocb-test PRIVATE
      "${_upstream_tests}"
      "${MOSH_UPSTREAM_SRC}/crypto"
      "${MOSH_UPSTREAM_SRC}/util")
    target_link_libraries(mosh-upstream-ocb-test PRIVATE
      mosh-core mosh-win32-platform)
    mosh_win_apply_executable_defaults(mosh-upstream-ocb-test)
    add_test(NAME core.ocb_upstream
      COMMAND mosh-upstream-ocb-test)
    set_tests_properties(core.ocb_upstream PROPERTIES
      LABELS "core;crypto;upstream"
      TIMEOUT 120)
  endif()
endif()
