# Akkado CLI - Compiler

add_executable(akkado-cli
    main.cpp
)

target_link_libraries(akkado-cli
    PRIVATE
        akkado
        enkido_compiler_options
)

# Install
install(TARGETS akkado-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
