#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <objbase.h>
#include <wincodec.h>
#include <vulkan/vulkan.h>

#include "desktop_overlay.h"
#include "liquid_glass_shared.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr int kInitialWidth = 960;
constexpr int kInitialHeight = 640;
constexpr int kMaxFramesInFlight = 2;
constexpr float kPillTransitionResponsiveness = 9.25f;
constexpr float kPillTransitionSnapEpsilon = 0.35f;
constexpr float kGooDecayResponsiveness = 3.85f;
constexpr float kGooSnapEpsilon = 0.015f;
constexpr int kToggleButtonWidth = 128;
constexpr int kToggleButtonHeight = 44;
constexpr int kToggleButtonMargin = 18;
constexpr UINT kTrayCallbackMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTrayShowCommand = 1001;
constexpr UINT kTrayExitCommand = 1002;
constexpr int kDesktopHotkeyId = 2001;
constexpr UINT kDesktopHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_NOREPEAT;
constexpr UINT kDesktopHotkeyKey = 'P';
constexpr wchar_t kBackgroundImagePath[] = L"assets\\Newspaper.png";

struct ImagePixels {
    std::vector<std::uint8_t> rgba;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

using PushConstants = LiquidGlassPushConstants;

struct QueueFamilyIndices {
    std::optional<std::uint32_t> graphicsFamily;
    std::optional<std::uint32_t> presentFamily;

    bool Complete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupport {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

struct AppState {
    HWND window = nullptr;
    HINSTANCE instanceHandle = nullptr;
    int width = kInitialWidth;
    int height = kInitialHeight;
    bool minimized = false;
    bool framebufferResized = false;
    bool needsRedraw = true;
    bool forceFullRedraw = true;
    bool running = true;
    bool vulkanReady = false;
    bool desktopMode = false;
    bool externalMemoryWin32Enabled = false;
    bool keyedMutexEnabled = false;
    bool trayIconAdded = false;
    bool desktopHotkeyRegistered = false;
    DesktopOverlay* desktopOverlay = nullptr;
    DWORD normalStyle = 0;
    DWORD normalExStyle = 0;
    RECT normalWindowRect{};
    WINDOWPLACEMENT normalPlacement{};
    bool normalPlacementValid = false;

    float pillX = kInitialWidth * 0.5f;
    float pillY = kInitialHeight * 0.52f;
    float pillBaseW = 430.0f;
    float pillBaseH = 150.0f;
    float pillW = 430.0f;
    float pillH = 150.0f;
    float visualPillX = kInitialWidth * 0.5f;
    float visualPillY = kInitialHeight * 0.52f;
    float visualPillW = 430.0f;
    float visualPillH = 150.0f;
    bool pillVertical = false;
    int pillDockSide = 0;
    int pillDockEdgeY = 0;
    bool pillAnimating = false;
    float sideGooAmount = 0.0f;
    int sideGooSide = 0;
    bool dragging = false;
    float dragOffsetX = 0.0f;
    float dragOffsetY = 0.0f;
    std::chrono::steady_clock::time_point startedAt;
    std::chrono::steady_clock::time_point pillAnimationUpdatedAt;

    VkInstance instance = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;
    QueueFamilyIndices queueFamilies;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImage> swapchainImages;
    std::vector<VkImageView> swapchainImageViews;
    std::vector<VkFramebuffer> clearFramebuffers;
    std::vector<VkFramebuffer> loadFramebuffers;
    std::vector<bool> imageInitialized;
    std::vector<VkRect2D> imagePillRects;
    VkRect2D dirtyRect{};

    VkRenderPass clearRenderPass = VK_NULL_HANDLE;
    VkRenderPass loadRenderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline clearPipeline = VK_NULL_HANDLE;
    VkPipeline loadPipeline = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkCommandPool commandPool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers{};
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailableSemaphores{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinishedSemaphores{};
    std::array<VkFence, kMaxFramesInFlight> inFlightFences{};
    std::uint32_t currentFrame = 0;

    VkImage backgroundImage = VK_NULL_HANDLE;
    VkDeviceMemory backgroundMemory = VK_NULL_HANDLE;
    VkImageView backgroundImageView = VK_NULL_HANDLE;
    VkSampler backgroundSampler = VK_NULL_HANDLE;

};

AppState* g_app = nullptr;

void AddTrayIcon(AppState& app);
void RemoveTrayIcon(AppState& app);
void RegisterGlobalHotkeys(AppState& app);
void UnregisterGlobalHotkeys(AppState& app);

float Clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

float SmoothStep(float edge0, float edge1, float value) {
    const float t = Clamp01((value - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

float Lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

std::vector<char> ReadBinaryFile(const std::wstring& path) {
    std::ifstream file(path.c_str(), std::ios::ate | std::ios::binary);
    if (!file) {
        throw std::runtime_error("Could not open a required SPIR-V shader file.");
    }

    const std::streamsize size = file.tellg();
    std::vector<char> data(static_cast<size_t>(size));
    file.seekg(0);
    file.read(data.data(), size);
    return data;
}

std::wstring ExeDirectory() {
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) {
        throw std::runtime_error("Could not resolve executable path.");
    }
    std::wstring result(path.data(), length);
    const size_t slash = result.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : result.substr(0, slash);
}

std::string LowerAscii(const char* text) {
    std::string value = text ? text : "";
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool HasCommandLineSwitch(const std::string& commandLine, const char* name) {
    return commandLine.find(name) != std::string::npos;
}

bool ShouldStartInDesktopMode(LPSTR commandLine) {
    const std::string flags = LowerAscii(commandLine);
    if (HasCommandLineSwitch(flags, "--window") || HasCommandLineSwitch(flags, "--app") ||
        HasCommandLineSwitch(flags, "/window") || HasCommandLineSwitch(flags, "/app")) {
        return false;
    }
    return true;
}

void CheckVk(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

void CheckHr(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(message);
    }
}

void ShowError(const std::string& message) {
    MessageBoxA(nullptr, message.c_str(), "LiquidGlassPill", MB_ICONERROR | MB_OK);
}

struct ComInitializer {
    HRESULT result = S_OK;
    bool uninitialize = false;

    ComInitializer() {
        result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == RPC_E_CHANGED_MODE) {
            result = S_OK;
            return;
        }
        uninitialize = SUCCEEDED(result);
    }

    ~ComInitializer() {
        if (uninitialize) {
            CoUninitialize();
        }
    }
};

template <typename T>
struct ComPtr {
    T* ptr = nullptr;

    ComPtr() = default;

    ~ComPtr() {
        Reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }

    T* Get() const {
        return ptr;
    }

    T** GetAddressOf() {
        return &ptr;
    }

    T** ReleaseAndGetAddressOf() {
        Reset();
        return &ptr;
    }

    void Reset(T* value = nullptr) {
        if (ptr) {
            ptr->Release();
        }
        ptr = value;
    }

    T* operator->() const {
        return ptr;
    }

    explicit operator bool() const {
        return ptr != nullptr;
    }
};

ImagePixels LoadBackgroundPixels() {
    ComInitializer com;
    CheckHr(com.result, "Could not initialize Windows imaging.");

    ComPtr<IWICImagingFactory> factory;
    CheckHr(
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.GetAddressOf())),
        "Could not create the Windows imaging factory.");

    ComPtr<IWICBitmapDecoder> decoder;
    CheckHr(
        factory->CreateDecoderFromFilename(kBackgroundImagePath, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()),
        "Could not open assets\\Newspaper.png.");

    ComPtr<IWICBitmapFrameDecode> frame;
    CheckHr(decoder->GetFrame(0, frame.GetAddressOf()), "Could not decode the first frame of Newspaper.png.");

    UINT width = 0;
    UINT height = 0;
    CheckHr(frame->GetSize(&width, &height), "Could not read the Newspaper.png dimensions.");
    if (width == 0 || height == 0) {
        throw std::runtime_error("Newspaper.png has invalid dimensions.");
    }

    ComPtr<IWICFormatConverter> converter;
    CheckHr(factory->CreateFormatConverter(converter.GetAddressOf()), "Could not create the PNG format converter.");
    CheckHr(
        converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom),
        "Could not convert Newspaper.png to RGBA pixels.");

    const size_t stride = static_cast<size_t>(width) * 4u;
    const size_t bufferSize = stride * static_cast<size_t>(height);
    if (stride > UINT32_MAX || bufferSize > UINT32_MAX) {
        throw std::runtime_error("Newspaper.png is too large for this demo texture upload path.");
    }

    ImagePixels image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.resize(bufferSize);
    CheckHr(
        converter->CopyPixels(nullptr, static_cast<UINT>(stride), static_cast<UINT>(image.rgba.size()), image.rgba.data()),
        "Could not copy Newspaper.png pixels.");
    return image;
}

float PillSdfAt(float pillX, float pillY, float pillW, float pillH, float x, float y) {
    const float radius = std::min(pillW, pillH) * 0.5f;
    const float qx = std::fabs(x - pillX) - pillW * 0.5f + radius;
    const float qy = std::fabs(y - pillY) - pillH * 0.5f + radius;
    const float outsideX = std::max(qx, 0.0f);
    const float outsideY = std::max(qy, 0.0f);
    return std::min(std::max(qx, qy), 0.0f) + std::sqrt(outsideX * outsideX + outsideY * outsideY) - radius;
}

float PillSdf(const AppState& app, float x, float y) {
    return PillSdfAt(app.visualPillX, app.visualPillY, app.visualPillW, app.visualPillH, x, y);
}

bool HitPill(const AppState& app, float x, float y) {
    return PillSdf(app, x, y) <= 0.0f;
}

RECT ToggleButtonBounds(const AppState& app) {
    const int width = std::min(kToggleButtonWidth, std::max(app.width - kToggleButtonMargin * 2, 1));
    const int height = std::min(kToggleButtonHeight, std::max(app.height - kToggleButtonMargin * 2, 1));
    const int left = std::max(kToggleButtonMargin, app.width - width - kToggleButtonMargin);
    const int top = kToggleButtonMargin;
    return {left, top, left + width, top + height};
}

bool HitToggleButton(const AppState& app, float x, float y) {
    const RECT button = ToggleButtonBounds(app);
    return x >= static_cast<float>(button.left) && x <= static_cast<float>(button.right) &&
           y >= static_cast<float>(button.top) && y <= static_cast<float>(button.bottom);
}

float PillVisualDelta(const AppState& app) {
    const float positionDelta = std::max(std::fabs(app.visualPillX - app.pillX), std::fabs(app.visualPillY - app.pillY));
    const float sizeDelta = std::max(std::fabs(app.visualPillW - app.pillW), std::fabs(app.visualPillH - app.pillH));
    return std::max(positionDelta, sizeDelta);
}

void SyncPillVisualToTarget(AppState& app) {
    app.visualPillX = app.pillX;
    app.visualPillY = app.pillY;
    app.visualPillW = app.pillW;
    app.visualPillH = app.pillH;
    app.pillAnimating = false;
}

void StartPillTransition(AppState& app) {
    if (PillVisualDelta(app) <= kPillTransitionSnapEpsilon) {
        SyncPillVisualToTarget(app);
        return;
    }
    app.pillAnimating = true;
    app.pillAnimationUpdatedAt = std::chrono::steady_clock::now();
    app.needsRedraw = true;
}

void PulseSideGoo(AppState& app, int side, float amount) {
    if (side == 0) {
        return;
    }
    app.sideGooSide = std::clamp(side, -2, 2);
    app.sideGooAmount = std::max(app.sideGooAmount, Clamp01(amount));
    app.pillAnimationUpdatedAt = std::chrono::steady_clock::now();
    app.needsRedraw = true;
}

void SetSideGoo(AppState& app, int side, float amount) {
    if (side == 0) {
        return;
    }
    app.sideGooSide = std::clamp(side, -2, 2);
    app.sideGooAmount = Clamp01(amount);
    app.pillAnimationUpdatedAt = std::chrono::steady_clock::now();
    app.needsRedraw = true;
}

void FlushDesktopComposition() {
    if (FAILED(DwmFlush())) {
        Sleep(16);
    }
}

void ApplyPillOrientation(AppState& app) {
    app.pillW = app.pillVertical ? app.pillBaseH : app.pillBaseW;
    app.pillH = app.pillVertical ? app.pillBaseW : app.pillBaseH;
}

void ClampPill(AppState& app) {
    const float halfW = app.pillW * 0.5f;
    const float halfH = app.pillH * 0.5f;
    const float maxX = std::max(halfW, static_cast<float>(app.width) - halfW);
    const float maxY = std::max(halfH, static_cast<float>(app.height) - halfH);
    app.pillX = std::clamp(app.pillX, halfW, maxX);
    app.pillY = std::clamp(app.pillY, halfH, maxY);
}

void SetPillOrientation(AppState& app, bool vertical, int dockSide) {
    app.pillVertical = vertical;
    app.pillDockSide = vertical ? (dockSide < 0 ? -1 : 1) : 0;
    app.pillDockEdgeY = 0;
    ApplyPillOrientation(app);
    if (app.pillVertical) {
        app.pillX = app.pillDockSide < 0 ? app.pillW * 0.5f : static_cast<float>(app.width) - app.pillW * 0.5f;
        PulseSideGoo(app, app.pillDockSide, 0.66f);
    }
    ClampPill(app);
    StartPillTransition(app);
}

void UpdateDraggedPill(AppState& app, float pointerX, float pointerY) {
    const float rawX = pointerX - app.dragOffsetX;
    const float rawY = pointerY - app.dragOffsetY;
    app.pillX = rawX;
    app.pillY = rawY;
    ClampPill(app);

    constexpr float edgeEpsilon = 0.5f;
    const float leftLimit = app.pillW * 0.5f;
    const float rightLimit = static_cast<float>(app.width) - app.pillW * 0.5f;

    if (!app.pillVertical) {
        if (app.pillX <= leftLimit + edgeEpsilon) {
            SetPillOrientation(app, true, -1);
            app.dragOffsetX = pointerX - app.pillX;
            app.dragOffsetY = pointerY - app.pillY;
        } else if (app.pillX >= rightLimit - edgeEpsilon) {
            SetPillOrientation(app, true, 1);
            app.dragOffsetX = pointerX - app.pillX;
            app.dragOffsetY = pointerY - app.pillY;
        }
        if (!app.pillVertical) {
            const float topLimit = app.pillH * 0.5f;
            const float bottomLimit = static_cast<float>(app.height) - app.pillH * 0.5f;
            const float releaseDistance = app.pillH * 0.48f;
            if (app.pillDockEdgeY != 0) {
                const int edge = app.pillDockEdgeY < 0 ? -1 : 1;
                const int gooEdge = edge < 0 ? -2 : 2;
                const float dockY = edge < 0 ? topLimit : bottomLimit;
                const float inwardDistance = edge < 0 ? rawY - dockY : dockY - rawY;
                if (inwardDistance > releaseDistance) {
                    PulseSideGoo(app, gooEdge, 0.86f);
                    app.pillDockEdgeY = 0;
                    app.pillY = rawY;
                    ClampPill(app);
                } else {
                    app.pillY = dockY;
                    const float pull = SmoothStep(0.0f, 1.0f, Clamp01(inwardDistance / releaseDistance));
                    SetSideGoo(app, gooEdge, 0.18f + pull * 0.72f);
                }
            } else if (app.pillY <= topLimit + edgeEpsilon) {
                app.pillDockEdgeY = -1;
                app.pillY = topLimit;
                PulseSideGoo(app, -2, 0.58f);
            } else if (app.pillY >= bottomLimit - edgeEpsilon) {
                app.pillDockEdgeY = 1;
                app.pillY = bottomLimit;
                PulseSideGoo(app, 2, 0.58f);
            }
        }
        if (!app.pillAnimating) {
            SyncPillVisualToTarget(app);
        }
        return;
    }

    const int side = app.pillDockSide < 0 ? -1 : 1;
    const float dockX = side < 0 ? app.pillW * 0.5f : static_cast<float>(app.width) - app.pillW * 0.5f;
    const float unclampedCenterX = pointerX - app.dragOffsetX;
    const float releaseDistance = app.pillW * 0.50f;
    const float inwardDistance = side < 0 ? unclampedCenterX - dockX : dockX - unclampedCenterX;

    if (inwardDistance > releaseDistance) {
        PulseSideGoo(app, side, 0.92f);
        SetPillOrientation(app, false, 0);
        app.dragOffsetX = pointerX - app.pillX;
        app.dragOffsetY = pointerY - app.pillY;
    } else {
        app.pillX = dockX;
        ClampPill(app);
        const float pull = SmoothStep(0.0f, 1.0f, Clamp01(inwardDistance / releaseDistance));
        SetSideGoo(app, side, 0.18f + pull * 0.72f);
    }
    if (!app.pillAnimating) {
        SyncPillVisualToTarget(app);
    }
}

VkRect2D FullRect(const AppState& app) {
    return {{0, 0}, {static_cast<std::uint32_t>(std::max(app.width, 1)), static_cast<std::uint32_t>(std::max(app.height, 1))}};
}

VkRect2D PillRect(const AppState& app) {
    const int margin = static_cast<int>(std::ceil(app.visualPillH * 0.70f));
    const int left = static_cast<int>(std::floor(app.visualPillX - app.visualPillW * 0.5f)) - margin;
    const int top = static_cast<int>(std::floor(app.visualPillY - app.visualPillH * 0.5f)) - margin;
    const int right = static_cast<int>(std::ceil(app.visualPillX + app.visualPillW * 0.5f)) + margin;
    const int bottom = static_cast<int>(std::ceil(app.visualPillY + app.visualPillH * 0.5f)) + margin;

    const int clampedLeft = std::clamp(left, 0, std::max(app.width, 1));
    const int clampedTop = std::clamp(top, 0, std::max(app.height, 1));
    const int clampedRight = std::clamp(right, clampedLeft, std::max(app.width, 1));
    const int clampedBottom = std::clamp(bottom, clampedTop, std::max(app.height, 1));
    return {
        {clampedLeft, clampedTop},
        {
            static_cast<std::uint32_t>(clampedRight - clampedLeft),
            static_cast<std::uint32_t>(clampedBottom - clampedTop),
        },
    };
}

VkRect2D UnionRect(VkRect2D a, VkRect2D b) {
    const int left = std::min(a.offset.x, b.offset.x);
    const int top = std::min(a.offset.y, b.offset.y);
    const int right = std::max(a.offset.x + static_cast<int>(a.extent.width), b.offset.x + static_cast<int>(b.extent.width));
    const int bottom = std::max(a.offset.y + static_cast<int>(a.extent.height), b.offset.y + static_cast<int>(b.extent.height));
    return {
        {left, top},
        {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)},
    };
}

void InvalidateRect(AppState& app, VkRect2D rect) {
    app.dirtyRect = app.needsRedraw ? UnionRect(app.dirtyRect, rect) : rect;
    app.needsRedraw = true;
}

void InvalidatePill(AppState& app, VkRect2D previousPillRect) {
    InvalidateRect(app, UnionRect(previousPillRect, PillRect(app)));
}

void InvalidateFull(AppState& app) {
    app.dirtyRect = FullRect(app);
    app.needsRedraw = true;
    app.forceFullRedraw = true;
}

void UpdatePillTransition(AppState& app) {
    if (!app.pillAnimating && app.sideGooAmount <= 0.0f) {
        return;
    }

    const VkRect2D previous = PillRect(app);
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = std::chrono::duration<float>(now - app.pillAnimationUpdatedAt).count();
    const float dt = std::clamp(elapsed, 0.0f, 0.050f);
    app.pillAnimationUpdatedAt = now;

    if (app.pillAnimating) {
        const float t = 1.0f - std::exp(-kPillTransitionResponsiveness * dt);
        app.visualPillX = Lerp(app.visualPillX, app.pillX, t);
        app.visualPillY = Lerp(app.visualPillY, app.pillY, t);
        app.visualPillW = Lerp(app.visualPillW, app.pillW, t);
        app.visualPillH = Lerp(app.visualPillH, app.pillH, t);

        if (PillVisualDelta(app) <= kPillTransitionSnapEpsilon) {
            SyncPillVisualToTarget(app);
        }
    }

    if (app.sideGooAmount > 0.0f) {
        app.sideGooAmount *= std::exp(-kGooDecayResponsiveness * dt);
        if (app.sideGooAmount <= kGooSnapEpsilon) {
            app.sideGooAmount = 0.0f;
            app.sideGooSide = 0;
        }
    }

    InvalidatePill(app, previous);
}

void ResizePill(AppState& app, int width, int height) {
    app.width = std::max(width, 1);
    app.height = std::max(height, 1);
    app.minimized = width == 0 || height == 0;
    app.pillBaseW = std::min(430.0f, static_cast<float>(app.width) * 0.78f);
    app.pillBaseH = std::min(150.0f, std::max(88.0f, static_cast<float>(app.height) * 0.28f));
    if (app.pillBaseW < app.pillBaseH * 2.2f) {
        app.pillBaseW = app.pillBaseH * 2.2f;
    }
    app.pillBaseW = std::min(app.pillBaseW, static_cast<float>(app.width) * 0.92f);
    ApplyPillOrientation(app);
    if (app.pillVertical) {
        app.pillX = app.pillDockSide < 0 ? app.pillW * 0.5f : static_cast<float>(app.width) - app.pillW * 0.5f;
    } else if (app.pillDockEdgeY != 0) {
        app.pillY = app.pillDockEdgeY < 0 ? app.pillH * 0.5f : static_cast<float>(app.height) - app.pillH * 0.5f;
    }
    ClampPill(app);
    SyncPillVisualToTarget(app);
    app.framebufferResized = true;
    InvalidateFull(app);
}

bool RefreshClientSize(AppState& app) {
    RECT client{};
    if (!GetClientRect(app.window, &client)) {
        return false;
    }

    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width == 0 || height == 0) {
        app.minimized = true;
        return false;
    }

    app.minimized = false;
    if (width != app.width || height != app.height) {
        ResizePill(app, width, height);
    }
    return true;
}

bool SurfaceHasDrawableExtent(const VkSurfaceCapabilitiesKHR& capabilities, const AppState& app) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent.width > 0 && capabilities.currentExtent.height > 0;
    }
    return app.width > 0 && app.height > 0 && capabilities.maxImageExtent.width > 0 && capabilities.maxImageExtent.height > 0;
}

QueueFamilyIndices FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface) {
    QueueFamilyIndices indices;
    std::uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

    for (std::uint32_t i = 0; i < count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            indices.graphicsFamily = i;
        }
        VkBool32 presentSupported = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupported);
        if (presentSupported) {
            indices.presentFamily = i;
        }
        if (indices.Complete()) {
            break;
        }
    }
    return indices;
}

SwapchainSupport QuerySwapchainSupport(VkPhysicalDevice device, VkSurfaceKHR surface) {
    SwapchainSupport support;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &support.capabilities);

    std::uint32_t formatCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
    support.formats.resize(formatCount);
    if (formatCount > 0) {
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, support.formats.data());
    }

    std::uint32_t presentModeCount = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
    support.presentModes.resize(presentModeCount);
    if (presentModeCount > 0) {
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, support.presentModes.data());
    }
    return support;
}

bool DeviceSupportsExtension(VkPhysicalDevice device, const char* requiredExtension) {
    std::uint32_t extensionCount = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> extensions(extensionCount);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, extensions.data());
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, requiredExtension) == 0) {
            return true;
        }
    }
    return false;
}

bool DeviceSupportsSwapchain(VkPhysicalDevice device) {
    return DeviceSupportsExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
}

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (const auto& mode : modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, const AppState& app) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }
    VkExtent2D extent{
        static_cast<std::uint32_t>(std::max(app.width, 1)),
        static_cast<std::uint32_t>(std::max(app.height, 1)),
    };
    extent.width = std::clamp(extent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    extent.height = std::clamp(extent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return extent;
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities, const AppState& app) {
    (void)app;
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    }
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    }
    if ((capabilities.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) != 0) {
        return VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

std::uint32_t FindMemoryType(AppState& app, std::uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(app.physicalDevice, &memoryProperties);
    for (std::uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) != 0 && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Could not find a suitable Vulkan memory type.");
}

void CreateBuffer(AppState& app, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    CheckVk(vkCreateBuffer(app.device, &bufferInfo, nullptr, &buffer), "Could not create Vulkan buffer.");

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(app.device, buffer, &requirements);

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(app, requirements.memoryTypeBits, properties);
    CheckVk(vkAllocateMemory(app.device, &allocateInfo, nullptr, &memory), "Could not allocate Vulkan buffer memory.");
    CheckVk(vkBindBufferMemory(app.device, buffer, memory, 0), "Could not bind Vulkan buffer memory.");
}

VkCommandBuffer BeginSingleTimeCommands(AppState& app) {
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandPool = app.commandPool;
    allocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    CheckVk(vkAllocateCommandBuffers(app.device, &allocateInfo, &commandBuffer), "Could not allocate one-shot command buffer.");

    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Could not begin one-shot command buffer.");
    return commandBuffer;
}

void EndSingleTimeCommands(AppState& app, VkCommandBuffer commandBuffer) {
    CheckVk(vkEndCommandBuffer(commandBuffer), "Could not end one-shot command buffer.");

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;
    CheckVk(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE), "Could not submit one-shot command buffer.");
    CheckVk(vkQueueWaitIdle(app.graphicsQueue), "Could not wait for one-shot command buffer.");
    vkFreeCommandBuffers(app.device, app.commandPool, 1, &commandBuffer);
}

void TransitionImageLayout(AppState& app, VkImage image, VkFormat, VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands(app);

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    } else {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    EndSingleTimeCommands(app, commandBuffer);
}

void CopyBufferToImage(AppState& app, VkBuffer buffer, VkImage image, std::uint32_t width, std::uint32_t height) {
    VkCommandBuffer commandBuffer = BeginSingleTimeCommands(app);

    VkBufferImageCopy region{};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    EndSingleTimeCommands(app, commandBuffer);
}

VkImageView CreateImageView(AppState& app, VkImage image, VkFormat format) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    CheckVk(vkCreateImageView(app.device, &viewInfo, nullptr, &imageView), "Could not create Vulkan image view.");
    return imageView;
}

void CreateBackgroundTexture(AppState& app) {
    const ImagePixels image = LoadBackgroundPixels();
    const VkDeviceSize imageSize = image.rgba.size();

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    CreateBuffer(app, imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingMemory);

    void* mapped = nullptr;
    CheckVk(vkMapMemory(app.device, stagingMemory, 0, imageSize, 0, &mapped), "Could not map staging memory.");
    std::memcpy(mapped, image.rgba.data(), image.rgba.size());
    vkUnmapMemory(app.device, stagingMemory);

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {image.width, image.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    CheckVk(vkCreateImage(app.device, &imageInfo, nullptr, &app.backgroundImage), "Could not create background texture.");
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(app.device, app.backgroundImage, &requirements);

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(app, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(app.device, &allocateInfo, nullptr, &app.backgroundMemory), "Could not allocate background texture memory.");
    CheckVk(vkBindImageMemory(app.device, app.backgroundImage, app.backgroundMemory, 0), "Could not bind background texture memory.");

    TransitionImageLayout(app, app.backgroundImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CopyBufferToImage(app, stagingBuffer, app.backgroundImage, image.width, image.height);
    TransitionImageLayout(app, app.backgroundImage, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    vkDestroyBuffer(app.device, stagingBuffer, nullptr);
    vkFreeMemory(app.device, stagingMemory, nullptr);

    app.backgroundImageView = CreateImageView(app, app.backgroundImage, VK_FORMAT_R8G8B8A8_UNORM);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    CheckVk(vkCreateSampler(app.device, &samplerInfo, nullptr, &app.backgroundSampler), "Could not create Vulkan sampler.");
}

void CreateInstance(AppState& app, HINSTANCE instanceHandle) {
    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "Liquid Glass Pill";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "Native Vulkan";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_0;

    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(std::size(extensions));
    createInfo.ppEnabledExtensionNames = extensions;
    CheckVk(vkCreateInstance(&createInfo, nullptr, &app.instance), "Could not create Vulkan instance.");

    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = instanceHandle;
    surfaceInfo.hwnd = app.window;
    CheckVk(vkCreateWin32SurfaceKHR(app.instance, &surfaceInfo, nullptr, &app.surface), "Could not create Vulkan Win32 surface.");
}

void PickPhysicalDevice(AppState& app) {
    std::uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(app.instance, &deviceCount, nullptr);
    if (deviceCount == 0) {
        throw std::runtime_error("No Vulkan-capable GPU was found.");
    }

    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(app.instance, &deviceCount, devices.data());

    VkPhysicalDevice fallback = VK_NULL_HANDLE;
    QueueFamilyIndices fallbackIndices;
    for (const auto& device : devices) {
        QueueFamilyIndices indices = FindQueueFamilies(device, app.surface);
        if (!indices.Complete() || !DeviceSupportsSwapchain(device)) {
            continue;
        }
        SwapchainSupport support = QuerySwapchainSupport(device, app.surface);
        if (support.formats.empty() || support.presentModes.empty()) {
            continue;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            app.physicalDevice = device;
            app.queueFamilies = indices;
            return;
        }
        if (fallback == VK_NULL_HANDLE) {
            fallback = device;
            fallbackIndices = indices;
        }
    }

    if (fallback == VK_NULL_HANDLE) {
        throw std::runtime_error("No Vulkan device with graphics, present, and swapchain support was found.");
    }
    app.physicalDevice = fallback;
    app.queueFamilies = fallbackIndices;
}

void CreateLogicalDevice(AppState& app) {
    const float queuePriority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::unordered_set<std::uint32_t> uniqueFamilies = {*app.queueFamilies.graphicsFamily, *app.queueFamilies.presentFamily};
    for (const std::uint32_t family : uniqueFamilies) {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueInfo);
    }

    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    app.externalMemoryWin32Enabled =
        DeviceSupportsExtension(app.physicalDevice, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) &&
        DeviceSupportsExtension(app.physicalDevice, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
    app.keyedMutexEnabled = DeviceSupportsExtension(app.physicalDevice, VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
    if (app.externalMemoryWin32Enabled && app.keyedMutexEnabled) {
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        extensions.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        extensions.push_back(VK_KHR_WIN32_KEYED_MUTEX_EXTENSION_NAME);
    } else {
        app.externalMemoryWin32Enabled = false;
        app.keyedMutexEnabled = false;
    }

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.queueCreateInfoCount = static_cast<std::uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();
    createInfo.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();
    CheckVk(vkCreateDevice(app.physicalDevice, &createInfo, nullptr, &app.device), "Could not create Vulkan logical device.");

    vkGetDeviceQueue(app.device, *app.queueFamilies.graphicsFamily, 0, &app.graphicsQueue);
    vkGetDeviceQueue(app.device, *app.queueFamilies.presentFamily, 0, &app.presentQueue);
}

void CreateSwapchain(AppState& app) {
    SwapchainSupport support = QuerySwapchainSupport(app.physicalDevice, app.surface);
    if (support.formats.empty() || support.presentModes.empty() || !SurfaceHasDrawableExtent(support.capabilities, app)) {
        throw std::runtime_error("Vulkan surface is not ready for a swapchain.");
    }

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(support.formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(support.presentModes);
    const VkExtent2D extent = ChooseExtent(support.capabilities, app);

    std::uint32_t imageCount = support.capabilities.minImageCount + 1;
    if (support.capabilities.maxImageCount > 0 && imageCount > support.capabilities.maxImageCount) {
        imageCount = support.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = app.surface;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    const std::uint32_t queueFamilyIndices[] = {*app.queueFamilies.graphicsFamily, *app.queueFamilies.presentFamily};
    if (app.queueFamilies.graphicsFamily != app.queueFamilies.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = support.capabilities.currentTransform;
    createInfo.compositeAlpha = ChooseCompositeAlpha(support.capabilities, app);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    CheckVk(vkCreateSwapchainKHR(app.device, &createInfo, nullptr, &app.swapchain), "Could not create Vulkan swapchain.");

    vkGetSwapchainImagesKHR(app.device, app.swapchain, &imageCount, nullptr);
    app.swapchainImages.resize(imageCount);
    vkGetSwapchainImagesKHR(app.device, app.swapchain, &imageCount, app.swapchainImages.data());
    app.swapchainImageFormat = surfaceFormat.format;
    app.swapchainExtent = extent;
    app.imageInitialized.assign(app.swapchainImages.size(), false);
    app.imagePillRects.assign(app.swapchainImages.size(), FullRect(app));
    InvalidateFull(app);
}

void CreateSwapchainImageViews(AppState& app) {
    app.swapchainImageViews.resize(app.swapchainImages.size());
    for (size_t i = 0; i < app.swapchainImages.size(); ++i) {
        app.swapchainImageViews[i] = CreateImageView(app, app.swapchainImages[i], app.swapchainImageFormat);
    }
}

VkRenderPass CreateRenderPass(AppState& app, VkAttachmentLoadOp loadOp, VkImageLayout initialLayout) {
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = app.swapchainImageFormat;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = loadOp;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = initialLayout;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorReference{};
    colorReference.attachment = 0;
    colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    renderPassInfo.attachmentCount = 1;
    renderPassInfo.pAttachments = &colorAttachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    CheckVk(vkCreateRenderPass(app.device, &renderPassInfo, nullptr, &renderPass), "Could not create Vulkan render pass.");
    return renderPass;
}

void CreateRenderPasses(AppState& app) {
    app.clearRenderPass = CreateRenderPass(app, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED);
    app.loadRenderPass = CreateRenderPass(app, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

VkShaderModule CreateShaderModule(AppState& app, const std::vector<char>& code) {
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const std::uint32_t*>(code.data());

    VkShaderModule module = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(app.device, &createInfo, nullptr, &module), "Could not create Vulkan shader module.");
    return module;
}

void CreateDescriptorSetLayout(AppState& app) {
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorCount = 1;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;
    CheckVk(vkCreateDescriptorSetLayout(app.device, &layoutInfo, nullptr, &app.descriptorSetLayout), "Could not create Vulkan descriptor layout.");
}

VkPipeline CreatePipelineForRenderPass(
    AppState& app,
    VkRenderPass renderPass,
    const std::vector<char>& vertCode,
    const std::vector<char>& fragCode,
    VkColorComponentFlags colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    bool multiplyRgbByDstAlpha = false) {
    VkShaderModule vertModule = CreateShaderModule(app, vertCode);
    VkShaderModule fragModule = CreateShaderModule(app, fragCode);

    VkPipelineShaderStageCreateInfo vertexStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertModule;
    vertexStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragmentStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragModule;
    fragmentStage.pName = "main";
    const VkPipelineShaderStageCreateInfo shaderStages[] = {vertexStage, fragmentStage};

    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = colorWriteMask;
    if (multiplyRgbByDstAlpha) {
        colorBlendAttachment.blendEnable = VK_TRUE;
        colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
        colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }

    VkPipelineColorBlendStateCreateInfo colorBlending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<std::uint32_t>(std::size(dynamicStates));
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = app.pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    CheckVk(vkCreateGraphicsPipelines(app.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline), "Could not create Vulkan graphics pipeline.");

    vkDestroyShaderModule(app.device, fragModule, nullptr);
    vkDestroyShaderModule(app.device, vertModule, nullptr);
    return pipeline;
}

void CreateGraphicsPipelines(AppState& app) {
    const std::wstring shaderDir = ExeDirectory() + L"\\shaders\\";
    const auto vertCode = ReadBinaryFile(shaderDir + L"liquid.vert.spv");
    const auto fragCode = ReadBinaryFile(shaderDir + L"liquid.frag.spv");

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &app.descriptorSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    CheckVk(vkCreatePipelineLayout(app.device, &layoutInfo, nullptr, &app.pipelineLayout), "Could not create Vulkan pipeline layout.");

    app.clearPipeline = CreatePipelineForRenderPass(app, app.clearRenderPass, vertCode, fragCode);
    app.loadPipeline = CreatePipelineForRenderPass(app, app.loadRenderPass, vertCode, fragCode);
}

void CreateFramebuffers(AppState& app) {
    app.clearFramebuffers.resize(app.swapchainImageViews.size());
    app.loadFramebuffers.resize(app.swapchainImageViews.size());
    for (size_t i = 0; i < app.swapchainImageViews.size(); ++i) {
        VkImageView attachments[] = {app.swapchainImageViews[i]};
        VkFramebufferCreateInfo framebufferInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = attachments;
        framebufferInfo.width = app.swapchainExtent.width;
        framebufferInfo.height = app.swapchainExtent.height;
        framebufferInfo.layers = 1;
        framebufferInfo.renderPass = app.clearRenderPass;
        CheckVk(vkCreateFramebuffer(app.device, &framebufferInfo, nullptr, &app.clearFramebuffers[i]), "Could not create Vulkan framebuffer.");
        framebufferInfo.renderPass = app.loadRenderPass;
        CheckVk(vkCreateFramebuffer(app.device, &framebufferInfo, nullptr, &app.loadFramebuffers[i]), "Could not create Vulkan framebuffer.");
    }
}

void CreateCommandPool(AppState& app) {
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = *app.queueFamilies.graphicsFamily;
    CheckVk(vkCreateCommandPool(app.device, &poolInfo, nullptr, &app.commandPool), "Could not create Vulkan command pool.");
}

void CreateDescriptorPoolAndSet(AppState& app) {
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;

    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    CheckVk(vkCreateDescriptorPool(app.device, &poolInfo, nullptr, &app.descriptorPool), "Could not create Vulkan descriptor pool.");

    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = app.descriptorPool;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &app.descriptorSetLayout;
    CheckVk(vkAllocateDescriptorSets(app.device, &allocateInfo, &app.descriptorSet), "Could not allocate Vulkan descriptor set.");

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = app.backgroundImageView;
    imageInfo.sampler = app.backgroundSampler;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = app.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(app.device, 1, &write, 0, nullptr);
}

void CreateCommandBuffers(AppState& app) {
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = app.commandPool;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<std::uint32_t>(app.commandBuffers.size());
    CheckVk(vkAllocateCommandBuffers(app.device, &allocateInfo, app.commandBuffers.data()), "Could not allocate Vulkan command buffers.");
}

void CreateSyncObjects(AppState& app) {
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        CheckVk(vkCreateSemaphore(app.device, &semaphoreInfo, nullptr, &app.imageAvailableSemaphores[i]), "Could not create Vulkan semaphore.");
        CheckVk(vkCreateSemaphore(app.device, &semaphoreInfo, nullptr, &app.renderFinishedSemaphores[i]), "Could not create Vulkan semaphore.");
        CheckVk(vkCreateFence(app.device, &fenceInfo, nullptr, &app.inFlightFences[i]), "Could not create Vulkan fence.");
    }
}

void CleanupSwapchain(AppState& app) {
    for (VkFramebuffer framebuffer : app.clearFramebuffers) {
        vkDestroyFramebuffer(app.device, framebuffer, nullptr);
    }
    app.clearFramebuffers.clear();
    for (VkFramebuffer framebuffer : app.loadFramebuffers) {
        vkDestroyFramebuffer(app.device, framebuffer, nullptr);
    }
    app.loadFramebuffers.clear();

    if (app.clearPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(app.device, app.clearPipeline, nullptr);
        app.clearPipeline = VK_NULL_HANDLE;
    }
    if (app.loadPipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(app.device, app.loadPipeline, nullptr);
        app.loadPipeline = VK_NULL_HANDLE;
    }
    if (app.pipelineLayout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(app.device, app.pipelineLayout, nullptr);
        app.pipelineLayout = VK_NULL_HANDLE;
    }
    if (app.clearRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(app.device, app.clearRenderPass, nullptr);
        app.clearRenderPass = VK_NULL_HANDLE;
    }
    if (app.loadRenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(app.device, app.loadRenderPass, nullptr);
        app.loadRenderPass = VK_NULL_HANDLE;
    }
    for (VkImageView imageView : app.swapchainImageViews) {
        vkDestroyImageView(app.device, imageView, nullptr);
    }
    app.swapchainImageViews.clear();
    if (app.swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(app.device, app.swapchain, nullptr);
        app.swapchain = VK_NULL_HANDLE;
    }
}

void CreateSwapchainObjects(AppState& app) {
    CreateSwapchain(app);
    CreateSwapchainImageViews(app);
    CreateRenderPasses(app);
    CreateGraphicsPipelines(app);
    CreateFramebuffers(app);
}

void RecreateSwapchain(AppState& app) {
    if (app.minimized || !RefreshClientSize(app)) {
        return;
    }

    const SwapchainSupport support = QuerySwapchainSupport(app.physicalDevice, app.surface);
    if (support.formats.empty() || support.presentModes.empty() || !SurfaceHasDrawableExtent(support.capabilities, app)) {
        app.minimized = true;
        app.framebufferResized = false;
        return;
    }

    CheckVk(vkDeviceWaitIdle(app.device), "Could not wait for Vulkan device before swapchain recreation.");
    CleanupSwapchain(app);
    CreateSwapchainObjects(app);
    app.framebufferResized = false;
}

VkRect2D ClampRectToSwapchain(const AppState& app, VkRect2D rect) {
    const int width = static_cast<int>(app.swapchainExtent.width);
    const int height = static_cast<int>(app.swapchainExtent.height);
    const int left = std::clamp(rect.offset.x, 0, width);
    const int top = std::clamp(rect.offset.y, 0, height);
    const int right = std::clamp(rect.offset.x + static_cast<int>(rect.extent.width), left, width);
    const int bottom = std::clamp(rect.offset.y + static_cast<int>(rect.extent.height), top, height);
    return {{left, top}, {static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top)}};
}

void RecordCommandBuffer(AppState& app, VkCommandBuffer commandBuffer, std::uint32_t imageIndex, VkRect2D renderRect, bool fullRedraw) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Could not begin Vulkan command buffer.");

    VkClearValue clearColor{};
    if (app.desktopMode) {
        clearColor.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    } else {
        clearColor.color = {{0.02f, 0.04f, 0.05f, 1.0f}};
    }

    VkRenderPassBeginInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderPassInfo.renderPass = fullRedraw ? app.clearRenderPass : app.loadRenderPass;
    renderPassInfo.framebuffer = fullRedraw ? app.clearFramebuffers[imageIndex] : app.loadFramebuffers[imageIndex];
    renderPassInfo.renderArea = renderRect;
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clearColor;

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport viewport{};
    viewport.width = static_cast<float>(app.swapchainExtent.width);
    viewport.height = static_cast<float>(app.swapchainExtent.height);
    viewport.maxDepth = 1.0f;
    VkRect2D scissor{};
    scissor = renderRect;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    const auto now = std::chrono::steady_clock::now();
    const float seconds = std::chrono::duration<float>(now - app.startedAt).count();
    PushConstants constants{};
    constants.resolution[0] = static_cast<float>(app.swapchainExtent.width);
    constants.resolution[1] = static_cast<float>(app.swapchainExtent.height);
    constants.time = seconds;
    constants.dragging = app.dragging ? 1.0f : 0.0f;
    constants.pillCenter[0] = app.visualPillX;
    constants.pillCenter[1] = app.visualPillY;
    constants.pillSize[0] = app.visualPillW;
    constants.pillSize[1] = app.visualPillH;
    constants.gooAmount = app.sideGooAmount;
    constants.edgeDockSide = static_cast<float>(app.sideGooSide);
    constants.desktopMode = app.desktopMode ? 1.0f : 0.0f;
    const RECT button = ToggleButtonBounds(app);
    constants.buttonCenterX = (static_cast<float>(button.left) + static_cast<float>(button.right)) * 0.5f;
    constants.buttonCenterY = (static_cast<float>(button.top) + static_cast<float>(button.bottom)) * 0.5f;
    constants.buttonWidth = static_cast<float>(button.right - button.left);
    constants.buttonHeight = static_cast<float>(button.bottom - button.top);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, fullRedraw ? app.clearPipeline : app.loadPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, app.pipelineLayout, 0, 1, &app.descriptorSet, 0, nullptr);
    vkCmdPushConstants(commandBuffer, app.pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &constants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);

    CheckVk(vkEndCommandBuffer(commandBuffer), "Could not end Vulkan command buffer.");
}

void DrawFrame(AppState& app) {
    if (app.minimized || !app.needsRedraw) {
        return;
    }

    CheckVk(vkWaitForFences(app.device, 1, &app.inFlightFences[app.currentFrame], VK_TRUE, UINT64_MAX), "Could not wait for Vulkan frame fence.");

    std::uint32_t imageIndex = 0;
    VkResult acquireResult = vkAcquireNextImageKHR(app.device, app.swapchain, UINT64_MAX, app.imageAvailableSemaphores[app.currentFrame], VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR) {
        RecreateSwapchain(app);
        return;
    }
    if (acquireResult != VK_SUCCESS && acquireResult != VK_SUBOPTIMAL_KHR) {
        throw std::runtime_error("Could not acquire Vulkan swapchain image.");
    }

    CheckVk(vkResetFences(app.device, 1, &app.inFlightFences[app.currentFrame]), "Could not reset Vulkan frame fence.");
    CheckVk(vkResetCommandBuffer(app.commandBuffers[app.currentFrame], 0), "Could not reset Vulkan command buffer.");

    const bool fullRedraw = app.forceFullRedraw || !app.imageInitialized[imageIndex];
    VkRect2D renderRect = fullRedraw ? VkRect2D{{0, 0}, app.swapchainExtent}
                                     : UnionRect(app.dirtyRect, app.imagePillRects[imageIndex]);
    renderRect = ClampRectToSwapchain(app, renderRect);
    if (renderRect.extent.width == 0 || renderRect.extent.height == 0) {
        renderRect = {{0, 0}, app.swapchainExtent};
    }

    RecordCommandBuffer(app, app.commandBuffers[app.currentFrame], imageIndex, renderRect, fullRedraw);

    const VkSemaphore waitSemaphores[] = {app.imageAvailableSemaphores[app.currentFrame]};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {app.renderFinishedSemaphores[app.currentFrame]};

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &app.commandBuffers[app.currentFrame];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    CheckVk(vkQueueSubmit(app.graphicsQueue, 1, &submitInfo, app.inFlightFences[app.currentFrame]), "Could not submit Vulkan draw command.");

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &app.swapchain;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult = vkQueuePresentKHR(app.presentQueue, &presentInfo);

    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR || app.framebufferResized) {
        RecreateSwapchain(app);
        return;
    } else if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Could not present Vulkan swapchain image.");
    }

    app.imageInitialized[imageIndex] = true;
    app.imagePillRects[imageIndex] = PillRect(app);
    app.needsRedraw = false;
    app.forceFullRedraw = false;
    app.currentFrame = (app.currentFrame + 1) % kMaxFramesInFlight;
}

void InitVulkan(AppState& app, HINSTANCE instanceHandle) {
    CreateInstance(app, instanceHandle);
    PickPhysicalDevice(app);
    CreateLogicalDevice(app);
    CreateSwapchain(app);
    CreateSwapchainImageViews(app);
    CreateDescriptorSetLayout(app);
    CreateRenderPasses(app);
    CreateGraphicsPipelines(app);
    CreateFramebuffers(app);
    CreateCommandPool(app);
    CreateBackgroundTexture(app);
    CreateDescriptorPoolAndSet(app);
    CreateCommandBuffers(app);
    CreateSyncObjects(app);
}

void Cleanup(AppState& app) {
    UnregisterGlobalHotkeys(app);
    RemoveTrayIcon(app);
    if (app.desktopOverlay) {
        app.desktopOverlay->Stop();
        delete app.desktopOverlay;
        app.desktopOverlay = nullptr;
    }
    if (app.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(app.device);
        CleanupSwapchain(app);
        for (int i = 0; i < kMaxFramesInFlight; ++i) {
            if (app.imageAvailableSemaphores[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(app.device, app.imageAvailableSemaphores[i], nullptr);
            }
            if (app.renderFinishedSemaphores[i] != VK_NULL_HANDLE) {
                vkDestroySemaphore(app.device, app.renderFinishedSemaphores[i], nullptr);
            }
            if (app.inFlightFences[i] != VK_NULL_HANDLE) {
                vkDestroyFence(app.device, app.inFlightFences[i], nullptr);
            }
        }
        if (app.descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(app.device, app.descriptorPool, nullptr);
        }
        if (app.backgroundSampler != VK_NULL_HANDLE) {
            vkDestroySampler(app.device, app.backgroundSampler, nullptr);
        }
        if (app.backgroundImageView != VK_NULL_HANDLE) {
            vkDestroyImageView(app.device, app.backgroundImageView, nullptr);
        }
        if (app.backgroundImage != VK_NULL_HANDLE) {
            vkDestroyImage(app.device, app.backgroundImage, nullptr);
        }
        if (app.backgroundMemory != VK_NULL_HANDLE) {
            vkFreeMemory(app.device, app.backgroundMemory, nullptr);
        }
        if (app.commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(app.device, app.commandPool, nullptr);
        }
        if (app.descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(app.device, app.descriptorSetLayout, nullptr);
        }
        vkDestroyDevice(app.device, nullptr);
    }
    if (app.surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(app.instance, app.surface, nullptr);
    }
    if (app.instance != VK_NULL_HANDLE) {
        vkDestroyInstance(app.instance, nullptr);
    }
}

POINT PillScreenCenter(const AppState& app) {
    if (app.desktopMode && app.desktopOverlay) {
        return app.desktopOverlay->CenterScreenPoint();
    }

    POINT center{
        static_cast<LONG>(std::lround(app.pillX)),
        static_cast<LONG>(std::lround(app.pillY)),
    };
    ClientToScreen(app.window, &center);
    return center;
}

void AddTrayIcon(AppState& app) {
    if (app.trayIconAdded || !app.window) {
        return;
    }

    NOTIFYICONDATAA data{};
    data.cbSize = sizeof(data);
    data.hWnd = app.window;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    lstrcpynA(data.szTip, "Liquid Glass Pill is running in desktop mode", static_cast<int>(sizeof(data.szTip)));
    if (Shell_NotifyIconA(NIM_ADD, &data)) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconA(NIM_SETVERSION, &data);
        app.trayIconAdded = true;
    }
}

void RemoveTrayIcon(AppState& app) {
    if (!app.trayIconAdded || !app.window) {
        return;
    }

    NOTIFYICONDATAA data{};
    data.cbSize = sizeof(data);
    data.hWnd = app.window;
    data.uID = kTrayIconId;
    Shell_NotifyIconA(NIM_DELETE, &data);
    app.trayIconAdded = false;
}

void RegisterGlobalHotkeys(AppState& app) {
    if (!app.window || app.desktopHotkeyRegistered) {
        return;
    }
    app.desktopHotkeyRegistered = RegisterHotKey(app.window, kDesktopHotkeyId, kDesktopHotkeyModifiers, kDesktopHotkeyKey) != FALSE;
}

void UnregisterGlobalHotkeys(AppState& app) {
    if (!app.desktopHotkeyRegistered || !app.window) {
        return;
    }
    UnregisterHotKey(app.window, kDesktopHotkeyId);
    app.desktopHotkeyRegistered = false;
}

void ShowTrayMenu(AppState& app) {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuA(menu, MF_STRING, kTrayShowCommand, "Show app");
    AppendMenuA(menu, MF_STRING, kTrayExitCommand, "Exit");
    SetForegroundWindow(app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, cursor.x, cursor.y, 0, app.window, nullptr);
    DestroyMenu(menu);
}

void SetDesktopMode(AppState& app, bool enabled) {
    if (!app.window || app.desktopMode == enabled) {
        return;
    }

    if (app.dragging) {
        app.dragging = false;
        if (GetCapture() == app.window) {
            ReleaseCapture();
        }
    }

    POINT screenCenter = PillScreenCenter(app);

    if (enabled) {
        app.normalPlacement = {};
        app.normalPlacement.length = sizeof(WINDOWPLACEMENT);
        app.normalPlacementValid = GetWindowPlacement(app.window, &app.normalPlacement) != FALSE;
        GetWindowRect(app.window, &app.normalWindowRect);

        DesktopOverlayVulkanContext overlayContext{};
        overlayContext.instanceHandle = app.instanceHandle;
        overlayContext.instance = app.instance;
        overlayContext.physicalDevice = app.physicalDevice;
        overlayContext.device = app.device;
        overlayContext.graphicsQueue = app.graphicsQueue;
        overlayContext.presentQueue = app.presentQueue;
        overlayContext.graphicsFamily = *app.queueFamilies.graphicsFamily;
        overlayContext.presentFamily = *app.queueFamilies.presentFamily;
        overlayContext.externalMemoryWin32Enabled = app.externalMemoryWin32Enabled;
        overlayContext.keyedMutexEnabled = app.keyedMutexEnabled;
        overlayContext.shaderDirectory = ExeDirectory() + L"\\shaders";

        ShowWindow(app.window, SW_HIDE);
        FlushDesktopComposition();

        DesktopOverlay* overlay = new DesktopOverlay();
        if (!overlay->Start(overlayContext, screenCenter)) {
            const std::string error = overlay->lastError().empty()
                                          ? "The standalone desktop overlay could not start."
                                          : overlay->lastError();
            delete overlay;
            ShowWindow(app.window, SW_SHOW);
            FlushDesktopComposition();
            ShowError("Desktop overlay did not start:\n" + error);
            return;
        }
        app.desktopOverlay = overlay;
        app.desktopMode = true;
        AddTrayIcon(app);
    } else {
        if (app.desktopOverlay) {
            app.desktopOverlay->Stop();
            delete app.desktopOverlay;
            app.desktopOverlay = nullptr;
        }
        RemoveTrayIcon(app);
        app.desktopMode = false;
        SetWindowPos(
            app.window,
            HWND_NOTOPMOST,
            app.normalWindowRect.left,
            app.normalWindowRect.top,
            app.normalWindowRect.right - app.normalWindowRect.left,
            app.normalWindowRect.bottom - app.normalWindowRect.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        ShowWindow(app.window, SW_SHOW);
        if (app.normalPlacementValid) {
            app.normalPlacement.length = sizeof(WINDOWPLACEMENT);
            SetWindowPlacement(app.window, &app.normalPlacement);
        }

        RECT client{};
        GetClientRect(app.window, &client);
        ResizePill(app, client.right - client.left, client.bottom - client.top);

        POINT clientCenter = screenCenter;
        ScreenToClient(app.window, &clientCenter);
        app.pillX = static_cast<float>(clientCenter.x);
        app.pillY = static_cast<float>(clientCenter.y);
        ClampPill(app);
        SyncPillVisualToTarget(app);
    }

    SetWindowTextA(app.window, app.desktopMode ? "Liquid Glass Pill - Desktop Mode" : "Liquid Glass Pill - C++ Vulkan");
    InvalidateFull(app);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    AppState* app = g_app;
    switch (message) {
    case kTrayCallbackMessage:
        if (app) {
            if (LOWORD(lParam) == WM_CONTEXTMENU || LOWORD(lParam) == WM_RBUTTONUP) {
                ShowTrayMenu(*app);
                return 0;
            }
            if (LOWORD(lParam) == NIN_SELECT || LOWORD(lParam) == WM_LBUTTONDBLCLK) {
                SetDesktopMode(*app, false);
                return 0;
            }
        }
        return 0;
    case WM_HOTKEY:
        if (app && wParam == kDesktopHotkeyId) {
            SetDesktopMode(*app, !app->desktopMode);
            return 0;
        }
        break;
    case WM_COMMAND:
        if (app) {
            switch (LOWORD(wParam)) {
            case kTrayShowCommand:
                SetDesktopMode(*app, false);
                return 0;
            case kTrayExitCommand:
                app->running = false;
                RemoveTrayIcon(*app);
                if (app->desktopOverlay) {
                    app->desktopOverlay->Stop();
                    delete app->desktopOverlay;
                    app->desktopOverlay = nullptr;
                }
                DestroyWindow(window);
                return 0;
            default:
                break;
            }
        }
        break;
    case WM_ERASEBKGND:
        if (app && !app->vulkanReady) {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wParam), &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        }
        return 1;
    case WM_PAINT:
        if (app) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            if (!app->vulkanReady) {
                FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            }
            EndPaint(window, &paint);
            if (app->vulkanReady) {
                InvalidateFull(*app);
            }
        }
        return 0;
    case WM_SIZE:
        if (app && !app->desktopMode) {
            ResizePill(*app, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    case WM_DISPLAYCHANGE:
        if (app && app->desktopMode && app->desktopOverlay) {
            app->desktopOverlay->OnDisplayChanged();
        }
        return 0;
    case WM_SETCURSOR:
        if (app && LOWORD(lParam) == HTCLIENT) {
            POINT point;
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            if (app->dragging ||
                HitPill(*app, static_cast<float>(point.x), static_cast<float>(point.y)) ||
                HitToggleButton(*app, static_cast<float>(point.x), static_cast<float>(point.y))) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
        }
        break;
    case WM_KEYDOWN:
        if (app && app->desktopMode && wParam == VK_ESCAPE) {
            SetDesktopMode(*app, false);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
        if (app) {
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            if (HitToggleButton(*app, x, y)) {
                SetDesktopMode(*app, !app->desktopMode);
                return 0;
            }
            if (HitPill(*app, x, y)) {
                const VkRect2D previous = PillRect(*app);
                app->dragging = true;
                app->dragOffsetX = x - app->pillX;
                app->dragOffsetY = y - app->pillY;
                SetCapture(window);
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                InvalidatePill(*app, previous);
            }
        }
        return 0;
    case WM_MOUSEMOVE:
        if (app && app->dragging) {
            const VkRect2D previous = PillRect(*app);
            const float x = static_cast<float>(GET_X_LPARAM(lParam));
            const float y = static_cast<float>(GET_Y_LPARAM(lParam));
            UpdateDraggedPill(*app, x, y);
            InvalidatePill(*app, previous);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        if (app && app->dragging) {
            const VkRect2D previous = PillRect(*app);
            app->dragging = false;
            ReleaseCapture();
            InvalidatePill(*app, previous);
        }
        return 0;
    case WM_CLOSE:
        if (app) {
            app->running = false;
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

void CreateWindowForApp(AppState& app, HINSTANCE instance, int showCommand) {
    WNDCLASSA wc{};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = "LiquidGlassPillVulkanWindow";
    if (!RegisterClassA(&wc)) {
        throw std::runtime_error("Could not register Win32 window class.");
    }

    RECT rect = {0, 0, app.width, app.height};
    AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    app.window = CreateWindowExA(
        0,
        wc.lpszClassName,
        "Liquid Glass Pill - C++ Vulkan",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        nullptr,
        nullptr,
        instance,
        nullptr);
    if (!app.window) {
        throw std::runtime_error("Could not create Win32 window.");
    }

    RECT client{};
    GetClientRect(app.window, &client);
    ResizePill(app, client.right - client.left, client.bottom - client.top);
    app.pillX = app.width * 0.5f;
    app.pillY = app.height * 0.52f;
    ClampPill(app);

    ShowWindow(app.window, showCommand);
    UpdateWindow(app.window);
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR commandLine, int showCommand) {
    SetProcessDPIAware();

    const bool startInDesktopMode = ShouldStartInDesktopMode(commandLine);

    AppState app;
    g_app = &app;
    app.instanceHandle = instance;
    app.startedAt = std::chrono::steady_clock::now();

    try {
        CreateWindowForApp(app, instance, showCommand);
        RegisterGlobalHotkeys(app);
        InitVulkan(app, instance);
        app.vulkanReady = true;
        if (startInDesktopMode) {
            SetDesktopMode(app, true);
        } else {
            InvalidateFull(app);
        }

        MSG message{};
        while (app.running) {
            while (PeekMessageA(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    app.running = false;
                    break;
                }
                TranslateMessage(&message);
                DispatchMessageA(&message);
            }

            if (app.desktopMode) {
                if (!app.desktopOverlay || app.desktopOverlay->restoreRequested() || !app.desktopOverlay->Tick()) {
                    SetDesktopMode(app, false);
                    continue;
                }
                Sleep(8);
                continue;
            }

            if (app.minimized) {
                WaitMessage();
                continue;
            }
            UpdatePillTransition(app);
            if (app.needsRedraw) {
                DrawFrame(app);
            } else {
                WaitMessage();
            }
        }
    } catch (const std::exception& error) {
        ShowError(error.what());
        Cleanup(app);
        g_app = nullptr;
        return 1;
    }

    Cleanup(app);
    g_app = nullptr;
    return 0;
}
