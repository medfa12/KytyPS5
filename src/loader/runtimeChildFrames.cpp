#include "loader/runtimeChildFrames.h"
#include <cstring>
#include <mutex>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::RuntimeChild {
namespace {
std::mutex source_mutex;
FrameSource source;
uint64_t source_revision = 0;
uint64_t shell_status = 0, shell_mask = 0;
bool SourceVisible() {
	return source.visible && (!(shell_mask & 6) || (shell_status & 6));
}
bool ValidExtent(uint32_t w, uint32_t h, uint32_t format, size_t bytes) {
	// RGBA/BGRA UNORM/SRGB and the two packed HDR10 layouts, all 4 bytes.
	return w && h && w <= FrameMailbox::MaxWidth && h <= FrameMailbox::MaxHeight &&
	    (format == 37 || format == 43 || format == 44 || format == 50 || format == 58 || format == 64) && bytes == size_t(w) * h * 4;
}
#if defined(_WIN32)
struct Slot {
	volatile LONG state; // free=0, writing=1, ready=2, reading=3
	uint32_t width, height, format, hdr;
	uint64_t serial;
	uint8_t pixels[FrameMailbox::MaxBytes];
};
struct SharedFrames {
	uint32_t magic, version;
	Key key;
	alignas(8) volatile LONG64 revision;
	Slot slots[3];
};
std::wstring Name(const Key& key) {
	return L"Local\\KytyFrames-" + std::to_wstring(key.app) + L"-" + std::to_wstring(key.sequence) + L"-" +
	    std::to_wstring(key.nonce_lo) + L"-" + std::to_wstring(key.nonce_hi);
}
#endif
}
FrameMailbox::~FrameMailbox() {
#if defined(_WIN32)
	if (view_) UnmapViewOfFile(view_);
	if (mapping_) CloseHandle(mapping_);
#endif
}
bool FrameMailbox::Map(const Key& key, bool create) {
#if defined(_WIN32)
	if (view_ || !key.Valid()) return false;
	const auto name = Name(key);
	HANDLE handle = create ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedFrames), name.c_str())
	                       : OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
	if (!handle) return false;
	if (create && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(handle); return false; }
	auto* view = static_cast<SharedFrames*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedFrames)));
	if (!view) { CloseHandle(handle); return false; }
	if (create) { view->magic = 0x4b46524d; view->version = 1; view->key = key; }
	if (view->magic != 0x4b46524d || view->version != 1 || view->key != key) { UnmapViewOfFile(view); CloseHandle(handle); return false; }
	mapping_ = handle; view_ = view; key_ = key; return true;
#else
	return false;
#endif
}
bool FrameMailbox::Create(const Key& key) { return Map(key, true); }
bool FrameMailbox::Open(const Key& key) { return Map(key, false); }
uint64_t FrameMailbox::Revision() const {
#if defined(_WIN32)
	return view_ ? static_cast<uint64_t>(InterlockedCompareExchange64(&static_cast<SharedFrames*>(view_)->revision, 0, 0)) : 0;
#else
	return 0;
#endif
}
bool FrameMailbox::Publish(uint32_t w, uint32_t h, uint32_t format, bool hdr, std::span<const uint8_t> pixels) {
	if (!view_ || !ValidExtent(w, h, format, pixels.size())) return false;
#if defined(_WIN32)
	auto& shared = *static_cast<SharedFrames*>(view_);
	// Single producer. Reclaim unread slots, but never overwrite a reader.
	for (auto& slot: shared.slots) {
		if (InterlockedCompareExchange(&slot.state, 1, 0) != 0 && InterlockedCompareExchange(&slot.state, 1, 2) != 2) continue;
		slot.width = w; slot.height = h; slot.format = format; slot.hdr = hdr; slot.serial = ++serial_;
		std::memcpy(slot.pixels, pixels.data(), pixels.size());
		InterlockedExchange(&slot.state, 2);
		InterlockedExchange64(&shared.revision, static_cast<LONG64>(serial_));
		return true;
	}
#endif
	return false;
}
bool FrameMailbox::Read(FramePixels* out) {
	if (!view_ || !out) return false;
#if defined(_WIN32)
	auto& shared = *static_cast<SharedFrames*>(view_);
	bool changed = false;
	for (auto& slot: shared.slots) {
		if (InterlockedCompareExchange(&slot.state, 3, 2) != 2) continue;
		const auto bytes = size_t(slot.width) * slot.height * 4;
		if (slot.serial > serial_ && slot.hdr <= 1 && ValidExtent(slot.width, slot.height, slot.format, bytes)) {
			try { out->pixels.resize(bytes); } catch (...) { InterlockedExchange(&slot.state, 0); throw; }
			std::memcpy(out->pixels.data(), slot.pixels, bytes);
			out->key = key_; out->serial = slot.serial; out->width = slot.width; out->height = slot.height;
			out->format = slot.format; out->hdr = slot.hdr != 0; serial_ = slot.serial; changed = true;
		}
		InterlockedExchange(&slot.state, 0);
	}
	return changed;
#else
	return false;
#endif
}
void SetFrameSource(std::shared_ptr<FrameMailbox> mailbox, const Key& key, bool visible) {
	std::lock_guard lock(source_mutex); source = {std::move(mailbox), key, visible, ++source_revision};
}
void SetFrameVisibility(const Key& key, bool visible) {
	std::lock_guard lock(source_mutex);
	if (source.key == key && source.visible != visible) { source.visible = visible; source.revision = ++source_revision; }
}
void RetireFrameSource(const Key& key) {
	std::lock_guard lock(source_mutex);
	if (source.key == key) { source = {}; source.revision = ++source_revision; }
}
void SetShellUiFrameStatus(uint64_t status, uint64_t mask) {
	std::lock_guard lock(source_mutex);
	const bool previous = SourceVisible();
	shell_status = (shell_status & ~mask) | (status & mask); shell_mask |= mask;
	if (SourceVisible() != previous) source.revision = ++source_revision;
}
FrameSource GetFrameSource() {
	std::lock_guard lock(source_mutex); auto result = source; result.visible = SourceVisible(); return result;
}
uint64_t FrameSourceRevision() {
	const auto next = GetFrameSource();
	// Each producer's revision restarts; source revision distinguishes lifetimes
	// and visibility even when no pixels are arriving.
	return (next.revision << 40) | (next.mailbox && next.visible ? next.mailbox->Revision() & 0xffffffffffull : 0);
}
}
