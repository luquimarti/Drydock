# Differential fuzzer: sh2gen-emitted C++ vs Ymir's SH-2 interpreter.
set(FUZZ_SEED 1 CACHE STRING "sh2 fuzz seed")
set(FUZZ_COUNT 2000 CACHE STRING "sh2 fuzz program count")
set(FUZZ_LEN 48 CACHE STRING "sh2 fuzz program length")

add_executable(sh2fuzz-gen gen_main.cpp)
target_link_libraries(sh2fuzz-gen PRIVATE sh2gen)

set(FUZZ_BIN ${CMAKE_CURRENT_BINARY_DIR}/fuzz_programs.bin)
set(FUZZ_CPP ${CMAKE_CURRENT_BINARY_DIR}/fuzz_gen.cpp)
add_custom_command(
    OUTPUT ${FUZZ_BIN} ${FUZZ_CPP}
    COMMAND sh2fuzz-gen ${FUZZ_SEED} ${FUZZ_COUNT} ${FUZZ_LEN} ${FUZZ_BIN} ${FUZZ_CPP}
    DEPENDS sh2fuzz-gen
    COMMENT "Generating SH-2 fuzz programs (seed ${FUZZ_SEED})"
)
add_executable(sh2fuzz-run run_main.cpp ${FUZZ_CPP})
target_link_libraries(sh2fuzz-run PRIVATE ymir::ymir-core drydock-runtime)

# The generated file is huge and goto-heavy: compile it quickly (no LTO, -O1).
# The link still uses LTO, as required by ymir-core.
set_source_files_properties(${FUZZ_CPP} PROPERTIES COMPILE_OPTIONS "-O1;-fno-lto")
