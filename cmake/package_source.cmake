# Copies the source tree (git archive of HEAD) into the install prefix, so a release
# package has CMakeLists.txt and tools/ next to BotWLauncher.exe and the launcher can build.
# Run by `cmake --install` / botw_package; BOTW_SRC and BOTW_GIT are set by CMakeLists.txt.
if(NOT BOTW_GIT OR NOT EXISTS "${BOTW_SRC}/.git")
  message(WARNING "botw_package: git or .git not found, source not included in the package")
  return()
endif()
file(MAKE_DIRECTORY "${CMAKE_INSTALL_PREFIX}")
set(_tar "${CMAKE_INSTALL_PREFIX}/.botw_source.tar")
execute_process(COMMAND "${BOTW_GIT}" -C "${BOTW_SRC}" archive --format=tar -o "${_tar}" HEAD
  RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "botw_package: git archive failed (${_rc})")
endif()
file(ARCHIVE_EXTRACT INPUT "${_tar}" DESTINATION "${CMAKE_INSTALL_PREFIX}")
file(REMOVE "${_tar}")
message(STATUS "botw_package: source included in ${CMAKE_INSTALL_PREFIX}")
