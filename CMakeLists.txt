cmake_minimum_required(VERSION 3.20)

project(LiquidGlassPill LANGUAGES CXX)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(LiquidGlassPill WIN32
    src/main.cpp
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
    target_compile_options(LiquidGlassPill PRIVATE -Wall -Wextra -pedantic)
endif()

target_link_libraries(LiquidGlassPill PRIVATE
    "C:/VulkanSDK/1.4.350.0/Lib/vulkan-1.lib"
    gdi32
    user32
    ole32
    windowscodecs
    uuid
)
