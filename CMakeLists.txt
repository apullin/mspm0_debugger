cmake_minimum_required(VERSION 3.20)

project(mspm0_debugger VERSION 0.2.0 LANGUAGES C)

# USB bcdDevice packs the semantic-version components as 0xMmp. Keep CMake's
# project version authoritative and reject components that cannot be encoded.
if(PROJECT_VERSION_MAJOR GREATER 9 OR PROJECT_VERSION_MINOR GREATER 9 OR
   PROJECT_VERSION_PATCH GREATER 9)
    message(FATAL_ERROR "Firmware version ${PROJECT_VERSION} cannot be represented as USB bcdDevice 0xMmp")
endif()
math(EXPR PROBE_USB_BCD_DEVICE
    "(${PROJECT_VERSION_MAJOR} << 8) | (${PROJECT_VERSION_MINOR} << 4) | ${PROJECT_VERSION_PATCH}"
    OUTPUT_FORMAT HEXADECIMAL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Older Arm GNU releases produce a C1104 dual image that exceeds 16 KB and
# their linker lacks flags used by this build. GCC 9 is the oldest verified
# release that both fits and supports the current link workflow.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND
   CMAKE_C_COMPILER_VERSION VERSION_LESS 9.0)
    message(FATAL_ERROR
        "Arm GNU Toolchain 9 or newer is required (found ${CMAKE_C_COMPILER_VERSION})")
endif()

set(MSPM0_SDK_PATH "/Applications/ti/mspm0_sdk_2_09_00_01/" CACHE PATH "Path to TI MSPM0 SDK")

set(PROBE_DEVICE "MSPM0C1104" CACHE STRING "Probe MCU (MSPM0C1104, MSPM0C1105, or MSPM0G5187)")
set_property(CACHE PROBE_DEVICE PROPERTY STRINGS MSPM0C1104 MSPM0C1105 MSPM0G5187)
set(PROBE_UART_BAUD "115200" CACHE STRING "UART baud rate for GDB RSP")
set(PROBE_SWD_DELAY_US "0" CACHE STRING "Microseconds delay per SWD clock cycle (0=max speed, increase for slow targets/long wires)")
set(PROBE_RISCV_JTAG_IR_LEN "5" CACHE STRING "RISC-V JTAG DTM instruction-register length in bits (5-32)")
option(PROBE_ALLOW_ODIO_SWD_PINS
    "Acknowledge placeholder PA0/PA1 ODIO pins for shared SWCLK/TCK and SWDIO/TMS (not production-capable)"
    OFF)
option(PROBE_WARNINGS_AS_ERRORS "Treat firmware compiler warnings as errors" OFF)

# Auto-select TINY_RAM based on device (C1104 has only 1KB SRAM)
if(PROBE_DEVICE STREQUAL "MSPM0C1104")
    set(_tiny_ram_default ON)
else()
    set(_tiny_ram_default OFF)
endif()
option(PROBE_TINY_RAM "Minimize static RAM usage (smaller RSP buffers, auto-selected per device)" ${_tiny_ram_default})

# Re-evaluate the auto-select when PROBE_DEVICE changes in an existing build
# dir (option() never updates a cached value): if the cached choice still
# matches the OLD device's default, follow the new device's default.
if(DEFINED PROBE_DEVICE_LAST AND NOT PROBE_DEVICE_LAST STREQUAL PROBE_DEVICE)
    if(PROBE_DEVICE_LAST STREQUAL "MSPM0C1104")
        set(_tiny_ram_old_default ON)
    else()
        set(_tiny_ram_old_default OFF)
    endif()
    if((PROBE_TINY_RAM AND _tiny_ram_old_default) OR (NOT PROBE_TINY_RAM AND NOT _tiny_ram_old_default))
        set(PROBE_TINY_RAM ${_tiny_ram_default} CACHE BOOL
            "Minimize static RAM usage (smaller RSP buffers, auto-selected per device)" FORCE)
    else()
        message(WARNING "PROBE_DEVICE changed from '${PROBE_DEVICE_LAST}' to '${PROBE_DEVICE}' but PROBE_TINY_RAM=${PROBE_TINY_RAM} was set explicitly; keeping it.")
    endif()
endif()
option(PROBE_ENABLE_SYSOSC_FCL "Enable SYSOSC Frequency Correction Loop (FCL) at boot (may require ROSC resistor; locks until BOOTRST)" OFF)

# HFXT crystal support (C1105/C1106 only - C1104 has no HFXT hardware)
option(PROBE_USE_HFXT "Use external HFXT crystal for MCLK (C1105 only; requires crystal on HFXIN/HFXOUT pins)" OFF)
set(PROBE_HFXT_FREQ_HZ "32000000" CACHE STRING "HFXT crystal frequency in Hz (used when PROBE_USE_HFXT=ON)")
if(PROBE_USE_HFXT)
    if(NOT PROBE_HFXT_FREQ_HZ MATCHES "^[0-9]+$")
        message(FATAL_ERROR "PROBE_HFXT_FREQ_HZ must be an integer frequency in Hz")
    elseif(PROBE_HFXT_FREQ_HZ LESS 4000000 OR PROBE_HFXT_FREQ_HZ GREATER 32000000)
        message(FATAL_ERROR
            "PROBE_HFXT_FREQ_HZ=${PROBE_HFXT_FREQ_HZ} is outside the C1105 HFXT range of 4-32 MHz")
    endif()
endif()

# Target architecture support (compile-time selection). These must be declared
# before profile-dependent feature defaults are calculated.
option(PROBE_ENABLE_CORTEXM "Enable Cortex-M debug target support (SWD)" ON)
option(PROBE_ENABLE_JTAG "Enable JTAG bit-bang wire protocol (for RISC-V targets)" OFF)
option(PROBE_ENABLE_RISCV "Enable RISC-V debug target support (requires JTAG)" OFF)
if(NOT PROBE_RISCV_JTAG_IR_LEN MATCHES "^[0-9]+$")
    message(FATAL_ERROR "PROBE_RISCV_JTAG_IR_LEN must be an integer from 5 through 32")
elseif(PROBE_RISCV_JTAG_IR_LEN LESS 5 OR PROBE_RISCV_JTAG_IR_LEN GREATER 32)
    message(FATAL_ERROR
        "PROBE_RISCV_JTAG_IR_LEN=${PROBE_RISCV_JTAG_IR_LEN} is outside the legal range of 5-32 bits")
endif()

set(_probe_full_feature_default OFF)
if(NOT PROBE_TINY_RAM)
    set(_probe_full_feature_default ON)
endif()

set(_probe_qxfer_default ON)
if(PROBE_DEVICE STREQUAL "MSPM0C1104" AND PROBE_ENABLE_CORTEXM AND PROBE_ENABLE_RISCV)
    # The combined Cortex + RISC-V image cannot fit the Cortex XML strings in
    # 16 KB. The correct 42-register legacy ARM layout is used instead.
    set(_probe_qxfer_default OFF)
endif()
option(PROBE_ENABLE_QXFER_TARGET_XML "Serve GDB target XML via qXfer:features:read (preferred compact Cortex-M register layout)" ${_probe_qxfer_default})
if(DEFINED PROBE_QXFER_DEFAULT_LAST AND
   NOT "${PROBE_QXFER_DEFAULT_LAST}" STREQUAL "${_probe_qxfer_default}")
    if("${PROBE_ENABLE_QXFER_TARGET_XML}" STREQUAL "${PROBE_QXFER_DEFAULT_LAST}")
        set(PROBE_ENABLE_QXFER_TARGET_XML ${_probe_qxfer_default} CACHE BOOL
            "Serve GDB target XML via qXfer:features:read (preferred compact Cortex-M register layout)" FORCE)
    else()
        message(WARNING
            "The generated qXfer default changed to ${_probe_qxfer_default}, but PROBE_ENABLE_QXFER_TARGET_XML=${PROBE_ENABLE_QXFER_TARGET_XML} differs from the previous generated default; keeping it.")
    endif()
endif()
set(PROBE_QXFER_DEFAULT_LAST "${_probe_qxfer_default}" CACHE INTERNAL
    "Last generated qXfer default" FORCE)
if(PROBE_DEVICE STREQUAL "MSPM0C1104" AND PROBE_ENABLE_CORTEXM AND
   PROBE_ENABLE_RISCV AND PROBE_ENABLE_QXFER_TARGET_XML)
    message(FATAL_ERROR
        "C1104 dual-architecture builds cannot fit target XML in 16 KB flash; "
        "configure with -DPROBE_ENABLE_QXFER_TARGET_XML=OFF or use C1105/G5187")
endif()
# The C1104 dual image cannot afford the Cortex XML strings, but an RV32 GDB
# still needs an architecture description or it may default to RV64. Serve the
# tiny RV32-only descriptor dynamically while retaining Cortex's legacy layout.
set(_probe_enable_riscv_minimal_xml OFF)
if(PROBE_DEVICE STREQUAL "MSPM0C1104" AND PROBE_ENABLE_CORTEXM AND
   PROBE_ENABLE_RISCV AND NOT PROBE_ENABLE_QXFER_TARGET_XML)
    set(_probe_enable_riscv_minimal_xml ON)
endif()
option(PROBE_ENABLE_DWT_WATCHPOINTS "Enable DWT watchpoints (Z2/Z3/Z4) when supported by the target" ${_probe_full_feature_default})
set(_probe_flash_default OFF)
if(_probe_full_feature_default AND PROBE_ENABLE_CORTEXM)
    set(_probe_flash_default ON)
endif()
option(PROBE_ENABLE_FLASH_MSPM0 "Enable GDB 'load' to MSPM0-target flash (vFlash* + memory map via target FLASHCTL)" ${_probe_flash_default})
if(DEFINED PROBE_FLASH_DEFAULT_LAST AND
   NOT "${PROBE_FLASH_DEFAULT_LAST}" STREQUAL "${_probe_flash_default}")
    if("${PROBE_ENABLE_FLASH_MSPM0}" STREQUAL "${PROBE_FLASH_DEFAULT_LAST}")
        set(PROBE_ENABLE_FLASH_MSPM0 ${_probe_flash_default} CACHE BOOL
            "Enable GDB 'load' to MSPM0-target flash (vFlash* + memory map via target FLASHCTL)" FORCE)
    else()
        message(WARNING
            "The generated flash default changed to ${_probe_flash_default}, but PROBE_ENABLE_FLASH_MSPM0=${PROBE_ENABLE_FLASH_MSPM0} differs from the previous generated default; keeping it.")
    endif()
endif()
set(PROBE_FLASH_DEFAULT_LAST "${_probe_flash_default}" CACHE INTERNAL
    "Last generated MSPM0 flash default" FORCE)

# Device-dependent options otherwise stick forever in CMake's cache. If a
# cached value still matches the previous device's generated default, migrate
# it to the new default. Values that differ are treated as explicit choices.
if(DEFINED PROBE_DEVICE_LAST AND NOT PROBE_DEVICE_LAST STREQUAL PROBE_DEVICE)
    if(DEFINED PROBE_FULL_FEATURE_DEFAULT_LAST)
        set(_probe_full_feature_old_default "${PROBE_FULL_FEATURE_DEFAULT_LAST}")
    elseif(PROBE_DEVICE_LAST STREQUAL "MSPM0C1104")
        set(_probe_full_feature_old_default OFF)
    else()
        set(_probe_full_feature_old_default ON)
    endif()

    foreach(_probe_option PROBE_ENABLE_DWT_WATCHPOINTS)
        if("${${_probe_option}}" STREQUAL "${_probe_full_feature_old_default}")
            set(${_probe_option} ${_probe_full_feature_default} CACHE BOOL
                "Auto-migrated device-dependent feature option" FORCE)
        else()
            message(WARNING
                "PROBE_DEVICE changed from '${PROBE_DEVICE_LAST}' to '${PROBE_DEVICE}', but ${_probe_option}=${${_probe_option}} differs from the old generated default; keeping it.")
        endif()
    endforeach()
endif()
set(PROBE_FULL_FEATURE_DEFAULT_LAST "${_probe_full_feature_default}" CACHE INTERNAL
    "Last generated default for full-profile feature options" FORCE)

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
option(PROBE_TARGET_M55 "Experimental Cortex-M55/ADIv6 target support (not implemented)" OFF)
if(PROBE_TARGET_M55)
    message(FATAL_ERROR
        "PROBE_TARGET_M55 is not implemented: M55 requires ADIv6 AP discovery, which this probe does not yet support")
endif()

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
elseif(PROBE_DEVICE STREQUAL "MSPM0G5187")
    set(PROBE_DEVICE_DEFINE "__MSPM0G5187__")
    set(PROBE_BOARD_SRC "src/board_mspm0g5187.c")
    set(PROBE_STARTUP_SRC "startup/startup_mspm0g518x_gcc.c")
    set(PROBE_LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/mspm0g5187.lds")
    set(PROBE_DRIVERLIB_A "${MSPM0_SDK_PATH}/source/ti/driverlib/lib/gcc/m0p/mspm0g518x/driverlib.a")
    # The G5187 board currently runs MCLK from SYSOSC at 32 MHz (80 MHz
    # SYSPLL is a TODO in board_mspm0g5187.c) - keep these in sync.
    set(PROBE_CORE_CLK_HZ_DEFAULT "32000000")
    set(PROBE_HAS_USB ON)
else()
    message(FATAL_ERROR "Unknown PROBE_DEVICE='${PROBE_DEVICE}'. Use MSPM0C1104, MSPM0C1105, or MSPM0G5187.")
endif()

if(NOT PROBE_ALLOW_ODIO_SWD_PINS)
    message(FATAL_ERROR
        "The current board files still route shared SWCLK/TCK and SWDIO/TMS to PA0/PA1 ODIO pins. "
        "Those open-drain pins cannot generate a production SWD or JTAG clock. Finalize the schematic/pin map, "
        "or pass -DPROBE_ALLOW_ODIO_SWD_PINS=ON to acknowledge this bring-up-only placeholder.")
endif()

# The board clock setup, not a free-form cache variable, is authoritative.
# Deriving this on every configure prevents device switches and HFXT toggles
# from leaving stale UART/delay constants behind.
if(PROBE_USE_HFXT)
    if(NOT PROBE_DEVICE STREQUAL "MSPM0C1105")
        message(FATAL_ERROR "PROBE_USE_HFXT=ON requires PROBE_DEVICE=MSPM0C1105 (C1104 has no HFXT hardware)")
    endif()
    set(_probe_core_clk_hz "${PROBE_HFXT_FREQ_HZ}")
else()
    set(_probe_core_clk_hz "${PROBE_CORE_CLK_HZ_DEFAULT}")
endif()
set(PROBE_CORE_CLK_HZ "${_probe_core_clk_hz}" CACHE INTERNAL
    "Derived probe core clock frequency in Hz" FORCE)

set(PROBE_DEVICE_LAST "${PROBE_DEVICE}" CACHE INTERNAL "Last configured PROBE_DEVICE" FORCE)

add_executable(mspm0_debugger.elf
    main.c
    src/intmath.c
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
    PROBE_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    PROBE_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    PROBE_VERSION_PATCH=${PROJECT_VERSION_PATCH}
    PROBE_USB_BCD_DEVICE=${PROBE_USB_BCD_DEVICE}
    PROBE_UART_BAUD=${PROBE_UART_BAUD}
    PROBE_CORE_CLK_HZ=${PROBE_CORE_CLK_HZ}
    SWD_DELAY_US=${PROBE_SWD_DELAY_US}
)

if(PROBE_ENABLE_SYSOSC_FCL)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_SYSOSC_FCL=1)
endif()

if(PROBE_USE_HFXT)
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_USE_HFXT=1
        PROBE_HFXT_FREQ_HZ=${PROBE_HFXT_FREQ_HZ}
    )
endif()

if(PROBE_ENABLE_QXFER_TARGET_XML)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_QXFER_TARGET_XML=1)
endif()
if(_probe_enable_riscv_minimal_xml)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_RISCV_MINIMAL_XML=1)
endif()

if(PROBE_ENABLE_DWT_WATCHPOINTS)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_DWT_WATCHPOINTS=1)
endif()

if(PROBE_ENABLE_FLASH_MSPM0)
    if(NOT PROBE_ENABLE_CORTEXM)
        message(FATAL_ERROR "PROBE_ENABLE_FLASH_MSPM0=ON requires PROBE_ENABLE_CORTEXM=ON (flash is driven over SWD MEM-AP)")
    endif()
    target_sources(mspm0_debugger.elf PRIVATE src/flash_mspm0.c)
    target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_FLASH_MSPM0=1)
endif()

if(PROBE_ENABLE_JTAG)
    target_sources(mspm0_debugger.elf PRIVATE src/jtag_bitbang.c)
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_ENABLE_JTAG=1
        PROBE_RISCV_JTAG_IR_LEN=${PROBE_RISCV_JTAG_IR_LEN}
    )
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
    if(NOT PROBE_ENABLE_QXFER_TARGET_XML AND PROBE_ENABLE_CORTEXM)
        # Stock ARM GDB's no-XML G packet is one command byte plus 42
        # registers encoded as 336 hex characters.
        set(_probe_tiny_rsp_max_payload 337)
        set(_probe_tiny_packet_size_hex "151")
    elseif(PROBE_ENABLE_RISCV)
        # RV32 G is one command byte plus 33 registers encoded as 264 hex
        # characters. This also leaves room to retransmit the 264-byte g reply.
        set(_probe_tiny_rsp_max_payload 265)
        set(_probe_tiny_packet_size_hex "109")
    else()
        set(_probe_tiny_rsp_max_payload 256)
        set(_probe_tiny_packet_size_hex "100")
    endif()
    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_TINY_RAM=1
        RSP_MAX_PAYLOAD=${_probe_tiny_rsp_max_payload}
        RSP_PACKET_SIZE_HEX=\"${_probe_tiny_packet_size_hex}\"
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

# USB/TinyUSB support (MSPM0G5187 only)
if(PROBE_HAS_USB)
    option(PROBE_ENABLE_VCOM "Enable VCOM port for target UART bridge (USB-CDC port 1)" ON)

    set(TINYUSB_PATH "${MSPM0_SDK_PATH}/source/third_party/tinyusb")

    target_sources(mspm0_debugger.elf PRIVATE
        # Local USB config
        src/usb/usb_descriptors.c
        # TinyUSB core
        ${TINYUSB_PATH}/src/tusb.c
        ${TINYUSB_PATH}/src/common/tusb_fifo.c
        # TinyUSB device stack
        ${TINYUSB_PATH}/src/device/usbd.c
        ${TINYUSB_PATH}/src/device/usbd_control.c
        # TinyUSB CDC class
        ${TINYUSB_PATH}/src/class/cdc/cdc_device.c
        # TinyUSB MSPM0 portable driver (mentor musb)
        ${TINYUSB_PATH}/src/portable/mentor/musb/dcd_musb.c
    )

    target_include_directories(mspm0_debugger.elf PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/usb
        ${TINYUSB_PATH}/src
        ${TINYUSB_PATH}/hw/mcu/ti/mspm0
        ${TINYUSB_PATH}/hw/mcu/ti/mspm0/ti/devices/msp
    )

    target_compile_definitions(mspm0_debugger.elf PRIVATE
        PROBE_HAS_USB=1
        CFG_TUSB_MCU=OPT_MCU_MSPM0G518X
    )

    if(PROBE_ENABLE_VCOM)
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_VCOM=1)
    else()
        target_compile_definitions(mspm0_debugger.elf PRIVATE PROBE_ENABLE_VCOM=0)
    endif()
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
    -Wall
    -Wextra
    -Wno-unused-parameter
)
if(PROBE_WARNINGS_AS_ERRORS)
    target_compile_options(mspm0_debugger.elf PRIVATE -Werror)
endif()

# -nostartfiles: boot goes through the vector table's resetISR, so crt0 is
# dead weight - but as the default ELF entry it drags in exit() and newlib's
# stdio-flush machinery (~440 B of SRAM on the 1KB parts).
target_link_options(mspm0_debugger.elf PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -nostartfiles
    -Wl,--entry=Reset_Handler
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

if(NOT CMAKE_SIZE OR NOT CMAKE_OBJCOPY)
    message(FATAL_ERROR "The ARM toolchain file must define absolute CMAKE_SIZE and CMAKE_OBJCOPY paths")
endif()

set(PROBE_MAP_FILE "${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.map")
set(PROBE_HEX_FILE "${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.hex")
set(PROBE_BIN_FILE "${CMAKE_CURRENT_BINARY_DIR}/mspm0_debugger.bin")

# Remove flashable artifacts before linking so a failed relink cannot leave an
# apparently current .hex/.bin behind. BYPRODUCTS and ADDITIONAL_CLEAN_FILES
# also make every generator remove all generated outputs on `clean`.
add_custom_command(TARGET mspm0_debugger.elf PRE_LINK
    COMMAND ${CMAKE_COMMAND} -E rm -f
        "${PROBE_MAP_FILE}" "${PROBE_HEX_FILE}" "${PROBE_BIN_FILE}"
    VERBATIM
)

add_custom_command(TARGET mspm0_debugger.elf POST_BUILD
    BYPRODUCTS "${PROBE_HEX_FILE}" "${PROBE_BIN_FILE}"
    COMMAND "${CMAKE_SIZE}" $<TARGET_FILE:mspm0_debugger.elf>
    COMMAND "${CMAKE_OBJCOPY}" -O ihex $<TARGET_FILE:mspm0_debugger.elf> "${PROBE_HEX_FILE}"
    COMMAND "${CMAKE_OBJCOPY}" -O binary $<TARGET_FILE:mspm0_debugger.elf> "${PROBE_BIN_FILE}"
    VERBATIM
)
set_property(TARGET mspm0_debugger.elf APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "${PROBE_MAP_FILE};${PROBE_HEX_FILE};${PROBE_BIN_FILE}")
