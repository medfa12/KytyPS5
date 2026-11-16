// A hidden CHILD-owned Win32 Vulkan surface, using the production embedding and
// control bridge. No SDL gamepad, HID, audio, foreground or desktop-mode changes.
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>
#include "graphics/presentation/window/runtimeWindowBridge.h"
#include "loader/runtimeChildClient.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
bool fail_focus = false;
bool FocusServices(bool foreground, bool controller, uint64_t epoch) {
	std::cout << "focus services " << epoch << " " << foreground << " " << controller << '\n' << std::flush;
	return !fail_focus || !foreground;
}
void Check(VkResult result, const char* why) { if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) { throw std::runtime_error(std::string(why) + ": " + std::to_string(result)); } }
class Output {
public:
	VkInstance instance = nullptr;
	VkSurfaceKHR surface = nullptr;
	VkPhysicalDevice physical = nullptr;
	VkDevice device = nullptr;
	VkQueue queue = nullptr;
	VkSwapchainKHR swapchain = nullptr;
	VkCommandPool pool = nullptr;
	VkCommandBuffer command = nullptr;
	VkSemaphore acquired = nullptr, rendered = nullptr;
	uint32_t family = 0;
	VkExtent2D extent {};
	unsigned presents = 0;
	std::vector<VkImage> images;
	~Output() {
		if (device) {
			vkDeviceWaitIdle(device);
			if (swapchain) { vkDestroySwapchainKHR(device, swapchain, nullptr); }
			if (acquired) { vkDestroySemaphore(device, acquired, nullptr); }
			if (rendered) { vkDestroySemaphore(device, rendered, nullptr); }
			if (pool) { vkDestroyCommandPool(device, pool, nullptr); }
			vkDestroyDevice(device, nullptr);
		}
		if (surface) { vkDestroySurfaceKHR(instance, surface, nullptr); }
		if (instance) { vkDestroyInstance(instance, nullptr); }
	}
	void Initialize(HWND window) {
		VkApplicationInfo app {VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.pApplicationName = "Kyty hidden subprocess WSI test"; app.apiVersion = VK_API_VERSION_1_1;
		const char* extensions[] {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
		VkInstanceCreateInfo create {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; create.pApplicationInfo = &app; create.enabledExtensionCount = 2; create.ppEnabledExtensionNames = extensions;
		Check(vkCreateInstance(&create, nullptr, &instance), "instance");
		VkWin32SurfaceCreateInfoKHR win {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR}; win.hinstance = GetModuleHandleW(nullptr); win.hwnd = window;
		Check(vkCreateWin32SurfaceKHR(instance, &win, nullptr, &surface), "child-owned surface");
		uint32_t count = 0; Check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "devices"); std::vector<VkPhysicalDevice> devices(count);
		Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "device list");
		for (auto candidate: devices) {
			uint32_t queues = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queues, nullptr); std::vector<VkQueueFamilyProperties> properties(queues);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queues, properties.data());
			for (uint32_t i = 0; i != queues; ++i) { VkBool32 supported = false; Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &supported), "surface queue");
				if (supported && (properties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { physical = candidate; family = i; break; }
			}
			if (physical) { break; }
		}
		if (!physical) { throw std::runtime_error("no graphics/present device"); }
		float priority = 1; VkDeviceQueueCreateInfo q {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; q.queueFamilyIndex = family; q.queueCount = 1; q.pQueuePriorities = &priority;
		const char* swap = VK_KHR_SWAPCHAIN_EXTENSION_NAME; VkDeviceCreateInfo d {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; d.queueCreateInfoCount = 1; d.pQueueCreateInfos = &q; d.enabledExtensionCount = 1; d.ppEnabledExtensionNames = &swap;
		Check(vkCreateDevice(physical, &d, nullptr, &device), "device"); vkGetDeviceQueue(device, family, 0, &queue);
		VkCommandPoolCreateInfo p {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; p.queueFamilyIndex = family; p.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		Check(vkCreateCommandPool(device, &p, nullptr, &pool), "pool");
		VkCommandBufferAllocateInfo a {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; a.commandPool = pool; a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; a.commandBufferCount = 1;
		Check(vkAllocateCommandBuffers(device, &a, &command), "command buffer");
		VkSemaphoreCreateInfo s {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; Check(vkCreateSemaphore(device, &s, nullptr, &acquired), "acquire semaphore"); Check(vkCreateSemaphore(device, &s, nullptr, &rendered), "render semaphore");
	}
	bool DrawIfChanged(HWND window) {
		RECT client {}; GetClientRect(window, &client);
		if (swapchain && extent.width == uint32_t(client.right) && extent.height == uint32_t(client.bottom)) { return false; }
		Check(vkDeviceWaitIdle(device), "resize drain");
		if (swapchain) { vkDestroySwapchainKHR(device, swapchain, nullptr); swapchain = nullptr; }
		VkSurfaceCapabilitiesKHR caps {}; Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps), "surface caps");
		uint32_t count = 0; Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr), "formats"); std::vector<VkSurfaceFormatKHR> formats(count);
		Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()), "format list");
		if (formats.empty() || !(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { throw std::runtime_error("required test surface usage unavailable"); }
		extent = caps.currentExtent;
		if (extent.width == UINT32_MAX) { extent = {uint32_t(client.right), uint32_t(client.bottom)}; }
		VkSwapchainCreateInfoKHR create {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; create.surface = surface;
		create.minImageCount = caps.minImageCount + 1; if (caps.maxImageCount) { create.minImageCount = std::min(create.minImageCount, caps.maxImageCount); }
		create.imageFormat = formats[0].format; create.imageColorSpace = formats[0].colorSpace; create.imageExtent = extent; create.imageArrayLayers = 1;
		create.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT; create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = caps.currentTransform;
		create.compositeAlpha = VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1)); create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
		Check(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain), "embedded swapchain");
		Check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr), "images"); images.resize(count); Check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()), "image list");
		uint32_t index = 0; Check(vkAcquireNextImageKHR(device, swapchain, 2000000000ull, acquired, nullptr, &index), "hidden acquire");
		Check(vkResetCommandBuffer(command, 0), "reset"); VkCommandBufferBeginInfo begin {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(command, &begin), "begin");
		VkImageMemoryBarrier barrier {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = images[index]; barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,nullptr,0,nullptr,1,&barrier);
		VkClearColorValue color {}; color.float32[3] = 1; vkCmdClearColorImage(command, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &barrier.subresourceRange);
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = 0;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,nullptr,0,nullptr,1,&barrier);
		Check(vkEndCommandBuffer(command), "end"); VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
		VkSubmitInfo submit {VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &wait;
		submit.commandBufferCount = 1; submit.pCommandBuffers = &command; submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &rendered;
		Check(vkQueueSubmit(queue, 1, &submit, nullptr), "submit");
		VkPresentInfoKHR present {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &rendered; present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
		Check(vkQueuePresentKHR(queue, &present), "actual hidden queue present"); Check(vkQueueWaitIdle(queue), "present drain"); ++presents;
		std::cout << "accepted hidden Vulkan present " << presents << " " << extent.width << 'x' << extent.height << '\n' << std::flush;
		return true;
	}
};
}
int wmain(int argc, wchar_t** argv) try {
	std::string descriptor; uint64_t parent = 0; uint32_t pid = 0;
	for (int i=1; i+1<argc; i+=2) { const std::wstring key=argv[i], value=argv[i+1];
		if (key==L"--vsh-child") { descriptor.assign(value.begin(),value.end()); }
		else if (key==L"--parent") { parent=std::stoull(value); }
		else if (key==L"--parent-pid") { pid=std::stoul(value); }
		else if (key==L"--fail-focus") { fail_focus=value==L"1"; }
	}
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	if (!Libs::Graphics::ConfigureRuntimeWindowRole(false,parent,pid) || !Loader::ConfigureRuntimeChildClient(descriptor,Loader::RuntimeChild::EmbeddedWindow)) { return 20; }
	Libs::Graphics::ConfigureRuntimeFocusServices(FocusServices,nullptr);
	WNDCLASSW cls {}; cls.lpfnWndProc=DefWindowProcW; cls.hInstance=GetModuleHandleW(nullptr); cls.lpszClassName=L"KytyHiddenVulkanChildProbe"; RegisterClassW(&cls);
	HWND window=CreateWindowExW(0,cls.lpszClassName,L"Hidden test child",WS_OVERLAPPEDWINDOW,0,0,64,64,nullptr,nullptr,cls.hInstance,nullptr);
	if (!window || !Libs::Graphics::AttachRuntimeChildWindow(reinterpret_cast<uint64_t>(window))) { return 21; }
	Loader::NotifyRuntimeChildEntry(100);
	{
		Output output; output.Initialize(window); output.DrawIfChanged(window); Loader::NotifyRuntimeChildPresent(output.presents);
		while (Libs::Graphics::PumpRuntimeWindowBridge()) {
			MSG message; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
			output.DrawIfChanged(window); Sleep(4);
		}
	}
	DestroyWindow(window); return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 22; }
