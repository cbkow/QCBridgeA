cmake_minimum_required(VERSION 3.24)
project(QCBridgeA VERSION 0.0.1 LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------------------------------------------------------------------------
# The AJA NTV2 SDK (MIT), a submodule pinned to the ntv2_17_5 branch
# (PLAN A1). Static library only: no demos, tools, tests, driver, RDMA or
# plugin loading (the last would pull mbedtls in).
# ---------------------------------------------------------------------------
set(QCBA_AJA_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/libajantv2" CACHE PATH "libajantv2 checkout")
if(NOT EXISTS "${QCBA_AJA_DIR}/ajantv2/CMakeLists.txt")
    message(FATAL_ERROR "libajantv2 not found at ${QCBA_AJA_DIR} — run: git submodule update --init")
endif()
if(APPLE AND NOT CMAKE_OSX_SYSROOT)
    # The SDK's CMakeOptions parses the SDK name out of CMAKE_OSX_SYSROOT,
    # which a command-line-tools CMake leaves empty.
    # It also wants the version in the name (MacOSX26.0.sdk, not MacOSX.sdk).
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE _qcba_sdk OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND xcrun --show-sdk-version OUTPUT_VARIABLE _qcba_sdk_ver OUTPUT_STRIP_TRAILING_WHITESPACE)
    get_filename_component(_qcba_sdk_dir "${_qcba_sdk}" DIRECTORY)
    if(EXISTS "${_qcba_sdk_dir}/MacOSX${_qcba_sdk_ver}.sdk")
        set(CMAKE_OSX_SYSROOT "${_qcba_sdk_dir}/MacOSX${_qcba_sdk_ver}.sdk")
    else()
        set(CMAKE_OSX_SYSROOT "${_qcba_sdk}")
    endif()
endif()
set(AJANTV2_DISABLE_DEMOS       ON CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TOOLS       ON CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_TESTS       ON CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_DRIVER      ON CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_RDMA        ON CACHE BOOL "" FORCE)
set(AJANTV2_DISABLE_PLUGIN_LOAD ON CACHE BOOL "" FORCE)
set(AJANTV2_BUILD_SHARED        OFF CACHE BOOL "" FORCE)
add_subdirectory("${QCBA_AJA_DIR}" "${CMAKE_BINARY_DIR}/libajantv2" EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# ---------------------------------------------------------------------------
# qcbae_wire — the vendored wire contract (MIT, vendor/qcbae/LICENSE): the
# shared mapping, the slot protocol, the output ring and the audio ring.
# Re-copied from QCBridgeAE src/common/, never edited here.
# ---------------------------------------------------------------------------
add_library(qcbae_wire STATIC
    vendor/qcbae/ring_mapping.cpp
    vendor/qcbae/output_ring.cpp
    vendor/qcbae/audio_ring.cpp
)
target_include_directories(qcbae_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/vendor)
if(MSVC)
    target_compile_options(qcbae_wire PRIVATE /W4)
else()
    target_compile_options(qcbae_wire PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# qcvo-probe — both ends of the bus, for measuring either side alone
# (the same tool as QCBridgeN's).
# ---------------------------------------------------------------------------
add_executable(qcvo-probe tools/qcvo-probe/main.cpp)
target_link_libraries(qcvo-probe PRIVATE qcbae_wire)
if(NOT MSVC)
    target_compile_options(qcvo-probe PRIVATE -Wall -Wextra)
endif()

# ---------------------------------------------------------------------------
# QCBridgeA — the sink: QCView's rings in, the AJA device's SDI/HDMI out.
# Headless (PLAN A7); QCView's Output row shows its status line.
# ---------------------------------------------------------------------------
add_executable(QCBridgeA
    src/sink/main.cpp
    src/sink/bus_sink.cpp
    src/sink/bus_sink.h
    src/sink/aja_output.cpp
    src/sink/aja_output.h
)
target_compile_definitions(QCBridgeA PRIVATE QCBRIDGEA_VERSION="${PROJECT_VERSION}")
target_link_libraries(QCBridgeA PRIVATE qcbae_wire ajantv2)
if(MSVC)
    target_compile_options(QCBridgeA PRIVATE /W4 /wd4100)
else()
    target_compile_options(QCBridgeA PRIVATE -Wall -Wextra)
endif()
