cmake_minimum_required(VERSION 3.10)
project(NEVRSupervisor_PowerShell)

set(SCRIPT_FILES
	main.ps1
	versioning.ps1
	firewall.ps1
	logging.ps1
	ports.ps1
	instance.ps1
	serverserver.ps1
)

set(OUTPUT_SCRIPT "${CMAKE_BINARY_DIR}/NEVRSupervisor.ps1")

add_custom_command(
	OUTPUT ${OUTPUT_SCRIPT}
	COMMAND ${CMAKE_COMMAND} -E echo "# Combined NEVRSupervisor.ps1" > ${OUTPUT_SCRIPT}
	COMMAND ${CMAKE_COMMAND} -E cat ${SCRIPT_FILES} >> ${OUTPUT_SCRIPT}
	DEPENDS ${SCRIPT_FILES}
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
	COMMENT "Concatenating PowerShell scripts into ${OUTPUT_SCRIPT}"
)

add_custom_target(combine ALL DEPENDS ${OUTPUT_SCRIPT})
