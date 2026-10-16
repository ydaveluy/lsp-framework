file(REMOVE_RECURSE "${WORK}")
set(prefix "${WORK}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DCMAKE_INSTALL_PREFIX=${prefix}" "-DCMAKE_INSTALL_CONFIG_NAME=${CONFIG}"
    -P "${BUILD}/cmake_install.cmake" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Framework install failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${WORK}/build"
    "-DCMAKE_PREFIX_PATH=${prefix}" "-DCMAKE_CXX_COMPILER=${CXX}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer configure failed: ${result}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/build"
    --config "${CONFIG}" --parallel 2 RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer build failed: ${result}")
endif()
set(executable "${WORK}/build/consumer${EXE_SUFFIX}")
if(EXISTS "${WORK}/build/${CONFIG}/consumer${EXE_SUFFIX}")
  set(executable "${WORK}/build/${CONFIG}/consumer${EXE_SUFFIX}")
endif()
execute_process(COMMAND "${executable}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed consumer execution failed: ${result}")
endif()
