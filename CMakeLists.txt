cmake_minimum_required(VERSION 3.19)
project(slug_wgvk C)
include(FetchContent)

# --- WGVK (WebGPU on Vulkan) with simple_wgsl shader compiler ---
set(WGVK_WGSL_SUPPORT "SIMPLE_WGSL" CACHE STRING "" FORCE)
set(WGVK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WGVK_BUILD_GLSL_SUPPORT OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(WGSL_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# Use local WGVK if available (sibling directory), otherwise fetch from GitHub
set(WGVK_LOCAL "${CMAKE_CURRENT_SOURCE_DIR}/../WGVK")
if(EXISTS "${WGVK_LOCAL}/CMakeLists.txt")
    add_subdirectory("${WGVK_LOCAL}" "${CMAKE_CURRENT_BINARY_DIR}/wgvk")
    set(WGVK_SOURCE_DIR "${WGVK_LOCAL}")
else()
    FetchContent_Declare(wgvk
        GIT_REPOSITORY https://github.com/manuel5975p/WGVK.git
        GIT_TAG        master
        GIT_SHALLOW    True
    )
    FetchContent_MakeAvailable(wgvk)
    set(WGVK_SOURCE_DIR "${wgvk_SOURCE_DIR}")
endif()

# --- GLFW ---
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
set(GLFW_CLIENT_LIBRARY "none" CACHE STRING "" FORCE)
set(GLFW_INCLUDE_NONE ON CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    URL https://github.com/glfw/glfw/archive/refs/tags/3.4.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP True
)
FetchContent_MakeAvailable(glfw)

# --- stb_truetype (single header, downloaded once) ---
set(STB_DIR "${CMAKE_CURRENT_BINARY_DIR}/stb")
if(NOT EXISTS "${STB_DIR}/stb_truetype.h")
    file(DOWNLOAD
        "https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h"
        "${STB_DIR}/stb_truetype.h"
    )
endif()

# --- Build targets ---
add_executable(slug_wgvk main.c slug.c)
add_executable(slug_headless headless.c slug.c)
target_include_directories(slug_wgvk PRIVATE
    "${STB_DIR}"
    "${WGVK_SOURCE_DIR}/examples"   # for common.h
)
target_link_libraries(slug_wgvk PUBLIC wgvk glfw)
target_compile_features(slug_wgvk PRIVATE c_std_11)

target_include_directories(slug_headless PRIVATE
    "${STB_DIR}"
    "${WGVK_SOURCE_DIR}/examples"
)
target_link_libraries(slug_headless PUBLIC wgvk glfw)
target_compile_features(slug_headless PRIVATE c_std_11)
if(NOT MSVC)
    target_link_libraries(slug_wgvk PUBLIC m)
    target_link_libraries(slug_headless PUBLIC m)
endif()

# Copy shader files to build directory so the executable can find them
configure_file(SlugVertexShader.wgsl ${CMAKE_CURRENT_BINARY_DIR}/SlugVertexShader.wgsl COPYONLY)
configure_file(SlugPixelShader.wgsl  ${CMAKE_CURRENT_BINARY_DIR}/SlugPixelShader.wgsl  COPYONLY)
