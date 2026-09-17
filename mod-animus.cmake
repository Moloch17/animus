#
# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# mod-animus needs animus-lib, the code it shares with mod-animus-forge. When modules/mod-animus-lib is missing it is
# cloned from ANIMUS_LIB_GIT_URL at ANIMUS_LIB_GIT_REF and built with this configure.
#
# Installs the exported models (models/*.amdl, each with its .json layout manifest, which a model does not load
# without) into the data directory, where the worldserver finds them through Animus.ModelDir (default "animus",
# resolved against DataDir).
#
# The default destination matches the Docker layout (AC_DATA_DIR=/azerothcore/env/dist/data). If
# your worldserver's DataDir is elsewhere -- the stock worldserver.conf uses ".", the working
# directory -- point this at <DataDir>/animus instead:
#
#     cmake ... -DANIMUS_MODELS_INSTALL_DIR=/path/to/data/animus
#

set(ANIMUS_LIB_GIT_URL "https://github.com/Moloch17/animus-lib.git" CACHE STRING
  "Where mod-animus and mod-animus-forge clone animus-lib from when modules/mod-animus-lib is missing")
set(ANIMUS_LIB_GIT_REF "master" CACHE STRING "The animus-lib branch or tag to clone")

ModuleNameToVariable(mod-animus ANIMUS_LINKAGE_VARIABLE)
if(NOT "${${ANIMUS_LINKAGE_VARIABLE}}" MATCHES "static|dynamic")
  return()
endif()

# Only when the module is built: a core that disables it (the forge) must not install into its data directory, which
# the Docker layout mounts read-only.
set(ANIMUS_MODELS_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/data/animus" CACHE PATH
  "Where mod-animus installs its .amdl models; should be <DataDir>/<Animus.ModelDir>")

install(
  DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/models/"
  DESTINATION "${ANIMUS_MODELS_INSTALL_DIR}"
  FILES_MATCHING PATTERN "*.amdl" PATTERN "*.json")

message(STATUS "  mod-animus models install to ${ANIMUS_MODELS_INSTALL_DIR}")

set(ANIMUS_LIB_CHECKOUT "${CMAKE_SOURCE_DIR}/modules/mod-animus-lib")
if(NOT EXISTS "${ANIMUS_LIB_CHECKOUT}/cmake/AnimusLibDependency.cmake")
  if(EXISTS "${ANIMUS_LIB_CHECKOUT}")
    message(FATAL_ERROR "${ANIMUS_LIB_CHECKOUT} exists but is not animus-lib; remove it to have it cloned again")
  endif()

  find_package(Git REQUIRED)
  message(STATUS "  mod-animus: cloning animus-lib ${ANIMUS_LIB_GIT_REF} from ${ANIMUS_LIB_GIT_URL}")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" clone --branch "${ANIMUS_LIB_GIT_REF}" "${ANIMUS_LIB_GIT_URL}" "${ANIMUS_LIB_CHECKOUT}"
    RESULT_VARIABLE ANIMUS_LIB_CLONE_RESULT)
  if(NOT ANIMUS_LIB_CLONE_RESULT EQUAL 0)
    message(FATAL_ERROR "Could not clone animus-lib from ${ANIMUS_LIB_GIT_URL}; clone it into ${ANIMUS_LIB_CHECKOUT} "
      "by hand")
  endif()
endif()

include("${ANIMUS_LIB_CHECKOUT}/cmake/AnimusLibDependency.cmake")
AnimusLibRequire(mod-animus)
