# Drydock libraries, for this repository and for the Ymir frontend (fork):
#
#   drydock-runtime   runs recompiled code inside Ymir, loads game libraries
#   sh2gen            SH-2 -> C++ emitter
#   sh2recomp-lib     whole-game recompiler (static discovery, modules)
#   drydock-build     recipe + player's disc -> game library (calls Clang)
#
# Requires the target ymir::ymir-core.

if (NOT TARGET drydock-runtime)
    set(DRYDOCK_ROOT ${CMAKE_CURRENT_LIST_DIR})

    add_library(drydock-runtime STATIC ${DRYDOCK_ROOT}/runtime/src_runtime.cpp)
    target_include_directories(drydock-runtime PUBLIC ${DRYDOCK_ROOT}/runtime/include)
    target_link_libraries(drydock-runtime PUBLIC ymir::ymir-core ${CMAKE_DL_LIBS})

    add_library(sh2gen STATIC ${DRYDOCK_ROOT}/tools/sh2gen/emit.cpp)
    target_include_directories(sh2gen PUBLIC ${DRYDOCK_ROOT}/tools/sh2gen)
    target_link_libraries(sh2gen PUBLIC ymir::ymir-core)

    add_library(sh2recomp-lib STATIC ${DRYDOCK_ROOT}/tools/sh2recomp/generate.cpp
                                     ${DRYDOCK_ROOT}/tools/sh2recomp/discover.cpp)
    target_include_directories(sh2recomp-lib PUBLIC ${DRYDOCK_ROOT}/tools/sh2recomp)
    target_link_libraries(sh2recomp-lib PUBLIC sh2gen)

    # abi.h is written next to the generated code when building a game: embed it.
    set(DRYDOCK_ABI_H ${DRYDOCK_ROOT}/runtime/include/ymir_recomp/abi.h)
    file(READ ${DRYDOCK_ABI_H} DRYDOCK_ABI_TEXT)
    set(DRYDOCK_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/drydock-gen)
    file(WRITE ${DRYDOCK_GEN_DIR}/abi_embed.hpp.in
         "#pragma once\n// Generated from runtime/include/ymir_recomp/abi.h\nstatic const char *const DRYDOCK_ABI_HEADER = R\"DRYDOCK_ABI(@DRYDOCK_ABI_TEXT@)DRYDOCK_ABI\";\n")
    configure_file(${DRYDOCK_GEN_DIR}/abi_embed.hpp.in ${DRYDOCK_GEN_DIR}/abi_embed.hpp @ONLY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DRYDOCK_ABI_H})

    add_library(drydock-build STATIC ${DRYDOCK_ROOT}/tools/drydock/build.cpp ${DRYDOCK_ROOT}/tools/drydock/recipe.cpp)
    target_include_directories(drydock-build PUBLIC ${DRYDOCK_ROOT}/tools/drydock PRIVATE ${DRYDOCK_GEN_DIR})
    target_link_libraries(drydock-build PUBLIC sh2recomp-lib drydock-runtime ymir::ymir-core)
endif ()
