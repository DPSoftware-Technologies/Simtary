# IncrementBuild.cmake — persistent build counter + version.h generator.
#
# Invoked in CMake script mode (cmake -P) from CMakeLists.txt. Reads an integer
# from COUNTER_FILE, optionally increments it (default), writes it back, then
# generates OUTPUT from TEMPLATE with the build number / version / date filled in.
#
# Required -D args:
#   COUNTER_FILE  path to the persistent build-number file
#   TEMPLATE      path to version.h.in
#   OUTPUT        path to the generated version.h
#   PROJECT_VER   the project version string (e.g. 1.0.0)
# Optional:
#   BUMP=OFF      generate the header from the current count WITHOUT incrementing
#                 (used at configure time so the header exists for IDEs/indexers).

if(EXISTS "${COUNTER_FILE}")
    file(READ "${COUNTER_FILE}" _count)
    string(STRIP "${_count}" _count)
else()
    set(_count 0)
endif()

# Guard against a corrupted/empty counter file.
if(NOT _count MATCHES "^[0-9]+$")
    set(_count 0)
endif()

if(NOT DEFINED BUMP OR BUMP)
    math(EXPR _count "${_count} + 1")
    file(WRITE "${COUNTER_FILE}" "${_count}")
endif()

set(ST_APP_BUILD_NUMBER ${_count})
set(ST_APP_VERSION       "${PROJECT_VER}")
string(TIMESTAMP ST_APP_BUILD_DATE "%Y-%m-%d %H:%M:%S")

configure_file("${TEMPLATE}" "${OUTPUT}" @ONLY)

message(STATUS "App version ${ST_APP_VERSION} build #${ST_APP_BUILD_NUMBER} (${ST_APP_BUILD_DATE})")
