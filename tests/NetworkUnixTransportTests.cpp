#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/network.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {

namespace Net        = Libs::Network::Net;
namespace FileSystem = Libs::LibKernel::FileSystem;
namespace Posix      = Libs::Posix;

constexpr int AF_LOCAL_GUEST     = 1;
constexpr int SOCK_STREAM_GUEST  = 1;
constexpr uint32_t GUEST_SUN_LEN = 106; // u8 len, u8 family, char path[104]

void Check(bool value, const char* text) {
	if (std::getenv("KYTY_TEST_TRACE") != nullptr) { std::fprintf(stderr, "check: %s\n", text); }
	if (!value) {
		std::fprintf(stderr, "NetworkUnixTransportTests: failed: %s\n", text);
		std::abort();
	}
}

int Errno() {
	return *Posix::GetErrorAddr();
}

// Guest layout exactly as libSceComposite builds it: zero filled, family byte 1, path at offset 2.
std::array<uint8_t, GUEST_SUN_LEN> GuestAddress(const std::string& path) {
	std::array<uint8_t, GUEST_SUN_LEN> address {};
	address[0] = static_cast<uint8_t>(2 + path.size() + 1);
	address[1] = AF_LOCAL_GUEST;
	Check(path.size() <= GUEST_SUN_LEN - 2, "guest path fits fixture");
	std::memcpy(address.data() + 2, path.data(), path.size());
	return address;
}

class TempDirectory {
public:
	TempDirectory() {
		const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
		m_path            = std::filesystem::temp_directory_path() /
		         ("kyty_unix_" + std::to_string(unique % 1000000));
		Check(std::filesystem::create_directories(m_path), "create temporary directory");
	}
	~TempDirectory() {
		std::error_code error;
		std::filesystem::remove_all(m_path, error);
	}
	const std::filesystem::path& Path() const { return m_path; }

private:
	std::filesystem::path m_path;
};

void CheckPathRejection(const std::filesystem::path& root) {
	const int client = Net::Socket(AF_LOCAL_GUEST, SOCK_STREAM_GUEST, 0);
	Check(client >= 0, "AF_LOCAL stream sockets are created");

	// Empty path (only the 2 byte header, and a NUL first byte).
	std::array<uint8_t, 2> header {2, AF_LOCAL_GUEST};
	Check(Net::Connect(client, header.data(), header.size()) == -1 && Errno() == Posix::POSIX_ENOENT,
	      "header-only sockaddr_un is an empty path");
	auto empty = GuestAddress("");
	Check(Net::Connect(client, empty.data(), empty.size()) == -1 && Errno() == Posix::POSIX_ENOENT,
	      "NUL first byte is an empty path");

	// A path outside every mount never reaches the host filesystem.
	auto unmapped = GuestAddress("/not_mounted/GnmCompositor");
	Check(Net::Connect(client, unmapped.data(), unmapped.size()) == -1 &&
	          Errno() == Posix::POSIX_ENOENT,
	      "unmapped guest path is ENOENT");
	Check(Net::Bind(client, unmapped.data(), unmapped.size()) == -1 &&
	          Errno() == Posix::POSIX_ENOENT,
	      "bind of an unmapped guest path is ENOENT");

	// Larger than a BSD sockaddr_un.
	std::array<uint8_t, GUEST_SUN_LEN + 1> oversized {};
	oversized[1] = AF_LOCAL_GUEST;
	oversized[2] = '/';
	Check(Net::Connect(client, oversized.data(), oversized.size()) == -1 &&
	          Errno() == Posix::POSIX_EINVAL,
	      "oversized guest sockaddr_un is EINVAL");

	// Fits the guest structure but not the host sun_path: rejected, never truncated.
	const std::string long_name(90, 'a');
	Check(Common::PathToString(root).size() + 1 + long_name.size() >= 108,
	      "fixture host path exceeds sun_path");
	auto too_long = GuestAddress("/system_tmp/" + long_name);
	Check(Net::Bind(client, too_long.data(), too_long.size()) == -1 &&
	          Errno() == Posix::POSIX_ENAMETOOLONG,
	      "overlong host path is ENAMETOOLONG");

	Check(Net::SocketClose(client) == 0, "close rejection socket");
}

void CheckLoopbackExchange(const std::filesystem::path& root) {
	const std::string guest_path = "/system_tmp/GnmCompositorTest";
	const auto        host_path  = root / "GnmCompositorTest";
	auto              address    = GuestAddress(guest_path);

	const int server = Net::Socket(AF_LOCAL_GUEST, SOCK_STREAM_GUEST, 0);
	Check(server >= 0, "create server socket");
	Check(Net::Bind(server, address.data(), address.size()) == 0, "bind guest AF_LOCAL path");
	// Windows AF_UNIX endpoints are reparse points; status() may throw on them.
	std::error_code endpoint_error;
	Check(std::filesystem::symlink_status(host_path, endpoint_error).type() !=
	          std::filesystem::file_type::not_found,
	      "bind created the mapped host endpoint");
	Check(Net::Listen(server, 1) == 0, "listen");

	const int duplicate = Net::Socket(AF_LOCAL_GUEST, SOCK_STREAM_GUEST, 0);
	Check(Net::Bind(duplicate, address.data(), address.size()) == -1, "address already in use");
	Check(Net::SocketClose(duplicate) == 0, "close duplicate");

	// getsockname reports the guest name, truncates to capacity and returns the full length.
	std::array<uint8_t, 64> name {};
	uint32_t                name_size = name.size();
	Check(Net::Getsockname(server, name.data(), &name_size) == 0, "listener getsockname");
	Check(name_size == 2 + guest_path.size() + 1 && name[0] == name_size && name[1] == 1 &&
	          std::memcmp(name.data() + 2, guest_path.c_str(), guest_path.size() + 1) == 0,
	      "listener reports the bound guest sockaddr_un");
	uint32_t zero_capacity = 0;
	Check(Net::Getsockname(server, name.data(), &zero_capacity) == 0 &&
	          zero_capacity == 2 + guest_path.size() + 1,
	      "zero capacity still reports the complete length");

	const int client = Net::Socket(AF_LOCAL_GUEST, SOCK_STREAM_GUEST, 0);
	Check(client >= 0, "create client socket");
#if defined(_WIN32)
	const int one = 1;
	Check(Net::Setsockopt(client, 0xffff, 0x800, &one, sizeof(one)) == 0, "SO_NOSIGPIPE succeeds");
	Check(Net::Setsockopt(client, 0xffff, 0x800, &one, 1) == -1 && Errno() == Posix::POSIX_EINVAL,
	      "SO_NOSIGPIPE validates optlen");
#endif
	Check(Net::Connect(client, address.data(), address.size()) == 0, "connect with full-size guest sockaddr");

	// Accept with a null buffer and a non-null length (ignored) like the compositor server.
	uint32_t ignored = 0;
	const int accepted = Net::Accept(server, nullptr, &ignored);
	Check(accepted >= 0, "accept connection");
	name_size = name.size();
	Check(Net::Getsockname(accepted, name.data(), &name_size) == 0 &&
	          name_size == 2 + guest_path.size() + 1,
	      "accepted endpoint shares the listener name");
	name_size = name.size();
	Check(Net::Getsockname(client, name.data(), &name_size) == 0 && name_size == 16 && name[1] == 1,
	      "connected client is unnamed");
	int peer_pid = 0;
	Check(Net::GetUnixPeerProcessId(accepted, &peer_pid) == 0 &&
	          peer_pid == Net::GUEST_PROCESS_ID,
	      "accepted peer reports the guest process id");

	// Byte exchange: 16 byte request header, 16 byte reply (the client wire shape).
	const std::array<uint8_t, 16> request {1, 0, 0, 0, 0, 0x30, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0};
	Check(Net::Send(client, request.data(), request.size(), 0) == 16, "client sends request");
	std::array<uint8_t, 16> received {};
	Check(Net::Recv(accepted, received.data(), received.size(), 0) == 16 && received == request,
	      "server receives the request bytes");
	const std::array<uint8_t, 16> reply {0, 0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0};
	Check(Net::Send(accepted, reply.data(), reply.size(), 0) == 16, "server sends reply");
	std::array<uint8_t, 16> answer {};
	Check(Net::Recv(client, answer.data(), answer.size(), 0) == 16 && answer == reply,
	      "client receives the reply bytes");

	Check(Net::SocketClose(accepted) == 0 && Net::SocketClose(client) == 0 &&
	          Net::SocketClose(server) == 0,
	      "close sockets");
	std::error_code error;
	std::filesystem::remove(host_path, error);
}

} // namespace

int main() {
	Common::InitializeThreads();
	Common::Subsystems subsystems;
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();

	TempDirectory temporary;
	FileSystem::Initialize();
	FileSystem::Mount(temporary.Path(), "/system_tmp");
	CheckPathRejection(temporary.Path());
	CheckLoopbackExchange(temporary.Path());
	FileSystem::Shutdown();
	subsystems.Destroy();

	std::printf("NetworkUnixTransportTests: all cases passed\n");
	return 0;
}
