// The Android half of VkContext.
//
// Quest (OpenXR): the runtime creates the instance and the device (xrCreateVulkanInstanceKHR / DeviceKHR) and the
// renderer gets them through the Existing constructor; there is no window surface.
//
// Phones (flat screen, android/phone): the window constructor below creates its own Vulkan 1.3 instance with
// VK_KHR_android_surface, the surface of the activity's ANativeWindow and a device with dynamic rendering and the
// swapchain, exactly what vk_context_sdl.cpp does for the desktop. Android destroys the window while the activity is
// in the background, so the surface can be released and created again for the next window on the same device.
#include "gt2view/vk_context.h"

#include <vulkan/vulkan_android.h>
#include <android/native_window.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt2view {
namespace {
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan: ") + operation + " failed (" + std::to_string(result) + ")");
}
bool Has(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}
VkSurfaceKHR CreateSurface(VkInstance instance, ANativeWindow* window) {
    VkAndroidSurfaceCreateInfoKHR info{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    info.window = window;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    Check(vkCreateAndroidSurfaceKHR(instance, &info, nullptr, &surface), "vkCreateAndroidSurfaceKHR");
    return surface;
}
} // namespace

VkContext::VkContext(void*, void* window) : window_(window) {
    if (!window) throw std::runtime_error("Android rendering needs the activity's window");
    auto* nativeWindow = static_cast<ANativeWindow*>(window);
    ANativeWindow_acquire(nativeWindow); // WindowExtent reads it; released with the surface
    // A throwing constructor must release already-created Vulkan objects itself.
    try {
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "GT2";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.enabledExtensionCount = 2;
        ici.ppEnabledExtensionNames = extensions;
        Check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance (Vulkan 1.3 required)");
        surface_ = CreateSurface(instance_, nativeWindow);

        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "enumerate GPUs");
        for (auto pd : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(pd, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) {
                std::printf("graphics: %s offers Vulkan %u.%u only\n", properties.deviceName, VK_VERSION_MAJOR(properties.apiVersion),
                            VK_VERSION_MINOR(properties.apiVersion));
                continue;
            }
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(pd, &features);
            if (!f13.dynamicRendering) continue;
            uint32_t n = 0;
            Check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr), "device extensions");
            std::vector<VkExtensionProperties> devExtensions(n);
            Check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, devExtensions.data()), "device extensions");
            if (!Has(devExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, queues.data());
            for (uint32_t q = 0; q < n && !physical_; ++q) {
                VkBool32 present = VK_FALSE;
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present), "presentation support");
                if ((queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physical_ = pd;
                    queueFamily_ = q;
                }
            }
            if (physical_) break;
        }
        if (!physical_) throw std::runtime_error("This phone has no Vulkan 1.3 GPU with dynamic rendering. Update the system or use a newer phone.");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        const char* devExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f13;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = devExtensions;
        Check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        std::printf("graphics: Android Vulkan %u.%u.%u, %s, driver %u, window %dx%d\n", VK_VERSION_MAJOR(properties.apiVersion),
                    VK_VERSION_MINOR(properties.apiVersion), VK_VERSION_PATCH(properties.apiVersion), properties.deviceName,
                    properties.driverVersion, ANativeWindow_getWidth(nativeWindow), ANativeWindow_getHeight(nativeWindow));
    } catch (...) {
        if (device_) vkDestroyDevice(device_, nullptr);
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        ANativeWindow_release(nativeWindow);
        throw;
    }
}

VkContext::VkContext(const Existing& e) : instance_(e.instance), physical_(e.physical), device_(e.device),
    queueFamily_(e.queueFamily), queue_(e.queue), owns_(false), shadingRate_(e.shadingRate) {}

VkContext::~VkContext() {
    if (!owns_) return; // the OpenXR runtime's objects
    if (device_) vkDestroyDevice(device_, nullptr);
    ReleaseWindowSurface();
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VkContext::ReleaseWindowSurface() {
    if (!owns_) return;
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (window_) ANativeWindow_release(static_cast<ANativeWindow*>(window_));
    window_ = nullptr;
}

void VkContext::AttachWindowSurface(void* window) {
    if (!owns_) throw std::runtime_error("the OpenXR Vulkan context has no window surface");
    ReleaseWindowSurface();
    if (!window) return;
    auto* nativeWindow = static_cast<ANativeWindow*>(window);
    surface_ = CreateSurface(instance_, nativeWindow);
    VkBool32 present = VK_FALSE;
    Check(vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queueFamily_, surface_, &present), "presentation support");
    if (!present) {
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
        throw std::runtime_error("Vulkan: the new window cannot be presented from the graphics queue");
    }
    ANativeWindow_acquire(nativeWindow);
    window_ = window;
    std::printf("graphics: new window surface %dx%d\n", ANativeWindow_getWidth(nativeWindow), ANativeWindow_getHeight(nativeWindow));
}

// Android surfaces always report currentExtent, and with an IDENTITY preTransform on a rotated display every present
// returns SUBOPTIMAL: answering here would let the renderer's resize heuristic rebuild the swapchain on any mismatch of
// the window's buffer size. The fixed-size landscape activity has no resize; OUT_OF_DATE still rebuilds.
bool VkContext::WindowExtent(VkExtent2D&) const { return false; }

} // namespace gt2view
