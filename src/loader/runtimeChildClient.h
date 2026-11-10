#pragma once
#include <filesystem>
#include <span>
#include "loader/runtimeChildProtocol.h"
#include "loader/runtimeChildInput.h"

namespace Loader {
// Called before subsystem/guest startup, only for the explicitly inherited role.
bool ConfigureRuntimeChildClient(const std::string& arguments, uint64_t capabilities = 0);
bool RuntimeChildComposedFrames() noexcept;
bool PublishRuntimeChildFrame(uint32_t width, uint32_t height, uint32_t format, bool hdr, std::span<const uint8_t> pixels);
bool IsRuntimeChild() noexcept;
uint32_t RuntimeChildAppId() noexcept;
bool RuntimeChildEntryAdmitted() noexcept;
// Applied window-focus state, not a pending parent request. A managed child
// starts hidden and becomes background again on close/channel loss.
bool RuntimeChildInBackground() noexcept;
// Terminal bridge/window loss: revoke visibility even if the pipe is still open.
void RevokeRuntimeChildFocus() noexcept;
bool PollRuntimeChildPad(RuntimeChild::PadReport* report) noexcept;
bool SendRuntimeChildPadOutput(RuntimeChild::PadOutput output) noexcept;
void NotifyRuntimeChildPs() noexcept;
bool RuntimeChildFocusHeld(RuntimeChild::PadReport* held) noexcept;
// Exact direct executable-entry seam; not process creation/module bootstrap.
void NotifyRuntimeChildEntry(int guest_pid = 100) noexcept;
void NotifyRuntimeChildWindow(uint64_t child_window, uint64_t parent_window) noexcept;
void NotifyRuntimeChildPresent(uint64_t guest_frame_serial) noexcept;
bool PollRuntimeChildCommand(RuntimeChild::Message* message) noexcept;
bool RuntimeChildChannelLost() noexcept;
void AcknowledgeRuntimeChildFocus(uint64_t epoch, int status = 0) noexcept;
bool ValidateRuntimeChildStoreLease(const std::filesystem::path& root);
bool RuntimeChildHasStoreLease() noexcept;
}
