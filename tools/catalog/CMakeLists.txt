# catalog CLI - sample catalog scan/search/stats

# Target label is `catalog_cli` to avoid colliding with the `catalog` library
# target. OUTPUT_NAME controls the produced binary filename.
add_executable(catalog_cli
    main.cpp
)

set_target_properties(catalog_cli PROPERTIES
    OUTPUT_NAME nkido-catalog
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

target_link_libraries(catalog_cli
    PRIVATE
        catalog
        nkido_compiler_options
)

install(TARGETS catalog_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
