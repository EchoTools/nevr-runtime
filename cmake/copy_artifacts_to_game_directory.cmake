# Copies the build artifacts to the game directory if COPY_TO_GAME_DIR is set
function(copy_artifacts_to_game_directory)
  if(COPY_TO_GAME_DIR)
    message(
      STATUS "Copying build artifacts to game directory: ${ECHOVR_BIN_PATH}")

    # Base commands for the custom target
    set(COPY_COMMANDS
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:GamePatches> ${ECHOVR_BIN_PATH}/dbgcore.dll COMMAND
        ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GameServer>
        ${ECHOVR_BIN_PATH}/pnsradgameserver.dll)

    # Add PDB copy commands only for Debug builds if(CMAKE_BUILD_TYPE STREQUAL
    # "Debug")
    list(
      APPEND
      COPY_COMMANDS
      COMMAND
      ${CMAKE_COMMAND}
      -E
      copy_if_different
      $<TARGET_PDB_FILE:GamePatches>
      ${ECHOVR_BIN_PATH}/dbgcore.pdb
      COMMAND
      ${CMAKE_COMMAND}
      -E
      copy_if_different
      $<TARGET_PDB_FILE:GameServer>
      ${ECHOVR_BIN_PATH}/pnsradgameserver.pdb)
    set(COPY_MESSAGE
        "Copied GamePatches and GameServer with PDBs to ${ECHOVR_BIN_PATH}")
    # else() set(COPY_MESSAGE "Copied GamePatches and GameServer to
    # ${ECHOVR_BIN_PATH} (skipping PDBs in ${CMAKE_BUILD_TYPE} build)" ) endif()

    # Create the custom target with the appropriate commands
    add_custom_target(
      CopyToGameDir ALL
      ${COPY_COMMANDS}
      COMMENT "${COPY_MESSAGE}"
      DEPENDS GamePatches GameServer)
  else()
    message(STATUS "Skipping copying to game directory")
  endif()
endfunction()
