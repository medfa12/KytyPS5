#include "graphics/presentation/videoOutStatus.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "VideoOutOutputSnapshotTests.inc"

namespace {
using namespace Libs::VideoOut;

void Check(bool condition, const char* message) {
	if (!condition) {
		std::fprintf(stderr, "VideoOutStatusTests: %s\n", message);
		std::abort();
	}
}

void TestNull() {
	constexpr int invalid = static_cast<int>(0x80290002u);
	Check(VideoOutPortStatusIsUsable(nullptr) == invalid, "null usability input");
	Check(VideoOutPortStatusIsDeviceConnected(nullptr) == invalid, "null connection input");
	Check(VideoOutOutputStatusIsHdcp14Enabled(nullptr) == invalid, "null HDCP 1.4 input");
	Check(VideoOutOutputStatusIsHdcp23Enabled(nullptr) == invalid, "null HDCP 2.3 input");
}

void TestRawPortStatus() {
	// Deliberately unaligned, with canaries. Exhaust all combinations of the
	// meaningful low byte and ensure unrelated high bits cannot alter results.
	std::array<uint8_t, 58> storage;
	storage.fill(0xa5);
	auto* payload = storage.data() + 9; // Result56's payload is at +8.
	for (uint32_t byte = 0; byte < 256; ++byte) {
		for (uint32_t upper: {0u, 0xffffff00u}) {
			const uint32_t flags = byte | upper;
			std::memcpy(payload, &flags, sizeof(flags));
			const auto before = storage;
			Check(VideoOutPortStatusIsUsable(payload) == ((byte & 1u) == 0),
			      "raw port usability bit, not resolution enumeration");
			Check(VideoOutPortStatusIsDeviceConnected(payload) == ((byte & 0x20u) == 0),
			      "independent raw port connection bit");
			Check(storage == before, "port interpreters must not write inputs or canaries");
		}
	}
	// The connection interpreter's ABI needs just a single readable byte.
	const uint8_t connected = 0, disconnected = 0x20;
	Check(VideoOutPortStatusIsDeviceConnected(&connected) == 1 &&
	          VideoOutPortStatusIsDeviceConnected(&disconnected) == 0,
	      "connection decoder reads one byte");
}

void TestOutputStatus() {
	std::array<uint8_t, 58> storage;
	storage.fill(0xa5);
	auto* payload = storage.data() + 9;
	// Exercise every subset of the three tested HDCP bits, plus ignored bits
	// both below and above the mask. No HDCP bits means neither capability.
	for (uint64_t subset = 0; subset < 8; ++subset) {
		const uint64_t tested = ((subset & 3u) | ((subset & 4u) << 1u)) << 32u;
		for (uint64_t ignored: {uint64_t {0}, ~uint64_t {0xb00000000ull}}) {
			const uint64_t flags = tested | ignored;
			std::memcpy(payload + 0x10, &flags, sizeof(flags));
			const auto before = storage;
			Check(VideoOutOutputStatusIsHdcp14Enabled(payload) == (tested == 0x300000000ull),
			      "HDCP 1.4 exact mask at payload +0x10");
			Check(VideoOutOutputStatusIsHdcp23Enabled(payload) == (tested == 0xb00000000ull),
			      "HDCP 2.3 exact mask at payload +0x10");
			Check(storage == before, "HDCP interpreters must not write inputs or canaries");
		}
	}
}
} // namespace

int main() {
	TestNull();
	TestRawPortStatus();
	TestOutputStatus();
	TestOutputSnapshots();
	TestOutputSnapshotConcurrency();
	std::puts("VideoOutStatusTests: all cases passed");
}
