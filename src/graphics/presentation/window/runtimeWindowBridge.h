#pragma once
#include <cstdint>

namespace Libs::Graphics {
// The CHILD owns/subclasses its HWND and Vulkan surface. Never wrap/subclass a
// foreign-process HWND as an SDL window; only native SetParent links the trees.
class NativeChildWindow {
public:
	bool Attach(uint64_t child, uint64_t parent, uint32_t parent_pid);
	bool Fit();
	bool Show(bool visible);
	bool Connected() const;
	bool Visible() const { return visible_; }
	uint64_t Parent() const { return parent_; }
private:
	uint64_t child_ = 0, parent_ = 0;
	uint32_t parent_pid_ = 0;
	int width_ = 0, height_ = 0;
	bool visible_ = false;
};
bool ConfigureRuntimeWindowRole(bool parent, uint64_t parent_window, uint32_t parent_pid);
// Installed before startup; child UI applies real input/audio policy before ack.
void ConfigureRuntimeFocusServices(bool (*focus)(bool, bool, uint64_t), void (*pump)());
void RegisterRuntimeParentWindow(uint64_t window);
uint64_t RuntimeParentWindow() noexcept;
uint32_t RuntimeParentPid() noexcept;
bool AttachRuntimeChildWindow(uint64_t window);
// Main/UI thread only. False requests actual child host shutdown, not LNC exit.
bool PumpRuntimeWindowBridge();
bool RuntimeWindowBridgeActive() noexcept;
bool RuntimeWindowTreeForeground() noexcept;
void RequestRuntimeParentKeyboardFocus() noexcept;
}
