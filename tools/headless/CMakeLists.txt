set(RECOMP_GENERATED "" CACHE STRING "Generated native code to link into ymir-headless (sh2recomp output: a directory or .cpp files)")
set(RECOMP_GENERATED_FLAGS "-O1;-fno-lto" CACHE STRING "Compiler flags for the generated code")

add_executable(ymir-headless main.cpp)
target_link_libraries(ymir-headless PRIVATE ymir::ymir-core drydock-runtime stb::stb)
if (RECOMP_GENERATED)
    if (IS_DIRECTORY "${RECOMP_GENERATED}")
        file(GLOB RECOMP_GENERATED CONFIGURE_DEPENDS "${RECOMP_GENERATED}/*.cpp")
    endif ()
    target_sources(ymir-headless PRIVATE ${RECOMP_GENERATED})
    target_compile_definitions(ymir-headless PRIVATE YMIR_RECOMP_HAVE_NATIVE)
    # Large, goto-heavy generated code: no LTO for it, to keep build times sane.
    set_source_files_properties(${RECOMP_GENERATED} PROPERTIES COMPILE_OPTIONS "${RECOMP_GENERATED_FLAGS}")
endif ()

# ymir-core reads physical CD drives through SetupAPI on Windows
if (WIN32)
    target_link_libraries(ymir-headless PRIVATE setupapi)
endif ()
