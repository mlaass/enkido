# Cedar CLI - Standalone synth player

add_executable(cedar-cli
    main.cpp
)

target_link_libraries(cedar-cli
    PRIVATE
        cedar
        enkido_compiler_options
)

# Install
install(TARGETS cedar-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
