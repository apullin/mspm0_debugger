# Host-side unit tests for the RSP layer. Standalone project - build with
# the host compiler, NOT the ARM toolchain:
#
#   cmake -S tests -B build_tests
#   cmake --build build_tests && ctest --test-dir build_tests --output-on-failure

cmake_minimum_required(VERSION 3.20)
project(mspm0_debugger_tests C)

set(CMAKE_C_STANDARD 11)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Werror -g -O1
        -fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif()

set(RSP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../src/rsp.c)
set(TEST_SRC ${CMAKE_CURRENT_SOURCE_DIR}/mock_env.c ${CMAKE_CURRENT_SOURCE_DIR}/test_rsp.c)
set(INCS ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Full profile: mirrors the C1105/G5187 configuration (dual-arch, XML,
# flash programming, retransmit buffer).
add_executable(rsp_tests_full ${RSP_SRC} ${TEST_SRC})
target_include_directories(rsp_tests_full PRIVATE ${INCS})
target_compile_definitions(rsp_tests_full PRIVATE
    TEST_TINY=0
    PROBE_TINY_RAM=0
    RSP_MAX_PAYLOAD=512
    RSP_PACKET_SIZE_HEX=\"200\"
    RSP_IOBUF_SIZE=256
    PROBE_ENABLE_CORTEXM=1
    PROBE_ENABLE_RISCV=1
    PROBE_ENABLE_QXFER_TARGET_XML=1
    PROBE_ENABLE_FLASH_MSPM0=1
)

# Tiny profile: mirrors the C1104 configuration (Cortex-M only, small
# buffers, target XML enabled, flash disabled).
add_executable(rsp_tests_tiny ${RSP_SRC} ${TEST_SRC})
target_include_directories(rsp_tests_tiny PRIVATE ${INCS})
target_compile_definitions(rsp_tests_tiny PRIVATE
    TEST_TINY=1
    PROBE_TINY_RAM=1
    RSP_MAX_PAYLOAD=256
    RSP_PACKET_SIZE_HEX=\"100\"
    RSP_IOBUF_SIZE=128
    PROBE_ENABLE_CORTEXM=1
    PROBE_ENABLE_QXFER_TARGET_XML=1
)

# Explicit no-XML compatibility profile. Stock ARM GDB uses its historical
# 168-byte register layout in this mode, so keep it covered even though XML is
# enabled by default on all production devices.
add_executable(rsp_tests_legacy ${RSP_SRC} ${TEST_SRC})
target_include_directories(rsp_tests_legacy PRIVATE ${INCS})
target_compile_definitions(rsp_tests_legacy PRIVATE
    TEST_TINY=1
    TEST_LEGACY=1
    PROBE_TINY_RAM=1
    RSP_MAX_PAYLOAD=337
    RSP_PACKET_SIZE_HEX="151"
    RSP_IOBUF_SIZE=128
    PROBE_ENABLE_CORTEXM=1
    PROBE_ENABLE_RISCV=1
    PROBE_ENABLE_RISCV_MINIMAL_XML=1
)

# Tiny RISC-V-only profile needs room for 33 RV32 registers even though the
# Cortex-only tiny packet remains 0x100 bytes.
add_executable(rsp_tests_tiny_riscv ${RSP_SRC} ${TEST_SRC})
target_include_directories(rsp_tests_tiny_riscv PRIVATE ${INCS})
target_compile_definitions(rsp_tests_tiny_riscv PRIVATE
    TEST_TINY=1
    TEST_TINY_RISCV=1
    PROBE_TINY_RAM=1
    RSP_MAX_PAYLOAD=265
    RSP_PACKET_SIZE_HEX="109"
    RSP_IOBUF_SIZE=128
    PROBE_ENABLE_RISCV=1
    PROBE_ENABLE_QXFER_TARGET_XML=1
)

# Register-level tests compile the production FLASHCTL driver rather than the
# RSP mock, covering command sequencing, bank geometry, ECC-sized staging,
# protection registers, and range/order validation.
add_executable(flash_mspm0_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/flash_mspm0.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_flash_mspm0.c
)
target_include_directories(flash_mspm0_tests PRIVATE ${INCS})
target_compile_definitions(flash_mspm0_tests PRIVATE
    PROBE_ENABLE_CORTEXM=1
    PROBE_ENABLE_FLASH_MSPM0=1
)

add_executable(adiv5_target_mem_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/adiv5.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/target_mem.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_adiv5_target_mem.c
)
target_include_directories(adiv5_target_mem_tests PRIVATE ${INCS})

add_executable(cortex_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/cortex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_cortex.c
)
target_include_directories(cortex_tests PRIVATE ${INCS})
target_compile_definitions(cortex_tests PRIVATE
    PROBE_ENABLE_DWT_WATCHPOINTS=1
    PROBE_ENABLE_QXFER_TARGET_XML=1
    PROBE_TARGET_M3=1
)

# Register-level RISC-V Debug Module tests exercise the production driver
# against a deterministic DMI/CSR model, including failure cleanup paths.
add_executable(riscv_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/riscv.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_riscv.c
)
target_include_directories(riscv_tests PRIVATE ${INCS})
target_compile_definitions(riscv_tests PRIVATE PROBE_ENABLE_RISCV=1)

add_executable(intmath_tests
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/intmath.c
    ${CMAKE_CURRENT_SOURCE_DIR}/test_intmath.c
)
target_include_directories(intmath_tests PRIVATE ${INCS})

enable_testing()
add_test(NAME rsp_full COMMAND rsp_tests_full)
add_test(NAME rsp_tiny COMMAND rsp_tests_tiny)
add_test(NAME rsp_legacy_no_xml COMMAND rsp_tests_legacy)
add_test(NAME rsp_tiny_riscv COMMAND rsp_tests_tiny_riscv)
add_test(NAME flash_mspm0 COMMAND flash_mspm0_tests)
add_test(NAME adiv5_target_mem COMMAND adiv5_target_mem_tests)
add_test(NAME cortex COMMAND cortex_tests)
add_test(NAME riscv COMMAND riscv_tests)
add_test(NAME intmath COMMAND intmath_tests)
