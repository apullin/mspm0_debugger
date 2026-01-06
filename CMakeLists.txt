cmake_minimum_required(VERSION 3.20)

project(mspm0_debugger C)

set(MSPM0_SDK_PATH "/Applications/ti/mspm0_sdk_2_09_00_01/" CACHE PATH "Path to TI MSPM0 SDK")

set(PROBE_DEVICE "MSPM0C1104" CACHE STRING "Probe MCU (MSPM0C1104 or MSPM0C1105)")
set(PROBE_UART_BAUD "115200" CACHE STRING "UART baud rate for GDB RSP")
set(PROBE_SWD_DELAY_US "0" CACHE STRING "Microseconds delay per SWD clock cycle (0=max speed, increase for slow targets/long wires)")

# Auto-select TINY_RAM based on device (C1104 has only 1KB SRAM)
if(PROBE_DEVICE STREQUAL "MSPM0C1104")
    set(_tiny_ram_default ON)
else()
    set(_tiny_ram_default OFF)
endif()
option(PROBE_TINY_RAM "Minimize static RAM usage (smaller RSP buffers, auto-selected per device)" ${_tiny_ram_default})
option(PROBE_ENABLE_SYSOSC_FCL "Enable SYSOSC Frequency Correction Loop (FCL) at boot (may require ROSC resistor; locks until BOOTRST)" OFF)

# HFXT crystal support (C1105/C1106 only - C1104 has no HFXT hardware)
option(PROBE_USE_HFXT "Use external HFXT crystal for MCLK (C1105 only; requires crystal on HFXIN/HFXOUT pins)" OFF)
set(PROBE_HFXT_FREQ_HZ "32000000" CACHE STRING "HFXT crystal frequency in Hz (used when PROBE_USE_HFXT=ON)")

set(_probe_full_feature_default OFF)
if(NOT PROBE_TINY_RAM)
    set(_probe_full_feature_default ON)
endif()

option(PROBE_ENABLE_QXFER_TARGET_XML "Serve GDB target XML via qXfer:features:read (target.xml; helps support multiple Cortex-M variants)" ${_probe_full_feature_default})
option(PROBE_ENABLE_DWT_WATCHPOINTS "Enable DWT watchpoints (Z2/Z3/Z4) when supported by the target" ${_probe_full_feature_default})

# Target architecture support (compile-time selection)
option(PROBE_ENABLE_CORTEXM "Enable Cortex-M debug target support (SWD)" ON)
option(PROBE_ENABLE_JTAG "Enable JTAG bit-bang wire protocol (for RISC-V targets)" OFF)
option(PROBE_ENABLE_RISCV "Enable RISC-V debug target support (requires JTAG)" OFF)

# At least one target architecture must be enabled
if(NOT PROBE_ENABLE_CORTEXM AND NOT PROBE_ENABLE_RISCV)
    message(FATAL_ERROR "At least one target architecture must be enabled (PROBE_ENABLE_CORTEXM or PROBE_ENABLE_RISCV)")
endif()

# Cortex-M sub-options (only relevant when PROBE_ENABLE_CORTEXM=ON)
option(PROBE_TARGET_M0 "Support Cortex-M0 debug targets" ON)
option(PROBE_TARGET_M3 "Support Cortex-M3 debug targets" ON)
option(PROBE_TARGET_M4 "Support Cortex-M4 debug targets" ON)
option(PROBE_TARGET_M7 "Support Cortex-M7 debug targets" ON)
option(PROBE_TARGET_M23 "Support Cortex-M23 debug targets" ON)
option(PROBE_TARGET_M33 "Support Cortex-M33 debug targets" ON)
option(PROBE_TARGET_M55 "Support Cortex-M55 debug targets" ON)

set(RSP_MAX_PAYLOAD "512" CACHE STRING "Max RSP payload bytes")
set(RSP_PACKET_SIZE_HEX "200" CACHE STRING "RSP qSupported PacketSize (hex, bytes)")
set(RSP_IOBUF_SIZE "256" CACHE STRING "Max bytes buffered for m/M operations")

if(PROBE_DEVICE STREQUAL "MSPM0C1104")
    set(PROBE_DEVICE_DEFINE "__MSPM0C1104__")
    set(PROBE_BOARD_SRC "src/board_mspm0c1104.c")
    set(PROBE_STARTUP_SRC "startup/startup_mspm0c110x_gcc.c")
    set(PROBE_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/mspm0c1104.lds")
    set(PROBE_DRIVERLIB_A "${MSPM0_SDK_PATH}/source/ti/driverlib/lib/gcc/m0p/mspm0c110x/driverlib.a")
    # MSPM0C110x SYSOSC is a 24MHz internal oscillator (per datasheet/DFP metadata).
    set(PROBE_CORE_CLK_HZ_DEFAULT "24000000")
elseif(PROBE_DEVICE STREQUAL "MSPM0C1105")
    set(PROBE_DEVICE_DEFINE "__MSPM0C1105__")
    set(PROBE_BOARD_SRC "src/board_mspm0c1105.c")
    set(PROBE_STARTUP_SRC "startup/startup_mspm0c1105_c1106_gcc.c")
    set(PROBE_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/mspm0c1105.lds")
    set(PROBE_DRIVERLIB_A "${MSPM0_SDK_PATH}/source/ti/driverlib/lib/gcc/m0p/mspm0c1105_c1106/driverlib.a")
    set(PROBE_CORE_CLK_HZ_DEFAULT "32000000")
else()
    message(FATAL_ERROR "Unknown PROBE_DEVICE='${PROBE_DEVICE}'. Use MSPM0C1104 or MSPM0C1105.")
endif()

if(NOT DEFINED PROBE_CORE_CLK_HZ)
    set(PROBE_CORE_CLK_HZ "${PROBE_CORE_CLK_HZ_DEFAULT}" CACHE STRING "Probe core clock frequency in Hz (used for delay_us and UART baud)")
elseif(DEFINED PROBE_DEVICE_LAST AND NOT PROBE_DEVICE_LAST STREQUAL PROBE_DEVICE)
    set(_probe_old_default "")
    if(PROBE_DEVICE_LAST STREQUAL "MSPM0C1104")
        set(_probe_old_default "24000000")
    elseif(PROBE_DEVICE_LAST STREQUAL "MSPM0C1105")
        set(_probe_old_default "32000000")
    endif()

    if(_probe_old_default AND PROBE_CORE_CLK_HZ STREQUAL _probe_old_default)
        set(PROBE_CORE_CLK_HZ "${PROBE_CORE_CLK_HZ_DEFAULT}" CACHE STRING "Probe core clock frequency in Hz (used for delay_us and UART baud)" FORCE)
    else()
        message(WARNING "PROBE_DEVICE changed from '${PROBE_DEVICE_LAST}' to '${PROBE_DEVICE}', but PROBE_CORE_CLK_HZ is still '${PROBE_CORE_CLK_HZ}'. If this is unintentional, set -DPROBE_CORE_CLK_HZ=${PROBE_CORE_CLK_HZ_DEFAULT} or clear the build directory cache.")
    endif()
endif()

set(PROBE_DEVICE_LAST "${PROBE_DEVICE}" CACHE INTERNAL "Last configured PROBE_DEVICE" FORCE)

add_executable(mspm0_debugger.elf
    main.c
    src/probe.c
    src/rsp.c
    src/target.c
    ${PROBE_BOARD_SRC}
    ${PROBE_STARTUP_SRC}
)

# Cortex-M sources (SWD, ADIv5, memory access, core debug)
if(PROBE_ENABLE_CORTEXM)
    target_sources(mspm0_debugger.elf PRIVATE
        src/swd_bitbang.c
        src/adiv5.c
        src/target_mem.c
        src/cortex.c
    )
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_CORTEXM=1)
endif()

target_include_directories(mspm0_debugger.elf PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${MSPM0_SDK_PATH}/source
    ${MSPM0_SDK_PATH}/source/third_party/CMSIS/Core/Include
)

target_compile_definitions(mspm0_debugger.elf PRIVATE
    ${PROBE_DEVICE_DEFINE}
    PROBE_UART_BAUD=${PROBE_UART_BAUD}
    PROBE_CORE_CLK_HZ=${PROBE_CORE_CLK_HZ}
    SWD_DELAY_US=${PROBE_SWD_DELAY_US}
)

if(PROBE_ENABLE_SYSOSC_FCL)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_SYSOSC_FCL=1)
endif()

if(PROBE_USE_HFXT)
    if(NOT PROBE_DEVICE STREQUAL "MSPM0C1105")
        message(FATAL_ERROR "PROBE_USE_HFXT=ON requires PROBE_DEVICE=MSPM0C1105 (C1104 has no HFXT hardware)")
    endif()
    # When using HFXT, core clock = crystal frequency
    if(NOT PROBE_CORE_CLK_HZ STREQUAL PROBE_HFXT_FREQ_HZ)
        message(STATUS "PROBE_USE_HFXT=ON: overriding PROBE_CORE_CLK_HZ=${PROBE_CORE_CLK_HZ} -> ${PROBE_HFXT_FREQ_HZ}")
        set(PROBE_CORE_CLK_HZ "${PROBE_HFXT_FREQ_HZ}" CACHE STRING "Probe core clock frequency in Hz" FORCE)
    endif()
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_USE_HFXT=1
        PROBE_HFXT_FREQ_HZ=${PROBE_HFXT_FREQ_HZ}
    )
endif()

if(PROBE_ENABLE_QXFER_TARGET_XML)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_QXFER_TARGET_XML=1)
endif()

if(PROBE_ENABLE_DWT_WATCHPOINTS)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_DWT_WATCHPOINTS=1)
endif()

if(PROBE_ENABLE_JTAG)
    target_sources(mspm0_debugger.elf PRIVATE src/jtag_bitbang.c)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_JTAG=1)
endif()

if(PROBE_ENABLE_RISCV)
    if(NOT PROBE_ENABLE_JTAG)
        message(FATAL_ERROR "PROBE_ENABLE_RISCV=ON requires PROBE_ENABLE_JTAG=ON")
    endif()
    target_sources(mspm0_debugger.elf PRIVATE src/riscv.c)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_RISCV=1)
endif()

# Cortex-M variant support (only when PROBE_ENABLE_CORTEXM=ON)
if(PROBE_ENABLE_CORTEXM)
    if(PROBE_TARGET_M0)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M0=1)
    endif()
    if(PROBE_TARGET_M3)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M3=1)
    endif()
    if(PROBE_TARGET_M4)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M4=1)
    endif()
    if(PROBE_TARGET_M7)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M7=1)
    endif()
    if(PROBE_TARGET_M23)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M23=1)
    endif()
    if(PROBE_TARGET_M33)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M33=1)
    endif()
    if(PROBE_TARGET_M55)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_TARGET_M55=1)
    endif()
endif()

if(PROBE_TINY_RAM)
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_TINY_RAM=1
        RSP_MAX_PAYLOAD=256
        RSP_PACKET_SIZE_HEX=\"100\"
        RSP_IOBUF_SIZE=128
    )
else()
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_TINY_RAM=0
        RSP_MAX_PAYLOAD=${RSP_MAX_PAYLOAD}
        RSP_PACKET_SIZE_HEX=\"${RSP_PACKET_SIZE_HEX}\"
        RSP_IOBUF_SIZE=${RSP_IOBUF_SIZE}
    )
endif()

target_compile_options(mspm0_debugger.elf PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -mfloat-abi=soft
    -ffunction-sections
    -fdata-sections
    -fno-common
    -Os
    -g3
)

target_link_options(mspm0_debugger.elf PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -Wl,--gc-sections
    -Wl,--print-memory-usage
    -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.map
    -T${PROBE_LINKER_SCRIPT}
    -specs=nano.specs
    -specs=nosys.specs
)

target_link_libraries(mspm0_debugger.elf PRIVATE
    ${PROBE_DRIVERLIB_A}
)

add_custom_command(TARGET mspm0_debugger.elf POST_BUILD
    COMMAND arm-none-eabi-size $<TARGET_FILE:mspm0_debugger.elf>
    COMMAND arm-none-eabi-objcopy -O ihex $<TARGET_FILE:mspm0_debugger.elf> ${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.hex
    COMMAND arm-none-eabi-objcopy -O binary $<TARGET_FILE:mspm0_debugger.elf> ${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.bin
    VERBATIM
)
