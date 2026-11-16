#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
namespace Net = Libs::Network::Net;
namespace FS = Libs::LibKernel::FileSystem;
namespace Posix = Libs::Posix;

void Check(bool okay, const char* message) {
	if (!okay) {
		std::fprintf(stderr, "NetworkP2pOwnershipTests: %s (errno=%d)\n", message, *Posix::GetErrorAddr());
		std::abort();
	}
}

std::array<unsigned char, 16> Address(unsigned virtual_port) {
	// BSD sockaddr_in plus virtual port, network byte order. The implementation
	// supports physical UDP port 3658; bind only to host loopback in this fixture.
	return {16, 2, 0x0e, 0x4a, 127, 0, 0, 1,
	        static_cast<unsigned char>(virtual_port >> 8), static_cast<unsigned char>(virtual_port)};
}

void Name(int fd, unsigned virtual_port) {
	std::array<unsigned char, 16> actual {};
	uint32_t length = actual.size();
	Check(Net::Getsockname(fd, actual.data(), &length) == 0 && length == actual.size(), "read retained P2P address");
	Check(actual == Address(virtual_port), "physical address shared, virtual port independent");
}
}

void TestNetworkP2pOwnership() {
	// The process-level CTest timeout bounds a failed bind waiter. This exercises
	// sharing and conflict semantics, not guest suspension or deterministic timing.
	for (unsigned iteration = 0; iteration != 8; ++iteration) {
		const unsigned first_port = 14000 + 2 * iteration, second_port = first_port + 1;
		const auto first_address = Address(first_port), second_address = Address(second_port);
		const int first = Net::Socket(2, 6, 0), second = Net::Socket(2, 6, 0);
		Check(first >= 0 && second >= 0 && first != second, "create two logical P2P descriptors");
		std::atomic_bool start {false};
		int results[2] {-2, -2};
		int errors[2] {};
		auto bind = [&](int index, int fd, const auto& address) {
			while (!start.load()) { std::this_thread::yield(); }
			results[index] = Net::Bind(fd, address.data(), address.size());
			errors[index] = *Posix::GetErrorAddr();
		};
		std::thread left([&] { bind(0, first, first_address); });
		std::thread right([&] { bind(1, second, second_address); });
		start = true;
		left.join(); right.join();
		if (results[0] != 0 || results[1] != 0) {
			std::fprintf(stderr, "P2P binds returned (%d, %d), errno=(%d, %d)\n", results[0], results[1], errors[0], errors[1]);
		}
		Check(results[0] == 0 && results[1] == 0, "concurrent distinct virtual ports share one physical binding");
		Name(first, first_port); Name(second, second_port);
		const int conflict = Net::Socket(2, 6, 0);
		Check(conflict >= 0, "create conflict descriptor");
		Check(Net::Bind(conflict, first_address.data(), first_address.size()) == -1 &&
		      *Posix::GetErrorAddr() == Posix::POSIX_EADDRINUSE, "duplicate virtual port rejected");
		Check(Net::SocketClose(conflict) == 0, "close unpublished conflict socket");
		const int one = 1;
		Check(Net::Setsockopt(first, 0xffff, 0x1200, &one, sizeof(one)) == 0 &&
		      FS::KernelFcntl(first, 3, 0) == 6 && FS::KernelFcntl(second, 3, 0) == 2,
		      "nonblocking metadata belongs to logical descriptor");
		Check(Net::SocketClose(first) == 0, "close one shared transport owner");
		Name(second, second_port);
		const int replacement = Net::Socket(2, 6, 0);
		Check(replacement == first, "reuse closed numeric slot");
		Check(Net::Bind(replacement, first_address.data(), first_address.size()) == 0, "released virtual port rejoins surviving physical owner");
		Name(replacement, first_port);
		Check(FS::KernelFcntl(replacement, 3, 0) == 2, "replacement does not inherit closed logical mode");
		Check(Net::SocketClose(second) == 0 && Net::SocketClose(replacement) == 0, "retire all shared owners");
	}
}
