set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

# Locate the MinGW-w64 UCRT toolchain without assuming any machine-specific
# installation directory.  Resolution order:
#   1. -DMINGW64_ROOT=<dir> (or a CMakeUserPresets.json cache variable)
#   2. the MINGW64_ROOT environment variable
#   3. the directory containing gcc.exe found on PATH
# The toolchain itself (x64, UCRT, GCC version) is validated by the top-level
# CMakeLists.txt once the compilers are known.
if(NOT MINGW64_ROOT AND DEFINED ENV{MINGW64_ROOT} AND NOT "$ENV{MINGW64_ROOT}" STREQUAL "")
  file(TO_CMAKE_PATH "$ENV{MINGW64_ROOT}" _mingw64_root_from_env)
  set(MINGW64_ROOT "${_mingw64_root_from_env}" CACHE PATH
    "MinGW-w64 UCRT toolchain root")
endif()
if(NOT MINGW64_ROOT)
  find_program(_MINGW64_GCC_ON_PATH NAMES gcc.exe gcc NO_CACHE)
  if(_MINGW64_GCC_ON_PATH)
    get_filename_component(_mingw64_bin_on_path "${_MINGW64_GCC_ON_PATH}" DIRECTORY)
    get_filename_component(_mingw64_root_on_path "${_mingw64_bin_on_path}" DIRECTORY)
    set(MINGW64_ROOT "${_mingw64_root_on_path}" CACHE PATH
      "MinGW-w64 UCRT toolchain root")
  endif()
endif()
if(NOT MINGW64_ROOT)
  message(FATAL_ERROR
    "The MinGW-w64 UCRT toolchain was not found. Set -DMINGW64_ROOT=<dir>, "
    "set the MINGW64_ROOT environment variable, or put its bin directory on "
    "PATH.")
endif()

# try_compile() re-reads this file in a fresh project; forward the resolved
# root so that probe builds use exactly the same toolchain.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES MINGW64_ROOT)

set(_MINGW64_BIN "${MINGW64_ROOT}/bin")
if(NOT EXISTS "${_MINGW64_BIN}/gcc.exe")
  message(FATAL_ERROR
    "MINGW64_ROOT does not contain bin/gcc.exe: ${MINGW64_ROOT}")
endif()

set(CMAKE_C_COMPILER "${_MINGW64_BIN}/gcc.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_CXX_COMPILER "${_MINGW64_BIN}/g++.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RC_COMPILER "${_MINGW64_BIN}/windres.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_AR "${_MINGW64_BIN}/ar.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_RANLIB "${_MINGW64_BIN}/ranlib.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_STRIP "${_MINGW64_BIN}/strip.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_OBJDUMP "${_MINGW64_BIN}/objdump.exe" CACHE FILEPATH "" FORCE)
set(CMAKE_MAKE_PROGRAM "${_MINGW64_BIN}/mingw32-make.exe" CACHE FILEPATH "" FORCE)

set(CMAKE_FIND_ROOT_PATH "${MINGW64_ROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
