#include "desktop_overlay.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kMaxFramesInFlight = 2;
constexpr float kDesktopPillScale = 0.76f;
constexpr uint64_t kD3DWriteCompleteKey = 1;
constexpr uint64_t kVulkanReadCompleteKey = 0;
constexpr uint32_t kKeyedMutexTimeoutMs = 100;
constexpr const char* kOverlayWindowClass = "LiquidGlassCleanDesktopOverlay";

void CheckHr(HRESULT result, const char* message) {
    if (FAILED(result)) {
        throw std::runtime_error(message);
    }
}

void CheckVk(VkResult result, const char* message) {
    if (result != VK_SUCCESS) {
        throw std::runtime_error(message);
    }
}

std::vector<char> ReadBinaryFile(const std::wstring& path) {
    std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Could not open a required desktop overlay shader.");
    }

    const std::streamsize size = file.tellg();
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0);
    file.read(bytes.data(), size);
    return bytes;
}

template <typename T>
class DxPtr {
public:
    DxPtr() = default;
    ~DxPtr() { Reset(); }

    DxPtr(const DxPtr&) = delete;
    DxPtr& operator=(const DxPtr&) = delete;

    DxPtr(DxPtr&& other) noexcept : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    DxPtr& operator=(DxPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }
        return *this;
    }

    T* Get() const { return ptr_; }
    T** ReleaseAndGetAddressOf() {
        Reset();
        return &ptr_;
    }

    void Reset(T* value = nullptr) {
        if (ptr_) {
            ptr_->Release();
        }
        ptr_ = value;
    }

    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

bool SameLuid(const LUID& a, const LUID& b) {
    return a.HighPart == b.HighPart && a.LowPart == b.LowPart;
}

bool TryGetVulkanDeviceLuid(const DesktopOverlayVulkanContext& context, LUID& luid) {
    if (context.physicalDevice == VK_NULL_HANDLE) {
        return false;
    }

    VkPhysicalDeviceIDProperties idProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    properties.pNext = &idProperties;
    vkGetPhysicalDeviceProperties2(context.physicalDevice, &properties);
    if (!idProperties.deviceLUIDValid) {
        return false;
    }

    static_assert(sizeof(luid) == VK_LUID_SIZE, "Windows LUID size must match Vulkan LUID size.");
    std::memcpy(&luid, idProperties.deviceLUID, sizeof(luid));
    return true;
}

uint32_t FindMemoryType(
    VkPhysicalDevice physicalDevice,
    uint32_t typeBits,
    VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memoryProperties);
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Could not find a compatible Vulkan memory type for the desktop texture.");
}

VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(const VkSurfaceCapabilitiesKHR& capabilities) {
    const VkCompositeAlphaFlagBitsKHR preferred[] = {
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
    };
    for (VkCompositeAlphaFlagBitsKHR mode : preferred) {
        if ((capabilities.supportedCompositeAlpha & mode) != 0) {
            return mode;
        }
    }
    throw std::runtime_error("Desktop overlay surface does not expose a supported alpha mode.");
}

VkSurfaceFormatKHR ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (const auto& format : formats) {
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM &&
            format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return format;
        }
    }
    return formats.front();
}

VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes) {
    for (VkPresentModeKHR mode : modes) {
        if (mode == VK_PRESENT_MODE_FIFO_KHR) {
            return mode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR& capabilities, VkExtent2D requested) {
    if (capabilities.currentExtent.width != UINT32_MAX) {
        return capabilities.currentExtent;
    }

    requested.width = std::clamp(requested.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    requested.height = std::clamp(requested.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    return requested;
}

} // namespace

class DesktopOverlay::OverlayWindow {
public:
    bool Create(DesktopOverlay& owner, HINSTANCE instance, const RECT& screen);
    void Destroy();
    void Move(const RECT& screen);
    void Show(const RECT& screen);

    HWND hwnd() const { return hwnd_; }

private:
    static void Register(HINSTANCE instance);
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HWND hwnd_ = nullptr;
};

class DesktopOverlay::BackgroundProvider {
public:
    ~BackgroundProvider();

    bool Start(const DesktopOverlayVulkanContext& context, const RECT& screen);
    void Stop();
    bool CaptureFrame(bool waitForFrame);

    HANDLE sharedHandle() const { return sharedHandle_; }
    VkExtent2D extent() const { return {width_, height_}; }
    const std::string& lastError() const { return lastError_; }

private:
    struct CapturedOutput {
        DxPtr<IDXGIOutputDuplication> duplication;
        RECT desktopRect{};
        uint32_t width = 0;
        uint32_t height = 0;
    };

    DxPtr<IDXGIAdapter1> SelectAdapter(const DesktopOverlayVulkanContext& context);
    void CreateDevice(const DesktopOverlayVulkanContext& context);
    bool CreateOutputs();
    void CreateSharedDesktopTexture(const RECT& screen);

    DxPtr<IDXGIAdapter1> adapter_;
    DxPtr<ID3D11Device> device_;
    DxPtr<ID3D11DeviceContext> deviceContext_;
    std::vector<CapturedOutput> outputs_;
    DxPtr<ID3D11Texture2D> desktopTexture_;
    DxPtr<IDXGIKeyedMutex> keyedMutex_;
    HANDLE sharedHandle_ = nullptr;
    RECT screen_{};
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    std::string lastError_;
};

class DesktopOverlay::LiquidGlassRenderer {
public:
    void Create(
        const DesktopOverlayVulkanContext& context,
        VkFormat swapchainFormat,
        VkImageView backgroundView,
        VkSampler backgroundSampler);
    void Destroy(VkDevice device);
    void Record(
        VkCommandBuffer commandBuffer,
        const OverlayPillController& pill,
        VkExtent2D extent,
        float seconds) const;

    VkRenderPass renderPass() const { return renderPass_; }

private:
    VkShaderModule CreateShaderModule(VkDevice device, const std::vector<char>& bytes);
    void CreateDescriptorObjects(VkDevice device, VkImageView backgroundView, VkSampler backgroundSampler);
    void CreateRenderPass(VkDevice device, VkFormat swapchainFormat);
    void CreatePipeline(const DesktopOverlayVulkanContext& context);

    VkDescriptorSetLayout descriptorSetLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptorSet_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    std::wstring shaderDirectory_;
};

class DesktopOverlay::VulkanRenderer {
public:
    ~VulkanRenderer();

    bool Start(
        const DesktopOverlayVulkanContext& context,
        HWND window,
        HANDLE sharedBackgroundHandle,
        VkExtent2D backgroundExtent,
        VkExtent2D overlayExtent);
    void Stop();
    bool Render(const OverlayPillController& pill, float seconds);

    const std::string& lastError() const { return lastError_; }

private:
    struct ImportedBackground {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkExtent2D extent{};
        bool layoutReady = false;
    };

    void CreateSurface(HWND window);
    void CreateSwapchain(VkExtent2D requestedExtent);
    void ImportBackground(HANDLE sharedBackgroundHandle, VkExtent2D extent);
    void CreateFramebuffers();
    void CreateCommandPool();
    void CreateCommandBuffers();
    void CreateSyncObjects();
    void RecordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex, const OverlayPillController& pill, float seconds);
    VkImageView CreateImageView(VkImage image, VkFormat format);
    void DestroyBackground();
    void DestroySwapchain();

    DesktopOverlayVulkanContext context_{};
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent_{};
    std::vector<VkImage> swapchainImages_;
    std::vector<VkImageView> swapchainImageViews_;
    std::vector<VkFramebuffer> framebuffers_;
    VkCommandPool commandPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, kMaxFramesInFlight> commandBuffers_{};
    std::array<VkSemaphore, kMaxFramesInFlight> imageAvailable_{};
    std::array<VkSemaphore, kMaxFramesInFlight> renderFinished_{};
    std::array<VkFence, kMaxFramesInFlight> inFlight_{};
    uint32_t currentFrame_ = 0;
    ImportedBackground background_;
    LiquidGlassRenderer liquidGlass_;
    std::string lastError_;
};

bool DesktopOverlay::OverlayWindow::Create(DesktopOverlay& owner, HINSTANCE instance, const RECT& screen) {
    Register(instance);
    const int width = std::max<LONG>(screen.right - screen.left, 1);
    const int height = std::max<LONG>(screen.bottom - screen.top, 1);
    hwnd_ = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kOverlayWindowClass,
        "Liquid Glass Floating Pill",
        WS_POPUP,
        screen.left,
        screen.top,
        width,
        height,
        nullptr,
        nullptr,
        instance,
        &owner);
    if (!hwnd_) {
        return false;
    }

    SetWindowDisplayAffinity(hwnd_, WDA_EXCLUDEFROMCAPTURE);
    SetWindowPos(hwnd_, HWND_TOPMOST, screen.left, screen.top, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    return true;
}

void DesktopOverlay::OverlayWindow::Destroy() {
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

void DesktopOverlay::OverlayWindow::Move(const RECT& screen) {
    if (!hwnd_) {
        return;
    }
    const int width = std::max<LONG>(screen.right - screen.left, 1);
    const int height = std::max<LONG>(screen.bottom - screen.top, 1);
    SetWindowPos(hwnd_, HWND_TOPMOST, screen.left, screen.top, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | SWP_NOREDRAW);
}

void DesktopOverlay::OverlayWindow::Show(const RECT& screen) {
    if (!hwnd_) {
        return;
    }
    const int width = std::max<LONG>(screen.right - screen.left, 1);
    const int height = std::max<LONG>(screen.bottom - screen.top, 1);
    SetWindowPos(hwnd_, HWND_TOPMOST, screen.left, screen.top, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW | SWP_NOREDRAW);
}

void DesktopOverlay::OverlayWindow::Register(HINSTANCE instance) {
    static bool registered = false;
    if (registered) {
        return;
    }

    WNDCLASSA wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayWindowClass;
    if (!RegisterClassA(&wc)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Could not register the desktop overlay window class.");
        }
    }
    registered = true;
}

LRESULT CALLBACK DesktopOverlay::OverlayWindow::Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    DesktopOverlay* overlay = reinterpret_cast<DesktopOverlay*>(GetWindowLongPtrA(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTA*>(lParam);
        overlay = reinterpret_cast<DesktopOverlay*>(create->lpCreateParams);
        SetWindowLongPtrA(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }

    if (overlay) {
        return overlay->HandleWindowMessage(window, message, wParam, lParam);
    }
    return DefWindowProcA(window, message, wParam, lParam);
}

DesktopOverlay::BackgroundProvider::~BackgroundProvider() {
    Stop();
}

bool DesktopOverlay::BackgroundProvider::Start(const DesktopOverlayVulkanContext& context, const RECT& screen) {
    Stop();
    lastError_.clear();
    try {
        CreateDevice(context);
        if (!CreateOutputs()) {
            throw std::runtime_error("Could not duplicate any desktop output for the floating pill.");
        }
        CreateSharedDesktopTexture(screen);
        return CaptureFrame(true);
    } catch (const std::exception& error) {
        lastError_ = error.what();
        Stop();
        return false;
    } catch (...) {
        lastError_ = "Unknown desktop capture initialization failure.";
        Stop();
        return false;
    }
}

void DesktopOverlay::BackgroundProvider::Stop() {
    outputs_.clear();
    keyedMutex_.Reset();
    desktopTexture_.Reset();
    if (sharedHandle_) {
        CloseHandle(sharedHandle_);
        sharedHandle_ = nullptr;
    }
    deviceContext_.Reset();
    device_.Reset();
    adapter_.Reset();
    screen_ = {};
    width_ = 0;
    height_ = 0;
}

bool DesktopOverlay::BackgroundProvider::CaptureFrame(bool waitForFrame) {
    if (!deviceContext_ || !desktopTexture_ || !keyedMutex_) {
        return false;
    }

    if (FAILED(keyedMutex_->AcquireSync(kVulkanReadCompleteKey, kKeyedMutexTimeoutMs))) {
        return false;
    }

    bool accessLost = false;
    for (CapturedOutput& output : outputs_) {
        DXGI_OUTDUPL_FRAME_INFO frameInfo{};
        DxPtr<IDXGIResource> frameResource;
        const HRESULT acquireResult = output.duplication->AcquireNextFrame(waitForFrame ? 16 : 0, &frameInfo, frameResource.ReleaseAndGetAddressOf());
        if (acquireResult == DXGI_ERROR_WAIT_TIMEOUT) {
            continue;
        }
        if (acquireResult == DXGI_ERROR_ACCESS_LOST) {
            accessLost = true;
            break;
        }
        if (FAILED(acquireResult)) {
            continue;
        }

        DxPtr<ID3D11Texture2D> frameTexture;
        if (SUCCEEDED(frameResource->QueryInterface(IID_PPV_ARGS(frameTexture.ReleaseAndGetAddressOf())))) {
            D3D11_TEXTURE2D_DESC frameDesc{};
            frameTexture->GetDesc(&frameDesc);

            D3D11_BOX sourceBox{};
            sourceBox.right = std::min<UINT>(frameDesc.Width, output.width);
            sourceBox.bottom = std::min<UINT>(frameDesc.Height, output.height);
            sourceBox.back = 1;

            const UINT dstX = static_cast<UINT>(std::max<LONG>(output.desktopRect.left - screen_.left, 0));
            const UINT dstY = static_cast<UINT>(std::max<LONG>(output.desktopRect.top - screen_.top, 0));
            deviceContext_->CopySubresourceRegion(desktopTexture_.Get(), 0, dstX, dstY, 0, frameTexture.Get(), 0, &sourceBox);
        }
        output.duplication->ReleaseFrame();
    }

    deviceContext_->Flush();
    const HRESULT releaseResult = keyedMutex_->ReleaseSync(kD3DWriteCompleteKey);
    return !accessLost && SUCCEEDED(releaseResult);
}

DxPtr<IDXGIAdapter1> DesktopOverlay::BackgroundProvider::SelectAdapter(const DesktopOverlayVulkanContext& context) {
    DxPtr<IDXGIFactory1> factory;
    CheckHr(CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())), "Could not create a DXGI factory.");

    LUID vulkanLuid{};
    const bool haveVulkanLuid = TryGetVulkanDeviceLuid(context, vulkanLuid);
    DxPtr<IDXGIAdapter1> fallback;
    for (UINT i = 0;; ++i) {
        DxPtr<IDXGIAdapter1> adapter;
        const HRESULT enumResult = factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf());
        if (enumResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        CheckHr(enumResult, "Could not enumerate DXGI adapters.");

        DXGI_ADAPTER_DESC1 desc{};
        CheckHr(adapter->GetDesc1(&desc), "Could not read a DXGI adapter description.");
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
            continue;
        }
        if (haveVulkanLuid && SameLuid(desc.AdapterLuid, vulkanLuid)) {
            return adapter;
        }
        if (!fallback) {
            fallback = std::move(adapter);
        }
    }

    if (fallback) {
        return fallback;
    }
    throw std::runtime_error("Could not find a hardware DXGI adapter for desktop capture.");
}

void DesktopOverlay::BackgroundProvider::CreateDevice(const DesktopOverlayVulkanContext& context) {
    adapter_ = SelectAdapter(context);
    const D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL createdLevel{};
    CheckHr(
        D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            featureLevels,
            static_cast<UINT>(sizeof(featureLevels) / sizeof(featureLevels[0])),
            D3D11_SDK_VERSION,
            device_.ReleaseAndGetAddressOf(),
            &createdLevel,
            deviceContext_.ReleaseAndGetAddressOf()),
        "Could not create the D3D11 desktop capture device.");
}

bool DesktopOverlay::BackgroundProvider::CreateOutputs() {
    outputs_.clear();
    for (UINT i = 0;; ++i) {
        DxPtr<IDXGIOutput> output;
        const HRESULT enumResult = adapter_->EnumOutputs(i, output.ReleaseAndGetAddressOf());
        if (enumResult == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(enumResult)) {
            continue;
        }

        DXGI_OUTPUT_DESC desc{};
        if (FAILED(output->GetDesc(&desc))) {
            continue;
        }
        const LONG width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        const LONG height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
        if (width <= 0 || height <= 0) {
            continue;
        }

        DxPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.ReleaseAndGetAddressOf())))) {
            continue;
        }

        CapturedOutput captured;
        if (FAILED(output1->DuplicateOutput(device_.Get(), captured.duplication.ReleaseAndGetAddressOf()))) {
            continue;
        }
        captured.desktopRect = desc.DesktopCoordinates;
        captured.width = static_cast<uint32_t>(width);
        captured.height = static_cast<uint32_t>(height);
        outputs_.push_back(std::move(captured));
    }
    return !outputs_.empty();
}

void DesktopOverlay::BackgroundProvider::CreateSharedDesktopTexture(const RECT& screen) {
    screen_ = screen;
    width_ = static_cast<uint32_t>(std::max<LONG>(screen.right - screen.left, 1));
    height_ = static_cast<uint32_t>(std::max<LONG>(screen.bottom - screen.top, 1));

    D3D11_TEXTURE2D_DESC textureDesc{};
    textureDesc.Width = width_;
    textureDesc.Height = height_;
    textureDesc.MipLevels = 1;
    textureDesc.ArraySize = 1;
    textureDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Usage = D3D11_USAGE_DEFAULT;
    textureDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    textureDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    CheckHr(device_->CreateTexture2D(&textureDesc, nullptr, desktopTexture_.ReleaseAndGetAddressOf()), "Could not create the shared desktop texture.");
    CheckHr(desktopTexture_->QueryInterface(IID_PPV_ARGS(keyedMutex_.ReleaseAndGetAddressOf())), "Could not query the shared desktop texture mutex.");

    DxPtr<IDXGIResource1> resource;
    CheckHr(desktopTexture_->QueryInterface(IID_PPV_ARGS(resource.ReleaseAndGetAddressOf())), "Could not query the shared desktop texture resource.");
    CheckHr(
        resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle_),
        "Could not create a shared handle for the desktop texture.");
}

void DesktopOverlay::LiquidGlassRenderer::Create(
    const DesktopOverlayVulkanContext& context,
    VkFormat swapchainFormat,
    VkImageView backgroundView,
    VkSampler backgroundSampler) {
    shaderDirectory_ = context.shaderDirectory;
    CreateDescriptorObjects(context.device, backgroundView, backgroundSampler);
    CreateRenderPass(context.device, swapchainFormat);
    CreatePipeline(context);
}

void DesktopOverlay::LiquidGlassRenderer::Destroy(VkDevice device) {
    if (device == VK_NULL_HANDLE) {
        return;
    }
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
        descriptorSet_ = VK_NULL_HANDLE;
    }
    if (descriptorSetLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptorSetLayout_, nullptr);
        descriptorSetLayout_ = VK_NULL_HANDLE;
    }
}

void DesktopOverlay::LiquidGlassRenderer::Record(
    VkCommandBuffer commandBuffer,
    const OverlayPillController& pill,
    VkExtent2D extent,
    float seconds) const {
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{{0, 0}, extent};
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_, 0, 1, &descriptorSet_, 0, nullptr);

    LiquidGlassPushConstants constants{};
    pill.FillPushConstants(constants, seconds);
    vkCmdPushConstants(commandBuffer, pipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
}

VkShaderModule DesktopOverlay::LiquidGlassRenderer::CreateShaderModule(VkDevice device, const std::vector<char>& bytes) {
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.codeSize = bytes.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
    VkShaderModule module = VK_NULL_HANDLE;
    CheckVk(vkCreateShaderModule(device, &createInfo, nullptr, &module), "Could not create a desktop overlay shader module.");
    return module;
}

void DesktopOverlay::LiquidGlassRenderer::CreateDescriptorObjects(VkDevice device, VkImageView backgroundView, VkSampler backgroundSampler) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    CheckVk(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout_), "Could not create the desktop overlay descriptor layout.");

    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = 1;
    CheckVk(vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_), "Could not create the desktop overlay descriptor pool.");

    VkDescriptorSetAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocateInfo.descriptorPool = descriptorPool_;
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &descriptorSetLayout_;
    CheckVk(vkAllocateDescriptorSets(device, &allocateInfo, &descriptorSet_), "Could not allocate the desktop overlay descriptor set.");

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfo.imageView = backgroundView;
    imageInfo.sampler = backgroundSampler;

    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptorSet_;
    write.dstBinding = 0;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.descriptorCount = 1;
    write.pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

void DesktopOverlay::LiquidGlassRenderer::CreateRenderPass(VkDevice device, VkFormat swapchainFormat) {
    VkAttachmentDescription attachment{};
    attachment.format = swapchainFormat;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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
    renderPassInfo.pAttachments = &attachment;
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;
    CheckVk(vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass_), "Could not create the desktop overlay render pass.");
}

void DesktopOverlay::LiquidGlassRenderer::CreatePipeline(const DesktopOverlayVulkanContext& context) {
    const auto vertCode = ReadBinaryFile(shaderDirectory_ + L"\\liquid.vert.spv");
    const auto fragCode = ReadBinaryFile(shaderDirectory_ + L"\\overlay.frag.spv");
    VkShaderModule vertModule = CreateShaderModule(context.device, vertCode);
    VkShaderModule fragModule = CreateShaderModule(context.device, fragCode);

    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.size = sizeof(LiquidGlassPushConstants);

    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &descriptorSetLayout_;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    CheckVk(vkCreatePipelineLayout(context.device, &layoutInfo, nullptr, &pipelineLayout_), "Could not create the desktop overlay pipeline layout.");

    VkPipelineShaderStageCreateInfo vertexStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertModule;
    vertexStage.pName = "main";
    VkPipelineShaderStageCreateInfo fragmentStage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragModule;
    fragmentStage.pName = "main";
    const VkPipelineShaderStageCreateInfo stages[] = {vertexStage, fragmentStage};

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

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo blendState{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blendState.attachmentCount = 1;
    blendState.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamicState.dynamicStateCount = static_cast<uint32_t>(sizeof(dynamicStates) / sizeof(dynamicStates[0]));
    dynamicState.pDynamicStates = dynamicStates;

    VkGraphicsPipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &blendState;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout_;
    pipelineInfo.renderPass = renderPass_;
    CheckVk(vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_), "Could not create the desktop overlay graphics pipeline.");

    vkDestroyShaderModule(context.device, fragModule, nullptr);
    vkDestroyShaderModule(context.device, vertModule, nullptr);
}

DesktopOverlay::VulkanRenderer::~VulkanRenderer() {
    Stop();
}

bool DesktopOverlay::VulkanRenderer::Start(
    const DesktopOverlayVulkanContext& context,
    HWND window,
    HANDLE sharedBackgroundHandle,
    VkExtent2D backgroundExtent,
    VkExtent2D overlayExtent) {
    Stop();
    context_ = context;
    lastError_.clear();
    try {
        if (!context_.externalMemoryWin32Enabled || !context_.keyedMutexEnabled) {
            throw std::runtime_error("Floating pill GPU sharing requires Vulkan external memory and keyed mutex support.");
        }
        CreateCommandPool();
        CreateCommandBuffers();
        CreateSyncObjects();
        CreateSurface(window);
        CreateSwapchain(overlayExtent);
        ImportBackground(sharedBackgroundHandle, backgroundExtent);
        liquidGlass_.Create(context_, swapchainFormat_, background_.view, background_.sampler);
        CreateFramebuffers();
        return true;
    } catch (const std::exception& error) {
        lastError_ = error.what();
        Stop();
        return false;
    } catch (...) {
        lastError_ = "Unknown Vulkan desktop overlay initialization failure.";
        Stop();
        return false;
    }
}

void DesktopOverlay::VulkanRenderer::Stop() {
    if (context_.device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(context_.device);
        for (VkFence fence : inFlight_) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(context_.device, fence, nullptr);
            }
        }
        inFlight_.fill(VK_NULL_HANDLE);
        for (VkSemaphore semaphore : renderFinished_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(context_.device, semaphore, nullptr);
            }
        }
        renderFinished_.fill(VK_NULL_HANDLE);
        for (VkSemaphore semaphore : imageAvailable_) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(context_.device, semaphore, nullptr);
            }
        }
        imageAvailable_.fill(VK_NULL_HANDLE);
        DestroySwapchain();
        liquidGlass_.Destroy(context_.device);
        DestroyBackground();
        if (commandPool_ != VK_NULL_HANDLE) {
            vkDestroyCommandPool(context_.device, commandPool_, nullptr);
            commandPool_ = VK_NULL_HANDLE;
        }
        if (surface_ != VK_NULL_HANDLE) {
            vkDestroySurfaceKHR(context_.instance, surface_, nullptr);
            surface_ = VK_NULL_HANDLE;
        }
    }
    context_ = {};
    currentFrame_ = 0;
}

bool DesktopOverlay::VulkanRenderer::Render(const OverlayPillController& pill, float seconds) {
    if (swapchain_ == VK_NULL_HANDLE || framebuffers_.empty()) {
        return false;
    }

    CheckVk(vkWaitForFences(context_.device, 1, &inFlight_[currentFrame_], VK_TRUE, UINT64_MAX), "Could not wait for the desktop overlay frame fence.");

    uint32_t imageIndex = 0;
    const VkResult acquireResult = vkAcquireNextImageKHR(context_.device, swapchain_, UINT64_MAX, imageAvailable_[currentFrame_], VK_NULL_HANDLE, &imageIndex);
    if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR || acquireResult == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (acquireResult != VK_SUCCESS) {
        throw std::runtime_error("Could not acquire a desktop overlay swapchain image.");
    }

    CheckVk(vkResetFences(context_.device, 1, &inFlight_[currentFrame_]), "Could not reset the desktop overlay frame fence.");
    CheckVk(vkResetCommandBuffer(commandBuffers_[currentFrame_], 0), "Could not reset the desktop overlay command buffer.");
    RecordCommandBuffer(commandBuffers_[currentFrame_], imageIndex, pill, seconds);

    const VkSemaphore waitSemaphores[] = {imageAvailable_[currentFrame_]};
    const VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    const VkSemaphore signalSemaphores[] = {renderFinished_[currentFrame_]};

    const uint64_t acquireKey = kD3DWriteCompleteKey;
    const uint64_t releaseKey = kVulkanReadCompleteKey;
    const uint32_t timeout = kKeyedMutexTimeoutMs;
    VkWin32KeyedMutexAcquireReleaseInfoKHR keyedInfo{VK_STRUCTURE_TYPE_WIN32_KEYED_MUTEX_ACQUIRE_RELEASE_INFO_KHR};
    keyedInfo.acquireCount = 1;
    keyedInfo.pAcquireSyncs = &background_.memory;
    keyedInfo.pAcquireKeys = &acquireKey;
    keyedInfo.pAcquireTimeouts = &timeout;
    keyedInfo.releaseCount = 1;
    keyedInfo.pReleaseSyncs = &background_.memory;
    keyedInfo.pReleaseKeys = &releaseKey;

    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.pNext = &keyedInfo;
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers_[currentFrame_];
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;
    CheckVk(vkQueueSubmit(context_.graphicsQueue, 1, &submitInfo, inFlight_[currentFrame_]), "Could not submit the desktop overlay frame.");

    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain_;
    presentInfo.pImageIndices = &imageIndex;
    const VkResult presentResult = vkQueuePresentKHR(context_.presentQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR) {
        return false;
    }
    if (presentResult != VK_SUCCESS) {
        throw std::runtime_error("Could not present the desktop overlay frame.");
    }

    currentFrame_ = (currentFrame_ + 1) % kMaxFramesInFlight;
    return true;
}

void DesktopOverlay::VulkanRenderer::CreateSurface(HWND window) {
    VkWin32SurfaceCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    createInfo.hinstance = context_.instanceHandle;
    createInfo.hwnd = window;
    CheckVk(vkCreateWin32SurfaceKHR(context_.instance, &createInfo, nullptr, &surface_), "Could not create the desktop overlay Vulkan surface.");
}

void DesktopOverlay::VulkanRenderer::CreateSwapchain(VkExtent2D requestedExtent) {
    VkSurfaceCapabilitiesKHR capabilities{};
    CheckVk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(context_.physicalDevice, surface_, &capabilities), "Could not read desktop overlay surface capabilities.");

    uint32_t formatCount = 0;
    CheckVk(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice, surface_, &formatCount, nullptr), "Could not count desktop overlay surface formats.");
    if (formatCount == 0) {
        throw std::runtime_error("Desktop overlay surface has no formats.");
    }
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    CheckVk(vkGetPhysicalDeviceSurfaceFormatsKHR(context_.physicalDevice, surface_, &formatCount, formats.data()), "Could not read desktop overlay surface formats.");

    uint32_t presentModeCount = 0;
    CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice, surface_, &presentModeCount, nullptr), "Could not count desktop overlay present modes.");
    std::vector<VkPresentModeKHR> presentModes(std::max<uint32_t>(presentModeCount, 1));
    if (presentModeCount > 0) {
        CheckVk(vkGetPhysicalDeviceSurfacePresentModesKHR(context_.physicalDevice, surface_, &presentModeCount, presentModes.data()), "Could not read desktop overlay present modes.");
    } else {
        presentModes[0] = VK_PRESENT_MODE_FIFO_KHR;
    }

    const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(formats);
    const VkPresentModeKHR presentMode = ChoosePresentMode(presentModes);
    const VkExtent2D extent = ChooseExtent(capabilities, requestedExtent);

    uint32_t imageCount = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        imageCount = std::min(imageCount, capabilities.maxImageCount);
    }

    VkSwapchainCreateInfoKHR createInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    createInfo.surface = surface_;
    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t families[] = {context_.graphicsFamily, context_.presentFamily};
    if (context_.graphicsFamily != context_.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = families;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    createInfo.preTransform = capabilities.currentTransform;
    createInfo.compositeAlpha = ChooseCompositeAlpha(capabilities);
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;
    CheckVk(vkCreateSwapchainKHR(context_.device, &createInfo, nullptr, &swapchain_), "Could not create the desktop overlay swapchain.");

    vkGetSwapchainImagesKHR(context_.device, swapchain_, &imageCount, nullptr);
    swapchainImages_.resize(imageCount);
    vkGetSwapchainImagesKHR(context_.device, swapchain_, &imageCount, swapchainImages_.data());
    swapchainFormat_ = surfaceFormat.format;
    swapchainExtent_ = extent;

    swapchainImageViews_.resize(swapchainImages_.size());
    for (size_t i = 0; i < swapchainImages_.size(); ++i) {
        swapchainImageViews_[i] = CreateImageView(swapchainImages_[i], swapchainFormat_);
    }
}

void DesktopOverlay::VulkanRenderer::ImportBackground(HANDLE sharedBackgroundHandle, VkExtent2D extent) {
    background_.extent = extent;

    VkExternalMemoryImageCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    externalInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR;

    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.pNext = &externalInfo;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_B8G8R8A8_UNORM;
    imageInfo.extent = {extent.width, extent.height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    CheckVk(vkCreateImage(context_.device, &imageInfo, nullptr, &background_.image), "Could not create the imported desktop image.");

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(context_.device, background_.image, &requirements);

    VkImportMemoryWin32HandleInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT_KHR;
    importInfo.handle = sharedBackgroundHandle;

    VkMemoryAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocateInfo.pNext = &importInfo;
    allocateInfo.allocationSize = requirements.size;
    allocateInfo.memoryTypeIndex = FindMemoryType(context_.physicalDevice, requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CheckVk(vkAllocateMemory(context_.device, &allocateInfo, nullptr, &background_.memory), "Could not import the shared desktop texture memory.");
    CheckVk(vkBindImageMemory(context_.device, background_.image, background_.memory, 0), "Could not bind the imported desktop image.");

    background_.view = CreateImageView(background_.image, VK_FORMAT_B8G8R8A8_UNORM);

    VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.maxLod = 1.0f;
    CheckVk(vkCreateSampler(context_.device, &samplerInfo, nullptr, &background_.sampler), "Could not create the desktop texture sampler.");
}

void DesktopOverlay::VulkanRenderer::CreateFramebuffers() {
    framebuffers_.resize(swapchainImageViews_.size());
    for (size_t i = 0; i < swapchainImageViews_.size(); ++i) {
        VkImageView attachment[] = {swapchainImageViews_[i]};
        VkFramebufferCreateInfo createInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        createInfo.renderPass = liquidGlass_.renderPass();
        createInfo.attachmentCount = 1;
        createInfo.pAttachments = attachment;
        createInfo.width = swapchainExtent_.width;
        createInfo.height = swapchainExtent_.height;
        createInfo.layers = 1;
        CheckVk(vkCreateFramebuffer(context_.device, &createInfo, nullptr, &framebuffers_[i]), "Could not create a desktop overlay framebuffer.");
    }
}

void DesktopOverlay::VulkanRenderer::CreateCommandPool() {
    VkCommandPoolCreateInfo createInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    createInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    createInfo.queueFamilyIndex = context_.graphicsFamily;
    CheckVk(vkCreateCommandPool(context_.device, &createInfo, nullptr, &commandPool_), "Could not create the desktop overlay command pool.");
}

void DesktopOverlay::VulkanRenderer::CreateCommandBuffers() {
    VkCommandBufferAllocateInfo allocateInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocateInfo.commandPool = commandPool_;
    allocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocateInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers_.size());
    CheckVk(vkAllocateCommandBuffers(context_.device, &allocateInfo, commandBuffers_.data()), "Could not allocate desktop overlay command buffers.");
}

void DesktopOverlay::VulkanRenderer::CreateSyncObjects() {
    VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < kMaxFramesInFlight; ++i) {
        CheckVk(vkCreateSemaphore(context_.device, &semaphoreInfo, nullptr, &imageAvailable_[i]), "Could not create a desktop overlay semaphore.");
        CheckVk(vkCreateSemaphore(context_.device, &semaphoreInfo, nullptr, &renderFinished_[i]), "Could not create a desktop overlay semaphore.");
        CheckVk(vkCreateFence(context_.device, &fenceInfo, nullptr, &inFlight_[i]), "Could not create a desktop overlay fence.");
    }
}

void DesktopOverlay::VulkanRenderer::RecordCommandBuffer(
    VkCommandBuffer commandBuffer,
    uint32_t imageIndex,
    const OverlayPillController& pill,
    float seconds) {
    VkCommandBufferBeginInfo beginInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CheckVk(vkBeginCommandBuffer(commandBuffer, &beginInfo), "Could not begin the desktop overlay command buffer.");

    VkImageMemoryBarrier backgroundBarrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    backgroundBarrier.srcAccessMask = background_.layoutReady ? VK_ACCESS_SHADER_READ_BIT : 0;
    backgroundBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    backgroundBarrier.oldLayout = background_.layoutReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    backgroundBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    backgroundBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backgroundBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backgroundBarrier.image = background_.image;
    backgroundBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    backgroundBarrier.subresourceRange.levelCount = 1;
    backgroundBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        commandBuffer,
        background_.layoutReady ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &backgroundBarrier);
    background_.layoutReady = true;

    VkClearValue clear{};
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};

    VkRenderPassBeginInfo renderPassInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    renderPassInfo.renderPass = liquidGlass_.renderPass();
    renderPassInfo.framebuffer = framebuffers_[imageIndex];
    renderPassInfo.renderArea = {{0, 0}, swapchainExtent_};
    renderPassInfo.clearValueCount = 1;
    renderPassInfo.pClearValues = &clear;
    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    liquidGlass_.Record(commandBuffer, pill, swapchainExtent_, seconds);
    vkCmdEndRenderPass(commandBuffer);

    CheckVk(vkEndCommandBuffer(commandBuffer), "Could not end the desktop overlay command buffer.");
}

VkImageView DesktopOverlay::VulkanRenderer::CreateImageView(VkImage image, VkFormat format) {
    VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    CheckVk(vkCreateImageView(context_.device, &viewInfo, nullptr, &view), "Could not create a desktop overlay image view.");
    return view;
}

void DesktopOverlay::VulkanRenderer::DestroyBackground() {
    if (background_.sampler != VK_NULL_HANDLE) {
        vkDestroySampler(context_.device, background_.sampler, nullptr);
    }
    if (background_.view != VK_NULL_HANDLE) {
        vkDestroyImageView(context_.device, background_.view, nullptr);
    }
    if (background_.image != VK_NULL_HANDLE) {
        vkDestroyImage(context_.device, background_.image, nullptr);
    }
    if (background_.memory != VK_NULL_HANDLE) {
        vkFreeMemory(context_.device, background_.memory, nullptr);
    }
    background_ = {};
}

void DesktopOverlay::VulkanRenderer::DestroySwapchain() {
    for (VkFramebuffer framebuffer : framebuffers_) {
        vkDestroyFramebuffer(context_.device, framebuffer, nullptr);
    }
    framebuffers_.clear();
    for (VkImageView view : swapchainImageViews_) {
        vkDestroyImageView(context_.device, view, nullptr);
    }
    swapchainImageViews_.clear();
    swapchainImages_.clear();
    if (swapchain_ != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(context_.device, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
}

DesktopOverlay::~DesktopOverlay() {
    Stop();
}

bool DesktopOverlay::Start(const DesktopOverlayVulkanContext& context, POINT initialCenterScreen) {
    Stop();
    context_ = context;
    restoreRequested_ = false;
    lastError_.clear();
    virtualScreen_ = VirtualScreenRect();

    const int width = std::max<LONG>(virtualScreen_.right - virtualScreen_.left, 1);
    const int height = std::max<LONG>(virtualScreen_.bottom - virtualScreen_.top, 1);
    const float centerX = static_cast<float>(initialCenterScreen.x - virtualScreen_.left);
    const float centerY = static_cast<float>(initialCenterScreen.y - virtualScreen_.top);
    pill_.Reset(width, height, centerX, centerY, kDesktopPillScale);
    startedAt_ = std::chrono::steady_clock::now();

    window_ = new OverlayWindow();
    if (!window_->Create(*this, context_.instanceHandle, virtualScreen_)) {
        lastError_ = "Could not create the floating pill overlay window.";
        Stop();
        return false;
    }
    ApplyWindowRegion();

    if (!StartGpuPipeline()) {
        Stop();
        return false;
    }

    running_ = true;
    if (!Tick()) {
        if (lastError_.empty()) {
            lastError_ = "The floating pill could not render its first frame.";
        }
        Stop();
        return false;
    }
    window_->Show(virtualScreen_);
    return true;
}

void DesktopOverlay::Stop() {
    StopGpuPipeline();
    if (window_) {
        window_->Destroy();
        delete window_;
        window_ = nullptr;
    }
    running_ = false;
}

bool DesktopOverlay::Tick() {
    if (!running_ || restoreRequested_) {
        return !restoreRequested_;
    }
    if (!background_ || !renderer_) {
        return false;
    }

    try {
        pill_.Tick();
        ApplyWindowRegion();
        if (!background_->CaptureFrame(false)) {
            lastError_ = "Desktop capture was lost or timed out.";
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        const float seconds = std::chrono::duration<float>(now - startedAt_).count();
        if (!renderer_->Render(pill_, seconds)) {
            lastError_ = "The floating pill renderer could not present a frame.";
            return false;
        }
        return true;
    } catch (const std::exception& error) {
        lastError_ = error.what();
        return false;
    } catch (...) {
        lastError_ = "Unknown floating pill rendering failure.";
        return false;
    }
}

void DesktopOverlay::OnDisplayChanged() {
    if (!running_ || !window_) {
        return;
    }

    const POINT center = CenterScreenPoint();
    StopGpuPipeline();
    virtualScreen_ = VirtualScreenRect();
    window_->Move(virtualScreen_);

    const int width = std::max<LONG>(virtualScreen_.right - virtualScreen_.left, 1);
    const int height = std::max<LONG>(virtualScreen_.bottom - virtualScreen_.top, 1);
    pill_.Resize(width, height, static_cast<float>(center.x - virtualScreen_.left), static_cast<float>(center.y - virtualScreen_.top));
    ApplyWindowRegion();

    if (!StartGpuPipeline()) {
        RequestRestore();
    }
}

POINT DesktopOverlay::CenterScreenPoint() const {
    return pill_.CenterScreenPoint(virtualScreen_);
}

RECT DesktopOverlay::VirtualScreenRect() const {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = std::max(GetSystemMetrics(SM_CXVIRTUALSCREEN), 1);
    const int height = std::max(GetSystemMetrics(SM_CYVIRTUALSCREEN), 1);
    return {left, top, left + width, top + height};
}

bool DesktopOverlay::StartGpuPipeline() {
    StopGpuPipeline();
    background_ = new BackgroundProvider();
    if (!background_->Start(context_, virtualScreen_)) {
        lastError_ = background_->lastError().empty() ? "Could not initialize desktop capture." : background_->lastError();
        StopGpuPipeline();
        return false;
    }

    renderer_ = new VulkanRenderer();
    const VkExtent2D extent{
        static_cast<uint32_t>(std::max<LONG>(virtualScreen_.right - virtualScreen_.left, 1)),
        static_cast<uint32_t>(std::max<LONG>(virtualScreen_.bottom - virtualScreen_.top, 1)),
    };
    if (!renderer_->Start(context_, window_->hwnd(), background_->sharedHandle(), background_->extent(), extent)) {
        lastError_ = renderer_->lastError().empty() ? "Could not initialize the floating pill Vulkan renderer." : renderer_->lastError();
        StopGpuPipeline();
        return false;
    }
    return true;
}

void DesktopOverlay::StopGpuPipeline() {
    if (renderer_) {
        renderer_->Stop();
        delete renderer_;
        renderer_ = nullptr;
    }
    if (background_) {
        background_->Stop();
        delete background_;
        background_ = nullptr;
    }
}

void DesktopOverlay::ApplyWindowRegion() {
    if (!window_ || !window_->hwnd()) {
        return;
    }

    const RECT bounds = pill_.VisualBounds();
    const int width = std::max<LONG>(bounds.right - bounds.left, 1);
    const int height = std::max<LONG>(bounds.bottom - bounds.top, 1);
    const int margin = std::max(8, static_cast<int>(std::ceil(std::min(width, height) * 0.06f)));
    const int radius = std::max(std::min(width, height) + margin * 2, 1);
    HRGN region = CreateRoundRectRgn(
        bounds.left - margin,
        bounds.top - margin,
        bounds.right + margin,
        bounds.bottom + margin,
        radius,
        radius);
    if (!region) {
        return;
    }
    if (SetWindowRgn(window_->hwnd(), region, FALSE) == 0) {
        DeleteObject(region);
    }
}

void DesktopOverlay::RequestRestore() {
    restoreRequested_ = true;
}

LRESULT DesktopOverlay::HandleWindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCCREATE:
        return TRUE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_NCHITTEST: {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (pill_.dragging() || pill_.HitTest(static_cast<float>(point.x), static_cast<float>(point.y))) {
            return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_DISPLAYCHANGE:
        OnDisplayChanged();
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            RequestRestore();
            return 0;
        }
        break;
    case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(GET_X_LPARAM(lParam));
        const float y = static_cast<float>(GET_Y_LPARAM(lParam));
        if (pill_.HitTest(x, y)) {
            pill_.BeginDrag(x, y);
            SetCapture(window);
            SetCursor(LoadCursor(nullptr, IDC_HAND));
            ApplyWindowRegion();
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (pill_.dragging()) {
            pill_.DragTo(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)));
            ApplyWindowRegion();
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        if (pill_.dragging()) {
            pill_.EndDrag();
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            ApplyWindowRegion();
        }
        return 0;
    case WM_CLOSE:
        RequestRestore();
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrA(window, GWLP_USERDATA, 0);
        return 0;
    default:
        break;
    }
    return DefWindowProcA(window, message, wParam, lParam);
}
