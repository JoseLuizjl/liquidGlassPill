# Liquid Glass Pill

Native Windows C++ Vulkan app for a draggable Liquid Glass pill button that can
run as a system-wide desktop overlay.

The app uses Win32 for the window and input, Vulkan for rendering, a cached GPU
texture for the floral wallpaper, and SPIR-V shaders for transparent liquid-glass
refraction, edge/corner distortion, chromatic edge dispersion, caustic
highlights, and shadow. Desktop mode is a separate GPU-driven overlay that uses
DXGI Desktop Duplication, a shared D3D11 texture, and Vulkan external memory.

Rendering is event-driven: the app sleeps when nothing changes. During dragging,
it tracks the old and new pill bounds and uses cached swapchain images plus
Vulkan scissor/render areas to update only the dirty glass region when possible.

Desktop overlay mode is intentionally isolated from the normal app renderer:
`DesktopOverlay` owns the transparent overlay window, desktop capture, Vulkan
overlay renderer, and desktop-only pill controller. The normal in-app Liquid
Glass renderer stays on the regular app path.

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
to SPIR-V files under `build/shaders/`. It also compiles an overlay variant of
the fragment shader to `build/shaders/overlay.frag.spv`.

## Run

```powershell
.\build\LiquidGlassPill.exe
```

By default the app starts directly in desktop overlay mode: the main window is
hidden, the pill floats above the Windows desktop and other apps, and the tray
icon remains available for "Show app" and "Exit". Use `Ctrl+Alt+P` to toggle the
desktop pill from anywhere when Windows accepts the global hotkey.

To start in the normal in-app demo window instead:

```powershell
.\build\LiquidGlassPill.exe --window
```

In the normal window, drag the large pill with the mouse. Click the OS button in
the top-right of the app to activate desktop mode; click it again, use the tray
icon, or press `Ctrl+Alt+P` to return to the normal app window. Resize the window
to see the shader and drag constraints adapt to the new client area.
