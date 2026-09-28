if(NOT DEFINED LAUNCHER OR NOT EXISTS "${LAUNCHER}")
  message(FATAL_ERROR "LAUNCHER is missing or does not exist: ${LAUNCHER}")
endif()
if(NOT DEFINED FAKE_SSH OR NOT EXISTS "${FAKE_SSH}")
  message(FATAL_ERROR "FAKE_SSH is missing or does not exist: ${FAKE_SSH}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E env
    MOSH_FAKE_SSH_MODE=malformed
    "${LAUNCHER}"
    "--ssh-path=${FAKE_SSH}"
    "--client=${FAKE_SSH}"
    --remote-ip=remote
    fake-host
  RESULT_VARIABLE result
  OUTPUT_VARIABLE stdout
  ERROR_VARIABLE stderr
  TIMEOUT 10)

if(NOT result EQUAL 2)
  message(FATAL_ERROR
    "malformed bootstrap returned ${result}, expected 2\n"
    "stdout:\n${stdout}\n"
    "stderr:\n${stderr}")
endif()
if(NOT stderr MATCHES "mosh:.*MOSH CONNECT")
  message(FATAL_ERROR
    "launcher did not report the malformed bootstrap safely\n"
    "stdout:\n${stdout}\n"
    "stderr:\n${stderr}")
endif()
