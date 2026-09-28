# Apply the patch series in patches/series to the fetched upstream Mosh tree.
#
# Application is idempotent: a patch whose reverse applies cleanly is treated
# as already applied, so reconfiguring an existing build directory is safe.
#
# The upstream tree normally lives below this project's build directory, and
# therefore inside this project's Git work tree.  When git runs inside another
# repository, "git apply" resolves patch paths against that repository's root
# and silently skips everything outside the current directory, which would
# make both the forward and the reverse check succeed without touching a
# single file.  GIT_CEILING_DIRECTORIES stops repository discovery at the
# upstream tree, so git always behaves like a plain "patch" there.
function(mosh_apply_patch_series source_dir patch_dir series_file)
  find_package(Git REQUIRED)
  file(STRINGS "${series_file}" _series ENCODING UTF-8)

  get_filename_component(_ceiling "${source_dir}" DIRECTORY)
  set(_git
    "${CMAKE_COMMAND}" -E env
      --unset=GIT_DIR
      --unset=GIT_WORK_TREE
      "GIT_CEILING_DIRECTORIES=${_ceiling}"
    "${GIT_EXECUTABLE}")

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
      COMMAND ${_git} apply --reverse --check "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _already_applied
      OUTPUT_QUIET ERROR_QUIET)
    if(_already_applied EQUAL 0)
      message(STATUS "Mosh patch already applied: ${_entry}")
      continue()
    endif()

    execute_process(
      COMMAND ${_git} apply --check "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _check_result
      ERROR_VARIABLE _check_error)
    if(NOT _check_result EQUAL 0)
      message(FATAL_ERROR "Cannot apply ${_entry}:\n${_check_error}")
    endif()

    execute_process(
      COMMAND ${_git} apply "${_patch}"
      WORKING_DIRECTORY "${source_dir}"
      RESULT_VARIABLE _apply_result
      ERROR_VARIABLE _apply_error)
    if(NOT _apply_result EQUAL 0)
      message(FATAL_ERROR "Applying ${_entry} failed:\n${_apply_error}")
    endif()
    message(STATUS "Applied Mosh patch: ${_entry}")
  endforeach()
endfunction()
