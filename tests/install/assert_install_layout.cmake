# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the install step into a scratch prefix and checks that it produces
# exactly the documented layout, that the installed executables are the built
# ones, and that the installed launcher runs.
foreach(_var IN ITEMS BUILD_DIR PREFIX SOURCE_DIR BINDIR DOCDIR
                      LAUNCHER CLIENT VERSION)
  if(NOT DEFINED ${_var} OR "${${_var}}" STREQUAL "")
    message(FATAL_ERROR "${_var} is not set")
  endif()
endforeach()

set(config_args)
if(CONFIG)
  set(config_args --config "${CONFIG}")
endif()

file(REMOVE_RECURSE "${PREFIX}")
execute_process(
  COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}"
    ${config_args} --prefix "${PREFIX}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "cmake --install failed (${result}):\n${output}")
endif()

set(expected
  "${BINDIR}/mosh.exe"
  "${BINDIR}/mosh-client.exe"
  "${DOCDIR}/LICENSE"
  "${DOCDIR}/README.md"
  "${DOCDIR}/THIRD_PARTY_NOTICES.md")
file(GLOB licenses RELATIVE "${SOURCE_DIR}/third_party/licenses"
  "${SOURCE_DIR}/third_party/licenses/*")
if(NOT licenses)
  message(FATAL_ERROR "no bundled licenses in ${SOURCE_DIR}/third_party/licenses")
endif()
foreach(_license IN LISTS licenses)
  list(APPEND expected "${DOCDIR}/licenses/${_license}")
endforeach()
list(SORT expected)

file(GLOB_RECURSE installed RELATIVE "${PREFIX}" "${PREFIX}/*")
list(SORT installed)
if(NOT installed STREQUAL expected)
  string(REPLACE ";" "\n  " _installed "${installed}")
  string(REPLACE ";" "\n  " _expected "${expected}")
  message(FATAL_ERROR
    "unexpected install layout\ninstalled:\n  ${_installed}\n"
    "expected:\n  ${_expected}")
endif()

foreach(_pair IN ITEMS "mosh.exe|${LAUNCHER}" "mosh-client.exe|${CLIENT}")
  string(REPLACE "|" ";" _pair "${_pair}")
  list(GET _pair 0 _name)
  list(GET _pair 1 _built)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${PREFIX}/${BINDIR}/${_name}" "${_built}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed ${_name} differs from ${_built}")
  endif()
endforeach()

execute_process(
  COMMAND "${PREFIX}/${BINDIR}/mosh.exe" --version
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE output
  TIMEOUT 10)
if(NOT result EQUAL 0 OR NOT output MATCHES "^mosh ${VERSION}\n")
  message(FATAL_ERROR
    "installed mosh.exe --version returned ${result}:\n${output}")
endif()

file(REMOVE_RECURSE "${PREFIX}")
