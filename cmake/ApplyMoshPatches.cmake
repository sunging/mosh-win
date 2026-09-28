function(mosh_apply_patch_series source_dir patch_dir series_file)
  find_package(Git REQUIRED)
  file(STRINGS "${series_file}" _series ENCODING UTF-8)

  foreach(_entry IN LISTS _series)
    string(STRIP "${_entry}" _entry)
    if(_entry STREQUAL "" OR _entry MATCHES "^#")
      continue()
    endif()

    set(_patch "${patch_dir}/${_entry}")
    if(NOT EXISTS "${_patch}")
      message(FATAL_ERROR "Patch listed in ${series_file} is missing: ${_patch}")
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _already_applied
      OUTPUT_QUIET ERROR_QUIET)
    if(_already_applied EQUAL 0)
      message(STATUS "Mosh patch already applied: ${_entry}")
      continue()
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply --check "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _check_result
      ERROR_VARIABLE _check_error)
    if(NOT _check_result EQUAL 0)
      message(FATAL_ERROR "Cannot apply ${_entry}:\n${_check_error}")
    endif()

    execute_process(
      COMMAND "${GIT_EXECUTABLE}" apply "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _apply_result
      ERROR_VARIABLE _apply_error)
    if(NOT _apply_result EQUAL 0)
      message(FATAL_ERROR "Applying ${_entry} failed:\n${_apply_error}")
    endif()
    message(STATUS "Applied Mosh patch: ${_entry}")
  endforeach()
endfunction()

