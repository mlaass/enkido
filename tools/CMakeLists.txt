# CLI Tools

if(NKIDO_BUILD_AKKADO)
    add_subdirectory(akkado)
    add_subdirectory(nkido)
endif()

if(TARGET catalog)
    add_subdirectory(catalog)
endif()
