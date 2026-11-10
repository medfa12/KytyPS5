#include "loader/runtimeChildClient.h"
#include "loader/runtimeChildFrames.h"
#include "loader/runtimeStoreLease.h"
#include <atomic>
#include <mutex>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader {
namespace {
struct Client {
	std::mutex mutex;
	RuntimeChild::Channel channel;
	// Intentionally process-lifetime: guest threads have no complete join fence.
	// OS closure at process death is later than all guest file activity.
	RuntimeChild::StoreLease& lease = *new RuntimeChild::StoreLease;
	bool lease_validated = false;
	RuntimeChild::InputMailbox input;
	RuntimeChild::FrameMailbox frames;
	bool composed = false;
	RuntimeChild::Key key;
	uint64_t sent = 0, received = 0, focus_epoch = 0, applied_epoch = 0;
	RuntimeChild::PadReport held;
	bool has_held = false;
	bool requested_foreground = false;
	std::atomic_bool foreground {false};
	bool closing = false;
	std::atomic_bool entry {false};
	bool window = false, present = false;
	std::atomic_bool configured {false}, lost {false};
	std::atomic_bool pending_ps {false};
	void FlushPs() {
		if (pending_ps.exchange(false) && !input.PulsePs()) { pending_ps.store(true); }
	}
	bool Send(RuntimeChild::Kind kind, int64_t a = 0, int64_t b = 0) {
		RuntimeChild::Message message; message.kind = kind; message.key = key; message.serial = ++sent;
		message.value[0] = a; message.value[1] = b;
		if (!channel.Write(message)) { lost.store(true); return false; }
		return true;
	}
};
Client& Current() { static Client client; return client; }
}
bool ConfigureRuntimeChildClient(const std::string& arguments, uint64_t capabilities) {
	RuntimeChild::ChildArguments parsed;
	if (!RuntimeChild::DecodeArguments(arguments, &parsed) || (capabilities & ~uint64_t(15))) { return false; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (client.configured.load() || !client.channel.Adopt(parsed.receive, parsed.send)) { return false; }
	client.key = parsed.key;
	if (parsed.lease && !client.lease.Adopt(parsed.lease, parsed.lease_volume, parsed.lease_file)) { return false; }
	client.composed = (capabilities & RuntimeChild::ComposedFrames) != 0;
	if (client.composed && !client.frames.Open(parsed.key)) return false;
	if (parsed.input && !client.input.Adopt(parsed.input, parsed.key)) { return false; }
	if ((capabilities & RuntimeChild::BrokeredInput) && !parsed.input) { return false; }
#if defined(_WIN32)
	if (!client.Send(RuntimeChild::Kind::Hello, GetCurrentProcessId(), static_cast<int64_t>(capabilities))) { return false; }
#else
	return false;
#endif
	client.configured.store(true, std::memory_order_release); return true;
}
bool IsRuntimeChild() noexcept { return Current().configured.load(std::memory_order_acquire); }
bool RuntimeChildHasStoreLease() noexcept { return Current().lease.Handle() != 0; }
bool ValidateRuntimeChildStoreLease(const std::filesystem::path& root) {
	auto& client = Current(); std::lock_guard lock(client.mutex);
	uint64_t volume = 0, file = 0;
	if (!IsRuntimeChild() || client.lease_validated || !client.lease.MatchesRoot(root) || !client.lease.Identity(&volume, &file)) { return false; }
	client.lease_validated = client.Send(RuntimeChild::Kind::LeaseAck, static_cast<int64_t>(volume), static_cast<int64_t>(file));
	return client.lease_validated;
}
bool RuntimeChildComposedFrames() noexcept { return IsRuntimeChild() && Current().composed; }
bool PublishRuntimeChildFrame(uint32_t width, uint32_t height, uint32_t format, bool hdr, std::span<const uint8_t> pixels) {
	return RuntimeChildComposedFrames() && Current().frames.Publish(width, height, format, hdr, pixels);
}
uint32_t RuntimeChildAppId() noexcept { return IsRuntimeChild() ? Current().key.app : 0; }
bool RuntimeChildEntryAdmitted() noexcept { return IsRuntimeChild() && Current().entry.load(std::memory_order_acquire); }
bool RuntimeChildInBackground() noexcept {
	return IsRuntimeChild() && (Current().lost.load() || !Current().foreground.load());
}
void RevokeRuntimeChildFocus() noexcept {
	if (!IsRuntimeChild()) { return; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	client.closing = true; client.foreground.store(false);
}
bool PollRuntimeChildPad(RuntimeChild::PadReport* report) noexcept {
	if (!IsRuntimeChild()) { return false; }
	Current().FlushPs();
	return Current().input.Pop(report);
}
bool SendRuntimeChildPadOutput(RuntimeChild::PadOutput output) noexcept {
	if (!IsRuntimeChild()) { return false; }
	return Current().input.WriteOutput(output);
}
void NotifyRuntimeChildPs() noexcept {
	if (IsRuntimeChild()) { Current().pending_ps.store(true); Current().FlushPs(); }
}
bool RuntimeChildFocusHeld(RuntimeChild::PadReport* held) noexcept {
	if (!IsRuntimeChild() || !held) { return false; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (!client.has_held) { return false; }
	*held = client.held; return true;
}
void NotifyRuntimeChildEntry(int guest_pid) noexcept {
	if (!IsRuntimeChild()) { return; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (client.lease.Handle() && !client.lease_validated) { client.lost.store(true); return; }
	if (!client.entry && guest_pid > 0) { client.entry = true; client.Send(RuntimeChild::Kind::Entry, guest_pid); }
}
void NotifyRuntimeChildWindow(uint64_t window, uint64_t parent) noexcept {
	if (!IsRuntimeChild()) { return; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (!client.window && window && parent) { client.window = true; client.Send(RuntimeChild::Kind::Window, static_cast<int64_t>(window), static_cast<int64_t>(parent)); }
}
void NotifyRuntimeChildPresent(uint64_t serial) noexcept {
	if (!IsRuntimeChild()) { return; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (client.entry && client.window && !client.present && serial) { client.present = true; client.Send(RuntimeChild::Kind::Present, static_cast<int64_t>(serial)); }
}
bool PollRuntimeChildCommand(RuntimeChild::Message* out) noexcept {
	if (!IsRuntimeChild() || !out) { return false; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	const auto result = client.channel.Read(out);
	if (result == RuntimeChild::ReadResult::Pending) { return false; }
	if (result != RuntimeChild::ReadResult::Received || !RuntimeChild::ValidMessage(*out, client.key) || out->serial != client.received + 1 ||
	    (out->kind != RuntimeChild::Kind::Close && out->kind != RuntimeChild::Kind::Focus)) { client.lost.store(true); return false; }
	if (out->kind == RuntimeChild::Kind::Focus && ((out->value[0] != 0 && out->value[0] != 1) || (out->value[1] != 0 && out->value[1] != 1) ||
	    out->value[2] <= 0 || static_cast<uint64_t>(out->value[2]) <= client.focus_epoch ||
	    out->value[3] < 0 || uint64_t(out->value[3]) > UINT32_MAX || out->value[4] < 0 || uint64_t(out->value[4]) > 0xffffffffffffull ||
	    (out->value[5] != 0 && (out->value[5] < 0x100 || out->value[5] > 0x103)))) { client.lost.store(true); return false; }
	++client.received;
	if (out->kind == RuntimeChild::Kind::Focus) {
		client.requested_foreground = out->value[0] != 0;
		client.focus_epoch = static_cast<uint64_t>(out->value[2]); client.has_held = (out->value[5] & 0x100) != 0;
		client.held = {}; client.held.buttons = static_cast<uint32_t>(out->value[3]);
		for (unsigned i = 0; i != client.held.axes.size(); ++i) { client.held.axes[i] = static_cast<uint8_t>(uint64_t(out->value[4]) >> (i * 8)); }
		client.held.touch[0].down = (out->value[5] & 1) != 0; client.held.touch[1].down = (out->value[5] & 2) != 0;
	}
	if (out->kind == RuntimeChild::Kind::Close) { client.closing = true; client.foreground.store(false); }
	return true;
}
bool RuntimeChildChannelLost() noexcept { return IsRuntimeChild() && Current().lost.load(); }
void AcknowledgeRuntimeChildFocus(uint64_t epoch, int status) noexcept {
	if (!IsRuntimeChild()) { return; }
	auto& client = Current(); std::lock_guard lock(client.mutex);
	if (!client.closing && !client.lost.load() && epoch && epoch == client.focus_epoch && epoch > client.applied_epoch) {
		client.applied_epoch = epoch;
		client.foreground.store(status == 0 && client.requested_foreground);
		client.Send(RuntimeChild::Kind::FocusAck, static_cast<int64_t>(epoch), status);
	}
}
}
