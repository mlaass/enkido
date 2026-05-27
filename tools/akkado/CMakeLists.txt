# akkado CLI - Compiler

# Target label is `akkado_cli` to avoid colliding with the `akkado` library
# target. OUTPUT_NAME controls the produced binary filename ("akkado" /
# "akkado.exe").
add_executable(akkado_cli
    main.cpp
)

set_target_properties(akkado_cli PROPERTIES
    OUTPUT_NAME akkado
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_link_libraries(akkado_cli
    PRIVATE
        akkado
        nkido_compiler_options
)

# Install
install(TARGETS akkado_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
