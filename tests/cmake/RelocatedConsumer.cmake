# Install into an isolated staging tree and move it before resolving imported targets.
file(MAKE_DIRECTORY "${WORK}")
string(RANDOM LENGTH 12 run)
set(stage "${WORK}/${run}-stage")
set(prefix "${WORK}/${run}-relocated")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${RHI_BUILD}" --prefix "${stage}" --config "${CONFIG}"
                COMMAND_ERROR_IS_FATAL ANY)
file(RENAME "${stage}" "${prefix}")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE}" -B "${WORK}/consumer-${run}" -G "${GENERATOR}"
    "-DCMAKE_BUILD_TYPE=${CONFIG}" "-DCMAKE_PREFIX_PATH=${prefix}" "-DZenFoundation_DIR=${FOUNDATION_DIR}"
    "-DSDL3_DIR=${SDL3_DIR}" "-DEXAMPLES=${SOURCE}/../../../examples" "-Dspdlog_DIR=${SPDLOG_DIR}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${WORK}/consumer-${run}" --config "${CONFIG}"
                COMMAND_ERROR_IS_FATAL ANY)
if(RUN_INSTALLED_TESTS)
    execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${WORK}/consumer-${run}"
        -C "${CONFIG}" --output-on-failure COMMAND_ERROR_IS_FATAL ANY)
endif()
