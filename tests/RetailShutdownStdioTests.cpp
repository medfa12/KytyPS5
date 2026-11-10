// Offline: production GuestFopen + registered CFread/CFseek/CFclose, mounted
// retail assets. No guest code or renderer execution. Only the host-buffer GPU
// invalidation dependency is replaced, as in GuestDirectoryTests.
#include "common/abi.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/fileSystem.h"
#include "libs/errno.h"
#include "libs/libGuestFile.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Libs::LibcInternal {
void* KYTY_SYSV_ABI GuestFopen(const char*, const char*);
size_t KYTY_SYSV_ABI GuestFread(void*, size_t, size_t, void*);
int KYTY_SYSV_ABI GuestFclose(void*);
}
namespace Libs::ShellCoreFloor::LibcFloor { void InitShellCoreFloorLibc_1(Loader::SymbolDatabase*); }
namespace Libs::LibKernel::Memory { void InvalidateMemory(uint64_t, uint64_t) {} }

using Call = uint64_t (KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t);
static void Check(bool ok, const char* message) { if (!ok) { throw std::runtime_error(message); } }
static uint64_t Address(const void* p) { return reinterpret_cast<uint64_t>(p); }
static std::vector<unsigned char> Bytes(const std::filesystem::path& path) {
	std::ifstream stream(path, std::ios::binary);
	Check(stream.is_open(), "fixture input missing");
	return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) try {
	if (argc != 3) { return 2; }
	namespace FS = Libs::LibKernel::FileSystem;
	namespace GF = Libs::GuestFile;
	using namespace Libs::LibcInternal;
	Common::InitializeThreads();
	Common::Subsystems systems;
	systems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Silent;
	Config::Load(options);
	systems.Initialize<Log::Lifecycle>();
	FS::Initialize();
	FS::Mount(argv[1], "/system_ex/vsh_asset", true);
	FS::Mount(argv[2], "/fixture", true);
	Loader::SymbolDatabase symbols;
	Libs::ShellCoreFloor::LibcFloor::InitShellCoreFloorLibc_1(&symbols);
	const auto resolve = [&](const char* nid) {
		const auto* record = symbols.FindByNid(nid, Loader::SymbolType::Func);
		Check(record != nullptr, "production stdio export missing");
		return reinterpret_cast<Call>(record->vaddr);
	};
	const auto read = resolve("lbB+UlZqVG0");
	const auto seek = resolve("rQFVBXp-Cxg");
	const auto close = resolve("uodLYyUip20");
	for (bool direct: {false, true}) {
		Libs::SetDirectTitleBoot(direct);
		for (const char* asset: {"shutdown_ramp.gnf", "Sce.Vsh.ShellUI.BGLayer.Particle0.gnf", "Sce.Vsh.ShellUI.BGLayer.Particle1.gnf"}) {
			const auto expected = Bytes(std::filesystem::path(argv[1]) / asset);
			Check(expected.size() >= 72 && std::memcmp(expected.data(), "GNF ", 4) == 0, "pinned GNF fixture header");
			const auto path = std::string("/system_ex/vsh_asset/") + asset;
			auto* stream = GuestFopen(path.c_str(), "rb");
			Check(stream != nullptr && GF::RegisteredForeignShimHost(stream), "production fopen registration");
			Libs::LibKernel::FileSystem::FileStat info {};
			Check(FS::KernelStat(path.c_str(), &info) == 0 && info.st_size == static_cast<int64_t>(expected.size()), "native-style intervening stat and asset size");
			std::array<unsigned char, 72> header {};
			Check(read(Address(header.data()), 72, 1, Address(stream)) == 1, "native GNF header fread must return one item");
			Check(std::memcmp(header.data(), expected.data(), header.size()) == 0 && GF::Tell(stream) == 72, "header bytes and cursor");
			if (std::strcmp(asset, "shutdown_ramp.gnf") == 0) {
				// Actual IDA +0x15cee0 computes 0xf8+8+0 for this pinned v5
				// asset; +0x15cdbE..dc then reads the 0x800-byte payload as 1 item.
				Check(expected.size() == 2304 && seek(Address(stream), 256, SEEK_SET, 0) == 0, "native shutdown payload seek");
				std::array<unsigned char, 2048> payload {};
				Check(read(Address(payload.data()), payload.size(), 1, Address(stream)) == 1 &&
				      std::memcmp(payload.data(), expected.data() + 256, payload.size()) == 0, "native shutdown payload bytes");
			}
			Check(seek(Address(stream), 0, SEEK_SET, 0) == 0, "cross-family rewind seek");
			std::vector<unsigned char> all(expected.size());
			Check(read(Address(all.data()), all.size(), 1, Address(stream)) == 1 && all == expected, "full GNF item and exact bytes");
			header.fill(0xcc);
			Check(read(Address(header.data()), 72, 1, Address(stream)) == 0 && header[0] == 0xcc, "EOF is not fabricated success");
			Check(GF::Seek(stream, 0, SEEK_SET) == 0 && GF::Getc(stream) == 'G' && GF::Tell(stream) == 1, "shared getc cursor");
			Check(close(Address(stream), 0, 0, 0) == 0 && !GF::RegisteredForeignShimHost(stream), "CFclose returns to owner and unregisters");
			Check(read(Address(header.data()), 1, 1, Address(stream)) == 0, "closed stream rejected");
			Check(GuestFclose(stream) == -1, "owning close rejects closed shim");
		}
		// _Fiopen-family own streams (RNPS style) still work unchanged.
		auto* own = GF::Open("/system_ex/vsh_asset/shutdown_ramp.gnf", 0);
		std::array<unsigned char, 72> header {};
		Check(own && read(Address(header.data()), 72, 1, Address(own)) == 1, "own-family reader regression");
		Check(GF::Seek(own, 0, SEEK_SET) == 0 && GuestFread(header.data(), 72, 1, own) == 1, "reverse-family read regression");
		Check(close(Address(own), 0, 0, 0) == 0 && !GF::OwnShimHost(own), "own-family close");
	}
	Libs::SetDirectTitleBoot(false);
	std::array<unsigned char, 8> buffer {};
	Check(read(Address(buffer.data()), 1, 1, 1) == 0 && GF::Seek(reinterpret_cast<void*>(1), 0, SEEK_SET) == -1, "unknown pointer rejected");
	FILE* raw = std::fopen((std::filesystem::path(argv[1]) / "shutdown_ramp.gnf").string().c_str(), "rb");
	Check(raw && read(Address(buffer.data()), 1, 1, Address(raw)) == 0 && std::ftell(raw) == 0, "unregistered host FILE rejected");
	std::fclose(raw);
	Check(!GuestFopen("/fixture/missing", "rb"), "missing file remains missing");
	Check(!GuestFopen("/system_ex/vsh_asset/shutdown_ramp.gnf", "wb") && *Libs::Posix::GetErrorAddr() == Libs::Posix::POSIX_EROFS, "retail asset remains read-only");
	auto* short_stream = GuestFopen("/fixture/short.bin", "rb");
	Check(short_stream, "short fixture opens");
	Check(read(Address(buffer.data()), 4, 2, Address(short_stream)) == 1 && GF::Tell(short_stream) == 5, "partial item semantics");
	Check(buffer[0] == 1 && buffer[4] == 5, "partial bytes preserved");
	Check(seek(Address(short_stream), 0, SEEK_SET, 0) == 0, "seek after short read");
	Check(read(Address(buffer.data()), 0, 2, Address(short_stream)) == 0 && GF::Tell(short_stream) == 0, "zero size does not advance");
	Check(read(Address(buffer.data()), 2, std::numeric_limits<size_t>::max(), Address(short_stream)) == 0 && GF::Tell(short_stream) == 0, "overflow does not advance");
	Check(read(0, 1, 1, Address(short_stream)) == 0 && GF::Tell(short_stream) == 0, "null buffer rejected");
	Check(close(Address(short_stream), 0, 0, 0) == 0, "short stream cleanup");
	FS::Shutdown();
	std::cout << "production stdio GNF header/full reads, chain/direct modes, cursors, owner cleanup, EOF/errors and read-only assets passed\n";
	return 0;
} catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
