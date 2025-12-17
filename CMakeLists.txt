cmake_minimum_required(VERSION 3.20)

project(mspm0_debugger C)

set(MSPM0_SDK_PATH "/Applications/ti/mspm0_sdk_2_08_00_03/" CACHE PATH "Path to TI MSPM0 SDK")

set(PROBE_DEVICE "MSPM0C1104" CACHE STRING "Probe MCU (MSPM0C1104 or MSPM0C1105)")
set(PROBE_UART_BAUD "115200" CACHE STRING "UART baud rate for GDB RSP")
option(PROBE_TINY_RAM "Minimize static RAM usage (smaller RSP buffers)" ON)
option(PROBE_ENABLE_SYSOSC_FCL "Enable SYSOSC Frequency Correction Loop (FCL) at boot (may require ROSC resistor; locks until BOOTRST)" OFF)
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
    src/swd_bitbang.c
    src/adiv5.c
    src/target_mem.c
    src/cortex.c
    ${PROBE_BOARD_SRC}
    ${PROBE_STARTUP_SRC}
)

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
)

if(PROBE_ENABLE_SYSOSC_FCL)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_SYSOSC_FCL=1)
endif()

if(PROBE_TINY_RAM)
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_TINY_RAM=1
        RSP_MAX_PAYLOAD=128
        RSP_PACKET_SIZE_HEX=\"80\"
        RSP_IOBUF_SIZE=64
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
