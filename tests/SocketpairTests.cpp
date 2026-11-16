// Main owns runner/CMake integration. Compile with the emulator test hooks;
// call TestSocketpairNetworkOnly() without Memory initialization, and call
// TestSocketpairFileDescriptors(root) in the hidden-Vulkan filesystem fixture.
#include "common/abi.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "kernel/guestDescriptorRange.h"
#include "libs/errno.h"
#include "libs/network.h"
#include "loader/symbolDatabase.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#if !defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
#error SocketpairTests requires KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS on its emulator sources too
#endif
namespace Libs {
void InitLibKernel_1(Loader::SymbolDatabase*);
namespace ShellCoreFloor { void InitShellCoreFloor_1(Loader::SymbolDatabase*); }
}

namespace {
namespace Net = Libs::Network::Net;
namespace FS = Libs::LibKernel::FileSystem;
namespace Posix = Libs::Posix;
using PairCall = int (KYTY_SYSV_ABI *)(int, int, int, int*);

// Protect even the tests deliberately attempting an empty nonblocking recv:
// if host mode regresses, fail the process instead of hanging in that call/join.
class PairDeadline {
	std::mutex mutex;
	std::condition_variable changed;
	bool done = false;
	std::thread watchdog;
public:
	PairDeadline(): watchdog([this] {
		std::unique_lock lock(mutex);
		if (!changed.wait_for(lock, std::chrono::seconds(120), [this] { return done; })) {
			std::fputs("SocketpairTests: fixture deadline exceeded\n", stderr);
			std::abort();
		}
	}) {}
	~PairDeadline() {
		{ std::lock_guard lock(mutex); done = true; }
		changed.notify_one(); watchdog.join();
	}
};

void PairCheck(bool value, const char* message) {
	if (!value) {
		std::fprintf(stderr, "SocketpairTests: %s (guest errno=%d)\n", message, *Posix::GetErrorAddr());
		std::abort();
	}
}
PairCall RegisteredPair() {
	Loader::SymbolDatabase symbols;
	Libs::InitLibKernel_1(&symbols);
	const auto* entry = symbols.FindByNid("MZb0GKT3mo8", Loader::SymbolType::Func);
	PairCheck(entry != nullptr, "real libkernel MZb export resolves");
	Loader::SymbolDatabase floor;
	Libs::ShellCoreFloor::InitShellCoreFloor_1(&floor);
	const auto* floor_entry = floor.FindByNid("MZb0GKT3mo8", Loader::SymbolType::Func);
	PairCheck(floor_entry != nullptr && floor_entry->vaddr == entry->vaddr,
	          "both registration routes resolve the same observed implementation");
	return reinterpret_cast<PairCall>(entry->vaddr);
}
void Resources(Net::SocketpairResources expected, const char* why) {
	const auto actual = Net::SocketpairResourcesForTest();
	PairCheck(actual.sockets == expected.sockets && actual.paths == expected.paths &&
	          actual.slots == expected.slots, why);
}
void ClosePair(const int* pair) {
	PairCheck(Net::SocketClose(pair[0]) == 0, "close first real endpoint");
	PairCheck(Net::SocketClose(pair[1]) == 0, "close second real endpoint");
}
void Ready(int fd, bool write = false) {
	std::array<uint64_t, Libs::GuestDescriptors::SocketEnd / 64> bits {};
	bits[fd / 64] = uint64_t{1} << (fd % 64);
	struct { int64_t seconds, microseconds; } timeout {2, 0};
	PairCheck(Net::Select(fd + 1, write ? nullptr : bits.data(), write ? bits.data() : nullptr,
	                     nullptr, &timeout) == 1, "bounded real select readiness");
	PairCheck((bits[fd / 64] & (uint64_t{1} << (fd % 64))) != 0, "select reports requested descriptor");
}
void Transfer(int writer, int reader, bool kernel_io, char payload) {
	Ready(writer, true);
	const auto sent = kernel_io ? FS::KernelWrite(writer, &payload, 1) : Net::Send(writer, &payload, 1, 0);
	PairCheck(sent == 1, "write one byte to peer");
	Ready(reader);
	char received = 0;
	const auto read = kernel_io ? FS::KernelRead(reader, &received, 1) : Net::Recv(reader, &received, 1, 0);
	PairCheck(read == 1 && received == payload, "real peer byte received");
}
void Exchange(PairCall call, bool kernel_io) {
	const auto baseline = Net::SocketpairResourcesForTest();
	int pair[2] {8, 0};
	*Posix::GetErrorAddr() = 79;
	PairCheck(call(1, 1, 0, pair) == 0 && *Posix::GetErrorAddr() == 79, "success preserves errno");
	PairCheck(pair[0] != pair[1] && Net::IsSocket(pair[0]) && Net::IsSocket(pair[1]), "publish two real distinct endpoints");
	Resources({baseline.sockets + 2, baseline.paths, baseline.slots + 2}, "only two endpoints remain, listener/path gone");
	PairCheck(FS::KernelFcntl(pair[0], 1, 0) == 0 && FS::KernelFcntl(pair[1], 1, 0) == 0, "CLOEXEC initially clear on both");
	PairCheck(FS::KernelFcntl(pair[0], 3, 0) == 2 && FS::KernelFcntl(pair[1], 3, 0) == 2, "actual request is blocking O_RDWR");
	Transfer(pair[0], pair[1], kernel_io, 'a');
	Transfer(pair[1], pair[0], kernel_io, 'b');
	std::array<unsigned char, 106> name {};
	uint32_t size = name.size();
	PairCheck(Net::Getsockname(pair[1], name.data(), &size) == 0 && size == 16 && name[1] == 1 && name[2] == 0,
	          "construction pathname never exposed on accepted endpoint");
	int pid = -1;
	PairCheck(Net::GetUnixPeerProcessId(pair[0], &pid) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP && pid == -1,
	          "no fabricated pair peer credentials");
	// Write-half-close leaves the reverse direction functional.
	Ready(pair[0], true);
	PairCheck(Net::Send(pair[0], "tail", 4, 0) == 4, "queue data before half-close");
	PairCheck(Net::Shutdown(pair[0], 1) == 0, "real write half-close");
	char data[4] {};
	for (size_t total = 0; total != sizeof(data);) {
		Ready(pair[1]);
		const auto n = Net::Recv(pair[1], data + total, sizeof(data) - total, 0);
		PairCheck(n > 0, "drain queued bytes before EOF"); total += static_cast<size_t>(n);
	}
	PairCheck(std::memcmp(data, "tail", 4) == 0, "buffer survives peer half-close");
	Ready(pair[1]);
	PairCheck(Net::Recv(pair[1], data, 1, 0) == 0, "real EOF after queued data");
	Transfer(pair[1], pair[0], kernel_io, 'c');
	PairCheck(Net::Send(pair[0], "x", 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EPIPE,
	          "send after own actual SHUT_WR is EPIPE");
	if (kernel_io) {
		PairCheck(FS::KernelClose(pair[0]) == 0 && FS::KernelClose(pair[1]) == 0, "kernel close dispatches both endpoints");
	} else { ClosePair(pair); }
	Resources(baseline, "half-close fixture resources retired");
	PairCheck(call(1, 1, 0, pair) == 0, "second connected pair");
	Ready(pair[0], true);
	PairCheck(Net::Send(pair[0], "z", 1, 0) == 1, "buffer before peer close");
	PairCheck(Net::SocketClose(pair[0]) == 0, "actual peer host close");
	Ready(pair[1]);
	PairCheck(Net::Recv(pair[1], data, 1, 0) == 1 && data[0] == 'z', "buffer survives peer close");
	Ready(pair[1]);
	PairCheck(Net::Recv(pair[1], data, 1, 0) == 0, "peer close EOF");
	PairCheck(Net::Send(pair[1], "x", 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EPIPE, "native EPIPE after real peer close");
	PairCheck(Net::SocketClose(pair[1]) == 0, "survivor fd still independently closable");
	Resources(baseline, "peer-close resources retired");
}
void Flags(PairCall call) {
	int pair[2];
	PairCheck(call(1, 0x10000001, 0, pair) == 0, "native socketpair ignores type CLOEXEC");
	PairCheck(FS::KernelFcntl(pair[0], 1, 0) == 0 && FS::KernelFcntl(pair[1], 1, 0) == 0, "CLOEXEC ignored on both");
	PairCheck(FS::KernelFcntl(pair[0], 2, 1) == 0 && FS::KernelFcntl(pair[0], 1, 0) == 1 &&
	          FS::KernelFcntl(pair[1], 1, 0) == 0, "SETFD per-descriptor metadata");
	PairCheck(FS::KernelFcntl(pair[0], 4, 6) == 0 && FS::KernelFcntl(pair[0], 3, 0) == 6, "SETFL real nonblocking");
	char byte;
	PairCheck(Net::Recv(pair[0], &byte, 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK, "nonblocking empty read cannot hang");
	PairCheck(FS::KernelFcntl(pair[1], 3, 0) == 2, "peer remains blocking");
	PairCheck(FS::KernelFcntl(pair[0], 4, 2) == 0 && FS::KernelFcntl(pair[0], 3, 0) == 2, "restore real blocking");
	PairCheck(FS::KernelFcntl(pair[0], 0, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP, "dup unsupported without fake descriptor");
	PairCheck(FS::KernelFcntl(pair[0], 4, 8) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EOPNOTSUPP &&
	          FS::KernelFcntl(pair[0], 3, 0) == 2, "unsupported SETFL leaves mode unchanged");
	const int old = pair[0]; ClosePair(pair);
	PairCheck(FS::KernelFcntl(old, 3, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EBADF, "closed socket fd has no stale metadata");
	PairCheck(call(1, 0x30000001, 0, pair) == 0, "both native type bits accepted");
	for (int fd: pair) {
		PairCheck(FS::KernelFcntl(fd, 1, 0) == 0 && FS::KernelFcntl(fd, 3, 0) == 6, "both nonblocking and neither CLOEXEC");
		PairCheck(Net::Recv(fd, &byte, 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK, "both host endpoints nonblocking");
	}
	ClosePair(pair);
	PairCheck(call(1, 1, 0, pair) == 0, "fd reuse");
	PairCheck(FS::KernelFcntl(pair[0], 1, 0) == 0 && FS::KernelFcntl(pair[0], 3, 0) == 2, "reused fd resets flags");
	ClosePair(pair);
}
void Failures(PairCall call) {
	const auto baseline = Net::SocketpairResourcesForTest();
	for (const auto& row: std::array<std::array<int,4>,7>{{
	    {-1,1,0,47}, {1,0,0,43}, {1,7,0,41}, {1,1,17,43}, {1,2,0,45}, {1,5,0,45}, {2,1,0,45}}}) {
		int output[2] {8,0};
		PairCheck(call(row[0],row[1],row[2],output) == -1 && *Posix::GetErrorAddr() == row[3] &&
		          output[0] == 8 && output[1] == 0, "native validation/subset error leaves output untouched");
	}
	PairCheck(call(1,7,0,nullptr) == -1 && *Posix::GetErrorAddr() == 41, "type validation precedes output fault");
	PairCheck(call(1,1,0,nullptr) == -1 && *Posix::GetErrorAddr() == 14, "null copyout EFAULT");
	Resources(baseline, "null copyout retires both endpoints");
	for (auto failure: {Net::SocketpairFailure::AfterAccept, Net::SocketpairFailure::SecondSlot
#if defined(_WIN32)
	    , Net::SocketpairFailure::AfterBind, Net::SocketpairFailure::AfterConnect
#endif
	}) {
		int output[2] {8,0};
		Net::SocketpairFailForTest(failure);
		PairCheck(call(1,1,0,output) == -1 && output[0] == 8 && output[1] == 0, "injected setup/second-slot failure preserves output");
		PairCheck(*Posix::GetErrorAddr() == (failure == Net::SocketpairFailure::SecondSlot ? 24 : 5), "injected resource/setup errno");
		Resources(baseline, "injected failure releases sockets/path/slots");
	}
#if defined(_WIN32)
	constexpr size_t page_size = 4096;
	auto* page = static_cast<int*>(VirtualAlloc(nullptr, page_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
	PairCheck(page != nullptr, "allocate fault fixture");
	page[0] = 8; page[1] = 0;
	DWORD old;
	PairCheck(VirtualProtect(page, page_size, PAGE_READONLY, &old) != 0, "protect output read-only");
#else
	const size_t page_size = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
	auto* page = static_cast<int*>(::mmap(nullptr, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
	PairCheck(page != MAP_FAILED, "allocate fault fixture");
	page[0] = 8; page[1] = 0;
	PairCheck(::mprotect(page, page_size, PROT_READ) == 0, "protect output read-only");
#endif
	PairCheck(call(1,1,0,page) == -1 && *Posix::GetErrorAddr() == 14 && page[0] == 8 && page[1] == 0, "permission-respecting read-only copyout failure");
	Resources(baseline, "read-only copyout rolls back exact slots");
#if defined(_WIN32)
	PairCheck(VirtualFree(page, page_size, MEM_DECOMMIT) != 0, "reserve inaccessible output address");
#else
	PairCheck(::mprotect(page, page_size, PROT_NONE) == 0, "inaccessible output address");
#endif
	PairCheck(call(1,1,0,page) == -1 && *Posix::GetErrorAddr() == 14, "inaccessible copyout EFAULT");
	Resources(baseline, "inaccessible copyout retires both endpoints");
#if defined(_WIN32)
	PairCheck(VirtualFree(page, 0, MEM_RELEASE) != 0, "release address reservation");
#else
	PairCheck(::munmap(page, page_size) == 0, "release address reservation");
#endif
	alignas(int) std::array<unsigned char, 9> unaligned {};
	PairCheck(call(1,1,0,reinterpret_cast<int*>(unaligned.data()+1)) == 0, "unaligned 8-byte publication");
	int fds[2]; std::memcpy(fds, unaligned.data()+1, sizeof(fds)); ClosePair(fds);
	Resources(baseline, "fault fixture resources balanced");
}
void ConcurrentReuse(PairCall call) {
	const auto baseline = Net::SocketpairResourcesForTest();
	std::atomic_bool start {false};
	auto work = [&](bool faults) {
		while (!start.load()) { std::this_thread::yield(); }
		for (int i = 0; i != 24; ++i) {
			if (faults) { PairCheck(call(1,1,0,nullptr) == -1 && *Posix::GetErrorAddr() == 14, "concurrent copyout rollback"); }
			int pair[2]; PairCheck(call(1,1,0,pair) == 0, "concurrent allocation");
			Transfer(pair[0], pair[1], false, static_cast<char>(i));
			PairCheck(FS::KernelFcntl(pair[0],2,1) == 0, "set flag before reuse");
			ClosePair(pair);
		}
	};
	std::thread first(work, true), second(work, false);
	start = true; first.join(); second.join();
	Resources(baseline, "concurrent rollback/close/reuse has no leaked or stolen slots");
}

struct RetainedSendGate {
	std::mutex mutex;
	std::condition_variable changed;
	bool arrived = false;
	bool released = false;
	static void Hook(void* context) {
		auto& gate = *static_cast<RetainedSendGate*>(context);
		PairCheck(Common::GetHostLockCriticalCounter()->load() == 0, "retained send hook runs outside Net critical scopes");
		std::unique_lock lock(gate.mutex);
		gate.arrived = true;
		gate.changed.notify_one();
		PairCheck(gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.released; }),
		          "bounded retained-send release gate");
	}
};

void RetainedCloseReuse(PairCall call) {
	const auto baseline = Net::SocketpairResourcesForTest();
	int original[2];
	PairCheck(call(1, 1, 0, original) == 0, "retained operation original pair");
	RetainedSendGate gate;
	int64_t sent = -1;
	std::thread sender([&] {
		Net::SocketRetainedSendHookForTest(&RetainedSendGate::Hook, &gate);
		sent = Net::Send(original[0], "o", 1, 0);
	});
	{
		std::unique_lock lock(gate.mutex);
		PairCheck(gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.arrived; }),
		          "send has retained original transport before close");
	}
	PairCheck(Net::SocketClose(original[0]) == 0, "close numeric fd while send retains transport");
	Resources({baseline.sockets + 2, baseline.paths, baseline.slots + 1}, "in-flight host ownership survives slot removal");
	int replacement[2];
	PairCheck(call(1, 0x20000001, 0, replacement) == 0, "allocate replacement pair during retained send");
	const int replacement_writer = replacement[0] == original[0] ? replacement[0] : replacement[1];
	const int replacement_peer = replacement[0] == original[0] ? replacement[1] : replacement[0];
	PairCheck(replacement_writer == original[0], "forced numeric fd reuse, not merely independent pairs");
	{
		std::lock_guard lock(gate.mutex);
		gate.released = true;
	}
	gate.changed.notify_one(); sender.join();
	PairCheck(sent == 1, "retained send completes against original host connection");
	char byte = 0;
	Ready(original[1]);
	PairCheck(Net::Recv(original[1], &byte, 1, 0) == 1 && byte == 'o', "only original peer receives retained send");
	Ready(original[1]);
	PairCheck(Net::Recv(original[1], &byte, 1, 0) == 0, "original transport closes when retained operation retires");
	PairCheck(Net::Recv(replacement_peer, &byte, 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK,
	          "replacement connection received no misrouted byte");
	Resources({baseline.sockets + 3, baseline.paths, baseline.slots + 3}, "only original survivor plus replacement remain");
	Transfer(replacement_writer, replacement_peer, false, 'n');
	PairCheck(Net::SocketClose(original[1]) == 0, "close original survivor");
	ClosePair(replacement);
	Resources(baseline, "coordinated retained close/reuse resources balanced");
}

void OrdinaryAcceptMode(const std::filesystem::path& root) {
	constexpr char guest_path[] = "/socketpair_test/accept-mode";
	std::array<uint8_t, 106> address {};
	address[0] = static_cast<uint8_t>(2 + sizeof(guest_path)); address[1] = 1;
	std::memcpy(address.data() + 2, guest_path, sizeof(guest_path));
	const int listener = Net::Socket(1, 1, 0);
	PairCheck(listener >= 0 && Net::Bind(listener, address.data(), address.size()) == 0, "ordinary AF_UNIX listener bind");
	PairCheck(Net::Listen(listener, 1) == 0 && FS::KernelFcntl(listener, 4, 6) == 0, "ordinary listener is nonblocking");
	PairCheck(Net::Accept(listener, nullptr, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK,
	          "empty ordinary accept would block");
	const int client = Net::Socket(1, 1, 0);
	PairCheck(client >= 0 && FS::KernelFcntl(client, 4, 6) == 0, "bounded ordinary nonblocking connect setup");
	const int connected = Net::Connect(client, address.data(), address.size());
	PairCheck(connected == 0 || (connected == -1 && (*Posix::GetErrorAddr() == Posix::POSIX_EINPROGRESS ||
	          *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK)), "ordinary local connect starts");
	Ready(client, true);
	int error = -1; uint32_t error_size = sizeof(error);
	PairCheck(Net::Getsockopt(client, 0xffff, 0x1007, &error, &error_size) == 0 && error == 0, "ordinary connect completes");
	Ready(listener);
	const int accepted = Net::Accept(listener, nullptr, nullptr);
#if defined(_WIN32)
	PairCheck(accepted >= 0 && FS::KernelFcntl(accepted, 3, 0) == 6, "accepted GETFL matches captured nonblocking listener mode");
#else
	PairCheck(accepted >= 0 && FS::KernelFcntl(accepted, 3, 0) == 2, "Linux accept retains its blocking default");
	PairCheck(FS::KernelFcntl(accepted, 4, 6) == 0, "make Linux accepted endpoint nonblocking for empty receive check");
#endif
	char byte = 0;
	PairCheck(Net::Recv(accepted, &byte, 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK,
	          "accepted host socket really would block on empty receive");
	PairCheck(FS::KernelFcntl(listener, 4, 2) == 0 && FS::KernelFcntl(accepted, 3, 0) == 6,
	          "accepted mode independent of subsequent listener mode changes");
	std::array<uint8_t, 106> name {}; uint32_t name_size = name.size();
	PairCheck(Net::Getsockname(accepted, name.data(), &name_size) == 0 && name_size == 2 + sizeof(guest_path) &&
	          std::memcmp(name.data() + 2, guest_path, sizeof(guest_path)) == 0, "immutable accepted name snapshot preserved");
	PairCheck(FS::KernelFcntl(accepted, 4, 2) == 0 && FS::KernelFcntl(accepted, 3, 0) == 2, "accepted SETFL restores blocking metadata");
	Transfer(client, accepted, false, 'a');
	const int nonblocking = 1;
	PairCheck(Net::Setsockopt(accepted, 0xffff, 0x1200, &nonblocking, sizeof(nonblocking)) == 0 &&
	          FS::KernelFcntl(accepted, 3, 0) == 6, "SO_NBIO and centralized GETFL share accepted mode");
	PairCheck(Net::Recv(accepted, &byte, 1, 0) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EWOULDBLOCK,
	          "accepted SO_NBIO performs real host mode change");
	PairCheck(Net::SocketClose(accepted) == 0 && Net::SocketClose(client) == 0 && Net::SocketClose(listener) == 0,
	          "close ordinary accept fixture");
	std::error_code cleanup_error;
	PairCheck(std::filesystem::remove(root / "accept-mode", cleanup_error) && !cleanup_error, "remove ordinary listener endpoint");
}

void HostHandleCleanup(PairCall call) {
#if defined(_WIN32)
	const auto baseline = Net::SocketpairResourcesForTest();
	auto cycle = [&] {
		int pair[2];
		PairCheck(call(1, 1, 0, pair) == 0, "handle-count success cycle");
		ClosePair(pair);
		for (auto failure: {Net::SocketpairFailure::AfterBind, Net::SocketpairFailure::AfterConnect,
		     Net::SocketpairFailure::AfterAccept, Net::SocketpairFailure::SecondSlot}) {
			Net::SocketpairFailForTest(failure);
			PairCheck(call(1, 1, 0, pair) == -1, "handle-count injected cleanup cycle");
		}
		PairCheck(call(1, 1, 0, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EFAULT,
		          "handle-count copyout rollback cycle");
		Resources(baseline, "handle-count cycle logical resources balanced");
	};
	// Same main thread, same success/failure paths, before concurrency fixtures.
	// Warm Winsock, BCrypt, provider lazy state and all cleanup branches first.
	for (int i = 0; i != 3; ++i) { cycle(); }
	DWORD before = 0, warmed = 0, after = 0;
	PairCheck(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "read warmed OS handle count");
	cycle();
	PairCheck(GetProcessHandleCount(GetCurrentProcess(), &warmed) != 0, "verify same-path warmup stability");
	PairCheck(warmed == before, "OS handle baseline must stabilize before leakage assertion");
	for (int i = 0; i != 8; ++i) { cycle(); }
	PairCheck(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "read post-cleanup OS handle count");
	std::fprintf(stderr, "SocketpairTests: OS handles baseline=%lu after=%lu\n",
	             static_cast<unsigned long>(warmed), static_cast<unsigned long>(after));
	PairCheck(after <= warmed, "no OS handle growth after repeated real setup/fault/success cleanup");
#endif
}

void Exhaustion(PairCall call) {
	const auto baseline = Net::SocketpairResourcesForTest();
	std::vector<int> fillers;
	constexpr int capacity = Libs::GuestDescriptors::SocketEnd - Libs::GuestDescriptors::SocketBegin;
	// Actual table occupancy, not a hook. These are real unconnected AF_UNIX
	// sockets; only pair construction creates a temporary pathname on Windows.
	for (int i = 0; i != capacity; ++i) {
		const int fd = Net::Socket(1, 1, 0);
		if (fd < 0) { break; }
		fillers.push_back(fd);
	}
	PairCheck(fillers.size() >= 2, "host supports table-exhaustion fixture");
	for (int fd = Libs::GuestDescriptors::SocketBegin; fd != Libs::GuestDescriptors::SocketEnd; ++fd) {
		PairCheck(Net::IsSocket(fd), "reached guest table capacity rather than earlier host limit");
	}
	int output[2] {8, 0};
	PairCheck(call(1, 1, 0, output) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EMFILE &&
	          output[0] == 8 && output[1] == 0, "full table preserves output");
	Resources(baseline, "full-table construction rollback");
	const int first = fillers.back(); fillers.pop_back();
	PairCheck(Net::SocketClose(first) == 0, "leave exactly one free slot");
	PairCheck(call(1, 1, 0, nullptr) == -1 && *Posix::GetErrorAddr() == Posix::POSIX_EMFILE,
	          "second slot exhaustion precedes bad pointer");
	PairCheck(!Net::IsSocket(first), "failed second allocation did not publish first slot");
	Resources(baseline, "one-free-slot construction rollback");
	const int second = fillers.back(); fillers.pop_back();
	PairCheck(Net::SocketClose(second) == 0, "leave exactly two free slots");
	PairCheck(call(1, 1, 0, output) == 0 &&
	          ((output[0] == first && output[1] == second) || (output[0] == second && output[1] == first)),
	          "pair uses exactly two available slots");
	ClosePair(output);
	for (int fd: fillers) {
		PairCheck(Net::IsSocket(fd) && Net::SocketClose(fd) == 0, "rollback preserved unrelated real sockets");
	}
	Resources(baseline, "exhaustion fixture resources balanced");
}
} // namespace

void TestSocketpairNetworkOnly() {
	PairDeadline deadline;
	const auto call = RegisteredPair();
	const auto baseline = Net::SocketpairResourcesForTest();
	Exchange(call, false); Flags(call); Failures(call); HostHandleCleanup(call);
	RetainedCloseReuse(call); ConcurrentReuse(call); Exhaustion(call);
	Resources(baseline, "complete network-only fixture resource balance");
}

void TestSocketpairFileDescriptors(const std::filesystem::path& root) {
	PairDeadline deadline;
	const auto call = RegisteredPair();
	FS::Mount(root, "/socketpair_test");
	OrdinaryAcceptMode(root);
	{ std::ofstream fixture(root / "pair-fd.txt", std::ios::binary); fixture << "original-file"; }
	std::vector<int> held;
	bool crossed = false;
	for (int i = 0; i != 140; ++i) {
		const int fd = FS::KernelOpen("/socketpair_test/pair-fd.txt", 0, 0);
		PairCheck(fd >= 3 && !Libs::GuestDescriptors::IsSocketRange(fd), "filesystem skips canonical Net range");
		held.push_back(fd); crossed |= fd >= Libs::GuestDescriptors::SocketEnd;
	}
	PairCheck(crossed, "file allocation continues beyond 1023 without new count cap");
	Exchange(call, true); // real KernelRead/Write dispatch while file range is populated
	std::atomic_bool start {false};
	std::thread files([&] {
		while (!start.load()) { std::this_thread::yield(); }
		for (int i = 0; i != 48; ++i) {
			const int fd = FS::KernelOpen("/socketpair_test/pair-fd.txt", 0, 0);
			PairCheck(fd >= 1024 && !Net::IsSocket(fd), "concurrent filesystem allocation disjoint");
			PairCheck(FS::KernelClose(fd) == 0, "close concurrent file");
		}
	});
	start = true; ConcurrentReuse(call); files.join();
	for (int fd: held) {
		char byte = 0;
		PairCheck(FS::KernelRead(fd, &byte, 1) == 1 && byte == 'o', "pair lifecycle preserved original filesystem ownership");
		PairCheck(FS::KernelClose(fd) == 0, "close held file");
	}
}
