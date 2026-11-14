cmake_minimum_required(VERSION 3.24)
project(m68kmake_host C)

# Musashi's opcode-table generator, built separately with the host toolchain
# because it runs on the build host (see cmake/Dependencies.cmake).

if(NOT SM2_M68KMAKE_SRC)
    message(FATAL_ERROR "SM2_M68KMAKE_SRC (path to Musashi's m68kmake.c) is required")
endif()

add_executable(m68kmake "${SM2_M68KMAKE_SRC}")
set_target_properties(m68kmake PROPERTIES C_STANDARD 99)
if(MSVC)
    target_compile_definitions(m68kmake PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(m68kmake PRIVATE -w)
endif()

install(TARGETS m68kmake RUNTIME DESTINATION bin)
