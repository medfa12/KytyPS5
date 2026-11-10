// Parent supplies Check and includes libs/standardWrite.h, libs/errno.h,
// <array>, <cstdint>, <cstdio>, and (on Windows) <windows.h> at file scope.
// Run serially with no concurrent standard-handle writers: SetStdHandle is
// process-global. No Check/logging occurs until both handles are restored.
// All pipe operations are single attempts; reads follow PeekNamedPipe and
// never request more than the already available bytes. No worker/retry loops.
namespace StandardWriteTest {
namespace Sw = Libs::StandardWrite;
constexpr const char* Name = "StandardWrite";

#ifdef _WIN32
struct OwnedHandle {
	HANDLE value = INVALID_HANDLE_VALUE;
	OwnedHandle() = default;
	OwnedHandle(const OwnedHandle&) = delete;
	OwnedHandle& operator=(const OwnedHandle&) = delete;
	~OwnedHandle() { Close(); }
	bool Valid() const { return value != nullptr && value != INVALID_HANDLE_VALUE; }
	void Close() {
		if (Valid()) { CloseHandle(value); }
		value = INVALID_HANDLE_VALUE;
	}
};

struct Redirect {
	DWORD descriptor;
	HANDLE previous;
	bool installed;
	bool& restored;
	Redirect(DWORD descriptor, HANDLE replacement, bool& restored):
	    descriptor(descriptor), previous(GetStdHandle(descriptor)),
	    installed(SetStdHandle(descriptor, replacement) != FALSE), restored(restored) {}
	Redirect(const Redirect&) = delete;
	Redirect& operator=(const Redirect&) = delete;
	~Redirect() {
		if (installed && !SetStdHandle(descriptor, previous)) { restored = false; }
	}
};

// The test exclusively owns the pipe; no competing reader can consume bytes
// between this peek and read. Even a broken implementation returning a fake
// positive count cannot make this helper block on nonexistent output.
bool ReadAvailable(HANDLE pipe, void* output, DWORD capacity, DWORD& available, DWORD& received) {
	available = received = 0;
	if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) { return false; }
	if (available == 0) { return true; }
	const DWORD request = available < capacity ? available : capacity;
	return ReadFile(pipe, output, request, &received, nullptr) != FALSE;
}

bool IsError(const Sw::Result& result, int error) {
	return result.count == -1 && result.error == error;
}

void BinaryAndValidation() {
	const std::array<uint8_t, 8> out {{'A', 0, '\n', '%', 's', 0xff, '\r', 'Z'}};
	const std::array<uint8_t, 5> err {{0, 'E', '\n', 0x80, '!'}};
	std::array<uint8_t, 64> received {};
	Sw::Result wrote_out, wrote_err, empty, null_buffer, too_large, stdin_write, negative, negative_empty;
	bool created = false, installed = false, restored = true, read_ok = false;
	DWORD available = 0, count = 0;
	{
		OwnedHandle reader, writer;
		created = CreatePipe(&reader.value, &writer.value, nullptr, 4096) != FALSE;
		if (created) {
			{
				Redirect stdout_redirect(STD_OUTPUT_HANDLE, writer.value, restored);
				Redirect stderr_redirect(STD_ERROR_HANDLE, writer.value, restored);
				installed = stdout_redirect.installed && stderr_redirect.installed;
				if (installed) {
					wrote_out = Sw::Write(1, out.data(), out.size());
					wrote_err = Sw::Write(2, err.data(), err.size());
					empty = Sw::Write(1, nullptr, 0);
					null_buffer = Sw::Write(2, nullptr, 1);
					too_large = Sw::Write(1, out.data(), static_cast<uint64_t>(INT64_MAX) + 1);
					stdin_write = Sw::Write(0, out.data(), 1);
					negative = Sw::Write(-7, out.data(), 1);
					negative_empty = Sw::Write(-1, nullptr, 0);
				}
			}
			read_ok = ReadAvailable(reader.value, received.data(), static_cast<DWORD>(received.size()), available, count);
		}
	}
	Check(Name, restored && installed && created, "anonymous-pipe setup/standard-handle restoration failed");
	Check(Name, wrote_out.count == static_cast<int64_t>(out.size()) && wrote_err.count == static_cast<int64_t>(err.size()),
	      "stdout/stderr did not return actual binary write counts");
	Check(Name, empty.count == 0 && IsError(null_buffer, Libs::Posix::POSIX_EFAULT) &&
	            IsError(too_large, Libs::Posix::POSIX_EINVAL) && IsError(stdin_write, Libs::Posix::POSIX_EBADF) &&
	            IsError(negative, Libs::Posix::POSIX_EBADF) && IsError(negative_empty, Libs::Posix::POSIX_EBADF),
	      "zero-length/null/SSIZE_MAX/descriptor validation contract");
	Check(Name, read_ok && available == out.size() + err.size() && count == available,
	      "binary output missing, duplicated, formatted, or produced by rejected writes");
	bool exact = true;
	for (size_t i = 0; i < out.size(); ++i) { exact = exact && received[i] == out[i]; }
	for (size_t i = 0; i < err.size(); ++i) { exact = exact && received[out.size() + i] == err[i]; }
	Check(Name, exact, "NUL/newline/high bytes changed on actual host pipe");
}

void EndpointErrors() {
	const char byte = 'x';
	Sw::Result absent, absent_empty, invalid, read_only, broken;
	bool restored = true, installed = true, created = false;
	{
		Redirect redirect(STD_OUTPUT_HANDLE, nullptr, restored);
		installed = installed && redirect.installed;
		if (redirect.installed) {
			absent = Sw::Write(1, &byte, 1);
			absent_empty = Sw::Write(1, nullptr, 0);
		}
	}
	{
		Redirect redirect(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE, restored);
		installed = installed && redirect.installed;
		if (redirect.installed) { invalid = Sw::Write(2, &byte, 1); }
	}
	{
		OwnedHandle reader, writer;
		created = CreatePipe(&reader.value, &writer.value, nullptr, 4096) != FALSE;
		if (created) {
			{
				Redirect redirect(STD_OUTPUT_HANDLE, reader.value, restored);
				installed = installed && redirect.installed;
				if (redirect.installed) { read_only = Sw::Write(1, &byte, 1); }
			}
			reader.Close(); // Real broken pipe, not a mocked WriteFile error.
			{
				Redirect redirect(STD_ERROR_HANDLE, writer.value, restored);
				installed = installed && redirect.installed;
				if (redirect.installed) { broken = Sw::Write(2, &byte, 1); }
			}
		}
	}
	Check(Name, restored && installed && created, "error-fixture setup/standard-handle restoration failed");
	Check(Name, IsError(absent, Libs::Posix::POSIX_EBADF) && IsError(absent_empty, Libs::Posix::POSIX_EBADF) &&
	            IsError(invalid, Libs::Posix::POSIX_EBADF) && IsError(read_only, Libs::Posix::POSIX_EBADF),
	      "absent/invalid/read-only host endpoints did not report EBADF");
	Check(Name, IsError(broken, Libs::Posix::POSIX_EPIPE), "closed pipe reader did not report EPIPE");
}

void KernelDescriptorWrite() {
	using namespace Libs::LibKernel;
	const std::array<uint8_t, 6> payload {{'K', 0, '\n', '%', 0xff, '\r'}};
	std::array<uint8_t, 64> received {};
	int64_t wrote = -1, empty = -1, broken = 0, absent = 0, null_buffer = 0, too_large = 0;
	bool restored = true, installed = false, created = false, read_ok = false;
	DWORD available = 0, count = 0;
	auto* error = Libs::Posix::GetErrorAddr();
	const int previous_error = *error;
	*error = 1234;
	{
		OwnedHandle reader, writer;
		created = CreatePipe(&reader.value, &writer.value, nullptr, 4096) != FALSE;
		if (created) {
			{
				Redirect redirect(STD_OUTPUT_HANDLE, writer.value, restored);
				installed = redirect.installed;
				if (installed) {
					wrote = Libs::LibKernel::FileSystem::KernelWrite(1, payload.data(), payload.size());
					empty = Libs::LibKernel::FileSystem::KernelWrite(1, nullptr, 0);
					null_buffer = Libs::LibKernel::FileSystem::KernelWrite(1, nullptr, 1);
					too_large = Libs::LibKernel::FileSystem::KernelWrite(1, payload.data(), static_cast<size_t>(INT64_MAX) + 1);
				}
			}
			read_ok = ReadAvailable(reader.value, received.data(), static_cast<DWORD>(received.size()), available, count);
			reader.Close();
			{
				Redirect redirect(STD_ERROR_HANDLE, writer.value, restored);
				installed = installed && redirect.installed;
				if (redirect.installed) { broken = Libs::LibKernel::FileSystem::KernelWrite(2, payload.data(), 1); }
			}
		}
	}
	{
		Redirect redirect(STD_OUTPUT_HANDLE, nullptr, restored);
		installed = installed && redirect.installed;
		if (redirect.installed) { absent = Libs::LibKernel::FileSystem::KernelWrite(1, payload.data(), 1); }
	}
	const bool errno_unchanged = *error == 1234;
	*error = previous_error;
	Check(Name, restored && installed && created, "kernel-write pipe setup/restoration failed");
	Check(Name, wrote == payload.size() && empty == 0 && read_ok && available == payload.size() && count == available &&
	            std::equal(payload.begin(), payload.end(), received.begin()), "kernel write changed or lost binary bytes");
	Check(Name, broken == KERNEL_ERROR_EPIPE && absent == KERNEL_ERROR_EBADF &&
	            null_buffer == KERNEL_ERROR_EFAULT && too_large == KERNEL_ERROR_EINVAL && errno_unchanged,
	      "kernel write must return specific kernel errors without changing guest errno");
}

void NonblockingWrite() {
	std::array<uint8_t, 65536> payload {}, received {};
	for (size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<uint8_t>(i * 37); }
	char name[128] {};
	std::snprintf(name, sizeof(name), "\\\\.\\pipe\\kyty-standard-write-%lu-%llu",
	              static_cast<unsigned long>(GetCurrentProcessId()), static_cast<unsigned long long>(GetTickCount64()));
	Sw::Result oversized, filled, full;
	bool server_ok = false, client_ok = false, connected = false, quota_ok = false;
	bool installed = false, restored = true, read_ok = false, oversized_ok = false;
	DWORD quota = 0, requested = 0, available = 0, count = 0;
	{
		OwnedHandle server, client;
		server.value = CreateNamedPipeA(name, PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE,
		                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_NOWAIT,
		                                1, 4096, 4096, 0, nullptr);
		server_ok = server.Valid();
		if (server_ok) {
			// Opening the sole local instance connects it; no WaitNamedPipe/retry.
			client.value = CreateFileA(name, GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			client_ok = client.Valid();
			if (client_ok) {
				connected = ConnectNamedPipe(server.value, nullptr) != FALSE;
				if (!connected) { connected = GetLastError() == ERROR_PIPE_CONNECTED; }
				// GetNamedPipeInfo requires a readable handle; the outbound server
				// is write-only. Query the connected read endpoint's same pipe quota.
				quota_ok = GetNamedPipeInfo(client.value, nullptr, &quota, nullptr, nullptr) != FALSE &&
				           quota != 0 && quota <= payload.size() - 257;
				if (connected && quota_ok) {
					requested = quota + 257; // Exceeds reported capacity; PIPE_NOWAIT cannot block to finish.
					{
						Redirect redirect(STD_OUTPUT_HANDLE, server.value, restored);
						installed = redirect.installed;
						if (installed) {
							oversized = Sw::Write(1, payload.data(), requested);
							// Windows versions can reject a request exceeding quota
							// without progress instead of producing a short write.
							const bool drained = ReadAvailable(client.value, received.data(), static_cast<DWORD>(received.size()), available, count);
							oversized_ok = drained && count == available &&
							    ((oversized.count > 0 && oversized.count < requested && count == oversized.count) ||
							     (IsError(oversized, Libs::Posix::POSIX_EAGAIN) && count == 0));
							for (size_t i = 0; i < count; ++i) { oversized_ok = oversized_ok && received[i] == payload[i]; }
							filled = Sw::Write(1, payload.data(), quota);
							full = Sw::Write(1, payload.data(), 1); // No reader has drained the first write.
						}
					}
					read_ok = ReadAvailable(client.value, received.data(), static_cast<DWORD>(received.size()), available, count);
				}
			}
		}
	}
	Check(Name, restored && installed && server_ok && client_ok && connected && quota_ok,
	      "PIPE_NOWAIT named-pipe setup/standard-handle restoration failed");
	Check(Name, oversized_ok, "oversized nonblocking write count/error differs from actual queued bytes");
	Check(Name, filled.count == quota && read_ok && available == filled.count && count == available, "filled-pipe count differs from actual queued bytes");
	bool exact = true;
	for (size_t i = 0; i < count; ++i) { exact = exact && received[i] == payload[i]; }
	Check(Name, exact, "queued bytes differ from the input prefix");
	// Regression: WriteFile on a full PIPE_NOWAIT byte pipe can succeed with
	// zero bytes. A nonzero POSIX write must report EAGAIN, not zero-byte success.
	Check(Name, IsError(full, Libs::Posix::POSIX_EAGAIN), "full nonblocking pipe returned zero success instead of EAGAIN");
}
#endif

} // namespace StandardWriteTest

void TestStandardWrite() {
#ifdef _WIN32
	StandardWriteTest::BinaryAndValidation();
	StandardWriteTest::EndpointErrors();
	StandardWriteTest::NonblockingWrite();
	StandardWriteTest::KernelDescriptorWrite();
#endif
}
