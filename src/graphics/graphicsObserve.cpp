#include "graphics/graphicsObserve.h"

#include <cstdlib>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::Graphics::Observe {

std::atomic<Buffer*> g_buffer {nullptr};
// Immutable after publication through g_buffer. Filtering preserves the bounded
// slots for low-frequency producer events during boot.
static uint64_t g_event_mask = 0xffff;

void Initialize() noexcept {
#ifdef _WIN32
	if (Enabled()) {
		return;
	}
	const char* enabled = std::getenv("KYTY_GRAPHICS_OBSERVE");
	if (enabled == nullptr || std::string_view(enabled) != "1") {
		return;
	}
	try {
		uint64_t event_mask = 0xffff;
		if (const char* text = std::getenv("KYTY_GRAPHICS_OBSERVE_MASK")) {
			char* end = nullptr;
			event_mask = std::strtoull(text, &end, 0);
			if (*text == '\0' || *end != '\0' || event_mask == 0 || event_mask > 0xffff) {
				return;
			}
		}
		uint64_t delay = 0;
		if (const char* text = std::getenv("KYTY_GRAPHICS_OBSERVE_DELAY")) {
			char* end = nullptr;
			delay = std::strtoull(text, &end, 10);
			if (*text == '\0' || *end != '\0' || delay > 3600) {
				return;
			}
		}
		wchar_t local[32768] {};
		const auto length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, 32768);
		if (length == 0 || length >= 32768) {
			return;
		}
		LARGE_INTEGER frequency {}, now {};
		if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&now)) {
			return;
		}
		auto directory = std::filesystem::path(local) / "Temp" / "opencode";
		std::filesystem::create_directories(directory);
		auto path = directory / ("graphics-observe-" + std::to_string(GetCurrentProcessId()) +
		                         "-" + std::to_string(now.QuadPart) + ".bin");
		HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
		                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		                          nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) {
			return;
		}
		HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(Buffer), nullptr);
		void* view = mapping == nullptr ? nullptr : MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Buffer));
		if (mapping != nullptr) {
			CloseHandle(mapping);
		}
		CloseHandle(file);
		if (view == nullptr) {
			DeleteFileW(path.c_str());
			return;
		}
		auto* buffer = new (view) Buffer {};
		buffer->pid = GetCurrentProcessId();
		buffer->frequency = static_cast<uint64_t>(frequency.QuadPart);
		buffer->start_tick = static_cast<uint64_t>(now.QuadPart);
		buffer->delay_ticks = delay * buffer->frequency;
		buffer->ready.store(1, std::memory_order_release);
		// Deliberately process-lifetime: no teardown can unmap under a producer.
		// No FlushViewOfFile/stdio or writer lock on any guest-waited path.
		g_event_mask = event_mask;
		g_buffer.store(buffer, std::memory_order_release);
	} catch (...) {
		// Observation failure must not prevent graphics initialization.
	}
#endif
}

void Record(Event event, const Values& values) noexcept {
	auto* buffer = g_buffer.load(std::memory_order_acquire);
	if (buffer == nullptr) {
		return;
	}
	const auto index = static_cast<uint64_t>(event);
	if (index >= 16 || (g_event_mask & (uint64_t {1} << index)) == 0) {
		return;
	}
#ifdef _WIN32
	LARGE_INTEGER now {};
	QueryPerformanceCounter(&now);
	buffer->Append(event, static_cast<uint64_t>(now.QuadPart), values);
#endif
}

} // namespace Libs::Graphics::Observe
