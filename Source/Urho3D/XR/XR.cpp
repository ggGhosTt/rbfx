// Copyright (c) 2022-2023 the rbfx project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "Urho3D/XR/XR.h"

#include "Urho3D/Core/CoreEvents.h"
#include "Urho3D/Engine/Engine.h"
#include "Urho3D/Engine/EngineDefs.h"
#include "Urho3D/Graphics/AnimatedModel.h"
#include "Urho3D/Graphics/Geometry.h"
#include "Urho3D/Graphics/Graphics.h"
#include "Urho3D/Graphics/GraphicsEvents.h"
#include "Urho3D/Graphics/IndexBuffer.h"
#include "Urho3D/Graphics/Material.h"
#include "Urho3D/Graphics/StaticModel.h"
#include "Urho3D/Graphics/Texture2D.h"
#include "Urho3D/Graphics/VertexBuffer.h"
#include "Urho3D/IO/File.h"
#include "Urho3D/IO/Log.h"
#include "Urho3D/IO/MemoryBuffer.h"
#include "Urho3D/RenderAPI/GAPIIncludes.h"
#include "Urho3D/RenderAPI/RenderAPIUtils.h"
#include "Urho3D/RenderAPI/RenderDevice.h"
#include "Urho3D/RenderPipeline/ShaderConsts.h"
#include "Urho3D/Resource/Localization.h"
#include "Urho3D/Resource/ResourceCache.h"
#include "Urho3D/Resource/XMLElement.h"
#include "Urho3D/Resource/XMLFile.h"
#include "Urho3D/Scene/Node.h"
#include "Urho3D/Scene/Scene.h"
#include "Urho3D/XR/OpenXRAPI.h"
#include "Urho3D/XR/VREvents.h"

#include <Diligent/Graphics/GraphicsEngine/interface/DeviceContext.h>
#if D3D11_SUPPORTED
    #include <Diligent/Graphics/GraphicsEngineD3D11/interface/RenderDeviceD3D11.h>
#endif
#if D3D12_SUPPORTED
    #include <Diligent/Graphics/GraphicsEngineD3D12/interface/RenderDeviceD3D12.h>
    #include <Diligent/Graphics/GraphicsEngineD3D12/interface/CommandQueueD3D12.h>
#endif
#if VULKAN_SUPPORTED
    #include <Diligent/Graphics/GraphicsEngineVulkan/interface/CommandQueueVk.h>
    #include <Diligent/Graphics/GraphicsEngineVulkan/interface/EngineFactoryVk.h>
    #include <Diligent/Graphics/GraphicsEngineVulkan/interface/RenderDeviceVk.h>
#endif

// need this for loading the GLBs
#include <ThirdParty/tinygltf/tiny_gltf.h>

#include <iostream>

#include <ThirdParty/OpenXRSDK/include/openxr/openxr_platform_defines.h>
#include <ThirdParty/OpenXRSDK/include/openxr/openxr_platform.h>

#include <EASTL/optional.h>

#include <SDL.h>

#include "../DebugNew.h"

#if URHO3D_PLATFORM_ANDROID
// TODO: This is a hack to get EGLConfig in SDL2.
// Replace with SDL_EGL_GetCurrentEGLConfig in SDL3.
extern "C" EGLConfig SDL_EGL_GetConfig();
#endif

namespace Urho3D
{

namespace
{

bool IsNativeOculusQuest2()
{
#ifdef URHO3D_OCULUS_QUEST
    return true;
#else
    return false;
#endif
}

StringVector EnumerateExtensionsXR()
{
    uint32_t count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);

    ea::vector<XrExtensionProperties> extensions;
    extensions.resize(count, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, extensions.size(), &count, extensions.data());

    StringVector result;
    for (const XrExtensionProperties& extension : extensions)
        result.push_back(extension.extensionName);
    return result;
}

bool IsExtensionSupported(const StringVector& extensions, const char* name)
{
    for (const ea::string& ext : extensions)
    {
        if (ext.comparei(name) == 0)
            return true;
    }
    return false;
}

bool ActivateOptionalExtension(StringVector& result, const StringVector& extensions, const char* name)
{
    if (IsExtensionSupported(extensions, name))
    {
        result.push_back(name);
        return true;
    }
    return false;
}

const char* GetBackendExtensionName(RenderBackend backend)
{
    switch (backend)
    {
#if D3D11_SUPPORTED
    case RenderBackend::D3D11: return XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
#endif
#if D3D12_SUPPORTED
    case RenderBackend::D3D12: return XR_KHR_D3D12_ENABLE_EXTENSION_NAME;
#endif
#if VULKAN_SUPPORTED
    case RenderBackend::Vulkan: return XR_KHR_VULKAN_ENABLE_EXTENSION_NAME;
#endif
#if GLES_SUPPORTED
    case RenderBackend::OpenGL: return XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
#endif
#if GL_SUPPORTED
    case RenderBackend::OpenGL: return XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#endif
    default: return "";
    }
}

XrInstancePtr CreateInstanceXR(
    const StringVector& extensions, const ea::string& engineName, const ea::string& applicationName)
{
    const auto extensionNames = ToCStringVector(extensions);

    XrInstanceCreateInfo info = {XR_TYPE_INSTANCE_CREATE_INFO};
    strncpy(info.applicationInfo.engineName, engineName.c_str(), XR_MAX_ENGINE_NAME_SIZE);
    strncpy(info.applicationInfo.applicationName, applicationName.c_str(), XR_MAX_APPLICATION_NAME_SIZE);
    info.applicationInfo.engineVersion = (1 << 24) + (0 << 16) + 0; // TODO: get an actual engine version
    info.applicationInfo.applicationVersion = 0; // TODO: application version?
    info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
    info.enabledExtensionCount = extensionNames.size();
    info.enabledExtensionNames = extensionNames.data();

#if URHO3D_PLATFORM_ANDROID
    JNIEnv* env = (JNIEnv*)SDL_AndroidGetJNIEnv();
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);

    XrInstanceCreateInfoAndroidKHR androidInfo = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = SDL_AndroidGetActivity();
    info.next = &androidInfo;
#endif

    XrInstance instance;
    if (!URHO3D_CHECK_OPENXR(xrCreateInstance(&info, &instance)))
        return nullptr;

    LoadOpenXRAPI(instance);

    const auto deleter = [](XrInstance instance)
    {
        xrDestroyInstance(instance);
        UnloadOpenXRAPI();
    };
    return XrInstancePtr(instance, deleter);
}

XrBool32 XRAPI_PTR DebugMessageLoggerXR(XrDebugUtilsMessageSeverityFlagsEXT severity,
    XrDebugUtilsMessageTypeFlagsEXT types, const XrDebugUtilsMessengerCallbackDataEXT* msg, void* user_data)
{
    if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        URHO3D_LOGERROR("XR Error: {}, {}", msg->functionName, msg->message);
    else if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        URHO3D_LOGWARNING("XR Warning: {}, {}", msg->functionName, msg->message);
    else if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        URHO3D_LOGINFO("XR Info: {}, {}", msg->functionName, msg->message);
    else if (severity & XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)
        URHO3D_LOGDEBUG("XR Debug: {}, {}", msg->functionName, msg->message);

    return false;
};

XrDebugUtilsMessengerEXTPtr CreateDebugMessengerXR(XrInstance instance)
{
    XrDebugUtilsMessengerCreateInfoEXT debugUtils = {XR_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};

    debugUtils.userCallback = DebugMessageLoggerXR;
    debugUtils.messageTypes = XR_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
        | XR_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT //
        | XR_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT //
        | XR_DEBUG_UTILS_MESSAGE_TYPE_CONFORMANCE_BIT_EXT;
    debugUtils.messageSeverities = XR_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
        | XR_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT //
        | XR_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT //
        | XR_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

    XrDebugUtilsMessengerEXT messenger;
    xrCreateDebugUtilsMessengerEXT(instance, &debugUtils, &messenger);
    if (!messenger)
        return nullptr;

    return XrDebugUtilsMessengerEXTPtr(messenger, xrDestroyDebugUtilsMessengerEXT);
}

ea::optional<XrSystemId> GetSystemXR(XrInstance instance)
{
    XrSystemGetInfo sysInfo = {XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    XrSystemId systemId;
    if (!URHO3D_CHECK_OPENXR(xrGetSystem(instance, &sysInfo, &systemId)))
        return ea::nullopt;

    return systemId;
}

ea::string GetSystemNameXR(XrInstance instance, XrSystemId system)
{
    XrSystemProperties properties = {XR_TYPE_SYSTEM_PROPERTIES};
    if (!URHO3D_CHECK_OPENXR(xrGetSystemProperties(instance, system, &properties)))
        return "";
    return properties.systemName;
}

ea::vector<XrEnvironmentBlendMode> GetBlendModesXR(XrInstance instance, XrSystemId system)
{
    uint32_t count = 0;
    xrEnumerateEnvironmentBlendModes(instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr);

    ea::vector<XrEnvironmentBlendMode> result(count);
    xrEnumerateEnvironmentBlendModes(
        instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, count, &count, result.data());

    if (count == 0)
    {
        URHO3D_LOGERROR("Failed to get OpenXR blend modes");
        return {};
    }

    return result;
}

ea::vector<XrViewConfigurationType> GetViewConfigurationsXR(XrInstance instance, XrSystemId system)
{
    uint32_t count = 0;
    xrEnumerateViewConfigurations(instance, system, 0, &count, nullptr);

    ea::vector<XrViewConfigurationType> result(count);
    xrEnumerateViewConfigurations(instance, system, count, &count, result.data());

    return result;
}

ea::vector<XrViewConfigurationView> GetViewConfigurationViewsXR(XrInstance instance, XrSystemId system)
{
    ea::vector<XrViewConfigurationView> result;
    result.push_back(XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result.push_back(XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});

    unsigned count = 0;
    if (URHO3D_CHECK_OPENXR(xrEnumerateViewConfigurationViews(
            instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, result.data())))
    {
        return result;
    }

    return {};
}

#if VULKAN_SUPPORTED
StringVector GetVulkanInstanceExtensionsXR(XrInstance instance, XrSystemId system)
{
    uint32_t bufferSize = 0;
    xrGetVulkanInstanceExtensionsKHR(instance, system, 0, &bufferSize, nullptr);
    ea::string buffer(bufferSize, '\0');
    xrGetVulkanInstanceExtensionsKHR(instance, system, bufferSize, &bufferSize, buffer.data());
    return buffer.split(' ');
}

StringVector GetVulkanDeviceExtensionsXR(XrInstance instance, XrSystemId system)
{
    uint32_t bufferSize = 0;
    xrGetVulkanDeviceExtensionsKHR(instance, system, 0, &bufferSize, nullptr);
    ea::string buffer(bufferSize, '\0');
    xrGetVulkanDeviceExtensionsKHR(instance, system, bufferSize, &bufferSize, buffer.data());
    return buffer.split(' ');
}
#endif

ea::vector<int64_t> GetSwapChainFormats(XrSession session)
{
    unsigned count = 0;
    xrEnumerateSwapchainFormats(session, 0, &count, 0);

    ea::vector<int64_t> result;
    result.resize(count);
    xrEnumerateSwapchainFormats(session, count, &count, result.data());

    return result;
}

/// Try to use sRGB texture formats whenever possible, i.e. linear output.
/// Oculus Quest 2 always expects linear input even if the framebuffer is not sRGB:
/// https://developer.oculus.com/resources/color-management-guide/
bool IsFallbackColorFormat(TextureFormat format)
{
    return SetTextureFormatSRGB(format, true) != format;
}

/// 16-bit depth is just not enough.
bool IsFallbackDepthFormat(TextureFormat format)
{
    return format == TextureFormat::TEX_FORMAT_D16_UNORM;
}

ea::pair<TextureFormat, int64_t> SelectColorFormat(RenderBackend backend, const ea::vector<int64_t>& formats)
{
    for (bool fallback : {false, true})
    {
        for (const auto internalFormat : formats)
        {
            const TextureFormat textureFormat = GetTextureFormatFromInternal(backend, internalFormat);

            // Oculus Quest 2 does not support sRGB framebuffers natively.
            if (IsNativeOculusQuest2() && IsTextureFormatSRGB(textureFormat))
                continue;

            if (IsColorTextureFormat(textureFormat) && IsFallbackColorFormat(textureFormat) == fallback)
                return {textureFormat, internalFormat};
        }
    }
    return {TextureFormat::TEX_FORMAT_UNKNOWN, 0};
}

ea::pair<TextureFormat, int64_t> SelectDepthFormat(RenderBackend backend, const ea::vector<int64_t>& formats)
{
    // Oculus Quest 2 returns non-framebuffer-compatible depth formats.
    if (!IsNativeOculusQuest2())
    {
        for (bool fallback : {false, true})
        {
            for (const auto internalFormat : formats)
            {
                const TextureFormat textureFormat = GetTextureFormatFromInternal(backend, internalFormat);
                if (IsDepthTextureFormat(textureFormat) && IsFallbackDepthFormat(textureFormat) == fallback)
                    return {textureFormat, internalFormat};
            }
        }
    }
    return {TextureFormat::TEX_FORMAT_UNKNOWN, 0};
}

XrSessionPtr CreateSessionXR(RenderDevice* renderDevice, XrInstance instance, XrSystemId system)
{
    XrSessionCreateInfo sessionCreateInfo = {XR_TYPE_SESSION_CREATE_INFO};
    sessionCreateInfo.systemId = system;

    XrSession session{};
    switch (renderDevice->GetBackend())
    {
#if D3D11_SUPPORTED
    case RenderBackend::D3D11:
    {
        XrGraphicsRequirementsD3D11KHR requisite = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        if (!URHO3D_CHECK_OPENXR(xrGetD3D11GraphicsRequirementsKHR(instance, system, &requisite)))
            return nullptr;

        const auto renderDeviceD3D11 = static_cast<Diligent::IRenderDeviceD3D11*>(renderDevice->GetRenderDevice());

        XrGraphicsBindingD3D11KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
        binding.device = renderDeviceD3D11->GetD3D11Device();
        sessionCreateInfo.next = &binding;

        if (!URHO3D_CHECK_OPENXR(xrCreateSession(instance, &sessionCreateInfo, &session)))
            return nullptr;

        break;
    }
#endif
#if D3D12_SUPPORTED
    case RenderBackend::D3D12:
    {
        XrGraphicsRequirementsD3D12KHR requisite = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D12_KHR};
        if (!URHO3D_CHECK_OPENXR(xrGetD3D12GraphicsRequirementsKHR(instance, system, &requisite)))
            return nullptr;

        const auto renderDeviceD3D12 = static_cast<Diligent::IRenderDeviceD3D12*>(renderDevice->GetRenderDevice());
        const auto immediateContext = renderDevice->GetImmediateContext();
        const auto commandQueue = immediateContext->LockCommandQueue();
        immediateContext->UnlockCommandQueue();
        const auto commandQueueD3D12 = static_cast<Diligent::ICommandQueueD3D12*>(commandQueue);

        XrGraphicsBindingD3D12KHR binding = {XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = renderDeviceD3D12->GetD3D12Device();
        binding.queue = commandQueueD3D12->GetD3D12CommandQueue();
        sessionCreateInfo.next = &binding;

        if (!URHO3D_CHECK_OPENXR(xrCreateSession(instance, &sessionCreateInfo, &session)))
            return nullptr;

        break;
    }
#endif
#if VULKAN_SUPPORTED
    case RenderBackend::Vulkan:
    {
        XrGraphicsRequirementsVulkanKHR requisite = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        if (!URHO3D_CHECK_OPENXR(xrGetVulkanGraphicsRequirementsKHR(instance, system, &requisite)))
            return nullptr;

        const auto renderDeviceVk = static_cast<Diligent::IRenderDeviceVk*>(renderDevice->GetRenderDevice());
        const auto immediateContext = renderDevice->GetImmediateContext();
        const auto commandQueue = immediateContext->LockCommandQueue();
        immediateContext->UnlockCommandQueue();
        const auto commandQueueVk = static_cast<Diligent::ICommandQueueVk*>(commandQueue);

        XrGraphicsBindingVulkanKHR binding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = renderDeviceVk->GetVkInstance();
        binding.physicalDevice = renderDeviceVk->GetVkPhysicalDevice();
        binding.device = renderDeviceVk->GetVkDevice();
        binding.queueFamilyIndex = commandQueueVk->GetQueueFamilyIndex();
        binding.queueIndex = 0; // TODO(xr): Revisit this place
        sessionCreateInfo.next = &binding;

        // We cannot do anything if the device does not match, in current architecture of Diligent.
        VkPhysicalDevice requiredPhysicalDevice{};
        xrGetVulkanGraphicsDeviceKHR(instance, system, binding.instance, &requiredPhysicalDevice);
        if (requiredPhysicalDevice != binding.physicalDevice)
        {
            URHO3D_LOGERROR("OpenXR cannot use current VkPhysicalDevice");
            return nullptr;
        }

        if (!URHO3D_CHECK_OPENXR(xrCreateSession(instance, &sessionCreateInfo, &session)))
            return nullptr;

        break;
    }
#endif
#if GL_SUPPORTED && URHO3D_PLATFORM_WINDOWS
    case RenderBackend::OpenGL:
    {
        XrGraphicsRequirementsOpenGLKHR requisite = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        if (!URHO3D_CHECK_OPENXR(xrGetOpenGLGraphicsRequirementsKHR(instance, system, &requisite)))
            return nullptr;

    #if URHO3D_PLATFORM_WINDOWS
        XrGraphicsBindingOpenGLWin32KHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
        binding.hDC = wglGetCurrentDC();
        binding.hGLRC = wglGetCurrentContext();
        sessionCreateInfo.next = &binding;
    #else
        URHO3D_ASSERTLOG(false, "OpenXR is not implemented for this platform");
        return nullptr;
    #endif

        if (!URHO3D_CHECK_OPENXR(xrCreateSession(instance, &sessionCreateInfo, &session)))
            return nullptr;

        break;
    }
#endif
#if GLES_SUPPORTED && URHO3D_PLATFORM_ANDROID
    case RenderBackend::OpenGL:
    {
        XrGraphicsRequirementsOpenGLESKHR requisite = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        if (!URHO3D_CHECK_OPENXR(xrGetOpenGLESGraphicsRequirementsKHR(instance, system, &requisite)))
            return nullptr;

    #if URHO3D_PLATFORM_ANDROID
        XrGraphicsBindingOpenGLESAndroidKHR binding = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
        binding.display = eglGetCurrentDisplay();
        binding.config = SDL_EGL_GetConfig();
        binding.context = eglGetCurrentContext();
        sessionCreateInfo.next = &binding;
    #else
        URHO3D_ASSERTLOG(false, "OpenXR is not implemented for this platform");
        return nullptr;
    #endif

        if (!URHO3D_CHECK_OPENXR(xrCreateSession(instance, &sessionCreateInfo, &session)))
            return nullptr;

        break;
    }
#endif
    default: URHO3D_ASSERTLOG(false, "OpenXR is not implemented for this backend"); return nullptr;
    }

    return XrSessionPtr(session, xrDestroySession);
}

ea::pair<XrSpacePtr, bool> CreateHeadSpaceXR(XrSession session)
{
    XrReferenceSpaceCreateInfo createInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    createInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    createInfo.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    bool isRoomScale = true;
    XrSpace space;
    if (!URHO3D_CHECK_OPENXR(xrCreateReferenceSpace(session, &createInfo, &space)))
    {
        isRoomScale = false;

        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!URHO3D_CHECK_OPENXR(xrCreateReferenceSpace(session, &createInfo, &space)))
            return {};
    }

    const auto wrappedSpace = XrSpacePtr(space, xrDestroySpace);
    return {wrappedSpace, isRoomScale};
}

XrSpacePtr CreateViewSpaceXR(XrSession session)
{
    XrReferenceSpaceCreateInfo createInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    createInfo.poseInReferenceSpace.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    createInfo.poseInReferenceSpace.position = {0.0f, 0.0f, 0.0f};

    XrSpace space;
    if (!URHO3D_CHECK_OPENXR(xrCreateReferenceSpace(session, &createInfo, &space)))
        return nullptr;

    return XrSpacePtr(space, xrDestroySpace);
}

template <class T, XrStructureType ImageStructureType> class OpenXRSwapChainBase : public OpenXRSwapChain
{
public:
    OpenXRSwapChainBase(
        XrSession session, TextureFormat format, int64_t internalFormat, const IntVector2& eyeSize, int msaaLevel)
    {
        format_ = format;
        textureSize_ = arraySize_ == 1 ? eyeSize * IntVector2{2, 1} : eyeSize;

        XrSwapchainCreateInfo swapInfo = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        swapInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;

        if (IsDepthTextureFormat(format))
            swapInfo.usageFlags |= XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        else
            swapInfo.usageFlags |= XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

        swapInfo.format = internalFormat;
        swapInfo.width = textureSize_.x_;
        swapInfo.height = textureSize_.y_;
        swapInfo.sampleCount = msaaLevel;
        swapInfo.faceCount = 1;
        swapInfo.arraySize = arraySize_;
        swapInfo.mipCount = 1;

        XrSwapchain swapChain;
        if (!URHO3D_CHECK_OPENXR(xrCreateSwapchain(session, &swapInfo, &swapChain)))
            return;

        swapChain_ = XrSwapchainPtr(swapChain, xrDestroySwapchain);

        uint32_t numImages = 0;
        if (!URHO3D_CHECK_OPENXR(xrEnumerateSwapchainImages(swapChain_.Raw(), 0, &numImages, nullptr)))
            return;

        ea::vector<T> images(numImages);
        for (T& image : images)
        {
            image.type = ImageStructureType;
            image.next = nullptr;
        }

        const auto imagesPtr = reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data());
        if (!URHO3D_CHECK_OPENXR(xrEnumerateSwapchainImages(swapChain_.Raw(), numImages, &numImages, imagesPtr)))
            return;

        images_ = ea::move(images);
    }

    virtual ~OpenXRSwapChainBase()
    {
        for (Texture2D* texture : textures_)
            texture->Destroy();
    }

    const T& GetImageXR(unsigned index) const { return images_[index]; }

protected:
    ea::vector<T> images_;
    IntVector2 textureSize_;
};

#if D3D11_SUPPORTED
class OpenXRSwapChainD3D11 : public OpenXRSwapChainBase<XrSwapchainImageD3D11KHR, XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR>
{
public:
    using BaseClass = OpenXRSwapChainBase<XrSwapchainImageD3D11KHR, XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR>;

    OpenXRSwapChainD3D11(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
        const IntVector2& eyeSize, int msaaLevel)
        : BaseClass(session, format, internalFormat, eyeSize, msaaLevel)
    {
        auto renderDevice = context->GetSubsystem<RenderDevice>();

        const unsigned numImages = images_.size();
        textures_.resize(numImages);
        for (unsigned i = 0; i < numImages; ++i)
        {
            URHO3D_ASSERT(arraySize_ == 1);

            textures_[i] = MakeShared<Texture2D>(context);
            textures_[i]->CreateFromD3D11Texture2D(images_[i].texture, format, msaaLevel);
        }
    }
};
#endif

#if D3D12_SUPPORTED
class OpenXRSwapChainD3D12 : public OpenXRSwapChainBase<XrSwapchainImageD3D12KHR, XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR>
{
public:
    using BaseClass = OpenXRSwapChainBase<XrSwapchainImageD3D12KHR, XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR>;

    OpenXRSwapChainD3D12(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
        const IntVector2& eyeSize, int msaaLevel)
        : BaseClass(session, format, internalFormat, eyeSize, msaaLevel)
    {
        auto renderDevice = context->GetSubsystem<RenderDevice>();

        const unsigned numImages = images_.size();
        textures_.resize(numImages);
        for (unsigned i = 0; i < numImages; ++i)
        {
            URHO3D_ASSERT(arraySize_ == 1);

            textures_[i] = MakeShared<Texture2D>(context);
            textures_[i]->CreateFromD3D12Resource(images_[i].texture, format, msaaLevel);
        }
    }
};
#endif

#if VULKAN_SUPPORTED
class OpenXRSwapChainVulkan : public OpenXRSwapChainBase<XrSwapchainImageVulkanKHR, XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR>
{
public:
    using BaseClass = OpenXRSwapChainBase<XrSwapchainImageVulkanKHR, XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR>;

    OpenXRSwapChainVulkan(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
        const IntVector2& eyeSize, int msaaLevel)
        : BaseClass(session, format, internalFormat, eyeSize, msaaLevel)
    {
        auto renderDevice = context->GetSubsystem<RenderDevice>();

        const bool isDepth = IsDepthTextureFormat(format);
        const unsigned numImages = images_.size();
        textures_.resize(numImages);
        for (unsigned i = 0; i < numImages; ++i)
        {
            URHO3D_ASSERT(arraySize_ == 1);

            RawTextureParams params;
            params.type_ = TextureType::Texture2D;
            params.format_ = format;
            params.flags_ = isDepth ? TextureFlag::BindDepthStencil : TextureFlag::BindRenderTarget;
            params.size_ = textureSize_.ToIntVector3(1);
            params.numLevels_ = 1;
            params.multiSample_ = msaaLevel;

            textures_[i] = MakeShared<Texture2D>(context);
            textures_[i]->CreateFromVulkanImage((uint64_t)images_[i].image, params);

            // Oculus Quest 2 always expects texture data in linear space.
            if (IsNativeOculusQuest2())
                textures_[i]->SetLinear(true);
        }
    }
};
#endif

#if GL_SUPPORTED
class OpenXRSwapChainGL : public OpenXRSwapChainBase<XrSwapchainImageOpenGLKHR, XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR>
{
public:
    using BaseClass = OpenXRSwapChainBase<XrSwapchainImageOpenGLKHR, XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR>;

    OpenXRSwapChainGL(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
        const IntVector2& eyeSize, int msaaLevel)
        : BaseClass(session, format, internalFormat, eyeSize, msaaLevel)
    {
        auto renderDevice = context->GetSubsystem<RenderDevice>();

        const bool isDepth = IsDepthTextureFormat(format);
        const unsigned numImages = images_.size();
        textures_.resize(numImages);
        for (unsigned i = 0; i < numImages; ++i)
        {
            URHO3D_ASSERT(arraySize_ == 1);

            textures_[i] = MakeShared<Texture2D>(context);
            textures_[i]->CreateFromGLTexture(images_[i].image, TextureType::Texture2D,
                isDepth ? TextureFlag::BindDepthStencil : TextureFlag::BindRenderTarget, format, arraySize_, msaaLevel);
        }
    }
};
#endif

#if GLES_SUPPORTED
class OpenXRSwapChainGLES : public OpenXRSwapChainBase<XrSwapchainImageOpenGLESKHR, XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR>
{
public:
    using BaseClass = OpenXRSwapChainBase<XrSwapchainImageOpenGLESKHR, XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR>;

    OpenXRSwapChainGLES(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
        const IntVector2& eyeSize, int msaaLevel)
        : BaseClass(session, format, internalFormat, eyeSize, msaaLevel)
    {
        auto renderDevice = context->GetSubsystem<RenderDevice>();

        const bool isDepth = IsDepthTextureFormat(format);
        const unsigned numImages = images_.size();
        textures_.resize(numImages);
        for (unsigned i = 0; i < numImages; ++i)
        {
            URHO3D_ASSERT(arraySize_ == 1);

            textures_[i] = MakeShared<Texture2D>(context);
            textures_[i]->CreateFromGLTexture(images_[i].image, TextureType::Texture2D,
                isDepth ? TextureFlag::BindDepthStencil : TextureFlag::BindRenderTarget, format, arraySize_, msaaLevel);
            // Oculus Quest 2 always expects texture data in linear space.
            textures_[i]->SetLinear(true);
        }
    }
};
#endif

OpenXRSwapChainPtr CreateSwapChainXR(Context* context, XrSession session, TextureFormat format, int64_t internalFormat,
    const IntVector2& eyeSize, int msaaLevel)
{
    auto renderDevice = context->GetSubsystem<RenderDevice>();

    OpenXRSwapChainPtr result;
    switch (renderDevice->GetBackend())
    {
#if D3D11_SUPPORTED
    case RenderBackend::D3D11:
        result = ea::make_shared<OpenXRSwapChainD3D11>(context, session, format, internalFormat, eyeSize, msaaLevel);
        break;
#endif
#if D3D12_SUPPORTED
    case RenderBackend::D3D12:
        result = ea::make_shared<OpenXRSwapChainD3D12>(context, session, format, internalFormat, eyeSize, msaaLevel);
        break;
#endif
#if VULKAN_SUPPORTED
    case RenderBackend::Vulkan:
        result = ea::make_shared<OpenXRSwapChainVulkan>(context, session, format, internalFormat, eyeSize, msaaLevel);
        break;
#endif
#if GL_SUPPORTED
    case RenderBackend::OpenGL:
        result = ea::make_shared<OpenXRSwapChainGL>(context, session, format, internalFormat, eyeSize, msaaLevel);
        break;
#endif
#if GLES_SUPPORTED
    case RenderBackend::OpenGL:
        result = ea::make_shared<OpenXRSwapChainGLES>(context, session, format, internalFormat, eyeSize, msaaLevel);
        break;
#endif
    default: URHO3D_ASSERTLOG(false, "OpenXR is not implemented for this backend"); break;
    }

    return result && result->GetNumTextures() != 0 ? result : nullptr;
}

ea::optional<VariantType> ParseBindingType(ea::string_view type)
{
    if (type == "boolean")
        return VAR_BOOL;
    else if (type == "vector1" || type == "single")
        return VAR_FLOAT;
    else if (type == "vector2")
        return VAR_VECTOR2;
    else if (type == "vector3")
        return VAR_VECTOR3;
    else if (type == "pose")
        return VAR_MATRIX3X4;
    else if (type == "haptic")
        return VAR_NONE;
    else
        return ea::nullopt;
}

XrActionType ToActionType(VariantType type)
{
    switch (type)
    {
    case VAR_BOOL: return XR_ACTION_TYPE_BOOLEAN_INPUT;
    case VAR_FLOAT: return XR_ACTION_TYPE_FLOAT_INPUT;
    case VAR_VECTOR2: return XR_ACTION_TYPE_VECTOR2F_INPUT;
    case VAR_VECTOR3: return XR_ACTION_TYPE_POSE_INPUT;
    case VAR_MATRIX3X4: return XR_ACTION_TYPE_POSE_INPUT;
    case VAR_NONE: return XR_ACTION_TYPE_VIBRATION_OUTPUT;
    default: URHO3D_ASSERT(false); return XR_ACTION_TYPE_BOOLEAN_INPUT;
    }
}

ea::array<XrPath, 2> GetHandPaths(XrInstance instance)
{
    ea::array<XrPath, 2> handPaths{};
    xrStringToPath(instance, "/user/hand/left", &handPaths[VR_HAND_LEFT]);
    xrStringToPath(instance, "/user/hand/right", &handPaths[VR_HAND_RIGHT]);
    return handPaths;
}

ea::pair<XrSpacePtr, XrSpacePtr> CreateActionSpaces(
    XrInstance instance, XrSession session, XrAction action, bool isHanded)
{
    XrActionSpaceCreateInfo spaceInfo = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = action;
    spaceInfo.poseInActionSpace = xrPoseIdentity;

    if (!isHanded)
    {
        XrSpace space{};
        if (!URHO3D_CHECK_OPENXR(xrCreateActionSpace(session, &spaceInfo, &space)))
            return {};

        const auto wrappedSpace = XrSpacePtr(space, xrDestroySpace);
        return {wrappedSpace, wrappedSpace};
    }

    const auto handPaths = GetHandPaths(instance);

    XrSpace spaceLeft{};
    spaceInfo.subactionPath = handPaths[VR_HAND_LEFT];
    if (!URHO3D_CHECK_OPENXR(xrCreateActionSpace(session, &spaceInfo, &spaceLeft)))
        return {};
    const auto wrappedSpaceLeft = XrSpacePtr(spaceLeft, xrDestroySpace);

    XrSpace spaceRight{};
    spaceInfo.subactionPath = handPaths[VR_HAND_RIGHT];
    if (!URHO3D_CHECK_OPENXR(xrCreateActionSpace(session, &spaceInfo, &spaceRight)))
        return {};
    const auto wrappedSpaceRight = XrSpacePtr(spaceRight, xrDestroySpace);

    return {wrappedSpaceLeft, wrappedSpaceRight};
}

ea::pair<SharedPtr<OpenXRBinding>, SharedPtr<OpenXRBinding>> CreateBinding(
    XrInstance instance, XrSession session, XrActionSet actionSet, XMLElement element)
{
    Context* context = Context::GetInstance();
    auto localization = context->GetSubsystem<Localization>();

    const auto handPaths = GetHandPaths(instance);

    const ea::string name = element.GetAttribute("name");
    const ea::string typeName = element.GetAttribute("type");
    const bool handed = element.GetBool("handed");

    // Create action
    XrActionCreateInfo createInfo = {XR_TYPE_ACTION_CREATE_INFO};
    if (handed)
    {
        createInfo.countSubactionPaths = 2;
        createInfo.subactionPaths = handPaths.data();
    }

    const ea::string localizedName = localization->Get(name);
    strcpy_s(createInfo.actionName, 64, name.c_str());
    strcpy_s(createInfo.localizedActionName, 128, localizedName.c_str());

    const auto type = ParseBindingType(typeName);
    if (!type)
    {
        URHO3D_LOGERROR("Unknown XR action type '{}' for action '{}'", typeName, name);
        return {};
    }
    createInfo.actionType = ToActionType(*type);

    XrAction action{};
    if (!URHO3D_CHECK_OPENXR(xrCreateAction(actionSet, &createInfo, &action)))
        return {};
    const auto wrappedAction = XrActionPtr(action, xrDestroyAction);

    const bool needActionSpace = createInfo.actionType == XR_ACTION_TYPE_POSE_INPUT;
    const auto actionSpaces =
        needActionSpace ? CreateActionSpaces(instance, session, action, handed) : ea::pair<XrSpacePtr, XrSpacePtr>{};

    if (handed)
    {
        const bool isPose = element.GetBool("grip");
        const bool isAimPose = element.GetBool("aim");

        const auto bindingLeft = MakeShared<OpenXRBinding>(context, name, localizedName, //
            VR_HAND_LEFT, *type, isPose, isAimPose, actionSet, wrappedAction, handPaths[VR_HAND_LEFT], actionSpaces.first);
        const auto bindingRight = MakeShared<OpenXRBinding>(context, name, localizedName, //
            VR_HAND_RIGHT, *type, isPose, isAimPose, actionSet, wrappedAction, handPaths[VR_HAND_RIGHT], actionSpaces.second);

        return {bindingLeft, bindingRight};
    }
    else
    {
        const auto binding = MakeShared<OpenXRBinding>(context, name, localizedName, //
            VR_HAND_NONE, *type, false, false, actionSet, wrappedAction, XrPath{}, actionSpaces.first);
        return {binding, binding};
    }
}

void SuggestInteractionProfile(XrInstance instance, XMLElement element, OpenXRActionGroup* actionGroup)
{
    const ea::string device = element.GetAttribute("device");
    XrPath devicePath{};
    xrStringToPath(instance, device.c_str(), &devicePath);

    XrInteractionProfileSuggestedBinding suggest = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggest.interactionProfile = devicePath;

    ea::vector<XrActionSuggestedBinding> bindings;
    for (auto child = element.GetChild("bind"); child.NotNull(); child = child.GetNext("bind"))
    {
        ea::string action = child.GetAttribute("action");
        ea::string bindPathString = child.GetAttribute("path");

        XrPath bindPath;
        xrStringToPath(instance, bindPathString.c_str(), &bindPath);

        if (OpenXRBinding* binding = actionGroup->FindBindingImpl(action))
        {
            XrActionSuggestedBinding suggestedBinding{};
            suggestedBinding.action = binding->action_.Raw();
            suggestedBinding.binding = bindPath;
            bindings.push_back(suggestedBinding);
        }
    }

    if (!bindings.empty())
    {
        suggest.countSuggestedBindings = bindings.size();
        suggest.suggestedBindings = bindings.data();

        URHO3D_CHECK_OPENXR(xrSuggestInteractionProfileBindings(instance, &suggest));
    }
}

SharedPtr<OpenXRActionGroup> CreateActionGroup(
    XrInstance instance, XrSession session, XMLElement element, const StringVector& activeExtensions)
{
    Context* context = Context::GetInstance();
    auto localization = context->GetSubsystem<Localization>();

    const ea::string name = element.GetAttribute("name");
    const ea::string localizedName = localization->Get(name);

    XrActionSetCreateInfo createInfo = {XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(createInfo.actionSetName, name.c_str(), XR_MAX_ACTION_SET_NAME_SIZE);
    strncpy(createInfo.localizedActionSetName, localizedName.c_str(), XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE);

    XrActionSet actionSet{};
    if (!URHO3D_CHECK_OPENXR(xrCreateActionSet(instance, &createInfo, &actionSet)))
        return nullptr;

    const auto wrappedActionSet = XrActionSetPtr(actionSet, xrDestroyActionSet);
    auto actionGroup = MakeShared<OpenXRActionGroup>(context, name, localizedName, wrappedActionSet);

    auto actionsElement = element.GetChild("actions");
    for (auto child = actionsElement.GetChild("action"); child.NotNull(); child = child.GetNext("action"))
    {
        const auto [bindingLeft, bindingRight] = CreateBinding(instance, session, actionSet, child);
        if (!bindingLeft || !bindingRight)
            return nullptr;

        actionGroup->AddBinding(bindingLeft);
        if (bindingLeft != bindingRight)
            actionGroup->AddBinding(bindingRight);
    }

    for (auto child = element.GetChild("profile"); child.NotNull(); child = child.GetNext("profile"))
    {
        const ea::string extension = child.GetAttribute("extension");
        if (!extension.empty() && !IsExtensionSupported(activeExtensions, extension.c_str()))
            continue;

        SuggestInteractionProfile(instance, child, actionGroup);
    }

    return actionGroup;
}

} // namespace

SharedPtr<Node> LoadGLTFModel(Context* ctx, tinygltf::Model& model);

#define XR_INIT_TYPE(D, T) for (auto& a : D) a.type = T

OpenXRBinding::OpenXRBinding(Context* context, const ea::string& name, const ea::string& localizedName, VRHand hand,
    VariantType dataType, bool isPose, bool isAimPose, XrActionSet set, XrActionPtr action, XrPath subPath,
    XrSpacePtr actionSpace)
    : XRBinding(context, name, localizedName, hand, dataType, isPose, isAimPose)
    , action_(action)
    , set_(set)
    , subPath_(subPath)
    , actionSpace_(actionSpace)
{
}

OpenXRActionGroup::OpenXRActionGroup(
    Context* context, const ea::string& name, const ea::string& localizedName, XrActionSetPtr set)
    : XRActionGroup(context, name, localizedName)
    , actionSet_(set)
{
}

void OpenXRActionGroup::AddBinding(OpenXRBinding* binding)
{
    bindings_.emplace_back(binding);
}

OpenXRBinding* OpenXRActionGroup::FindBindingImpl(const ea::string& name)
{
    return static_cast<OpenXRBinding*>(XRActionGroup::FindBinding(name, VR_HAND_NONE));
}

void OpenXRActionGroup::AttachToSession(XrSession session)
{
    XrActionSet actionSets[] = {actionSet_.Raw()};

    XrSessionActionSetsAttachInfo attachInfo = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.actionSets = actionSets;
    attachInfo.countActionSets = 1;
    xrAttachSessionActionSets(session, &attachInfo);
}

void OpenXRActionGroup::Synchronize(XrSession session)
{
    XrActiveActionSet activeSet = {};
    activeSet.actionSet = actionSet_.Raw();

    XrActionsSyncInfo sync = {XR_TYPE_ACTIONS_SYNC_INFO};
    sync.activeActionSets = &activeSet;
    sync.countActiveActionSets = 1;
    xrSyncActions(session, &sync);
}

OpenXR::OpenXR(Context* ctx)
    : BaseClassName(ctx)
{
    SubscribeToEvent(E_BEGINFRAME, &OpenXR::HandlePreUpdate);
    SubscribeToEvent(E_ENDRENDERING, &OpenXR::HandlePostRender);
}

OpenXR::~OpenXR()
{
    // TODO(xr): We shouldn't need this call
    ShutdownSession();
}

bool OpenXR::InitializeSystem(RenderBackend backend)
{
    if (instance_)
    {
        URHO3D_LOGERROR("OpenXR is already initialized");
        return false;
    }

    InitializeOpenXRLoader();

    supportedExtensions_ = EnumerateExtensionsXR();
    if (!IsExtensionSupported(supportedExtensions_, GetBackendExtensionName(backend)))
    {
        URHO3D_LOGERROR("Renderer backend is not supported by OpenXR runtime");
        return false;
    }

    InitializeActiveExtensions(backend);

    auto engine = GetSubsystem<Engine>();
    const ea::string& engineName = "Rebel Fork of Urho3D";
    const ea::string& applicationName = engine->GetParameter(EP_APPLICATION_NAME).GetString();
    instance_ = CreateInstanceXR(activeExtensions_, engineName, applicationName);
    if (!instance_)
        return false;

    XrInstanceProperties instProps = {XR_TYPE_INSTANCE_PROPERTIES};
    if (xrGetInstanceProperties(instance_.Raw(), &instProps) == XR_SUCCESS)
        URHO3D_LOGINFO("OpenXR Runtime is: {} version 0x{:x}", instProps.runtimeName, instProps.runtimeVersion);

    if (features_.debugOutput_)
        debugMessenger_ = CreateDebugMessengerXR(instance_.Raw());

    const auto systemId = GetSystemXR(instance_.Raw());
    if (!systemId)
        return false;

    system_ = *systemId;
    systemName_ = GetSystemNameXR(instance_.Raw(), system_);

    const auto blendModes = GetBlendModesXR(instance_.Raw(), system_);
    if (blendModes.empty())
        return false;

    blendMode_ = blendModes[0];

    const auto viewConfigurations = GetViewConfigurationsXR(instance_.Raw(), system_);
    if (!viewConfigurations.contains(XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO))
    {
        URHO3D_LOGERROR("Stereo rendering not supported on this device");
        return false;
    }

    const auto views = GetViewConfigurationViewsXR(instance_.Raw(), system_);
    if (views.empty())
        return false;

    recommendedMultiSample_ = views[VR_EYE_LEFT].recommendedSwapchainSampleCount;
    recommendedEyeTextureSize_.x_ =
        ea::min(views[VR_EYE_LEFT].recommendedImageRectWidth, views[VR_EYE_RIGHT].recommendedImageRectWidth);
    recommendedEyeTextureSize_.y_ =
        ea::min(views[VR_EYE_LEFT].recommendedImageRectHeight, views[VR_EYE_RIGHT].recommendedImageRectHeight);

    if (!InitializeTweaks(backend))
        return false;

    return true;
}

void OpenXR::InitializeActiveExtensions(RenderBackend backend)
{
    activeExtensions_ = {GetBackendExtensionName(backend)};

    features_.debugOutput_ = ActivateOptionalExtension( //
        activeExtensions_, supportedExtensions_, XR_EXT_DEBUG_UTILS_EXTENSION_NAME);
    features_.visibilityMask_ = ActivateOptionalExtension( //
        activeExtensions_, supportedExtensions_, XR_KHR_VISIBILITY_MASK_EXTENSION_NAME);
    features_.controllerModel_ = ActivateOptionalExtension( //
        activeExtensions_, supportedExtensions_, XR_MSFT_CONTROLLER_MODEL_EXTENSION_NAME);
    features_.depthLayer_ = ActivateOptionalExtension(
        activeExtensions_, supportedExtensions_, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);

    // Controllers
    ActivateOptionalExtension(
        activeExtensions_, supportedExtensions_, XR_HTC_VIVE_COSMOS_CONTROLLER_INTERACTION_EXTENSION_NAME);
    ActivateOptionalExtension(
        activeExtensions_, supportedExtensions_, XR_HTC_VIVE_FOCUS3_CONTROLLER_INTERACTION_EXTENSION_NAME);
    ActivateOptionalExtension( //
        activeExtensions_, supportedExtensions_, XR_EXT_HP_MIXED_REALITY_CONTROLLER_EXTENSION_NAME);
    ActivateOptionalExtension( //
        activeExtensions_, supportedExtensions_, XR_EXT_SAMSUNG_ODYSSEY_CONTROLLER_EXTENSION_NAME);

    for (const ea::string& extension : userExtensions_)
        ActivateOptionalExtension(activeExtensions_, supportedExtensions_, extension.c_str());
}

bool OpenXR::InitializeTweaks(RenderBackend backend)
{
    if (IsNativeOculusQuest2())
        tweaks_.orientation_ = ea::string{"LandscapeRight"};

#if VULKAN_SUPPORTED
    if (backend == RenderBackend::Vulkan)
    {
        tweaks_.vulkanInstanceExtensions_ = GetVulkanInstanceExtensionsXR(instance_.Raw(), system_);
        tweaks_.vulkanDeviceExtensions_ = GetVulkanDeviceExtensionsXR(instance_.Raw(), system_);

        // TODO: If we want to know required physical device ahead of time,
        // we should create dedicated OpenXR instance and system for this check.
        return true;
    }
#endif
    return true;
}

bool OpenXR::InitializeSession(const VRSessionParameters& params)
{
    auto cache = GetSubsystem<ResourceCache>();

    manifest_ = cache->GetResource<XMLFile>(params.manifestPath_);
    if (!manifest_)
    {
        URHO3D_LOGERROR("Unable to load OpenXR manifest '{}'", params.manifestPath_);
        return false;
    }

    multiSample_ = params.multiSample_ ? params.multiSample_ : recommendedMultiSample_;
    eyeTextureSize_ = VectorRoundToInt(recommendedEyeTextureSize_.ToVector2() * params.resolutionScale_);

    if (!OpenSession())
    {
        ShutdownSession();
        return false;
    }

    GetHiddenAreaMask();

    CreateDefaultRig(params.flatScreen_);
    return true;
}

void OpenXR::ShutdownSession()
{
    BaseClassName::ShutdownSession();

    for (int i = 0; i < 2; ++i)
    {
        wandModels_[i] = { };
        handGrips_[i].Reset();
        handAims_[i].Reset();
        handHaptics_[i].Reset();
        views_[i] = { XR_TYPE_VIEW };
    }
    manifest_.Reset();
    actionSets_.clear();
    activeActionSet_.Reset();
    sessionLive_ = false;

    swapChain_ = nullptr;
    depthChain_ = nullptr;

    headSpace_ = nullptr;
    viewSpace_ = nullptr;
    session_ = nullptr;
}

bool OpenXR::OpenSession()
{
    auto renderDevice = GetSubsystem<RenderDevice>();

    session_ = CreateSessionXR(renderDevice, instance_.Raw(), system_);
    if (!session_)
        return false;

    const auto [headSpace, isRoomScale] = CreateHeadSpaceXR(session_.Raw());
    headSpace_ = headSpace;
    isRoomScale_ = isRoomScale;
    viewSpace_ = CreateViewSpaceXR(session_.Raw());

    if (!headSpace_ || !viewSpace_)
        return false;

    if (manifest_)
        BindActions(manifest_);

    // if there's a default action set, then use it.
    VRInterface::SetCurrentActionSet("default");

    // Create swap chains
    const auto internalFormats = GetSwapChainFormats(session_.Raw());
    const auto [colorFormat, colorFormatInternal] = SelectColorFormat(renderDevice->GetBackend(), internalFormats);
    const auto [depthFormat, depthFormatInternal] = SelectDepthFormat(renderDevice->GetBackend(), internalFormats);

    swapChain_ = CreateSwapChainXR(
        GetContext(), session_.Raw(), colorFormat, colorFormatInternal, eyeTextureSize_, multiSample_);
    if (!swapChain_)
        return false;

    if (features_.depthLayer_ && depthFormatInternal)
    {
        depthChain_ = CreateSwapChainXR(
            GetContext(), session_.Raw(), depthFormat, depthFormatInternal, eyeTextureSize_, multiSample_);
    }

    return true;
}

void OpenXR::HandlePreUpdate(StringHash, VariantMap& data)
{
    // Check if we need to do anything at all.
    if (instance_ == 0 || session_ == 0)
        return;

    XrEventDataBuffer eventBuffer = { XR_TYPE_EVENT_DATA_BUFFER };
    while (xrPollEvent(instance_.Raw(), &eventBuffer) == XR_SUCCESS)
    {
        switch (eventBuffer.type)
        {
        case XR_TYPE_EVENT_DATA_VISIBILITY_MASK_CHANGED_KHR:
            GetHiddenAreaMask();
            break;
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            sessionLive_ = false;
            SendEvent(E_VREXIT); //?? does something need to be communicated beyond this?
            break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            UpdateBindingBound();
            SendEvent(E_VRINTERACTIONPROFILECHANGED);
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            XrEventDataSessionStateChanged* changed = (XrEventDataSessionStateChanged*)&eventBuffer;
            auto state = changed->state;
            switch (state)
            {
            case XR_SESSION_STATE_READY: {
                XrSessionBeginInfo beginInfo = { XR_TYPE_SESSION_BEGIN_INFO };
                beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                auto res = xrBeginSession(session_.Raw(), &beginInfo);
                if (res != XR_SUCCESS)
                {
                    URHO3D_LOGERRORF("Failed to begin XR session: %s", xrGetErrorStr(res));
                    sessionLive_ = false;
                    SendEvent(E_VRSESSIONSTART);
                }
                else
                    sessionLive_ = true; // uhhh what
            } break;
            case XR_SESSION_STATE_IDLE:
                SendEvent(E_VRPAUSE);
                sessionLive_ = false;
                break;
            case XR_SESSION_STATE_FOCUSED: // we're hooked up
                sessionLive_ = true;
                SendEvent(E_VRRESUME);
                break;
            case XR_SESSION_STATE_STOPPING:
                xrEndSession(session_.Raw());
                sessionLive_ = false;
                break;
            case XR_SESSION_STATE_EXITING:
            case XR_SESSION_STATE_LOSS_PENDING:
                sessionLive_ = false;
                SendEvent(E_VREXIT);
                break;
            }

        }

        eventBuffer = { XR_TYPE_EVENT_DATA_BUFFER };
    }

    if (!IsLive())
        return;

    XrFrameState frameState = { XR_TYPE_FRAME_STATE };
    xrWaitFrame(session_.Raw(), nullptr, &frameState);
    predictedTime_ = frameState.predictedDisplayTime;

    XrFrameBeginInfo begInfo = { XR_TYPE_FRAME_BEGIN_INFO };
    xrBeginFrame(session_.Raw(), &begInfo);
// head stuff
    headLoc_.next = &headVel_;
    xrLocateSpace(viewSpace_.Raw(), headSpace_.Raw(), frameState.predictedDisplayTime, &headLoc_);

    HandlePreRender();

    for (int i = 0; i < 2; ++i)
    {
        if (handAims_[i])
        {
            // ensure velocity is linked
            handAims_[i]->location_.next = &handAims_[i]->velocity_;
            xrLocateSpace(handAims_[i]->actionSpace_.Raw(), headSpace_.Raw(), frameState.predictedDisplayTime, &handAims_[i]->location_);
        }

        if (handGrips_[i])
        {
            handGrips_[i]->location_.next = &handGrips_[i]->velocity_;
            xrLocateSpace(handGrips_[i]->actionSpace_.Raw(), headSpace_.Raw(), frameState.predictedDisplayTime, &handGrips_[i]->location_);
        }
    }

// eyes
    XrViewLocateInfo viewInfo = { XR_TYPE_VIEW_LOCATE_INFO };
    viewInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    viewInfo.space = headSpace_.Raw();
    viewInfo.displayTime = frameState.predictedDisplayTime;

    XrViewState viewState = { XR_TYPE_VIEW_STATE };
    unsigned viewCt = 0;
    xrLocateViews(session_.Raw(), &viewInfo, &viewState, 2, &viewCt, views_);

// handle actions
    if (activeActionSet_)
    {
        auto setImpl = static_cast<OpenXRActionGroup*>(activeActionSet_.Get());
        setImpl->Synchronize(session_.Raw());

        using namespace BeginFrame;
        UpdateBindings(data[P_TIMESTEP].GetFloat());
    }

    ValidateCurrentRig();
    UpdateCurrentRig();
    UpdateHands();
}

void OpenXR::HandlePreRender()
{
    if (IsLive())
    {
        XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
        unsigned imgID;
        auto res = xrAcquireSwapchainImage(swapChain_->GetHandle(), &acquireInfo, &imgID);
        if (res != XR_SUCCESS)
        {
            URHO3D_LOGERRORF("Failed to acquire swapchain: %s", xrGetErrorStr(res));
            return;
        }

        XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
        waitInfo.timeout = XR_INFINITE_DURATION;
        res = xrWaitSwapchainImage(swapChain_->GetHandle(), &waitInfo);
        if (res != XR_SUCCESS)
            URHO3D_LOGERRORF("Failed to wait on swapchain: %s", xrGetErrorStr(res));

        // update which shared-texture we're using so UpdateRig will do things correctly.
        currentBackBufferColor_ = swapChain_->GetTexture(imgID);

        // If we've got depth then do the same and setup the linked depth stencil for the above shared texture.
        if (depthChain_)
        {
            // still remaking the objects here, assuming that at any time these may one day do something
            // in such a fashion that reuse is not a good thing.
            unsigned depthID;
            XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
            auto res = xrAcquireSwapchainImage(depthChain_->GetHandle(), &acquireInfo, &depthID);
            if (res == XR_SUCCESS)
            {
                XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
                waitInfo.timeout = XR_INFINITE_DURATION;
                res = xrWaitSwapchainImage(depthChain_->GetHandle(), &waitInfo);
                currentBackBufferDepth_ = depthChain_->GetTexture(depthID);
                currentBackBufferColor_->GetRenderSurface()->SetLinkedDepthStencil(currentBackBufferDepth_->GetRenderSurface());
            }
        }
    }
}

void OpenXR::HandlePostRender(StringHash, VariantMap&)
{
    if (IsLive())
    {
#define CHECKVIEW(EYE) (views_[EYE].fov.angleLeft == 0 || views_[EYE].fov.angleRight == 0 || views_[EYE].fov.angleUp == 0 || views_[EYE].fov.angleDown == 0)

        auto renderDevice = GetSubsystem<RenderDevice>();
        renderDevice->GetImmediateContext()->Flush();

        XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
        xrReleaseSwapchainImage(swapChain_->GetHandle(), &releaseInfo);
        if (depthChain_)
        {
            XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            xrReleaseSwapchainImage(depthChain_->GetHandle(), &releaseInfo);
        }

        // it's harmless but checking this will prevent early bad draws with null FOV
        // XR eats the error, but handle it anyways to keep a clean output log
        if (CHECKVIEW(VR_EYE_LEFT) || CHECKVIEW(VR_EYE_RIGHT))
            return;

        XrCompositionLayerProjectionView eyes[2] = { { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW }, { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW } };
        eyes[VR_EYE_LEFT].subImage.imageArrayIndex = 0;
        eyes[VR_EYE_LEFT].subImage.swapchain = swapChain_->GetHandle();
        eyes[VR_EYE_LEFT].subImage.imageRect = { { 0, 0 }, { eyeTextureSize_.x_, eyeTextureSize_.y_} };
        eyes[VR_EYE_LEFT].fov = views_[VR_EYE_LEFT].fov;
        eyes[VR_EYE_LEFT].pose = views_[VR_EYE_LEFT].pose;

        eyes[VR_EYE_RIGHT].subImage.imageArrayIndex = 0;
        eyes[VR_EYE_RIGHT].subImage.swapchain = swapChain_->GetHandle();
        eyes[VR_EYE_RIGHT].subImage.imageRect = { { eyeTextureSize_.x_, 0 }, { eyeTextureSize_.x_, eyeTextureSize_.y_} };
        eyes[VR_EYE_RIGHT].fov = views_[VR_EYE_RIGHT].fov;
        eyes[VR_EYE_RIGHT].pose = views_[VR_EYE_RIGHT].pose;

        static XrCompositionLayerDepthInfoKHR depth[2] = {
                { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR }, { XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR }
        };

        if (depthChain_)
        {
            // depth
            depth[VR_EYE_LEFT].subImage.imageArrayIndex = 0;
            depth[VR_EYE_LEFT].subImage.swapchain = depthChain_->GetHandle();
            depth[VR_EYE_LEFT].subImage.imageRect = { { 0, 0 }, { eyeTextureSize_.x_, eyeTextureSize_.y_} };
            depth[VR_EYE_LEFT].minDepth = 0.0f; // spec says range of 0-1, so doesn't respect GL -1 to 1?
            depth[VR_EYE_LEFT].maxDepth = 1.0f;
            depth[VR_EYE_LEFT].nearZ = rig_.nearDistance_;
            depth[VR_EYE_LEFT].farZ = rig_.farDistance_;

            depth[VR_EYE_RIGHT].subImage.imageArrayIndex = 0;
            depth[VR_EYE_RIGHT].subImage.swapchain = depthChain_->GetHandle();
            depth[VR_EYE_RIGHT].subImage.imageRect = { { eyeTextureSize_.x_, 0 }, { eyeTextureSize_.x_, eyeTextureSize_.y_} };
            depth[VR_EYE_RIGHT].minDepth = 0.0f;
            depth[VR_EYE_RIGHT].maxDepth = 1.0f;
            depth[VR_EYE_RIGHT].nearZ = rig_.nearDistance_;
            depth[VR_EYE_RIGHT].farZ = rig_.farDistance_;

            // These are chained to the relevant eye, not passed in through another mechanism.

            /* not attached at present as it's messed up, probably as referenced above in depth-info ext detection that it's probably a RenderBuffermanager copy issue */
            //eyes[VR_EYE_LEFT].next = &depth[VR_EYE_LEFT];
            //eyes[VR_EYE_RIGHT].next = &depth[VR_EYE_RIGHT];
        }

        XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
        proj.viewCount = 2;
        proj.views = eyes;
        proj.space = headSpace_.Raw();

        XrCompositionLayerBaseHeader* header = (XrCompositionLayerBaseHeader*)&proj;

        XrFrameEndInfo endInfo = { XR_TYPE_FRAME_END_INFO };
        endInfo.layerCount = 1;
        endInfo.layers = &header;
        endInfo.environmentBlendMode = blendMode_;
        endInfo.displayTime = predictedTime_;

        xrEndFrame(session_.Raw(), &endInfo);
    }
}

void OpenXR::BindActions(XMLFile* xmlFile)
{
    auto rootElement = xmlFile->GetRoot();
    for (auto child = rootElement.GetChild("actionset"); child.NotNull(); child = child.GetNext("actionset"))
    {
        auto actionGroup = CreateActionGroup(instance_.Raw(), session_.Raw(), child, activeExtensions_);
        actionSets_.insert({actionGroup->GetName(), actionGroup});
    }

    UpdateBindingBound();
}

void OpenXR::SetCurrentActionSet(SharedPtr<XRActionGroup> set)
{
    if (session_ && set != nullptr)
    {
        activeActionSet_ = set;

        const auto setImpl = static_cast<OpenXRActionGroup*>(set.Get());
        setImpl->AttachToSession(session_.Raw());
        UpdateBindingBound();
    }
}

void OpenXR::UpdateBindings(float t)
{
    if (instance_ == 0)
        return;

    if (!IsLive())
        return;

    auto& eventData = GetEventDataMap();
    using namespace VRBindingChange;

    eventData[VRBindingChange::P_ACTIVE] = true;

    for (auto b : activeActionSet_->GetBindings())
    {
        auto bind = b->Cast<OpenXRBinding>();
        if (bind->action_)
        {
            eventData[P_NAME] = bind->localizedName_;
            eventData[P_BINDING] = bind;

#define SEND_EVENT eventData[P_DATA] = bind->storedData_; eventData[P_DELTA] = bind->delta_;

            XrActionStateGetInfo getInfo = { XR_TYPE_ACTION_STATE_GET_INFO };
            getInfo.action = bind->action_.Raw();
            getInfo.subactionPath = bind->subPath_;

            switch (bind->dataType_)
            {
            case VAR_BOOL: {
                XrActionStateBoolean boolC = { XR_TYPE_ACTION_STATE_BOOLEAN };
                if (xrGetActionStateBoolean(session_.Raw(), &getInfo, &boolC) == XR_SUCCESS)
                {
                    bind->active_ = boolC.isActive;
                    if (boolC.changedSinceLastSync)
                    {
                        bind->storedData_ = boolC.currentState;
                        bind->changed_ = true;
                        SEND_EVENT;
                    }
                    else
                        bind->changed_ = false;
                }
            }
                break;
            case VAR_FLOAT: {
                XrActionStateFloat floatC = { XR_TYPE_ACTION_STATE_FLOAT };
                if (xrGetActionStateFloat(session_.Raw(), &getInfo, &floatC) == XR_SUCCESS)
                {
                    bind->active_ = floatC.isActive;
                    if (floatC.changedSinceLastSync || !Equals(floatC.currentState, bind->GetFloat()))
                    {
                        bind->storedData_ = floatC.currentState;
                        bind->changed_ = true;
                        SEND_EVENT;
                    }
                    else
                        bind->changed_ = false;
                }
            }
                break;
            case VAR_VECTOR2: {
                XrActionStateVector2f vec = { XR_TYPE_ACTION_STATE_VECTOR2F };
                if (xrGetActionStateVector2f(session_.Raw(), &getInfo, &vec) == XR_SUCCESS)
                {
                    bind->active_ = vec.isActive;
                    Vector2 v(vec.currentState.x, vec.currentState.y);
                    if (vec.changedSinceLastSync)
                    {
                        bind->storedData_ = v;
                        bind->changed_ = true;
                        SEND_EVENT;
                    }
                    else
                        bind->changed_ = false;
                }
            }
                break;
            case VAR_VECTOR3: {
                XrActionStatePose pose = { XR_TYPE_ACTION_STATE_POSE };
                if (xrGetActionStatePose(session_.Raw(), &getInfo, &pose) == XR_SUCCESS)
                {
                    // Should we be sending events for these? As it's tracking sensor stuff I think not? It's effectively always changing and we know that's the case.
                    bind->active_ = pose.isActive;
                    Vector3 v = ToVector3(bind->location_.pose.position) * scaleCorrection_;
                    bind->storedData_ = v;
                    bind->changed_ = true;
                    bind->extraData_[0] = ToVector3(bind->velocity_.linearVelocity) * scaleCorrection_;
                }
            } break;
            case VAR_MATRIX3X4: {
                XrActionStatePose pose = { XR_TYPE_ACTION_STATE_POSE };
                if (xrGetActionStatePose(session_.Raw(), &getInfo, &pose) == XR_SUCCESS)
                {
                    // Should we be sending events for these? As it's tracking sensor stuff I think not? It's effectively always changing and we know that's the case.
                    bind->active_ = pose.isActive;
                    Matrix3x4 m = ToMatrix3x4(bind->location_.pose, scaleCorrection_);
                    bind->storedData_ = m;
                    bind->changed_ = true;
                    bind->extraData_[0] = ToVector3(bind->velocity_.linearVelocity) * scaleCorrection_;
                    bind->extraData_[1] = ToVector3(bind->velocity_.angularVelocity) * scaleCorrection_;
                }
            } break;
            }
        }
    }

#undef SEND_EVENT
}

void OpenXR::GetHiddenAreaMask()
{
    // extension wasn't supported
    if (!features_.visibilityMask_)
        return;

    for (int eye = 0; eye < 2; ++eye)
    {
        XrVisibilityMaskKHR mask = { XR_TYPE_VISIBILITY_MASK_KHR };
    // hidden
        {

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);

            ea::vector<XrVector2f> verts;
            verts.resize(mask.vertexCountOutput);
            ea::vector<unsigned> indices;
            indices.resize(mask.indexCountOutput);

            mask.vertexCapacityInput = verts.size();
            mask.indexCapacityInput = indices.size();

            mask.vertices = verts.data();
            mask.indices = indices.data();

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_HIDDEN_TRIANGLE_MESH_KHR, &mask);

            ea::vector<Vector3> vtxData;
            vtxData.resize(verts.size());
            for (unsigned i = 0; i < verts.size(); ++i)
                vtxData[i] = Vector3(verts[i].x, verts[i].y, 0.0f);

            VertexBuffer* vtx = new VertexBuffer(GetContext());
            vtx->SetSize(vtxData.size(), { VertexElement(TYPE_VECTOR3, SEM_POSITION) });
            vtx->Update(vtxData.data());

            IndexBuffer* idx = new IndexBuffer(GetContext());
            idx->SetSize(indices.size(), true);
            idx->Update(indices.data());

            hiddenAreaMesh_[eye] = new Geometry(GetContext());
            hiddenAreaMesh_[eye]->SetVertexBuffer(0, vtx);
            hiddenAreaMesh_[eye]->SetIndexBuffer(idx);
            hiddenAreaMesh_[eye]->SetDrawRange(TRIANGLE_LIST, 0, indices.size());
        }

    // visible
        {
            mask.indexCapacityInput = 0;
            mask.vertexCapacityInput = 0;
            mask.indices = nullptr;
            mask.vertices = nullptr;
            mask.indexCountOutput = 0;
            mask.vertexCountOutput = 0;

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask);

            ea::vector<XrVector2f> verts;
            verts.resize(mask.vertexCountOutput);
            ea::vector<unsigned> indices;
            indices.resize(mask.indexCountOutput);

            mask.vertexCapacityInput = verts.size();
            mask.indexCapacityInput = indices.size();

            mask.vertices = verts.data();
            mask.indices = indices.data();

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_VISIBLE_TRIANGLE_MESH_KHR, &mask);

            ea::vector<Vector3> vtxData;
            vtxData.resize(verts.size());
            for (unsigned i = 0; i < verts.size(); ++i)
                vtxData[i] = Vector3(verts[i].x, verts[i].y, 0.0f);

            VertexBuffer* vtx = new VertexBuffer(GetContext());
            vtx->SetSize(vtxData.size(), { VertexElement(TYPE_VECTOR3, SEM_POSITION) });
            vtx->Update(vtxData.data());

            IndexBuffer* idx = new IndexBuffer(GetContext());
            idx->SetSize(indices.size(), true);
            idx->Update(indices.data());

            visibleAreaMesh_[eye] = new Geometry(GetContext());
            visibleAreaMesh_[eye]->SetVertexBuffer(0, vtx);
            visibleAreaMesh_[eye]->SetIndexBuffer(idx);
            visibleAreaMesh_[eye]->SetDrawRange(TRIANGLE_LIST, 0, indices.size());
        }

    // build radial from line loop, a centroid is calculated and the triangles are laid out in a fan
        {
            // Maybe do this several times for a couple of different sizes, to do strips that ring
            // the perimiter at different %s to save on overdraw. ie. ring 25%, ring 50%, center 25% and center 50%?
            // Then vignettes only need to do their work where actually required. A 25% distance outer ring is
            // in projected space massively smaller than 25% of FOV, likewise with a 50% outer ring, though less so.
            // Question is whether to ring in reference to centroid or to the line geometry as mitred?

            mask.indexCapacityInput = 0;
            mask.vertexCapacityInput = 0;
            mask.indices = nullptr;
            mask.vertices = nullptr;
            mask.indexCountOutput = 0;
            mask.vertexCountOutput = 0;

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &mask);

            ea::vector<XrVector2f> verts;
            verts.resize(mask.vertexCountOutput);
            ea::vector<unsigned> indices;
            indices.resize(mask.indexCountOutput);

            mask.vertexCapacityInput = verts.size();
            mask.indexCapacityInput = indices.size();

            mask.vertices = verts.data();
            mask.indices = indices.data();

            xrGetVisibilityMaskKHR(session_.Raw(), XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, eye, XR_VISIBILITY_MASK_TYPE_LINE_LOOP_KHR, &mask);

            struct V {
                Vector3 pos;
                unsigned color;
            };

            ea::vector<V> vtxData;
            vtxData.resize(verts.size());
            Vector3 centroid = Vector3::ZERO;
            Vector3 minVec = Vector3(10000, 10000, 10000);
            Vector3 maxVec = Vector3(-10000, -10000, -10000);

            const unsigned whiteColor = Color::WHITE.ToUInt();
            const unsigned transWhiteColor = Color(1.0f, 1.0f, 1.0f, 0.0f).ToUInt();

            for (unsigned i = 0; i < verts.size(); ++i)
            {
                vtxData[i] = { Vector3(verts[i].x, verts[i].y, 0.0f), whiteColor };
                centroid += vtxData[i].pos;
            }
            centroid /= verts.size();

            ea::vector<unsigned short> newIndices;
            vtxData.push_back({ { centroid.x_, centroid.y_, 0.0f }, transWhiteColor });

            // turn the line loop into a fan
            for (unsigned i = 0; i < indices.size(); ++i)
            {
                unsigned me = indices[i];
                unsigned next = indices[(i + 1) % indices.size()];

                newIndices.push_back(vtxData.size() - 1); // center is at the end
                newIndices.push_back(me);
                newIndices.push_back(next);
            }

            VertexBuffer* vtx = new VertexBuffer(GetContext());
            vtx->SetSize(vtxData.size(), { VertexElement(TYPE_VECTOR3, SEM_POSITION), VertexElement(TYPE_UBYTE4_NORM, SEM_COLOR) });
            vtx->Update(vtxData.data());

            IndexBuffer* idx = new IndexBuffer(GetContext());
            idx->SetSize(newIndices.size(), false);
            idx->Update(newIndices.data());

            radialAreaMesh_[eye] = new Geometry(GetContext());
            radialAreaMesh_[eye]->SetVertexBuffer(0, vtx);
            radialAreaMesh_[eye]->SetIndexBuffer(idx);
            radialAreaMesh_[eye]->SetDrawRange(TRIANGLE_LIST, 0, newIndices.size());
        }
    }
}

void OpenXR::LoadControllerModels()
{
    if (!features_.controllerModel_ || !IsLive())
        return;

    XrPath handPaths[2];
    xrStringToPath(instance_.Raw(), "/user/hand/left", &handPaths[0]);
    xrStringToPath(instance_.Raw(), "/user/hand/right", &handPaths[1]);

    XrControllerModelKeyStateMSFT states[2] = { { XR_TYPE_CONTROLLER_MODEL_KEY_STATE_MSFT }, { XR_TYPE_CONTROLLER_MODEL_KEY_STATE_MSFT } };
    XrResult errCodes[2];
    errCodes[0] = xrGetControllerModelKeyMSFT(session_.Raw(), handPaths[0], &states[0]);
    errCodes[1] = xrGetControllerModelKeyMSFT(session_.Raw(), handPaths[1], &states[1]);

    for (int i = 0; i < 2; ++i)
    {
        // skip if we're the same, we could change
        if (states[i].modelKey == wandModels_[i].modelKey_)
            continue;

        wandModels_[i].modelKey_ = states[i].modelKey;

        if (errCodes[i] == XR_SUCCESS)
        {
            unsigned dataSize = 0;
            auto loadErr = xrLoadControllerModelMSFT(session_.Raw(), states[i].modelKey, 0, &dataSize, nullptr);
            if (loadErr == XR_SUCCESS)
            {
                std::vector<unsigned char> data;
                data.resize(dataSize);

                // Can we actually fail in this case if the above was successful? Assuming that data/data-size are correct I would expect not?
                if (xrLoadControllerModelMSFT(session_.Raw(), states[i].modelKey, data.size(), &dataSize, data.data()) == XR_SUCCESS)
                {
                    tinygltf::Model model;
                    tinygltf::TinyGLTF ctx;
                    tinygltf::Scene scene;

                    std::string err, warn;
                    if (ctx.LoadBinaryFromMemory(&model, &err, &warn, data.data(), data.size()))
                    {
                        wandModels_[i].model_ = LoadGLTFModel(GetContext(), model);
                    }
                    else
                        wandModels_[i].model_.Reset();

                    XR_INIT_TYPE(wandModels_[i].properties_, XR_TYPE_CONTROLLER_MODEL_NODE_PROPERTIES_MSFT);

                    XrControllerModelPropertiesMSFT props = { XR_TYPE_CONTROLLER_MODEL_PROPERTIES_MSFT };
                    props.nodeCapacityInput = 256;
                    props.nodeCountOutput = 0;
                    props.nodeProperties = wandModels_[i].properties_;
                    if (xrGetControllerModelPropertiesMSFT(session_.Raw(), states[i].modelKey, &props) == XR_SUCCESS)
                    {
                        wandModels_[i].numProperties_ = props.nodeCountOutput;
                    }
                    else
                        wandModels_[i].numProperties_ = 0;

                    auto& data = GetEventDataMap();
                    data[VRControllerChange::P_HAND] = i;
                    SendEvent(E_VRCONTROLLERCHANGE, data);
                }
            }
            else
                URHO3D_LOGERRORF("xrLoadControllerModelMSFT failure: %s", xrGetErrorStr(errCodes[i]));
        }
        else
            URHO3D_LOGERRORF("xrGetControllerModelKeyMSFT failure: %s", xrGetErrorStr(errCodes[i]));
    }
}

SharedPtr<Node> OpenXR::GetControllerModel(VRHand hand)
{
    return wandModels_[hand].model_ != nullptr ? wandModels_[hand].model_ : nullptr;
}

void OpenXR::UpdateControllerModel(VRHand hand, SharedPtr<Node> model)
{
    if (!features_.controllerModel_)
        return;

    if (model == nullptr)
        return;

    if (wandModels_[hand].modelKey_ == 0)
        return;

    // nothing to animate
    if (wandModels_[hand].numProperties_ == 0)
        return;

    XrControllerModelNodeStateMSFT nodeStates[256];
    XR_INIT_TYPE(nodeStates, XR_TYPE_CONTROLLER_MODEL_NODE_STATE_MSFT);

    XrControllerModelStateMSFT state = { XR_TYPE_CONTROLLER_MODEL_STATE_MSFT };
    state.nodeCapacityInput = 256;
    state.nodeStates = nodeStates;

    auto errCode = xrGetControllerModelStateMSFT(session_.Raw(), wandModels_[hand].modelKey_, &state);
    if (errCode == XR_SUCCESS)
    {
        auto node = model;
        for (unsigned i = 0; i < state.nodeCountOutput; ++i)
        {
            SharedPtr<Node> bone;

            // If we've got a parent name, first seek that out. OXR allows name collisions, parent-name disambiguates.
            if (strlen(wandModels_[hand].properties_[i].parentNodeName))
            {
                if (auto parent = node->GetChild(wandModels_[hand].properties_[i].parentNodeName, true))
                    bone = parent->GetChild(wandModels_[hand].properties_[i].nodeName);
            }
            else
                bone = node->GetChild(wandModels_[hand].properties_[i].nodeName, true);

            if (bone != nullptr)
            {
                // we have a 1,1,-1 scale at the root to flip gltf coordinate system to ours,
                // because of that this transform needs to be direct and not converted, or it'll get unconverted
                // TODO: figure out how to properly fully flip the gltf nodes and vertices
                Vector3 t = Vector3(nodeStates[i].nodePose.position.x, nodeStates[i].nodePose.position.y, nodeStates[i].nodePose.position.z);
                auto& q = nodeStates[i].nodePose.orientation;
                Quaternion outQ = Quaternion(q.w, q.x, q.y, q.z);

                bone->SetTransformMatrix(Matrix3x4(t, outQ, Vector3(1,1,1)));
            }
        }
    }
}

void OpenXR::TriggerHaptic(VRHand hand, float durationSeconds, float cyclesPerSec, float amplitude)
{
    if (!activeActionSet_ || !IsLive())
        return;

    for (XRBinding* binding : activeActionSet_->GetBindings())
    {
        if (!binding->IsHaptic() || binding->Hand() != hand)
            continue;

        const auto bindingImpl = static_cast<OpenXRBinding*>(binding);

        XrHapticActionInfo info = {XR_TYPE_HAPTIC_ACTION_INFO};
        info.action = bindingImpl->action_.Raw();
        info.subactionPath = bindingImpl->subPath_;

        XrHapticVibration vibration = {XR_TYPE_HAPTIC_VIBRATION};
        vibration.amplitude = amplitude;
        vibration.frequency = cyclesPerSec;
        vibration.duration = durationSeconds * 1000.0f;

        xrApplyHapticFeedback(session_.Raw(), &info, reinterpret_cast<XrHapticBaseHeader*>(&vibration));
    }
}

Matrix3x4 OpenXR::GetHandTransform(VRHand hand) const
{
    if (hand == VR_HAND_NONE)
        return Matrix3x4();

    if (!handGrips_[hand])
        return Matrix3x4();

    auto q = ToQuaternion(handGrips_[hand]->location_.pose.orientation);
    auto v = ToVector3(handGrips_[hand]->location_.pose.position);

    // bring it into head space instead of stage space
    auto headInv = GetHeadTransform().Inverse();
    return headInv * Matrix3x4(v, q, 1.0f);
}

Matrix3x4 OpenXR::GetHandAimTransform(VRHand hand) const
{
    if (hand == VR_HAND_NONE)
        return Matrix3x4();

    if (!handAims_[hand])
        return Matrix3x4();

    // leave this in stage space, that's what we want
    auto q = ToQuaternion(handAims_[hand]->location_.pose.orientation);
    auto v = ToVector3(handAims_[hand]->location_.pose.position);
    return Matrix3x4(v, q, 1.0f);
}

Ray OpenXR::GetHandAimRay(VRHand hand) const
{
    if (hand == VR_HAND_NONE)
        return Ray();

    if (!handAims_[hand])
        return Ray();

    // leave this one is stage space, that's what we want
    auto q = ToQuaternion(handAims_[hand]->location_.pose.orientation);
    auto v = ToVector3(handAims_[hand]->location_.pose.position);
    return Ray(v, (q * Vector3(0, 0, 1)).Normalized());
}

void OpenXR::GetHandVelocity(VRHand hand, Vector3* linear, Vector3* angular) const
{
    if (hand == VR_HAND_NONE)
        return;

    if (!handGrips_[hand])
        return;

    if (linear && handGrips_[hand]->velocity_.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT)
        *linear = ToVector3(handGrips_[hand]->velocity_.linearVelocity);
    if (angular && handGrips_[hand]->velocity_.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT)
        *angular = ToVector3(handGrips_[hand]->velocity_.angularVelocity);
}

void OpenXR::UpdateHands()
{
    if (!IsLive() || !rig_.IsValid())
        return;

    // Check for changes in controller model state, if so, do reload as required.
    LoadControllerModels();

    Node* leftHand = rig_.leftHand_;
    Node* rightHand = rig_.rightHand_;

    // we need valid handles for these guys
    if (handGrips_[0] && handGrips_[1])
    {
        // TODO: can we do any tracking of our own such as using QEF for tracking recent velocity integration into position confidence
        // over the past interval of time to decide how much we trust integrating velocity when position has no-confidence / untracked.
        // May be able to fall-off a confidence factor provided the incoming velocity is still there, problem is how to rectify
        // when tracking kicks back in again later. If velocity integration is valid there should be no issue - neither a pop,
        // it'll already pop in a normal position tracking lost recovery situation anyways.

        const Quaternion leftRotation = ToQuaternion(handGrips_[VR_HAND_LEFT]->location_.pose.orientation);
        const Vector3 leftPosition = ToVector3(handGrips_[VR_HAND_LEFT]->location_.pose.position);

        // these fields are super important to rationalize what's happened between sample points
        // sensor reads are effectively Planck timing it between quantum space-time
        leftHand->SetVar("PreviousTransformLocal", leftHand->GetTransformMatrix());
        leftHand->SetVar("PreviousTransformWorld", leftHand->GetWorldTransform());
        leftHand->SetEnabled(handGrips_[VR_HAND_LEFT]->location_.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
        leftHand->SetPosition(leftPosition);
        if (handGrips_[VR_HAND_LEFT]->location_.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
            leftHand->SetRotation(leftRotation);

        const Quaternion rightRotation = ToQuaternion(handGrips_[VR_HAND_RIGHT]->location_.pose.orientation);
        const Vector3 rightPosition = ToVector3(handGrips_[VR_HAND_RIGHT]->location_.pose.position);

        rightHand->SetVar("PreviousTransformLocal", leftHand->GetTransformMatrix());
        rightHand->SetVar("PreviousTransformWorld", leftHand->GetWorldTransform());
        rightHand->SetEnabled(handGrips_[VR_HAND_RIGHT]->location_.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT));
        rightHand->SetPosition(rightPosition);
        if (handGrips_[VR_HAND_RIGHT]->location_.locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
            rightHand->SetRotation(rightRotation);
    }
}

Matrix3x4 OpenXR::GetEyeLocalTransform(VREye eye) const
{
    // TODO: fixme, why is view space not correct xrLocateViews( view-space )
    // one would expect them to be in head relative local space already ... but they're ... not?
    return GetHeadTransform().Inverse() * ToMatrix3x4(views_[eye].pose, scaleCorrection_);
}

Matrix4 OpenXR::GetProjection(VREye eye, float nearDist, float farDist) const
{
    return ToProjectionMatrix(nearDist, farDist, views_[eye].fov);
}

Matrix3x4 OpenXR::GetHeadTransform() const
{
    return ToMatrix3x4(headLoc_.pose, scaleCorrection_);
}

void OpenXR::UpdateBindingBound()
{
    if (session_ == 0)
        return;

    if (activeActionSet_)
    {
        for (auto b : activeActionSet_->GetBindings())
        {
            auto bind = b->Cast<OpenXRBinding>();
            XrBoundSourcesForActionEnumerateInfo info = { XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO };
            info.action = bind->action_.Raw();
            unsigned binds = 0;
            xrEnumerateBoundSourcesForAction(session_.Raw(), &info, 0, &binds, nullptr);
            b->isBound_ = binds > 0;

            if (b->isAimPose_)
                handAims_[b->Hand()] = b->Cast<OpenXRBinding>();
            if (b->isPose_)
                handGrips_[b->Hand()] = b->Cast<OpenXRBinding>();
        }
    }
}

void GLTFRecurseModel(Context* ctx, tinygltf::Model& gltf, Node* parent, int nodeIndex, int parentIndex, Material* mat, Matrix3x4 matStack)
{
    auto& n = gltf.nodes[nodeIndex];

    auto node = parent->CreateChild(n.name.c_str());

    // root node will deal with the 1,1,-1 - so just accept the transforms we get
    // same with vertex data later
    if (n.translation.size())
    {
        Vector3 translation = Vector3(n.translation[0], n.translation[1], n.translation[2]);
        Quaternion rotation = Quaternion(n.rotation[3], n.rotation[0], n.rotation[1], n.rotation[2]);
        Vector3 scale = Vector3(n.scale[0], n.scale[1], n.scale[2]);
        node->SetPosition(translation);
        node->SetRotation(rotation);
        node->SetScale(scale);
    }
    else if (n.matrix.size())
    {
        Matrix3x4 mat = Matrix3x4(
            n.matrix[0], n.matrix[4], n.matrix[8], n.matrix[12],
            n.matrix[1], n.matrix[5], n.matrix[9], n.matrix[13],
            n.matrix[2], n.matrix[6], n.matrix[10], n.matrix[14]
        );
        node->SetTransformMatrix(mat);
    }
    else
        node->SetTransformMatrix(Matrix3x4::IDENTITY);

    if (n.mesh != -1)
    {
        auto& mesh = gltf.meshes[n.mesh];
        BoundingBox bounds;
        bounds.Clear();
        for (auto& prim : mesh.primitives)
        {
            SharedPtr<Geometry> geom(new Geometry(ctx));

            if (prim.mode == TINYGLTF_MODE_TRIANGLES)
            {
                SharedPtr<IndexBuffer> idxBuffer(new IndexBuffer(ctx));
                ea::vector< SharedPtr<VertexBuffer> > vertexBuffers;

                struct Vertex {
                    Vector3 pos;
                    Vector3 norm;
                    Vector2 tex;
                };

                ea::vector<Vertex> verts;
                verts.resize(gltf.accessors[prim.attributes.begin()->second].count);

                for (auto c : prim.attributes)
                {
                    // only known case at the present
                    if (gltf.accessors[c.second].componentType == TINYGLTF_COMPONENT_TYPE_FLOAT)
                    {
                        auto& access = gltf.accessors[c.second];
                        auto& view = gltf.bufferViews[access.bufferView];
                        auto& buffer = gltf.buffers[view.buffer];

                        LegacyVertexElement element;

                        ea::string str(access.name.c_str());
                        if (str.contains("position", false))
                            element = ELEMENT_POSITION;
                        else if (str.contains("texcoord", false))
                            element = ELEMENT_TEXCOORD1;
                        else if (str.contains("normal", false))
                            element = ELEMENT_NORMAL;

                        SharedPtr<VertexBuffer> vtx(new VertexBuffer(ctx));

                        size_t sizeElem = access.type == TINYGLTF_TYPE_VEC2 ? sizeof(Vector2) : sizeof(Vector3);
                        if (access.type == TINYGLTF_TYPE_VEC3)
                        {
                            const float* d = (const float*)&buffer.data[view.byteOffset + access.byteOffset];
                            if (element == ELEMENT_NORMAL)
                            {
                                for (unsigned i = 0; i < access.count; ++i)
                                    verts[i].norm = Vector3(d[i * 3 + 0], d[i * 3 + 1], d[i * 3 + 2]);
                            }
                            else if (element == ELEMENT_POSITION)
                            {
                                for (unsigned i = 0; i < access.count; ++i)
                                    bounds.Merge(verts[i].pos = Vector3(d[i * 3 + 0], d[i * 3 + 1], d[i * 3 + 2]));
                            }
                        }
                        else
                        {
                            const float* d = (const float*)&buffer.data[view.byteOffset + access.byteOffset];
                            for (unsigned i = 0; i < access.count; ++i)
                                verts[i].tex = Vector2(d[i * 2 + 0], d[i * 2 + 1]);
                        }
                    }
                    else
                        URHO3D_LOGERRORF("Found unsupported GLTF component type for vertex data: %u", gltf.accessors[prim.indices].componentType);
                }

                VertexBuffer* buff = new VertexBuffer(ctx);
                buff->SetSize(verts.size(), { VertexElement(TYPE_VECTOR3, SEM_POSITION, 0, 0), VertexElement(TYPE_VECTOR3, SEM_NORMAL, 0, 0), VertexElement(TYPE_VECTOR2, SEM_TEXCOORD, 0, 0) });
                buff->Update(verts.data());
                vertexBuffers.push_back(SharedPtr<VertexBuffer>(buff));

                if (prim.indices != -1)
                {
                    auto& access = gltf.accessors[prim.indices];
                    auto& view = gltf.bufferViews[access.bufferView];
                    auto& buffer = gltf.buffers[view.buffer];

                    if (gltf.accessors[prim.indices].componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT)
                    {
                        ea::vector<unsigned> indexData;
                        indexData.resize(access.count);

                        const unsigned* indices = (const unsigned*)&buffer.data[view.byteOffset + access.byteOffset];
                        for (int i = 0; i < access.count; ++i)
                            indexData[i] = indices[i];

                        idxBuffer->SetSize(access.count, true, false);
                        idxBuffer->Update(indexData.data());
                    }
                    else if (gltf.accessors[prim.indices].componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT)
                    {
                        ea::vector<unsigned short> indexData;
                        indexData.resize(access.count);

                        const unsigned short* indices = (const unsigned short*)&buffer.data[view.byteOffset + access.byteOffset];
                        for (int i = 0; i < access.count; ++i)
                            indexData[i] = indices[i];
                        for (int i = 0; i < indexData.size(); i += 3)
                        {
                            ea::swap(indexData[i], indexData[i + 2]);
                        }

                        idxBuffer->SetSize(access.count, false, false);
                        idxBuffer->Update(indexData.data());
                    }
                    else
                    {
                        URHO3D_LOGERRORF("Found unsupported GLTF component type for index data: %u", gltf.accessors[prim.indices].componentType);
                        continue;
                    }
                }

                SharedPtr<Geometry> geom(new Geometry(ctx));
                geom->SetIndexBuffer(idxBuffer);
                geom->SetNumVertexBuffers(vertexBuffers.size());
                for (unsigned i = 0; i < vertexBuffers.size(); ++i)
                    geom->SetVertexBuffer(i, vertexBuffers[0]);
                geom->SetDrawRange(TRIANGLE_LIST, 0, idxBuffer->GetIndexCount(), false);

                SharedPtr<Model> m(new Model(ctx));
                m->SetNumGeometries(1);
                m->SetGeometry(0, 0, geom);
                m->SetName(mesh.name.c_str());
                m->SetBoundingBox(bounds);

                auto sm = node->CreateComponent<StaticModel>();
                sm->SetModel(m);
                sm->SetMaterial(mat);
            }
        }
    }

    for (auto child : n.children)
        GLTFRecurseModel(ctx, gltf, node, child, nodeIndex, mat, node->GetWorldTransform());
}

SharedPtr<Texture2D> LoadGLTFTexture(Context* ctx, tinygltf::Model& gltf, int index)
{
    auto img = gltf.images[index];
    SharedPtr<Texture2D> tex(new Texture2D(ctx));
    tex->SetSize(img.width, img.height, TextureFormat::TEX_FORMAT_RGBA8_UNORM);

    auto view = gltf.bufferViews[img.bufferView];

    MemoryBuffer buff(gltf.buffers[view.buffer].data.data() + view.byteOffset, view.byteLength);

    Image image(ctx);
    if (image.Load(buff))
    {
        tex->SetData(&image);
        return tex;
    }

    return nullptr;
}

SharedPtr<Node> LoadGLTFModel(Context* ctx, tinygltf::Model& gltf)
{
    if (gltf.scenes.empty())
        return SharedPtr<Node>();

    // cloning because controllers could change or possibly even not be the same on each hand
    SharedPtr<Material> material = ctx->GetSubsystem<ResourceCache>()->GetResource<Material>("Materials/XRController.xml")->Clone();
    if (!gltf.materials.empty() && !gltf.textures.empty())
    {
        material->SetTexture(ShaderResources::Albedo, LoadGLTFTexture(ctx, gltf, 0));
        if (gltf.materials[0].normalTexture.index)
            material->SetTexture(ShaderResources::Normal, LoadGLTFTexture(ctx, gltf, gltf.materials[0].normalTexture.index));
    }

    auto scene = gltf.scenes[gltf.defaultScene];
    SharedPtr<Node> root(new Node(ctx));
    root->SetScale(Vector3(1, 1, -1));
    //root->Rotate(Quaternion(45, Vector3::UP));
    for (auto n : scene.nodes)
        GLTFRecurseModel(ctx, gltf, root, n, -1, material, Matrix3x4::IDENTITY);

    return root;
}

}
