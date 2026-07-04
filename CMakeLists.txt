cmake_minimum_required(VERSION 3.20)

project(LiquidGlassPill LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(LiquidGlassPill WIN32
    src/main.cpp
    src/desktop_overlay.cpp
    src/overlay_pill_controller.cpp
)

target_compile_features(LiquidGlassPill PRIVATE cxx_std_17)
target_include_directories(LiquidGlassPill PRIVATE
    "C:/VulkanSDK/1.4.350.0/Include"
)
target_compile_definitions(LiquidGlassPill PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MINGW)
    target_compile_options(LiquidGlassPill PRIVATE -Wall -Wextra -pedantic -Wno-missing-field-initializers)
endif()

target_link_libraries(LiquidGlassPill PRIVATE
    "C:/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib"
    gdi32
    user32
    shell32
    ole32
    windowscodecs
    uuid
    dwmapi
    d3d11
    dxgi
)

set(SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(GLSLANG_VALIDATOR "C:/VulkanSDK/1.4.350.0/Bin/glslangValidator.exe")
set(SHADER_OUTPUTS
    "${SHADER_OUTPUT_DIR}/liquid.vert.spv"
    "${SHADER_OUTPUT_DIR}/liquid.frag.spv"
    "${SHADER_OUTPUT_DIR}/overlay.frag.spv"
)

add_custom_command(
    OUTPUT ${SHADER_OUTPUTS}
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SHADER_OUTPUT_DIR}"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/liquid.vert" -o "${SHADER_OUTPUT_DIR}/liquid.vert.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V "${CMAKE_CURRENT_SOURCE_DIR}/shaders/liquid.frag" -o "${SHADER_OUTPUT_DIR}/liquid.frag.spv"
    COMMAND "${GLSLANG_VALIDATOR}" -V -DDESKTOP_OVERLAY=1 "${CMAKE_CURRENT_SOURCE_DIR}/shaders/liquid.frag" -o "${SHADER_OUTPUT_DIR}/overlay.frag.spv"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/liquid.vert"
        "${CMAKE_CURRENT_SOURCE_DIR}/shaders/liquid.frag"
    VERBATIM
)

add_custom_target(LiquidGlassPillShaders ALL DEPENDS ${SHADER_OUTPUTS})
add_dependencies(LiquidGlassPill LiquidGlassPillShaders)
