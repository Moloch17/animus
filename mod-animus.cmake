#
# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# mod-animus needs animus-lib, the code it shares with mod-animus-forge. Its source is bundled in animus-lib/ (a git
# subtree of https://github.com/Moloch17/animus-lib; tools/update-animus-lib.sh updates it), so this folder builds
# offline. A modules/mod-animus-lib checkout, when present, is built instead of the bundle.
#
# Installing also creates etc/modules/mod_animus.conf from its .dist when there is none: AzerothCore reads a module's
# settings from the .conf only, and without one every Animus key logs "Missing property" and keeps its default. An
# existing .conf is never overwritten.
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

# Where the core installs module configs (CopyModuleConfig, src/cmake/macros/ConfigInstall.cmake).
if(WIN32)
  set(ANIMUS_MODULE_CONF_DIR "${CMAKE_INSTALL_PREFIX}/configs/modules")
else()
  set(ANIMUS_MODULE_CONF_DIR "${CONF_DIR}/modules")
endif()
install(CODE "
  set(animusConf \"\$ENV{DESTDIR}${ANIMUS_MODULE_CONF_DIR}/mod_animus.conf\")
  if(NOT EXISTS \"\${animusConf}\")
    message(STATUS \"Creating: \${animusConf}\")
    configure_file(\"${CMAKE_CURRENT_LIST_DIR}/conf/mod_animus.conf.dist\" \"\${animusConf}\" COPYONLY)
  endif()")

set(ANIMUS_LIB_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/animus-lib")
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
  include("${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
else()
  include("${ANIMUS_LIB_BUNDLE}/cmake/AnimusLibDependency.cmake")
endif()
AnimusLibRequire(mod-animus "${ANIMUS_LIB_BUNDLE}")
