# Coverage.cmake - Code coverage support for GCC/Clang
#
# Provides:
#   - ENKIDO_ENABLE_COVERAGE option
#   - Coverage compile/link flags when enabled
#   - 'coverage' target for generating HTML reports with lcov

option(ENKIDO_ENABLE_COVERAGE "Enable code coverage" OFF)

if(ENKIDO_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(COV_FLAGS "--coverage")
    target_compile_options(enkido_compiler_options INTERFACE $<$<CONFIG:Debug>:${COV_FLAGS}>)
    target_link_options(enkido_compiler_options INTERFACE $<$<CONFIG:Debug>:${COV_FLAGS}>)

    find_program(LCOV lcov)
    find_program(GENHTML genhtml)

    if(LCOV AND GENHTML)
        add_custom_target(coverage
            COMMAND ${LCOV} --directory . --zerocounters
            COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
            COMMAND ${LCOV} --directory . --capture -o coverage.info
            COMMAND ${LCOV} --remove coverage.info '/usr/*' '${CMAKE_BINARY_DIR}/_deps/*' '*/tests/*' -o coverage.info
            COMMAND ${GENHTML} -o coverage_report coverage.info
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            COMMENT "Generating code coverage report in coverage_report/"
        )
    else()
        message(WARNING "lcov/genhtml not found - coverage target unavailable")
    endif()
endif()
