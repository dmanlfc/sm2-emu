cmake_minimum_required(VERSION 3.24)
project(m68kmake_host C)

# m68kmake is Musashi's opcode-table generator. It must run on the build host,
# so this tiny project is configured and built separately with the host
# toolchain (see cmake/Dependencies.cmake), never the target one. It is fed the
# path to Musashi's m68kmake.c via SM2_M68KMAKE_SRC.

if(NOT SM2_M68KMAKE_SRC)
    message(FATAL_ERROR "SM2_M68KMAKE_SRC (path to Musashi's m68kmake.c) is required")
endif()

add_executable(m68kmake "${SM2_M68KMAKE_SRC}")
set_target_properties(m68kmake PROPERTIES C_STANDARD 99)
if(MSVC)
    target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)

    # Build the generator unoptimised. MSVC 19.44 miscompiles the loop that
    # folds a bit pattern such as "1010............" into an opcode mask: at /O2
    # every mask loses bit 14, so 0xf000 is emitted as 0xb000 and nothing ends
    # up carrying the 0xff00 mask that m68ki_build_opcode_table() scans for.
    # That scan then walks off the end of the handler table and fills the
    # instruction jump table with garbage, so the first 68000 instruction the
    # sound board executes calls through a null pointer. The loop itself is
    # well-defined C and /Od and /O1 both produce the right table; the generator
    # runs once per build and takes milliseconds, so drop the optimiser rather
    # than rely on which levels happen to be safe. The configuration's own /O
    # flags are stripped first so /Od does not merely override them (D9025).
    foreach(flags
            CMAKE_C_FLAGS
            CMAKE_C_FLAGS_RELEASE
            CMAKE_C_FLAGS_RELWITHDEBINFO
            CMAKE_C_FLAGS_MINSIZEREL)
        string(REGEX REPLACE "[-/]O[a-zA-Z0-9]+" "" ${flags} "${${flags}}")
    endforeach()
    target_compile_options(m68kmake PRIVATE /Od)
else()
    target_compile_options(m68kmake PRIVATE -w)
endif()

install(TARGETS m68kmake RUNTIME DESTINATION bin)
