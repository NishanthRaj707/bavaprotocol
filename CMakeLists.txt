# BAVA Protocol Unified CMake Build Script
# Automatically adapts to Zephyr RTOS Modules, ESP-IDF Components, and Standalone CMake Builds.

cmake_minimum_required(VERSION 3.13)

# Define source file list
set(BAVA_SOURCES
    src/bava_crc.c
    src/bava_dict.c
    src/bava_state.c
    src/bava_tx.c
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/bava.c")
    list(APPEND BAVA_SOURCES src/bava.c)
endif()

if(DEFINED ZEPHYR_BASE)
    # -------------------------------------------------------------------------
    # Native Zephyr RTOS Module Integration
    # -------------------------------------------------------------------------
    if(CONFIG_BAVA_PROTOCOL)
        zephyr_library()
        zephyr_library_sources(${BAVA_SOURCES})
        zephyr_library_include_directories(include)
    endif()

elseif(DEFINED IDF_PATH)
    # -------------------------------------------------------------------------
    # Native ESP-IDF Component Integration
    # -------------------------------------------------------------------------
    idf_component_register(
        SRCS ${BAVA_SOURCES}
        INCLUDE_DIRS "include"
        REQUIRES freertos driver log
    )
    if(TARGET ${COMPONENT_LIB})
        target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror -Wstrict-prototypes)
    endif()

else()
    # -------------------------------------------------------------------------
    # Standalone CMake Build (Bare-Metal / Desktop Host Verification)
    # -------------------------------------------------------------------------
    project(bava C)
    add_library(bava STATIC ${BAVA_SOURCES})
    target_include_directories(bava PUBLIC include)
    target_compile_options(bava PRIVATE -Wall -Wextra -Werror -Wstrict-prototypes)
endif()