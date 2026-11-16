#include "graphics/presentation/window/runtimeWindowBridge.h"
#include "loader/runtimeChildClient.h"
#include "loader/runtimeChildFrames.h"
#include <algorithm>
#include <atomic>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics {
namespace {
std::atomic<uint64_t> root_window {0};
std::atomic_bool parent_role {false}, restore_keyboard {false};
uint64_t requested_parent = 0;
uint32_t requested_parent_pid = 0;
NativeChildWindow child_window;
bool (*focus_services)(bool, bool, uint64_t) = nullptr;
void (*pump_services)() = nullptr;
uint64_t service_epoch = 0;
#if defined(_WIN32)
bool BelongsTo(uint64_t window, uint32_t pid) {
	DWORD actual = 0;
	return window && IsWindow(reinterpret_cast<HWND>(window)) &&
	    GetWindowThreadProcessId(reinterpret_cast<HWND>(window), &actual) && actual == pid;
}
bool Style(HWND window, int index, LONG_PTR style) {
	SetLastError(0); return SetWindowLongPtrW(window, index, style) || !GetLastError();
}
#endif
}
bool NativeChildWindow::Attach(uint64_t child, uint64_t parent, uint32_t parent_pid) {
#if defined(_WIN32)
	if (child_ || child == parent || parent_pid == GetCurrentProcessId() || !BelongsTo(child, GetCurrentProcessId()) || !BelongsTo(parent, parent_pid)) { return false; }
	auto hwnd = reinterpret_cast<HWND>(child);
	if (!AreDpiAwarenessContextsEqual(GetWindowDpiAwarenessContext(hwnd), GetWindowDpiAwarenessContext(reinterpret_cast<HWND>(parent)))) { return false; }
	// Must happen before SDL_Vulkan_CreateSurface. No visible top-level flash,
	// no topmost/foreground/maximize or desktop mode change.
	ShowWindow(hwnd, SW_HIDE);
	const auto old_style = GetWindowLongPtrW(hwnd, GWL_STYLE);
	const auto old_ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	const auto style = (old_style & ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU)) |
	    WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
	if (!Style(hwnd, GWL_STYLE, style) || !Style(hwnd, GWL_EXSTYLE, old_ex & ~(WS_EX_APPWINDOW | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE))) { return false; }
	SetLastError(0); const auto previous = SetParent(hwnd, reinterpret_cast<HWND>(parent));
	if (!previous && GetLastError()) { Style(hwnd, GWL_STYLE, old_style); Style(hwnd, GWL_EXSTYLE, old_ex); return false; }
	child_ = child; parent_ = parent; parent_pid_ = parent_pid;
	return Fit();
#else
	return false;
#endif
}
bool NativeChildWindow::Connected() const {
#if defined(_WIN32)
	return BelongsTo(child_, GetCurrentProcessId()) && BelongsTo(parent_, parent_pid_) &&
	    GetParent(reinterpret_cast<HWND>(child_)) == reinterpret_cast<HWND>(parent_);
#else
	return false;
#endif
}
bool NativeChildWindow::Fit() {
#if defined(_WIN32)
	if (!Connected()) { return false; }
	RECT rect {}; if (!GetClientRect(reinterpret_cast<HWND>(parent_), &rect)) { return false; }
	int width = rect.right - rect.left, height = rect.bottom - rect.top;
	if (Loader::RuntimeChildComposedFrames()) {
		// Bound staging memory even on an oversized desktop. Keep the aspect
		// ratio; the final parent presentation scales this application plane.
		const double scale = std::min({1.0, double(Loader::RuntimeChild::FrameMailbox::MaxWidth) / std::max(width, 1),
		    double(Loader::RuntimeChild::FrameMailbox::MaxHeight) / std::max(height, 1)});
		width = static_cast<int>(width * scale); height = static_cast<int>(height * scale);
	}
	if (width <= 0 || height <= 0) { return true; } // Minimized parent: retain the surface/extent.
	if (width == width_ && height == height_) { return true; }
	if (!SetWindowPos(reinterpret_cast<HWND>(child_), nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED)) { return false; }
	width_ = width; height_ = height; return true;
#else
	return false;
#endif
}
bool NativeChildWindow::Show(bool visible) {
#if defined(_WIN32)
	if (!Fit()) { return false; }
	ShowWindow(reinterpret_cast<HWND>(child_), visible ? SW_SHOWNOACTIVATE : SW_HIDE);
	visible_ = visible;
	if (visible) {
		// Never steal OS foreground from another app. Keyboard focus is local to
		// our embedded tree only while its existing parent is the foreground root.
		if (GetForegroundWindow() == GetAncestor(reinterpret_cast<HWND>(parent_), GA_ROOT)) { SetFocus(reinterpret_cast<HWND>(child_)); }
	}
	return true;
#else
	return false;
#endif
}
bool ConfigureRuntimeWindowRole(bool parent, uint64_t window, uint32_t pid) {
#if defined(_WIN32)
	if (window || pid) {
		if (parent || !window || !pid || pid == GetCurrentProcessId() || !BelongsTo(window, pid)) { return false; }
		requested_parent = window; requested_parent_pid = pid;
	}
	parent_role.store(parent); return true;
#else
	return !parent && !window && !pid;
#endif
}
void ConfigureRuntimeFocusServices(bool (*focus)(bool, bool, uint64_t), void (*pump)()) {
	focus_services = focus; pump_services = pump;
}
void RegisterRuntimeParentWindow(uint64_t window) {
#if defined(_WIN32)
	if (parent_role.load() && BelongsTo(window, GetCurrentProcessId())) {
		const auto hwnd = reinterpret_cast<HWND>(window);
		if (Style(hwnd, GWL_STYLE, GetWindowLongPtrW(hwnd, GWL_STYLE) | WS_CLIPCHILDREN)) {
			root_window.store(window, std::memory_order_release);
		}
	}
#endif
}
uint64_t RuntimeParentWindow() noexcept { return root_window.load(std::memory_order_acquire); }
uint32_t RuntimeParentPid() noexcept {
#if defined(_WIN32)
	return GetCurrentProcessId();
#else
	return 0;
#endif
}
bool AttachRuntimeChildWindow(uint64_t window) {
	if (!Loader::IsRuntimeChild() || !child_window.Attach(window, requested_parent, requested_parent_pid)) { return false; }
	Loader::NotifyRuntimeChildWindow(window, requested_parent); return true;
}
bool RuntimeWindowBridgeActive() noexcept { return parent_role.load() || Loader::IsRuntimeChild(); }
bool RuntimeWindowTreeForeground() noexcept {
#if defined(_WIN32)
	const auto root = reinterpret_cast<HWND>(RuntimeParentWindow());
	return root && GetForegroundWindow() == GetAncestor(root, GA_ROOT);
#else
	return false;
#endif
}
void RequestRuntimeParentKeyboardFocus() noexcept { restore_keyboard.store(true); }
bool PumpRuntimeWindowBridge() {
#if defined(_WIN32)
	if (parent_role.load() && restore_keyboard.exchange(false)) {
		const auto hwnd = reinterpret_cast<HWND>(RuntimeParentWindow());
		if (hwnd && GetForegroundWindow() == GetAncestor(hwnd, GA_ROOT)) { SetFocus(hwnd); }
	}
#endif
	if (!Loader::IsRuntimeChild()) { if (parent_role.load() && pump_services) { pump_services(); } return true; }
	if (!child_window.Fit() || Loader::RuntimeChildChannelLost()) {
		Loader::RevokeRuntimeChildFocus();
		if (focus_services) { focus_services(false, false, service_epoch); }
		return false;
	}
	if (pump_services) { pump_services(); }
	Loader::RuntimeChild::Message command;
	for (unsigned i = 0; i != 4 && Loader::PollRuntimeChildCommand(&command); ++i) {
		if (command.kind == Loader::RuntimeChild::Kind::Close) {
			if (focus_services) { focus_services(false, false, service_epoch); }
			return false;
		}
		if (command.kind == Loader::RuntimeChild::Kind::Focus) {
			service_epoch = static_cast<uint64_t>(command.value[2]);
			const bool foreground = command.value[0] != 0, controller = command.value[1] != 0;
			const bool policy_ok = !focus_services || focus_services(foreground, controller, service_epoch);
			// Composed children retain a hidden surface; only the parent owns visible
			// scanout and keyboard focus. Ordinary input arrives through the broker.
			const int status = policy_ok && child_window.Show(foreground && !Loader::RuntimeChildComposedFrames()) ? 0 : static_cast<int>(0x80020005u);
			if (status) {
				child_window.Show(false);
				if (focus_services) { focus_services(false, false, service_epoch); }
			}
			Loader::AcknowledgeRuntimeChildFocus(static_cast<uint64_t>(command.value[2]), status);
		}
	}
	return !Loader::RuntimeChildChannelLost();
}
}
