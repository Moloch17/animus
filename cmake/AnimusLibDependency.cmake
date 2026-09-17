# animus-lib as a dependency of another AzerothCore module (mod-animus, mod-animus-forge).
#
# Each dependent bundles the library's source as a git subtree in <module>/animus-lib, so a module folder builds
# offline against whatever core it is put in, with the library revision it was tested with. A dependent's
# <module>.cmake includes this file (from modules/mod-animus-lib when that is present, else from its own bundle) and
# calls AnimusLibRequire(<dependent> <bundle dir>). modules/CMakeLists.txt runs <module>.cmake files after the `modules`
# target exists, in its scope, so this sees the configure's module list and each module's linkage (static, dynamic,
# disabled). Exactly one copy of the library is built:
#
# - modules/mod-animus-lib is a module of this configure (a development checkout): it is the library. Static builds
#   need nothing more (every static module is in `modules`); a dynamic dependent links the library's own shared module,
#   so the library must be dynamic too. The bundles are ignored.
# - Otherwise the first static dependent to configure adds its bundle's sources and include directories to `modules`,
#   and the others find them added. The dependents' loaders run the library's scripts (Addmod_animus_libScripts is
#   idempotent). A dynamic dependent needs the library as its own module: copy a bundle to modules/mod-animus-lib.

set(ANIMUS_LIB_MODULE "mod-animus-lib")
set(ANIMUS_LIB_MODULE_DIR "${CMAKE_SOURCE_DIR}/modules/${ANIMUS_LIB_MODULE}")

function(AnimusLibRequire dependent bundleDir)
  ModuleNameToVariable(${dependent} dependentVariable)
  set(dependentLinkage "${${dependentVariable}}")
  if(NOT dependentLinkage MATCHES "static|dynamic")
    return()
  endif()

  string(TOLOWER "mod_${dependent}" dependentProject)
  string(TOLOWER "mod_${ANIMUS_LIB_MODULE}" libraryProject)

  list(FIND MODULES_MODULE_LIST ${ANIMUS_LIB_MODULE} libraryIndex)
  if(NOT libraryIndex EQUAL -1)
    ModuleNameToVariable(${ANIMUS_LIB_MODULE} libraryVariable)
    set(libraryLinkage "${${libraryVariable}}")

    if(NOT libraryLinkage MATCHES "static|dynamic")
      message(FATAL_ERROR "${dependent} needs ${ANIMUS_LIB_MODULE}, which is disabled: set ${libraryVariable} to "
        "${dependentLinkage}, remove ${ANIMUS_LIB_MODULE_DIR} to build ${dependent}'s bundled copy, or disable "
        "${dependent}")
    endif()

    if(NOT libraryLinkage STREQUAL dependentLinkage)
      message(FATAL_ERROR "${dependent} is built ${dependentLinkage} but ${ANIMUS_LIB_MODULE} ${libraryLinkage}: "
        "build both the same way (${libraryVariable}=${dependentLinkage})")
    endif()

    if(dependentLinkage STREQUAL "dynamic")
      target_link_libraries(${dependentProject} PUBLIC ${libraryProject})
    endif()

    get_property(announced GLOBAL PROPERTY ANIMUS_LIB_ANNOUNCED)
    if(NOT announced)
      set_property(GLOBAL PROPERTY ANIMUS_LIB_ANNOUNCED TRUE)
      message(STATUS "  animus-lib: using the ${ANIMUS_LIB_MODULE} module; bundled copies are ignored")
    endif()
    return()
  endif()

  if(dependentLinkage STREQUAL "dynamic")
    message(FATAL_ERROR "${dependent} is built dynamic, which needs animus-lib as its own module: copy "
      "${bundleDir} to ${ANIMUS_LIB_MODULE_DIR} and build it dynamic too")
  endif()

  get_property(alreadyAdded GLOBAL PROPERTY ANIMUS_LIB_ADDED_TO_MODULES)
  if(alreadyAdded)
    return()
  endif()

  if(NOT EXISTS "${bundleDir}/src/animus_lib_loader.cpp")
    message(FATAL_ERROR "${dependent}'s bundled animus-lib is missing (${bundleDir}/src); restore the module's "
      "animus-lib directory")
  endif()

  set_property(GLOBAL PROPERTY ANIMUS_LIB_ADDED_TO_MODULES TRUE)

  CollectSourceFiles("${bundleDir}/src" librarySources)
  CollectIncludeDirectories("${bundleDir}/src" libraryIncludes)
  target_sources(modules PRIVATE ${librarySources})
  target_include_directories(modules PUBLIC ${libraryIncludes})
  message(STATUS "  animus-lib: built from ${bundleDir}")
endfunction()
