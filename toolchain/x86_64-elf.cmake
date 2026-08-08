# Cross-compilation toolchain file for Sol OS.
#
# Requires an x86_64-elf cross compiler on PATH (x86_64-elf-gcc,
# x86_64-elf-binutils). Build one with crosstool-ng, or follow the
# OSDev.org "GCC Cross-Compiler" guide. Do NOT point this at your
# host system's gcc/ld — the kernel must never link against the
# host libc or host startup files.

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-elf-gcc)
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)

set(CMAKE_C_COMPILER_WORKS TRUE)

# Never search the host system's libraries/headers/programs — this is
# what actually enforces "never depend on the host compiler."
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
