# Liquid Glass Pill

Native Windows C++ Vulkan demo of a draggable Liquid Glass pill button.

The app uses Win32 for the window and input, Vulkan for rendering, a cached GPU
texture for the floral wallpaper, and SPIR-V shaders for transparent liquid-glass
refraction, edge/corner distortion, chromatic edge dispersion, caustic
highlights, and shadow.

Rendering is event-driven: the app sleeps when nothing changes. During dragging,
it tracks the old and new pill bounds and uses cached swapchain images plus
Vulkan scissor/render areas to update only the dirty glass region when possible.

## Build With The Local MinGW Toolchain

```powershell
.\build.ps1
```

## Build With CMake

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
```

If CMake cannot find `mingw32-make` or a working Ninja binary, use
`.\build.ps1`; it calls the MinGW compiler directly.

The build script also compiles `shaders/liquid.vert` and `shaders/liquid.frag`
to SPIR-V files under `build/shaders/`, which the executable loads at startup.

## Run

```powershell
.\build\LiquidGlassPill.exe
```

Drag the large pill with the mouse. Resize the window to see the shader and drag
constraints adapt to the new client area.
