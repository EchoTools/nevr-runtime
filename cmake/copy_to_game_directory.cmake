option(COPY_TO_GAME_DIR "Copy build artifacts to game directory" ON)

# Post-build commands for libraries - only executed if COPY_TO_GAME_DIR is
# enabled
if(COPY_TO_GAME_DIR)
  add_custom_target(
    CopyGamePatches ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GamePatches>
            ${ECHOVR_BIN_PATH}/dbgcore.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:GamePatches>
            ${ECHOVR_BIN_PATH}/dbgcore.pdb
    COMMENT "Copying GamePatches to output directory"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:GameServer>
            ${ECHOVR_BIN_PATH}/pnsradgameserver.dll
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_PDB_FILE:GameServer>
            ${ECHOVR_BIN_PATH}/pnsradgameserver.pdb
    COMMENT "Copying GameServer to output directory"
    DEPENDS GamePatches GameServer)
endif()
