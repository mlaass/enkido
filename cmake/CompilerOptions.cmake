# Compiler options and warning flags for Enkido

# Create interface library for shared compiler options
add_library(enkido_compiler_options INTERFACE)

# Warning flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(enkido_compiler_options INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wnon-virtual-dtor
        -Wold-style-cast
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wformat=2
        -Wnull-dereference
        -Wdouble-promotion
    )

    # GCC-specific warnings
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(enkido_compiler_options INTERFACE
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
            -Wuseless-cast
        )
    endif()

    # Clang-specific warnings
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        target_compile_options(enkido_compiler_options INTERFACE
            -Wno-c++98-compat
            -Wno-c++98-compat-pedantic
        )
    endif()

elseif(MSVC)
    target_compile_options(enkido_compiler_options INTERFACE
        /W4
        /permissive-
        /w14640  # thread-unsafe static member initialization
        /w14242  # conversion, possible loss of data
        /w14254  # conversion, possible loss of data
        /w14263  # member function does not override base class virtual
        /w14265  # class has virtual functions but destructor is not virtual
        /w14287  # unsigned/negative constant mismatch
        /w14296  # expression is always false
        /w14311  # pointer truncation
        /w14545  # expression before comma evaluates to function
        /w14546  # function call before comma missing argument list
        /w14547  # operator before comma has no effect
        /w14549  # operator before comma has no effect
        /w14555  # expression has no effect
        /w14619  # pragma warning: nonexistent warning number
        /w14640  # thread-unsafe static initialization
        /w14826  # conversion signed/unsigned mismatch
        /w14905  # wide string literal cast to LPSTR
        /w14906  # string literal cast to LPWSTR
        /w14928  # illegal copy-initialization
    )
endif()

# Debug/Release specific options
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(enkido_compiler_options INTERFACE
        $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>
        $<$<CONFIG:Release>:-O3 -DNDEBUG>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g -DNDEBUG>
    )
endif()

# Sanitizers (debug builds only, not on WASM)
option(ENKIDO_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENKIDO_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

if(NOT EMSCRIPTEN)
    if(ENKIDO_ENABLE_ASAN)
        target_compile_options(enkido_compiler_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address>
        )
        target_link_options(enkido_compiler_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address>
        )
    endif()

    if(ENKIDO_ENABLE_UBSAN)
        target_compile_options(enkido_compiler_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=undefined>
        )
        target_link_options(enkido_compiler_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=undefined>
        )
    endif()
endif()

# Emscripten-specific options
if(EMSCRIPTEN)
    target_compile_options(enkido_compiler_options INTERFACE
        -fno-exceptions
        -fno-rtti
        -pthread  # Required for AudioWorklet shared memory
    )
    target_link_options(enkido_compiler_options INTERFACE
        -pthread
    )
    target_compile_definitions(enkido_compiler_options INTERFACE
        ENKIDO_WASM=1
    )
endif()
