cmake_minimum_required(VERSION 3.28)
project(Drydock CXX C)
set(CMAKE_CXX_STANDARD 20)

# Ymir is consumed as a subproject (library only: no SDL3 frontend, no vcpkg).
set(Ymir_ENABLE_DEVLOG OFF CACHE BOOL "" FORCE)
set(Ymir_ENABLE_UPDATE_CHECKS OFF CACHE BOOL "" FORCE)
set(Ymir_EXTRA_INLINING ON CACHE BOOL "" FORCE)
add_subdirectory(external/Ymir ymir)

# ymir-core is built with LTO; our targets must link with the same toolchain
# (otherwise the bitcode inside libymir-core.a is unreadable to the linker).
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)

include(drydock.cmake)

add_subdirectory(tools/sh2recomp)
add_subdirectory(tools/drydock)
add_subdirectory(tools/headless)
add_subdirectory(tests/fuzz)
