cmake_minimum_required(VERSION 3.12)

project(lsp)

# compiler flags

# Standalone builds at 20; an embedding consumer's standard is respected.
#
# EACH OF THE THREE IS ONLY SET WHEN NOBODY ELSE HAS: they are global
# settings, and re-stating one here would silently override the value an
# embedding consumer already chose.
if(NOT DEFINED CMAKE_CXX_STANDARD)
	set(CMAKE_CXX_STANDARD 20)
endif()
if(NOT DEFINED CMAKE_CXX_STANDARD_REQUIRED)
	set(CMAKE_CXX_STANDARD_REQUIRED YES)
endif()
if(NOT DEFINED CMAKE_CXX_EXTENSIONS)
	set(CMAKE_CXX_EXTENSIONS NO)
endif()

option(LSP_BUILD_EXAMPLES "Build the examples" OFF)
option(LSP_BUILD_TESTS "Build the tests" OFF)
option(LSP_INSTALL "Configure lsp install configuration" ON)
set(LSP_METAMODEL_JSON ${CMAKE_CURRENT_SOURCE_DIR}/lspgen/metaModel.json CACHE FILEPATH "Path to the LSP meta model json passed to lspgen")


# WARNINGS ARE THE DEVELOPER'S, LIKE THE SANITIZERS. A consumer that embeds
# this project imposes its own set, so ours is only added when this project is
# built top-level.
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
	if(PROJECT_IS_TOP_LEVEL)
		add_compile_options(/W4)
	endif()
	add_compile_options(/bigobj)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
	option(LSP_USE_SANITIZERS "Use address and undefined behavior sanitizers for Debug builds" OFF)

	if(PROJECT_IS_TOP_LEVEL)
		add_compile_options(
			-Wall
			-Wextra
			-pedantic
			-Wsign-conversion
			-Wfloat-conversion
		)

		if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
			add_compile_options(
				-Wimplicit-int-conversion
				-Wimplicit-float-conversion
			)
		endif()
	endif()

	if(LSP_USE_SANITIZERS)
		add_compile_options(
			$<$<CONFIG:DEBUG>:-fsanitize=address,undefined>
			$<$<CONFIG:DEBUG>:-fno-omit-frame-pointer>
		)
		add_link_options($<$<CONFIG:DEBUG>:-fsanitize=address,undefined>)
	endif()

	if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
		add_compile_options(-Wa,-mbig-obj)
	endif()
endif()

if(WIN32)
	add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# lspgen

# EXCLUDE_FROM_ALL: the generator is out of the build path, which is what lets a
# cross build work -- a wasm32 `lspgen` is a file the host cannot run. Only
# `regenerate` builds it, and only a meta-model bump asks for that.
add_executable(lspgen EXCLUDE_FROM_ALL lspgen/lspgen.cpp lsp/json/json.cpp)
target_include_directories(lspgen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# liblsp

set(LSP_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(LSP_SRC_DIR ${LSP_DIR}/lsp)

set(LSP_HEADERS
	# lsp
	concepts.h
	connection.h
	enumeration.h
	error.h
	exception.h
	messagebase.h
	messagehandler.h
	nullable.h
	process.h
	requestresult.h
	serialization.h
	strmap.h
	threadpool.h
	uri.h
	# io
	io/socket.h
	io/standardio.h
	io/stream.h
	# json
	json/json.h
	# jsonrpc
	jsonrpc/jsonrpc.h
)
list(TRANSFORM LSP_HEADERS PREPEND ${LSP_SRC_DIR}/)

set(LSP_SOURCES
	# lsp
	connection.cpp
	messagehandler.cpp
	process.cpp
	threadpool.cpp
	uri.cpp
	# io
	io/socket.cpp
	io/standardio.cpp
	# json
	json/json.cpp
	# jsonrpc
	jsonrpc/jsonrpc.cpp
)
list(TRANSFORM LSP_SOURCES PREPEND ${LSP_SRC_DIR}/)

# THE GENERATED TYPES ARE CHECKED IN, and the meta model is why: it is the LSP
# specification's own file, which moves a few times a year. Regenerating it on
# every build paid a compile and a run of `lspgen` for an answer that had not
# changed -- and made CROSS BUILDS impossible, since a generator built for the
# target is a program the host cannot run.
#
# `make regenerate` is what a meta-model bump runs. CI runs it too and requires
# the diff to be EMPTY: that check is the whole reason committing generated
# files is safe, and without it they would silently drift from their source.
set(LSP_GENERATED_FILES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/generated)

set(LSP_GENERATED_HEADERS
	${LSP_GENERATED_FILES_DIR}/lsp/types.h
	${LSP_GENERATED_FILES_DIR}/lsp/messages.h
)

set(LSP_GENERATED_SOURCES
	${LSP_GENERATED_FILES_DIR}/lsp/types.cpp
)

add_custom_target(regenerate
	COMMAND lspgen ${LSP_METAMODEL_JSON}
	WORKING_DIRECTORY ${LSP_GENERATED_FILES_DIR}/lsp
	COMMENT "Regenerating lsp types from the meta model (commit the result)..."
)
add_dependencies(regenerate lspgen)

add_library(lsp
	${LSP_HEADERS}
	${LSP_SOURCES}
	${LSP_GENERATED_HEADERS}
	${LSP_GENERATED_SOURCES}
)
include(GNUInstallDirs)
target_include_directories(lsp PUBLIC
	$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
	$<BUILD_INTERFACE:${LSP_GENERATED_FILES_DIR}>
	$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
	set_target_properties(
		lsp PROPERTIES
		DEBUG_POSTFIX d
	)
endif()

if (LSP_INSTALL)
	include(cmake/utils.cmake)
	lsp_read_version()

	include(CMakePackageConfigHelpers)
	set(EXPORT_CMAKE_CONFIG_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/lsp/")

	# write and install config files
	write_basic_config_version_file(
		"${CMAKE_CURRENT_BINARY_DIR}/lspConfigVersion.cmake"
		VERSION ${LSP_VERSION}
		COMPATIBILITY SameMajorVersion
	)
	configure_package_config_file(
		"${CMAKE_CURRENT_SOURCE_DIR}/cmake/lspConfig.cmake.in"
		"${CMAKE_CURRENT_BINARY_DIR}/lspConfig.cmake"
		INSTALL_DESTINATION
		${EXPORT_CMAKE_CONFIG_DIR}
	)
	install(
		EXPORT lsp
		DESTINATION ${EXPORT_CMAKE_CONFIG_DIR}
		NAMESPACE lsp::
		FILE "lspConfigTargets.cmake"
	)
	install(
		FILES
			"${CMAKE_CURRENT_BINARY_DIR}/lspConfigVersion.cmake"
			"${CMAKE_CURRENT_BINARY_DIR}/lspConfig.cmake"
		DESTINATION ${EXPORT_CMAKE_CONFIG_DIR}
	)

	install(TARGETS lsp EXPORT lsp ARCHIVE LIBRARY)
	install(
		FILES ${LSP_GENERATED_HEADERS}
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/lsp
	)
	install(
		DIRECTORY ${LSP_SRC_DIR}
		DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
		FILES_MATCHING REGEX ".*\\.(h|inl)"
	)
endif()

if(WIN32)
	target_link_libraries(lsp PUBLIC Ws2_32)
endif()

if(LSP_BUILD_TESTS)
	enable_testing()
	# The checks are assert based, so NDEBUG would turn a Release run into a no-op.
	add_executable(LspRoundTripTest ${LSP_DIR}/tests/rpc_serialization_roundtrip.cpp)
	target_link_libraries(LspRoundTripTest lsp)

	if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		target_compile_options(LspRoundTripTest PRIVATE -UNDEBUG)
	endif()

	add_test(NAME RoundTrip COMMAND LspRoundTripTest)

	add_executable(LspCancellationTest ${LSP_DIR}/tests/cancellation.cpp)
	target_link_libraries(LspCancellationTest lsp)

	if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		target_compile_options(LspCancellationTest PRIVATE -UNDEBUG)
	endif()

	add_test(NAME Cancellation COMMAND LspCancellationTest)
endif()

if(LSP_BUILD_EXAMPLES)
	# Server
	add_executable(LspServerExample ${LSP_DIR}/examples/server.cpp)
	target_link_libraries(LspServerExample lsp)
	# Client
	add_executable(LspClientExample ${LSP_DIR}/examples/client.cpp)
	target_link_libraries(LspClientExample lsp)
endif()
