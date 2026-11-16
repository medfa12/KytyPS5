// Included inside Network::Net, after its transport table helpers. See
// networkSocketpair.md for the audited native subset and unsupported semantics.
namespace {

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
thread_local SocketpairFailure g_pair_failure = SocketpairFailure::None;
thread_local void (*g_retained_send_hook)(void*) = nullptr;
thread_local void* g_retained_send_context = nullptr;
bool PairFail(SocketpairFailure point) {
	if (g_pair_failure != point) { return false; }
	g_pair_failure = SocketpairFailure::None;
	return true;
}
#endif

void AdoptPairSocket(SocketTransport& owner, NativeSocket socket) {
	owner.socket = socket;
	owner.pair_owned = true;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (socket != INVALID_NATIVE_SOCKET) { ++g_pair_host_sockets; }
#endif
}

int PairSetNonblocking(SocketTransport& transport, bool enabled) {
#if defined(_WIN32)
	u_long mode = enabled ? 1 : 0;
	if (ioctlsocket(transport.socket, FIONBIO, &mode) != 0) { return SetHostSocketError(); }
#else
	const int flags = ::fcntl(transport.socket, F_GETFL, 0);
	if (flags < 0 || ::fcntl(transport.socket, F_SETFL,
	    enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) != 0) { return SetHostSocketError(); }
#endif
	transport.pair_nonblocking = enabled;
	return 0;
}

int SetSocketNonblocking(int fd, const SocketSlot& expected, bool enabled) {
	// Lock order is mode -> table, never table -> mode. Only this transport's
	// mode setters serialize; no host ioctl/fcntl executes with the table held.
	Common::HostLockCriticalGuard mode_critical;
	Common::LockGuard mode_lock(expected.transport->mode_mutex);
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		const auto& slot = g_sockets[fd];
		if (slot.generation != expected.generation || slot.transport != expected.transport) {
			return SetGuestSocketError(Posix::POSIX_EBADF);
		}
	}
	// Concurrent close after revalidation may remove the slot, but cannot
	// redirect this operation or its atomic mode metadata to a replacement.
	return PairSetNonblocking(*expected.transport, enabled);
}

#if defined(_WIN32)
using PairCopyout = LONG (NTAPI *)(HANDLE, PVOID, PVOID, SIZE_T, PSIZE_T);
PairCopyout ResolvePairCopyout() {
	// Resolve before taking the socket-table lock, independently of Memory::Initialize.
	const auto ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll == nullptr ? nullptr : reinterpret_cast<PairCopyout>(
	    GetProcAddress(ntdll, "NtWriteVirtualMemory"));
}
bool PairCopy(PairCopyout copy, int* destination, const int (&fds)[2]) {
	const auto begin = reinterpret_cast<uintptr_t>(destination);
	if (begin == 0 || begin > UINTPTR_MAX - sizeof(fds)) { return false; }
	// Reject guards without consuming them. NtWriteVirtualMemory also enforces
	// permissions when a concurrent protect/unmap invalidates this preflight.
	for (auto cursor = begin; cursor < begin + sizeof(fds);) {
		MEMORY_BASIC_INFORMATION memory {};
		if (VirtualQuery(reinterpret_cast<void*>(cursor), &memory, sizeof(memory)) == 0 ||
		    memory.State != MEM_COMMIT || (memory.Protect & PAGE_GUARD) != 0) { return false; }
		const DWORD protection = memory.Protect & 0xff;
		if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
		    protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY) { return false; }
		const auto base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
		if (memory.RegionSize > UINTPTR_MAX - base || base + memory.RegionSize <= cursor) { return false; }
		cursor = base + memory.RegionSize;
	}
	SIZE_T written = 0;
	return copy != nullptr && copy(GetCurrentProcess(), destination,
	    const_cast<int*>(fds), sizeof(fds), &written) >= 0 && written == sizeof(fds);
}

class PairListener {
public:
	SocketTransport transport {INVALID_NATIVE_SOCKET};
	char directory[MAX_PATH] {};
	char endpoint[108] {};
	bool bound = false;
	~PairListener() { Cleanup(); }
	bool Cleanup() {
		bool okay = transport.Close() == 0;
		if (bound) {
			if (DeleteFileA(endpoint) || GetLastError() == ERROR_FILE_NOT_FOUND) { bound = false; }
		}
		if (bound) { okay = false; }
		if (directory[0] != '\0' && !bound && RemoveDirectoryA(directory)) {
			directory[0] = '\0';
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			--g_pair_paths;
#endif
		}
		return okay && directory[0] == '\0';
	}
	int CreatePath() {
		char root[MAX_PATH] {};
		const DWORD length = GetTempPathA(MAX_PATH, root);
		if (length == 0 || length >= MAX_PATH) { return SetGuestSocketError(Posix::POSIX_EIO); }
		for (int attempt = 0; attempt != 8; ++attempt) {
			unsigned char nonce[16] {};
			if (BCryptGenRandom(nullptr, nonce, sizeof(nonce), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
				return SetGuestSocketError(Posix::POSIX_EIO);
			}
			char hex[33] {};
			constexpr char digits[] = "0123456789abcdef";
			for (size_t i = 0; i != sizeof(nonce); ++i) {
				hex[2*i] = digits[nonce[i] >> 4]; hex[2*i+1] = digits[nonce[i] & 15];
			}
			char candidate[MAX_PATH] {};
			const int n = std::snprintf(candidate, sizeof(candidate), "%skp-%s", root, hex);
			if (n <= 0 || static_cast<size_t>(n) + 3 >= sizeof(endpoint)) {
				return SetGuestSocketError(Posix::POSIX_ENAMETOOLONG);
			}
			// Exclusive directory ownership: never unlink a path from a failed bind
			// or delete an existing candidate. No guest path or mount participates.
			if (!CreateDirectoryA(candidate, nullptr)) {
				if (GetLastError() == ERROR_ALREADY_EXISTS) { continue; }
				return SetGuestSocketError(Posix::POSIX_EACCES);
			}
			std::memcpy(directory, candidate, static_cast<size_t>(n) + 1);
			std::snprintf(endpoint, sizeof(endpoint), "%s\\s", directory);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			++g_pair_paths;
#endif
			return 0;
		}
		return SetGuestSocketError(Posix::POSIX_EADDRINUSE);
	}
};

int PairWait(NativeSocket socket, bool writing, std::chrono::steady_clock::time_point deadline) {
	const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline -
	    std::chrono::steady_clock::now()).count();
	if (remaining <= 0) { return SetGuestSocketError(Posix::POSIX_ETIMEDOUT); }
	fd_set ready {}, errors {};
	FD_ZERO(&ready); FD_ZERO(&errors); FD_SET(socket, &ready); FD_SET(socket, &errors);
	timeval timeout {static_cast<long>(remaining / 1000000), static_cast<long>(remaining % 1000000)};
	const int result = ::select(0, writing ? nullptr : &ready, writing ? &ready : nullptr, &errors, &timeout);
	if (result < 0) { return SetHostSocketError(); }
	if (result == 0) { return SetGuestSocketError(Posix::POSIX_ETIMEDOUT); }
	int error = 0; int size = sizeof(error);
	if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) != 0) {
		return SetHostSocketError();
	}
	if (error != 0) { return SetGuestSocketError(ConvertHostSocketError(error)); }
	return 0;
}
#else
using PairCopyout = bool;
PairCopyout ResolvePairCopyout() {
#if defined(__linux__)
	return true;
#else
	return false; // no unsafe memcpy fallback on an unsupported host
#endif
}
bool PairCopy(PairCopyout, int* destination, const int (&fds)[2]) {
#if defined(__linux__)
	iovec source {const_cast<int*>(fds), sizeof(fds)}, target {destination, sizeof(fds)};
	return ::process_vm_writev(::getpid(), &source, 1, &target, 1, 0) == sizeof(fds);
#else
	return false;
#endif
}
#endif

int BuildPair(SocketTransport& first, SocketTransport& second) {
#if defined(_WIN32)
	PairListener listener;
	struct FailureEndpoints {
		SocketTransport& first;
		SocketTransport& second;
		bool complete = false;
		~FailureEndpoints() { if (!complete) { first.Close(); second.Close(); } }
	} retire {first, second}; // close connections before listener/path cleanup on failure
	if (listener.CreatePath() != 0) { return -1; }
	AdoptPairSocket(listener.transport, ::socket(AF_UNIX, SOCK_STREAM, 0));
	if (listener.transport.socket == INVALID_NATIVE_SOCKET) { return SetHostSocketError(); }
	sockaddr_un address {};
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, listener.endpoint, std::strlen(listener.endpoint) + 1);
	if (::bind(listener.transport.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
		return SetHostSocketError();
	}
	listener.bound = true;
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (PairFail(SocketpairFailure::AfterBind)) { return SetGuestSocketError(Posix::POSIX_EIO); }
#endif
	if (::listen(listener.transport.socket, 1) != 0) { return SetHostSocketError(); }
	AdoptPairSocket(first, ::socket(AF_UNIX, SOCK_STREAM, 0));
	if (first.socket == INVALID_NATIVE_SOCKET) { return SetHostSocketError(); }
	if (PairSetNonblocking(listener.transport, true) != 0 || PairSetNonblocking(first, true) != 0) { return -1; }
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	if (::connect(first.socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
		const int error = WSAGetLastError();
		if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) {
			return SetGuestSocketError(ConvertHostSocketError(error));
		}
		if (PairWait(first.socket, true, deadline) != 0) { return -1; }
	}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (PairFail(SocketpairFailure::AfterConnect)) { return SetGuestSocketError(Posix::POSIX_EIO); }
#endif
	if (PairWait(listener.transport.socket, false, deadline) != 0) { return -1; }
	AdoptPairSocket(second, ::accept(listener.transport.socket, nullptr, nullptr));
	if (second.socket == INVALID_NATIVE_SOCKET) { return SetHostSocketError(); }
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (PairFail(SocketpairFailure::AfterAccept)) { return SetGuestSocketError(Posix::POSIX_EIO); }
#endif
	if (PairSetNonblocking(first, false) != 0 || PairSetNonblocking(second, false) != 0) { return -1; }
	// Do not report success while the construction pathname/listener remains live.
	if (!listener.Cleanup()) { return SetGuestSocketError(Posix::POSIX_EIO); }
	retire.complete = true;
	return 0;
#else
	int fds[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) { return SetHostSocketError(); }
	AdoptPairSocket(first, fds[0]); AdoptPairSocket(second, fds[1]);
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
	if (PairFail(SocketpairFailure::AfterAccept)) { return SetGuestSocketError(Posix::POSIX_EIO); }
#endif
	return 0;
#endif
}

bool PairSendClosed(const SocketTransport& transport) {
	const auto& state = transport.pair;
	const unsigned self = transport.pair_index, peer = self ^ 1;
	return state && (state->closed[peer].load() || state->read_shutdown[peer].load() ||
	                 state->write_shutdown[self].load());
}
} // namespace

int __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Socketpair(int family, int type, int protocol, int* output) {
	// Native 13.60: only these two bits are stripped. CLOEXEC is ignored by
	// socketpair, and its two allocation flag words are zero.
	if (family != 1) {
		return SetGuestSocketError(family <= 0 || family > 255 ? Posix::POSIX_EAFNOSUPPORT : Posix::POSIX_EOPNOTSUPP);
	}
	const unsigned base_type = static_cast<unsigned>(type) & ~0x30000000u;
	if (protocol != 0 || base_type == 0) { return SetGuestSocketError(Posix::POSIX_EPROTONOSUPPORT); }
	if (base_type != 1) {
		return SetGuestSocketError(base_type == 2 || base_type == 5 ? Posix::POSIX_EOPNOTSUPP : Posix::POSIX_EPROTOTYPE);
	}
	const auto copy = ResolvePairCopyout();
	if (!copy) { return SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
	if (!EnsureSocketBackend()) { return SetGuestSocketError(Posix::POSIX_ENETDOWN); }
	try {
		// All potentially allocating metadata and all transport construction precede
		// the publication lock. Unpublished owners unwind on every failure path.
		auto state = std::make_shared<SocketPairState>();
		auto first = std::make_shared<SocketTransport>(INVALID_NATIVE_SOCKET);
		auto second = std::make_shared<SocketTransport>(INVALID_NATIVE_SOCKET);
		first->pair = second->pair = state;
		second->pair_index = 1;
		if (BuildPair(*first, *second) != 0) { return -1; }
		if ((static_cast<unsigned>(type) & 0x20000000u) != 0 &&
		    (PairSetNonblocking(*first, true) != 0 || PairSetNonblocking(*second, true) != 0)) { return -1; }
		int result = 0;
		{
			Common::HostLockCriticalGuard critical;
			Common::LockGuard lock(g_socket_mutex);
			int fds[2] {-1, -1};
			for (int fd = SOCKET_FD_MIN; fd < SOCKET_FD_MAX; ++fd) {
				if (!g_sockets[fd].transport) {
					if (fds[0] < 0) { fds[0] = fd; } else { fds[1] = fd; break; }
				}
			}
#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
			if (PairFail(SocketpairFailure::SecondSlot)) { fds[1] = -1; }
#endif
			if (fds[1] < 0) { result = Posix::POSIX_EMFILE; }
			else {
				g_sockets[fds[0]].transport = first;
				g_sockets[fds[1]].transport = second;
				g_sockets[fds[0]].generation = ++g_socket_generation;
				g_sockets[fds[1]].generation = ++g_socket_generation;
				if (!PairCopy(copy, output, fds)) {
					// Still under the same short lock: no close/reuse race. Owners
					// remain above, so slot removal runs no host close/destructor here.
					g_sockets[fds[0]] = {};
					g_sockets[fds[1]] = {};
					result = Posix::POSIX_EFAULT;
				}
			}
		}
		return result == 0 ? 0 : SetGuestSocketError(result);
	} catch (const std::bad_alloc&) { return SetGuestSocketError(Posix::POSIX_ENOMEM); }
}

bool SocketpairFcntl(int fd, int command, int64_t argument, int* result) {
	if (!GuestDescriptors::IsSocketRange(fd)) { return false; }
	// Includes empty slots, eliminating check-then-act fallback to a stale fd map.
	SocketSlot retained;
	{
		Common::HostLockCriticalGuard critical;
		Common::LockGuard lock(g_socket_mutex);
		auto& slot = g_sockets[fd];
		if (!slot.transport) { *result = SetGuestSocketError(Posix::POSIX_EBADF); return true; }
		const bool pair = slot.transport->pair != nullptr;
		*result = 0;
		switch (command) {
			case 1: *result = pair && slot.cloexec ? 1 : 0; break; // F_GETFD
			case 2: // F_SETFD; metadata only, no exec/dup inheritance is claimed.
				if (!pair) { break; } // preserve legacy non-pair FD flag behavior
				if ((argument & ~int64_t{1}) != 0) { *result = SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
				else { slot.cloexec = (argument & 1) != 0; }
				break;
			case 3:
				*result = 2 | ((slot.p2p ? slot.p2p->nonblocking != 0 : slot.transport->pair_nonblocking.load()) ? 4 : 0);
				break;
			case 4:
				if (pair && (argument & ~int64_t{7}) != 0) { *result = SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
				else if (slot.p2p) { slot.p2p->nonblocking = (argument & 4) != 0; }
				else { retained = slot; }
				break;
			default:
				// Preserve the older non-pair floor, but never certify pair dup/locks
				// or unsupported operations with that legacy success behavior.
				if (pair) { *result = SetGuestSocketError(Posix::POSIX_EOPNOTSUPP); }
				break;
		}
	}
	if (retained.transport) { *result = SetSocketNonblocking(fd, retained, (argument & 4) != 0); }
	return true;
}

#if defined(KYTY_VIRTUAL_MEMORY_ALLOCATION_TESTS)
void SocketpairFailForTest(SocketpairFailure failure) { g_pair_failure = failure; }
void SocketRetainedSendHookForTest(void (*hook)(void*), void* context) {
	g_retained_send_hook = hook;
	g_retained_send_context = context;
}
SocketpairResources SocketpairResourcesForTest() {
	Common::HostLockCriticalGuard critical;
	Common::LockGuard lock(g_socket_mutex);
	int slots = 0;
	for (const auto& slot: g_sockets) { slots += slot.transport && slot.transport->pair ? 1 : 0; }
	return {g_pair_host_sockets.load(), g_pair_paths.load(), slots};
}
#endif
