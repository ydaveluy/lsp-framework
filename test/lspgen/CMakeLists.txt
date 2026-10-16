set(AUDIT_GENERATED_ROOT ${CMAKE_CURRENT_BINARY_DIR}/generated)
set(AUDIT_GENERATED_DIR ${AUDIT_GENERATED_ROOT}/lsp)
file(MAKE_DIRECTORY ${AUDIT_GENERATED_DIR})
add_custom_command(
	OUTPUT ${AUDIT_GENERATED_DIR}/types.h ${AUDIT_GENERATED_DIR}/types.cpp
	       ${AUDIT_GENERATED_DIR}/client_endpoint.h ${AUDIT_GENERATED_DIR}/client_endpoint.cpp
	       ${AUDIT_GENERATED_DIR}/server_endpoint.h ${AUDIT_GENERATED_DIR}/server_endpoint.cpp
	       ${AUDIT_GENERATED_DIR}/messages.h ${AUDIT_GENERATED_DIR}/protocol_version.h
	COMMAND lspgen ${CMAKE_CURRENT_SOURCE_DIR}/regressions.json
	WORKING_DIRECTORY ${AUDIT_GENERATED_DIR}
	DEPENDS lspgen ${CMAKE_CURRENT_SOURCE_DIR}/regressions.json
)
lsp_add_test(generator)
target_sources(test_generator PRIVATE
	${LSP_DIR}/lspgen/meta_model.cpp
	${AUDIT_GENERATED_DIR}/types.cpp
)
target_include_directories(test_generator PRIVATE ${AUDIT_GENERATED_ROOT})
add_library(generator_endpoints_compile OBJECT
	${AUDIT_GENERATED_DIR}/client_endpoint.cpp
	${AUDIT_GENERATED_DIR}/server_endpoint.cpp
)
target_link_libraries(generator_endpoints_compile PRIVATE lsp)
target_include_directories(generator_endpoints_compile PRIVATE ${AUDIT_GENERATED_ROOT})
lsp_add_test(protocol_types)
