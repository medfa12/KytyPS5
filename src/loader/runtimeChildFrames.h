#pragma once
#include "loader/runtimeChildProtocol.h"
#include <memory>
#include <span>
#include <vector>

namespace Loader::RuntimeChild {
// Host-only frame ABI. Pixels are tightly packed; format is a validated VkFormat
// value. Slot ownership is transferred with Win32 interlocked acquire/release.
struct FramePixels {
	Key key;
	uint64_t serial = 0;
	uint32_t width = 0, height = 0, format = 0;
	bool hdr = false;
	std::vector<uint8_t> pixels;
};
class FrameMailbox {
public:
	static constexpr uint32_t MaxWidth = 3840, MaxHeight = 2160;
	static constexpr size_t MaxBytes = size_t(MaxWidth) * MaxHeight * 4;
	FrameMailbox() = default;
	~FrameMailbox();
	FrameMailbox(const FrameMailbox&) = delete;
	FrameMailbox& operator=(const FrameMailbox&) = delete;
	bool Create(const Key& key);
	bool Open(const Key& key);
	bool Publish(uint32_t width, uint32_t height, uint32_t format, bool hdr, std::span<const uint8_t> pixels);
	bool Read(FramePixels* out);
	uint64_t Revision() const;
private:
	bool Map(const Key& key, bool create);
	Key key_;
	void* mapping_ = nullptr;
	void* view_ = nullptr;
	uint64_t serial_ = 0;
};

// Pin the mapping while reading, never child GPU resources. A new launch gets a
// new key and mapping; clearing visibility/retiring invalidates retained pixels.
struct FrameSource {
	std::shared_ptr<FrameMailbox> mailbox;
	Key key;
	bool visible = false;
	uint64_t revision = 0;
};
void SetFrameSource(std::shared_ptr<FrameMailbox> mailbox, const Key& key, bool visible);
void SetFrameVisibility(const Key& key, bool visible);
void RetireFrameSource(const Key& key);
FrameSource GetFrameSource();
uint64_t FrameSourceRevision();
// Native ShellUI masked status writes, never host PS-button guesses.
void SetShellUiFrameStatus(uint64_t status, uint64_t mask);
}
