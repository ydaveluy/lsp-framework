include_directories(./testtest)

function(lsp_add_test name)
	add_executable(test_${name})
	target_sources(test_${name} PRIVATE test_${name}.cpp)
	target_link_libraries(test_${name} PRIVATE lsp)
	add_test(NAME test_${name} COMMAND test_${name})
endfunction()

add_subdirectory(lsp)
add_subdirectory(lspgen)
if(LSP_BUILD_EXAMPLES)
	add_test(NAME example_session
		COMMAND example_client --exe=$<TARGET_FILE:example_server>)
	set_tests_properties(example_session PROPERTIES TIMEOUT 30)
endif()
if(LSP_INSTALL AND NOT CMAKE_CROSSCOMPILING)
	add_test(NAME install_consumer
		COMMAND ${CMAKE_COMMAND}
			-DBUILD=${PROJECT_BINARY_DIR}
			-DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/install
			-DWORK=${CMAKE_CURRENT_BINARY_DIR}/install-consumer
			-DCXX=${CMAKE_CXX_COMPILER}
			-DEXE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}
			-DCONFIG=$<CONFIG>
			-P ${CMAKE_CURRENT_SOURCE_DIR}/install/consumer.cmake)
	set_tests_properties(install_consumer PROPERTIES TIMEOUT 120)
endif()
