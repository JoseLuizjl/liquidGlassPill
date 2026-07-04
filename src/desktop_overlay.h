#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <chrono>
#include <string>

#include "overlay_pill_controller.h"

struct DesktopOverlayVulkanContext {
    HINSTANCE instanceHandle = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    std::uint32_t graphicsFamily = 0;
    std::uint32_t presentFamily = 0;
    bool externalMemoryWin32Enabled = false;
    bool keyedMutexEnabled = false;
    std::wstring shaderDirectory;
};

class DesktopOverlay {
public:
    DesktopOverlay() = default;
    ~DesktopOverlay();

    DesktopOverlay(const DesktopOverlay&) = delete;
    DesktopOverlay& operator=(const DesktopOverlay&) = delete;

    bool Start(const DesktopOverlayVulkanContext& context, POINT initialCenterScreen);
    void Stop();
    bool Tick();
    void OnDisplayChanged();

    bool running() const { return running_; }
    bool restoreRequested() const { return restoreRequested_; }
    const std::string& lastError() const { return lastError_; }
    POINT CenterScreenPoint() const;

private:
    class OverlayWindow;
    class BackgroundProvider;
    class VulkanRenderer;
    class LiquidGlassRenderer;

    RECT VirtualScreenRect() const;
    bool StartGpuPipeline();
    void StopGpuPipeline();
    void ApplyWindowRegion();
    void RequestRestore();
    LRESULT HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    DesktopOverlayVulkanContext context_{};
    OverlayWindow* window_ = nullptr;
    BackgroundProvider* background_ = nullptr;
    VulkanRenderer* renderer_ = nullptr;
    OverlayPillController pill_;
    RECT virtualScreen_{};
    std::string lastError_;
    bool running_ = false;
    bool restoreRequested_ = false;
    std::chrono::steady_clock::time_point startedAt_{};
};
