#
# Included by modules/CMakeLists.txt (modules/<name>/<name>.cmake).
#
# Installs the exported models (models/*.amdl) into the data directory, where the worldserver finds
# them through Animus.ModelDir (default "animus", resolved against DataDir).
#
# The default destination matches the Docker layout (AC_DATA_DIR=/azerothcore/env/dist/data). If
# your worldserver's DataDir is elsewhere -- the stock worldserver.conf uses ".", the working
# directory -- point this at <DataDir>/animus instead:
#
#     cmake ... -DANIMUS_MODELS_INSTALL_DIR=/path/to/data/animus
#

set(ANIMUS_MODELS_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/data/animus" CACHE PATH
  "Where mod-animus installs its .amdl models; should be <DataDir>/<Animus.ModelDir>")

install(
  DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/models/"
  DESTINATION "${ANIMUS_MODELS_INSTALL_DIR}"
  FILES_MATCHING PATTERN "*.amdl")

message(STATUS "  mod-animus models install to ${ANIMUS_MODELS_INSTALL_DIR}")
