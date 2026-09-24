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
# without) with the module's configs, in <config dir>/modules/animus: under Docker only the bin/ and etc/ directories
# of the build reach the runtime image, so models installed into the data directory never arrived. The module looks
# there for Animus.ModelDir's default "animus" (after DataDir/animus, for models placed by hand). To install elsewhere:
#
#     cmake ... -DANIMUS_MODELS_INSTALL_DIR=/path/to/models

ModuleNameToVariable(mod-animus ANIMUS_LINKAGE_VARIABLE)
if(NOT "${${ANIMUS_LINKAGE_VARIABLE}}" MATCHES "static|dynamic")
  return()
endif()

# mod-animus-forge used to share the curriculum with this module through animus-lib, so a configure could enable
# both and link one copy. The library is gone: the forge carries those sources in its own src/, this module carries
# them in its bundle, and two copies in one build is a duplicate-symbol link failure with nothing to say why. A
# forge core is not a realm and has no use for companions, so the answer is to enable one of them.
list(FIND MODULES_MODULE_LIST "mod-animus-forge" ANIMUS_FORGE_INDEX)
if(NOT ANIMUS_FORGE_INDEX EQUAL -1)
  ModuleNameToVariable(mod-animus-forge ANIMUS_FORGE_LINKAGE)
  if("${${ANIMUS_FORGE_LINKAGE}}" MATCHES "static|dynamic")
    message(FATAL_ERROR "mod-animus and mod-animus-forge each carry their own copy of the curriculum since "
      "animus-lib was folded in, and cannot be built together. Disable one: -DMODULE_MOD-ANIMUS=disabled for a "
      "forge core, or -DMODULE_MOD-ANIMUS-FORGE=disabled for a realm.")
  endif()
endif()

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

# Only when the module is built: a core that disables it (the forge) installs no models.
set(ANIMUS_MODELS_INSTALL_DIR "${ANIMUS_MODULE_CONF_DIR}/animus" CACHE PATH
  "Where mod-animus installs its .amdl models and their .json manifests")

install(
  DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/models/"
  DESTINATION "${ANIMUS_MODELS_INSTALL_DIR}"
  FILES_MATCHING PATTERN "*.amdl" PATTERN "*.json")

message(STATUS "  mod-animus models install to ${ANIMUS_MODELS_INSTALL_DIR}")

set(ANIMUS_LIB_BUNDLE "${CMAKE_CURRENT_LIST_DIR}/animus-lib")
if(EXISTS "${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
  include("${CMAKE_SOURCE_DIR}/modules/mod-animus-lib/cmake/AnimusLibDependency.cmake")
else()
  include("${ANIMUS_LIB_BUNDLE}/cmake/AnimusLibDependency.cmake")
endif()
# mod-animus builds against a stock AzerothCore, so it takes animus-lib's runtime half only: the blocks, the layout
# and encoders, the characters, the model and the bots. The training half builds curriculum episodes, and it calls
# PathGenerator::SetIncludeFlags, which exists only on the Animus Forge core -- collecting it here would make the
# module need a core patch to compile, which for a long while it silently did. Nothing here builds an episode: the
# stage viewer that once did was removed, and companions never needed it.
AnimusLibRequire(mod-animus "${ANIMUS_LIB_BUNDLE}" RUNTIME_ONLY)
