# Adapted from tools/cmake/toolchains/arm-gcc.cmake from the amazon-freertos github repository

include("${CMAKE_CURRENT_LIST_DIR}/find_compiler.cmake")

set(CMAKE_SYSTEM_NAME Generic)

# Find GCC for ARM.
find_compiler(COMPILER_CC arm-none-eabi-gcc)
# Use gcc for compiling/assembling ASM
set(COMPILER_ASM "${COMPILER_CC}" CACHE INTERNAL "")

# Also find supporting utilities: objdump, objcopy, size, ar, nm
find_compiler(COMPILER_OBJDUMP arm-none-eabi-objdump)
find_compiler(COMPILER_OBJCOPY arm-none-eabi-objcopy)
find_compiler(COMPILER_SIZE arm-none-eabi-size)
find_compiler(COMPILER_AR arm-none-eabi-ar)
find_compiler(COMPILER_NM arm-none-eabi-nm)

# Specify the cross compiler.
set(CMAKE_C_COMPILER ${COMPILER_CC} CACHE FILEPATH "C compiler")
set(CMAKE_ASM_COMPILER ${COMPILER_ASM} CACHE FILEPATH "ASM compiler")
set(CMAKE_OBJCOPY ${COMPILER_OBJCOPY} CACHE FILEPATH "Object-copy utility")
set(CMAKE_SIZE ${COMPILER_SIZE} CACHE FILEPATH "Object-size utility")
set(CMAKE_AR ${COMPILER_AR} CACHE FILEPATH "Archiver")
set(CMAKE_NM ${COMPILER_NM} CACHE FILEPATH "Symbol utility")

# Disable compiler checks.
set(CMAKE_C_COMPILER_FORCED TRUE)

# Add target system root to cmake find path.
get_filename_component(COMPILER_DIR "${COMPILER_CC}" DIRECTORY)
get_filename_component(CMAKE_FIND_ROOT_PATH "${COMPILER_DIR}" DIRECTORY)

# Look for includes and libraries only in the target system prefix.
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)

# Helper variables to abstracts some common compiler flags.
set(COMPILER_NO_WARNINGS "-w" CACHE INTERNAL "")
