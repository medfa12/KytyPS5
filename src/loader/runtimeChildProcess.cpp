#include "loader/runtimeChildProcess.h"
#include <utility>
#include <cwchar>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::RuntimeChild {
namespace { constexpr uint32_t Aborted = 1067; }
std::wstring QuoteWindowsArgument(const std::wstring& value) {
	std::wstring result = L"\""; size_t slashes = 0;
	for (wchar_t c: value) {
		if (c == L'\\') { ++slashes; continue; }
		result.append(slashes * (c == L'"' ? 2 : 1), L'\\'); slashes = 0;
		if (c == L'"') { result += L'\\'; }
		result += c;
	}
	result.append(slashes * 2, L'\\'); return result + L'"';
}
#if defined(_WIN32)
namespace {
struct Handle {
	HANDLE value = nullptr;
	~Handle() { if (value && value != INVALID_HANDLE_VALUE) { CloseHandle(value); } }
	uint64_t Take() { return reinterpret_cast<uint64_t>(std::exchange(value, nullptr)); }
};
struct EnvironmentSnapshot {
	LPWCH value = GetEnvironmentStringsW();
	~EnvironmentSnapshot() { if (value) { FreeEnvironmentStringsW(value); } }
};
bool ChildEnvironment(std::vector<wchar_t>* block) {
	EnvironmentSnapshot snapshot;
	if (!snapshot.value) { return false; }
	constexpr const wchar_t* host_only[] = {L"KYTY_NATIVE_NP_ALTERNATE_STARTUP", L"KYTY_NATIVE_NP_OWNERSHIP",
	    L"KYTY_NATIVE_NP_WORKQUEUE", L"KYTY_NATIVE_NP_CALLOUT", L"KYTY_NATIVE_NP_HTTP_POOL", L"KYTY_NATIVE_NP_DISPATCHER",
	    L"KYTY_NATIVE_NP_EVENT_QUEUE"};
	for (const auto* entry = snapshot.value; *entry;) {
		const auto length = std::wcslen(entry);
		// Preserve special =C:=... drive entries and the snapshot's ordering.
		const auto* separator = std::wcschr(entry, L'=');
		bool host_selection = false;
		if (separator && separator != entry) {
			for (const auto* name: host_only) {
				if (CompareStringOrdinal(entry, static_cast<int>(separator - entry), name, -1, TRUE) == CSTR_EQUAL) {
					host_selection = true; break;
				}
			}
		}
		if (!host_selection) { block->insert(block->end(), entry, entry + length + 1); }
		entry += length + 1;
	}
	if (block->empty()) { block->push_back(L'\0'); }
	block->push_back(L'\0');
	return true;
}
}
#endif
std::unique_ptr<Process> Process::Create(const Spawn& s, uint32_t* error) {
	if (error) { *error = 87; }
	if (!s.key.Valid() || s.executable.empty() || s.directory.empty() || s.log.empty() || s.arguments.size() > 128) { return {}; }
#if defined(_WIN32)
	std::vector<wchar_t> environment;
	if (!ChildEnvironment(&environment)) { if (error) { *error = GetLastError(); } return {}; }
	auto run = std::unique_ptr<Process>(new Process);
	run->key_ = s.key; run->close_timeout_ = s.close_timeout;
	run->parent_window_ = s.parent_window;
	if (s.composed_frames) {
		run->frames_ = std::make_shared<FrameMailbox>();
		if (!run->frames_->Create(s.key)) { if (error) *error = GetLastError(); return {}; }
	}
	if (!run->input_.Create(s.key)) { if (error) { *error = GetLastError(); } return {}; }
	run->startup_deadline_ = std::chrono::steady_clock::now() + s.startup_timeout;
	Handle job, parent_read, parent_write, child_read, child_write, log, data_lock, child_input;
	StoreLease child_lease;
	if (s.canonical_store_lease && (s.exclusive_data.empty() || !run->lease_.Acquire(s.exclusive_data) ||
	    !run->lease_.Duplicate(&child_lease))) { if (error) { *error = ERROR_SHARING_VIOLATION; } return {}; }
	if (!DuplicateHandle(GetCurrentProcess(), reinterpret_cast<HANDLE>(run->input_.Handle()), GetCurrentProcess(), &child_input.value,
	    0, TRUE, DUPLICATE_SAME_ACCESS)) { if (error) { *error = GetLastError(); } return {}; }
	if (!s.canonical_store_lease && !s.exclusive_data.empty()) {
		data_lock.value = CreateFileW((s.exclusive_data / ".runtime-lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (data_lock.value == INVALID_HANDLE_VALUE) { if (error) { *error = GetLastError(); } return {}; }
	}
	job.value = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits {};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SECURITY_ATTRIBUTES security {sizeof(security), nullptr, TRUE};
	if (!job.value || !SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
	    !CreatePipe(&parent_read.value, &child_write.value, &security, 4096) ||
	    !CreatePipe(&child_read.value, &parent_write.value, &security, 4096) ||
	    !SetHandleInformation(parent_read.value, HANDLE_FLAG_INHERIT, 0) ||
	    !SetHandleInformation(parent_write.value, HANDLE_FLAG_INHERIT, 0)) {
		if (error) { *error = GetLastError(); } return {};
	}
	log.value = CreateFileW(s.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log.value == INVALID_HANDLE_VALUE) { if (error) { *error = GetLastError(); } return {}; }
	std::wstring command = QuoteWindowsArgument(s.executable.wstring());
	for (const auto& arg: s.arguments) { command += L" " + QuoteWindowsArgument(arg); }
	ChildArguments child_args {s.key, reinterpret_cast<uint64_t>(child_read.value), reinterpret_cast<uint64_t>(child_write.value), reinterpret_cast<uint64_t>(child_input.value)};
	if (child_lease.Handle()) {
		child_args.lease = child_lease.Handle();
		if (!child_lease.Identity(&child_args.lease_volume, &child_args.lease_file)) { return {}; }
	}
	const auto encoded = EncodeArguments(child_args);
	command += L" --vsh-child " + QuoteWindowsArgument(std::wstring(encoded.begin(), encoded.end()));
	if (command.size() >= 32767) { if (error) { *error = ERROR_BAD_LENGTH; } return {}; }
	STARTUPINFOEXW startup {}; startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = log.value;
	SIZE_T bytes = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
	std::vector<unsigned char> attributes(bytes);
	startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &bytes)) { if (error) { *error = GetLastError(); } return {}; }
	HANDLE inherited[] {child_read.value, child_write.value, log.value, child_input.value, reinterpret_cast<HANDLE>(child_lease.Handle())};
	PROCESS_INFORMATION process {};
	bool created = UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited,
	    sizeof(HANDLE) * (child_lease.Handle() ? 5 : 4), nullptr, nullptr) != FALSE;
#if defined(KYTY_SUBPROCESS_TESTS)
	created = created && s.fault != Spawn::Fault::BeforeCreate;
#endif
	created = created && CreateProcessW(s.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
	    CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
	    environment.data(), s.directory.c_str(), &startup.StartupInfo, &process);
	const auto create_error = created ? 0 : GetLastError();
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	if (!created) { if (error) { *error = create_error ? create_error : ERROR_CANCELLED; } return {}; }
	// From here on only nonallocating operations: every created process remains
	// owned by run, even assignment/resume failure, until Poll observes its exit.
	run->process_ = reinterpret_cast<uint64_t>(process.hProcess); run->pid_ = process.dwProcessId;
	run->job_ = job.Take();
	run->data_lock_ = data_lock.Take();
	const auto receive = parent_read.Take(), send = parent_write.Take();
	const bool channel_ready = run->channel_.Adopt(receive, send);
	if (!channel_ready) { CloseHandle(reinterpret_cast<HANDLE>(receive)); CloseHandle(reinterpret_cast<HANDLE>(send)); }
	bool assigned = AssignProcessToJobObject(reinterpret_cast<HANDLE>(run->job_), process.hProcess) != FALSE;
#if defined(KYTY_SUBPROCESS_TESTS)
	if (s.fault == Spawn::Fault::AssignJob) { assigned = false; }
#endif
	run->assigned_ = assigned;
	bool resumed = false;
	if (assigned && channel_ready) {
#if defined(KYTY_SUBPROCESS_TESTS)
		if (s.fault != Spawn::Fault::Resume)
#endif
		{ resumed = ResumeThread(process.hThread) != DWORD(-1); }
	}
	CloseHandle(process.hThread);
	if (!assigned || !resumed) { run->error_ = ERROR_PROCESS_ABORTED; run->Kill(); }
	if (error) { *error = 0; } return run;
#else
	if (error) { *error = 50; } return {};
#endif
}
Process::~Process() {
#if defined(_WIN32)
	// Destructor is HOST-only shutdown, never an early lifecycle completion.
	if (process_ && !retired_) { Kill(); WaitForSingleObject(reinterpret_cast<HANDLE>(process_), INFINITE); }
	if (process_) { CloseHandle(reinterpret_cast<HANDLE>(process_)); }
	if (job_) { CloseHandle(reinterpret_cast<HANDLE>(job_)); }
	if (data_lock_) { CloseHandle(reinterpret_cast<HANDLE>(data_lock_)); }
#endif
}
void Process::Kill() {
#if defined(_WIN32)
	if (!process_ || retired_) { return; }
	if (assigned_) { TerminateJobObject(reinterpret_cast<HANDLE>(job_), ERROR_PROCESS_ABORTED); }
	else { TerminateProcess(reinterpret_cast<HANDLE>(process_), ERROR_PROCESS_ABORTED); }
#endif
	killed_ = closing_ = true;
}
bool Process::Command(Kind kind, int64_t a, int64_t b, int64_t c, int64_t d, int64_t e, int64_t f) {
	Message message; message.key = key_; message.kind = kind; message.serial = ++sent_;
	message.value[0] = a; message.value[1] = b; message.value[2] = c;
	message.value[3] = d; message.value[4] = e; message.value[5] = f;
	return channel_.Write(message);
}
bool Process::Close(bool force) {
	if (retired_) { return true; }
	if (!closing_) { closing_ = true; close_deadline_ = std::chrono::steady_clock::now() + close_timeout_; Command(Kind::Close); }
	if (force) { Kill(); }
	return true;
}
bool Process::Focus(bool foreground, bool controller, uint64_t epoch, const PadReport* held) {
	if (!hello_ || closing_ || retired_ || focus_pending_ > focus_ack_ || epoch <= focus_pending_ || epoch > INT64_MAX) { return false; }
	uint64_t axes = 0;
	if (held) { for (unsigned i = 0; i != held->axes.size(); ++i) { axes |= uint64_t(held->axes[i]) << (i * 8); } }
	const auto touches = held ? 0x100 | (held->touch[0].down ? 1 : 0) | (held->touch[1].down ? 2 : 0) : 0;
	if (!Command(Kind::Focus, foreground, controller, static_cast<int64_t>(epoch), held ? held->buttons : 0, static_cast<int64_t>(axes), touches)) { return false; }
	focus_pending_ = epoch; return true;
}
bool Process::Accept(const Message& m) {
	if (!ValidMessage(m, key_) || m.serial != received_ + 1) { return false; }
	++received_;
	if (!hello_) {
		if (m.kind != Kind::Hello || m.value[0] != pid_ || m.value[1] < 0 || (uint64_t(m.value[1]) & ~uint64_t(15)) ||
		    (frames_ && !(m.value[1] & ComposedFrames))) { return false; }
		hello_ = true; capabilities_ = static_cast<uint64_t>(m.value[1]); return true;
	}
	switch (m.kind) {
		case Kind::LeaseAck: {
			uint64_t volume = 0, file = 0;
			if (lease_ack_ || !lease_.Identity(&volume, &file) || uint64_t(m.value[0]) != volume || uint64_t(m.value[1]) != file) { return false; }
			lease_ack_ = true; return true;
		}
		case Kind::Entry:
			if (lease_.Handle() && !lease_ack_) { return false; }
			if (entered_ || m.value[0] <= 0 || m.value[0] > INT32_MAX) { return false; }
			entered_ = true; return true;
		case Kind::Window:
			if (window_ || !m.value[0] || !parent_window_ || static_cast<uint64_t>(m.value[1]) != parent_window_) { return false; }
#if defined(_WIN32)
			{
				DWORD owner = 0, parent_owner = 0;
				if (!GetWindowThreadProcessId(reinterpret_cast<HWND>(m.value[0]), &owner) || owner != pid_ ||
				    !GetWindowThreadProcessId(reinterpret_cast<HWND>(parent_window_), &parent_owner) || parent_owner != GetCurrentProcessId() ||
				    GetParent(reinterpret_cast<HWND>(m.value[0])) != reinterpret_cast<HWND>(parent_window_)) { return false; }
			}
#endif
			window_ = true; return true;
		case Kind::Present:
			if (!window_ || !entered_ || presented_ || !m.value[0]) { return false; }
			presented_ = true; return true;
		case Kind::Failure:
			if (m.value[0] >= 0) { return false; }
			error_ = Aborted; Kill(); return true;
		case Kind::FocusAck:
			if (!focus_pending_ || m.value[0] <= 0 || uint64_t(m.value[0]) != focus_pending_ || focus_ack_ == focus_pending_ || m.value[1] > 0 || m.value[1] < INT32_MIN) { return false; }
			focus_ack_ = focus_pending_; focus_result_ = static_cast<int32_t>(m.value[1]); return true;
		default: return false;
	}
}
Tick Process::Poll() {
	Tick tick;
#if defined(_WIN32)
	if (retired_) { tick.retired = true; return tick; }
	for (unsigned i = 0; i != tick.messages.size() && !disconnected_; ++i) {
		Message message; const auto result = channel_.Read(&message);
		if (result == ReadResult::Pending) { break; }
		if (result == ReadResult::Closed) { disconnected_ = true; break; }
		if (result != ReadResult::Received || !Accept(message)) { error_ = ERROR_INVALID_DATA; Kill(); break; }
		tick.messages[tick.count++] = message;
	}
	const auto wait = WaitForSingleObject(reinterpret_cast<HANDLE>(process_), 0);
	if (wait == WAIT_OBJECT_0) {
		if (!GetExitCodeProcess(reinterpret_cast<HANDLE>(process_), reinterpret_cast<DWORD*>(&exit_))) { error_ = GetLastError(); return tick; }
		JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting {};
		if (assigned_ && (!QueryInformationJobObject(reinterpret_cast<HANDLE>(job_), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) || accounting.ActiveProcesses)) {
			Kill(); return tick; // Descendants cannot hold the next launch slot.
		}
		// A queued LNC completion may be consumed immediately by a relaunch.
		// Release the title writer lock at this proven OS/job boundary, not later
		// when the event coordinator finally destroys its retired record.
		if (data_lock_) { CloseHandle(reinterpret_cast<HANDLE>(data_lock_)); data_lock_ = 0; }
		lease_.Reset();
		retired_ = tick.retired = true; return tick;
	}
	if (wait == WAIT_FAILED) { error_ = GetLastError(); Kill(); return tick; }
	const auto now = std::chrono::steady_clock::now();
	// Pipe EOF commonly precedes the final CRT/native process epilogue. Give
	// that real exit a bounded grace instead of racing TerminateProcess and
	// overwriting an ordinary child's actual exit code with our kill code.
	if (!closing_ && disconnected_) { closing_ = true; close_deadline_ = now + close_timeout_; }
	if (!closing_ && !entered_ && now >= startup_deadline_) { error_ = ERROR_TIMEOUT; Kill(); }
	if (closing_ && !killed_ && now >= close_deadline_) {
		if (disconnected_ && !error_) { error_ = ERROR_BROKEN_PIPE; }
		Kill();
	}
#endif
	return tick;
}
}
