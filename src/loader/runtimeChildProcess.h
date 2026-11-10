#pragma once
#include "loader/runtimeChildProtocol.h"
#include "loader/runtimeChildInput.h"
#include "loader/runtimeChildFrames.h"
#include "loader/runtimeStoreLease.h"
#include <chrono>
#include <filesystem>
#include <memory>
#include <vector>

namespace Loader::RuntimeChild {
std::wstring QuoteWindowsArgument(const std::wstring& value);
struct Spawn {
	std::filesystem::path executable, directory, log, exclusive_data;
	std::vector<std::wstring> arguments;
	Key key;
	uint64_t parent_window = 0;
	bool composed_frames = false;
	bool canonical_store_lease = false; // Unwired opt-in; dedicated /data only.
	std::chrono::milliseconds startup_timeout {120000}, close_timeout {2000};
#if defined(KYTY_SUBPROCESS_TESTS)
	enum class Fault { None, BeforeCreate, AssignJob, Resume } fault = Fault::None;
#endif
};
struct Tick {
	std::array<Message, 16> messages {};
	size_t count = 0;
	bool retired = false; // Original process signaled AND assigned job empty.
};
// Serialized by its HOST coordinator; no detached reader or arbitrary callbacks.
// A created process is NEVER discarded before actual termination observation.
class Process {
public:
	static std::unique_ptr<Process> Create(const Spawn& spawn, uint32_t* error);
	~Process();
	Process(const Process&) = delete;
	Process& operator=(const Process&) = delete;
	Tick Poll();
	bool Close(bool force);
	bool Focus(bool foreground, bool controller, uint64_t epoch, const PadReport* held = nullptr);
	bool SendPad(PadReport report) { return !retired_ && input_.Push(report); }
	bool ReadPadOutput(PadOutput* output) { return !retired_ && input_.ReadOutput(output); }
	bool ReadPs(uint64_t* pulses) { return !retired_ && input_.ReadPs(pulses); }
	std::shared_ptr<FrameMailbox> Frames() const { return frames_; }
	const Key& Identity() const { return key_; }
	uint32_t HostPid() const { return pid_; }
	uint32_t ExitCode() const { return exit_; }
	uint32_t Error() const { return error_; }
	bool Entered() const { return entered_; }
	bool Retired() const { return retired_; }
	bool FocusReady() const { return hello_ && window_ && entered_ && !closing_ && !retired_; }
	bool PresentationReady() const { return window_ && presented_; }
	uint64_t Capabilities() const { return capabilities_; }
	uint64_t FocusAcknowledged() const { return focus_ack_; }
	int32_t FocusResult() const { return focus_result_; }
private:
	Process() = default;
	bool Command(Kind kind, int64_t a = 0, int64_t b = 0, int64_t c = 0, int64_t d = 0, int64_t e = 0, int64_t f = 0);
	bool Accept(const Message& message);
	void Kill();
	Key key_;
	Channel channel_;
	StoreLease lease_;
	bool lease_ack_ = false;
	InputMailbox input_;
	std::shared_ptr<FrameMailbox> frames_;
	uint64_t process_ = 0, job_ = 0, data_lock_ = 0;
	uint64_t parent_window_ = 0;
	int32_t focus_result_ = 0;
	uint32_t pid_ = 0, exit_ = 259, error_ = 0;
	uint64_t received_ = 0, sent_ = 0, capabilities_ = 0, focus_pending_ = 0, focus_ack_ = 0;
	bool assigned_ = false, hello_ = false, entered_ = false, window_ = false, presented_ = false;
	bool closing_ = false, killed_ = false, retired_ = false, disconnected_ = false;
	std::chrono::steady_clock::time_point startup_deadline_, close_deadline_;
	std::chrono::milliseconds close_timeout_ {2000};
};
}
