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

# The policy's forward pass (MlpPolicy::Decide) is a dot product per row, and its accumulator is a serial
# floating-point dependency chain. Float addition is not associative, so without permission to reorder it no
# compiler will vectorise the reduction, and every companion's decision costs about three times what it should:
# measured on a real model, 310 us a decision at -O2 against 94 us with these flags, and 60 us where AVX2 is
# also allowed.
#
# The flags are scoped to this one file and are deliberately NOT -ffast-math: nothing here assumes the absence
# of NaN or infinity, only that float addition may be reordered. Verified on five shipped models over 4,000
# decisions each -- every decision identical before and after.
#
# -O3 is needed as well: at -O2 the vectoriser's cost model declines this loop even with the flags.
function(AnimusLibTuneForwardPass)
  foreach(candidate ${ARGN})
    if(EXISTS "${candidate}")
      if(MSVC)
        set(tuning /O2 /fp:fast)
      elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(tuning -O3 -fassociative-math -fno-signed-zeros -fno-trapping-math)
      else()
        return()
      endif()
      set_source_files_properties("${candidate}" PROPERTIES COMPILE_OPTIONS "${tuning}")
      message(STATUS "  animus-lib: MlpPolicy built with the forward pass vectorised")
      return()
    endif()
  endforeach()
endfunction()

function(AnimusLibRequire dependent bundleDir)
  # RUNTIME_ONLY collects src/runtime alone: the blocks, the layout and encoders, the characters, the model and the
  # bots -- everything needed to run a trained model, and nothing that builds a training episode. It is what
  # mod-animus asks for, because src/training calls PathGenerator::SetIncludeFlags (Encounters/TravelEncounter.cpp),
  # which exists only on the Animus Forge core. Collecting both roots is the default and what mod-animus-forge does.
  cmake_parse_arguments(ANIMUS_LIB "RUNTIME_ONLY" "" "" ${ARGN})
  ModuleNameToVariable(${dependent} dependentVariable)
  set(dependentLinkage "${${dependentVariable}}")
  if(NOT dependentLinkage MATCHES "static|dynamic")
    return()
  endif()

  string(TOLOWER "mod_${dependent}" dependentProject)
  string(TOLOWER "mod_${ANIMUS_LIB_MODULE}" libraryProject)

  # Whichever copy of the library this configure ends up building, the forward pass is the same hot loop.
  get_property(tuned GLOBAL PROPERTY ANIMUS_LIB_FORWARD_PASS_TUNED)
  if(NOT tuned)
    set_property(GLOBAL PROPERTY ANIMUS_LIB_FORWARD_PASS_TUNED TRUE)
    AnimusLibTuneForwardPass("${ANIMUS_LIB_MODULE_DIR}/src/runtime/Model/MlpPolicy.cpp"
      "${bundleDir}/src/runtime/Model/MlpPolicy.cpp")
  endif()

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

    # A development checkout is built by AzerothCore's own module machinery, which collects the module's whole src
    # tree -- both roots, whatever the dependent asked for. So the training half is always present here and its
    # hooks must be registered; RUNTIME_ONLY cannot be honoured in this layout, and a dependent that needs a stock
    # core should build against its bundle instead (remove modules/mod-animus-lib).
    target_compile_definitions(modules PRIVATE ANIMUS_LIB_TRAINING)

    get_property(announced GLOBAL PROPERTY ANIMUS_LIB_ANNOUNCED)
    if(NOT announced)
      set_property(GLOBAL PROPERTY ANIMUS_LIB_ANNOUNCED TRUE)
      message(STATUS "  animus-lib: using the ${ANIMUS_LIB_MODULE} module; bundled copies are ignored")
      if(ANIMUS_LIB_RUNTIME_ONLY)
        message(STATUS "  animus-lib: ${dependent} asked for the runtime half only, but a ${ANIMUS_LIB_MODULE} "
          "checkout builds both roots; it needs the forge core's PathGenerator::SetIncludeFlags")
      endif()
    endif()
    return()
  endif()

  if(dependentLinkage STREQUAL "dynamic")
    message(FATAL_ERROR "${dependent} is built dynamic, which needs animus-lib as its own module: copy "
      "${bundleDir} to ${ANIMUS_LIB_MODULE_DIR} and build it dynamic too")
  endif()


  if(NOT EXISTS "${bundleDir}/src/runtime/animus_lib_loader.cpp")
    message(FATAL_ERROR "${dependent}'s bundled animus-lib is missing (${bundleDir}/src); restore the module's "
      "animus-lib directory")
  endif()

  # A root is added once per configure, but which roots are wanted depends on the dependent -- and a runtime-only
  # dependent configuring first must not stop a later one getting the training half. So the two are tracked apart
  # rather than under one "already added" flag.
  set(animusLibRoots runtime)
  if(NOT ANIMUS_LIB_RUNTIME_ONLY)
    list(APPEND animusLibRoots training)
  endif()

  foreach(root ${animusLibRoots})
    string(TOUPPER "${root}" rootUpper)
    get_property(rootAdded GLOBAL PROPERTY ANIMUS_LIB_ADDED_${rootUpper})
    if(rootAdded)
      continue()
    endif()

    set_property(GLOBAL PROPERTY ANIMUS_LIB_ADDED_${rootUpper} TRUE)
    CollectSourceFiles("${bundleDir}/src/${root}" rootSources)
    CollectIncludeDirectories("${bundleDir}/src/${root}" rootIncludes)
    target_sources(modules PRIVATE ${rootSources})
    target_include_directories(modules PUBLIC ${rootIncludes})
  endforeach()

  # The loader registers the training half's core hooks only when that half is compiled (animus_lib_loader.cpp).
  if(NOT ANIMUS_LIB_RUNTIME_ONLY)
    target_compile_definitions(modules PRIVATE ANIMUS_LIB_TRAINING)
  endif()

  string(REPLACE ";" " + " animusLibRootsShown "${animusLibRoots}")
  message(STATUS "  animus-lib: built from ${bundleDir} (${animusLibRootsShown})")
endfunction()
