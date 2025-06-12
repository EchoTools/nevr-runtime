# Copies the build artifacts to the game directory if COPY_TO_GAME_DIR is set

function(copy_artifacts_to_game_directory)
  if(COPY_TO_GAME_DIR)
    message(
      STATUS "Copying build artifacts to game directory: ${ECHOVR_BIN_PATH}")
    add_custom_target(
      CopyToGameDir ALL
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GamePatches>
              ${ECHOVR_BIN_PATH}/dbgcore.dll
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              $<TARGET_PDB_FILE:GamePatches> ${ECHOVR_BIN_PATH}/dbgcore.pdb
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GameServer>
              ${ECHOVR_BIN_PATH}/pnsradgameserver.dll
      COMMAND
        ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:GameServer>
        ${ECHOVR_BIN_PATH}/pnsradgameserver.pdb
      COMMENT "Copied GamePatches and GameServer to ${ECHOVR_BIN_PATH}"
      DEPENDS GamePatches GameServer)
  else()
    message(STATUS "Skipping copying to game directory")
  endif()
endfunction()
