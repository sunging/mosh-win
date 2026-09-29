# SPDX-License-Identifier: GPL-3.0-or-later
# Install rules and CPack settings.
#
#   <prefix>/bin/mosh.exe
#   <prefix>/bin/mosh-client.exe
#   <prefix>/share/doc/mosh-win/{README.md,LICENSE,THIRD_PARTY_NOTICES.md}
#   <prefix>/share/doc/mosh-win/licenses/<statically linked runtime licenses>
#
# Both executables go to the same directory because mosh.exe looks for
# mosh-client.exe next to itself.  Dependencies are built EXCLUDE_FROM_ALL
# with their own install rules disabled, so nothing else is installed; the
# install.layout test checks this.

# Without a prefix CMake picks "Program Files (x86)" for MinGW Makefiles,
# because it cannot tell the target is 64-bit when the platform module runs.
# Use the native Program Files directory instead.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT AND DEFINED ENV{ProgramW6432})
  file(TO_CMAKE_PATH "$ENV{ProgramW6432}/${PROJECT_NAME}" _mosh_default_prefix)
  set(CMAKE_INSTALL_PREFIX "${_mosh_default_prefix}"
    CACHE PATH "Install path prefix, prepended onto install directories." FORCE)
endif()

include(GNUInstallDirs)

foreach(_runtime IN ITEMS mosh-launcher mosh-client)
  if(TARGET "${_runtime}")
    install(TARGETS "${_runtime}" RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  endif()
endforeach()

install(FILES
    "${PROJECT_SOURCE_DIR}/README.md"
    "${PROJECT_SOURCE_DIR}/LICENSE"
    "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}")
# The executables link the MinGW-w64 runtime, winpthreads and libgcc/libstdc++
# statically; their licenses must travel with every binary distribution.
install(DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/licenses/"
  DESTINATION "${CMAKE_INSTALL_DOCDIR}/licenses")

set(CPACK_PACKAGE_NAME "mosh-win")
set(CPACK_PACKAGE_VERSION "${MOSH_WIN_VERSION}")
set(CPACK_GENERATOR "ZIP")
set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
include(CPack)
