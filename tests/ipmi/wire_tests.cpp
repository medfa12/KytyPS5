#include "kernel/ipmiWire.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Libs::LibKernel::IpmiWire;
#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; std::abort(); } } while (false)

static uint32_t Read32(const Bytes& b, size_t offset) {
	CHECK(offset + 4 <= b.size());
	return uint32_t {b[offset]} | (uint32_t {b[offset + 1]} << 8) |
	       (uint32_t {b[offset + 2]} << 16) | (uint32_t {b[offset + 3]} << 24);
}
static uint64_t Read64(const Bytes& b, size_t offset) {
	return Read32(b, offset) | (uint64_t {Read32(b, offset + 4)} << 32);
}

static void ExactGetStatusFrame() {
	SyncRequest request;
	request.native_session = 0x1122334455667788;
	request.caller_pid = 0x12345678;
	request.method = 0x160008;
	request.output_capacities = {24};
	Bytes encoded {0xee};
	CHECK(EncodeSyncPayload(request, SyncType::Inline, encoded) == Error::Ok);
	const Bytes expected {
		0x88,0x77,0x66,0x55,0x44,0x33,0x22,0x11, // native session
		0x78,0x56,0x34,0x12, 0x08,0x00,0x16,0x00, // supplied PID/method
		0,0,0,0, 1,0,0,0, 24,0,0,0,             // counts, capacity
		0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
	};
	CHECK(encoded.size() == 52 && encoded == expected);
	CHECK(EncodeSyncPayload(request, SyncType::Request, encoded) == Error::Ok);
	CHECK(encoded.size() == 28 && std::equal(encoded.begin(), encoded.end(), expected.begin()));
	Bytes receive;
	CHECK(EncodeReceiveInfo({52, 0x8041, -7, 0xfedcba9876543210}, receive) == Error::Ok);
	CHECK(receive.size() == 24 && Read64(receive, 0) == 52 && Read32(receive, 8) == 0x8041);
	CHECK(Read32(receive, 12) == 0xfffffff9 && Read64(receive, 16) == 0xfedcba9876543210);
}

static void PackedInputsHaveNoPadding() {
	const std::array<uint8_t, 3> first {0xaa, 0xbb, 0xcc};
	const std::array<uint8_t, 2> second {0xdd, 0xee};
	SyncRequest request;
	request.native_session = 0x1234; request.caller_pid = -3; request.method = 0x10203040;
	request.inputs = {{first.data(), first.size()}, {second.data(), second.size()}};
	request.output_capacities = {5, 7};
	Bytes encoded;
	CHECK(EncodeSyncPayload(request, SyncType::Inline, encoded) == Error::Ok);
	CHECK(encoded.size() == 57);
	const Bytes tail {3,0,0,0,0xaa,0xbb,0xcc, 2,0,0,0,0xdd,0xee, 5,0,0,0, 7,0,0,0,
	                  0,0,0,0,0,0,0,0,0,0,0,0};
	CHECK(std::equal(tail.begin(), tail.end(), encoded.begin() + 24));
	CHECK(Read32(encoded, 31) == 2 && Read32(encoded, 37) == 5 && Read32(encoded, 41) == 7);
	CHECK(Read32(encoded, 8) == 0xfffffffd && Read32(encoded, 16) == 2 && Read32(encoded, 20) == 2);
	CHECK((first == std::array<uint8_t, 3> {0xaa,0xbb,0xcc}));
	CHECK(EncodeSyncPayload(request, SyncType::Request, encoded) == Error::Ok && encoded.size() == 45);
}

static void ExactDescriptors() {
	Bytes encoded;
	CHECK(EncodeDataInfo({0xfedcba9876543210, 0x1234}, encoded) == Error::Ok);
	CHECK(encoded.size() == 16 && Read64(encoded, 0) == 0xfedcba9876543210 && Read64(encoded, 8) == 0x1234);
	CHECK(EncodeBufferInfo({0x8877665544332211, 24, 17}, encoded) == Error::Ok);
	CHECK(encoded.size() == 24 && Read64(encoded, 0) == 0x8877665544332211);
	CHECK(Read64(encoded, 8) == 24 && Read64(encoded, 16) == 17);
	CHECK(EncodeInvoke320({0x160008, 2, 1, 0x1122334455667788, 0x8877665544332211, 0x123456789abcdef0}, encoded) == Error::Ok);
	CHECK(encoded.size() == 0x30 && Read32(encoded, 0) == 0x160008);
	CHECK(Read32(encoded, 4) == 2 && Read32(encoded, 8) == 1 && Read32(encoded, 12) == 0);
	CHECK(Read64(encoded, 0x10) == 0x1122334455667788 && Read64(encoded, 0x18) == 0x8877665544332211);
	CHECK(Read64(encoded, 0x20) == 0x123456789abcdef0 && Read32(encoded, 0x28) == 1 && Read32(encoded, 0x2c) == 0);
	CHECK(EncodeReply232({-12345, 1, 0xfedcba9876543210}, encoded) == Error::Ok);
	CHECK(encoded.size() == 0x18 && Read32(encoded, 0) == static_cast<uint32_t>(-12345));
	CHECK(Read32(encoded, 4) == 1 && Read64(encoded, 8) == 0xfedcba9876543210);
	CHECK(Read32(encoded, 0x10) == 1 && Read32(encoded, 0x14) == 0);
}

static void ConnectOffsetsAndArguments() {
	const std::array<uint8_t, 3> argument {0x41, 0, 0xff};
	Connect connect;
	connect.client_pid = -9; connect.sync_count = 2;
	connect.sync_input_limit = 0x101; connect.sync_output_limit = 0x202;
	connect.async_count = 3; connect.async_input_limit = 0x303; connect.async_output_limit = 0x404;
	connect.event_count = 4; connect.queue_sizes = {0x505, 0x606};
	connect.argument = {argument.data(), argument.size()};
	Bytes encoded;
	CHECK(EncodeConnectPayload(connect, encoded) == Error::Ok && encoded.size() == 0x157);
	CHECK(Read32(encoded, 0) == 0xfffffff7 && Read32(encoded, 8) == 2);
	CHECK(Read64(encoded, 0x10) == 0x101 && Read64(encoded, 0x18) == 0x202);
	CHECK(Read32(encoded, 0x20) == 3 && Read64(encoded, 0x28) == 0x303 && Read64(encoded, 0x30) == 0x404);
	CHECK(Read32(encoded, 0x38) == 4 && Read32(encoded, 0x40) == 2);
	CHECK(Read64(encoded, 0x48) == 0x505 && Read64(encoded, 0x50) == 0x606);
	CHECK(std::all_of(encoded.begin() + 0x58, encoded.begin() + 0x150, [](auto byte) { return byte == 0; }));
	CHECK(Read64(encoded, 0x148) == 0 && Read32(encoded, 0x150) == 3);
	CHECK(std::equal(argument.begin(), argument.end(), encoded.begin() + 0x154));
	for (size_t padding: {4, 12, 0x24, 0x3c, 0x44}) { CHECK(Read32(encoded, padding) == 0); }
	connect.queue_sizes.assign(32, 1); connect.queue_sizes[31] = 0x7788;
	connect.argument = {};
	CHECK(EncodeConnectPayload(connect, encoded) == Error::Ok && encoded.size() == 0x154);
	CHECK(Read64(encoded, 0x140) == 0x7788 && Read64(encoded, 0x148) == 0);
}

static void RejectionNeverPublishesPartialOutput() {
	const Bytes sentinel {0xde, 0xad, 0xbe, 0xef};
	Bytes out = sentinel;
	auto rejected = [&](Error actual, Error expected) { CHECK(actual == expected); CHECK(out == sentinel); };
	Limits small;
	SyncRequest request; request.method = 0x160008; request.output_capacities = {24};
	small.max_frame_bytes = 51;
	rejected(EncodeSyncPayload(request, SyncType::Inline, out, small), Error::LimitExceeded);
	small = {}; small.max_output_bytes = 23;
	rejected(EncodeSyncPayload(request, SyncType::Request, out, small), Error::LimitExceeded);
	request.output_capacities = {UINT64_C(0x100000000)};
	rejected(EncodeSyncPayload(request, SyncType::Request, out), Error::Overflow);
	request.output_capacities.assign(11, 1);
	rejected(EncodeSyncPayload(request, SyncType::Request, out), Error::LimitExceeded);
	request.output_capacities.clear(); request.inputs.assign(11, {});
	rejected(EncodeSyncPayload(request, SyncType::Request, out), Error::LimitExceeded);
	request.inputs = {{nullptr, 1}};
	rejected(EncodeSyncPayload(request, SyncType::Inline, out), Error::Invalid);
	// Poison host pointer proves overflowing lengths are rejected before copying.
	request.inputs = {{reinterpret_cast<const uint8_t*>(1), UINT64_MAX}};
	rejected(EncodeSyncPayload(request, SyncType::Inline, out), Error::Overflow);
	const uint8_t data[4] {1,2,3,4}; request.inputs = {{data,4},{data,4}};
	small = {}; small.max_input_bytes = 7;
	rejected(EncodeSyncPayload(request, SyncType::Request, out, small), Error::LimitExceeded);
	request.inputs.clear(); request.output_capacities = {4,4}; small = {}; small.max_output_bytes = 7;
	rejected(EncodeSyncPayload(request, SyncType::Request, out, small), Error::LimitExceeded);
	rejected(EncodeSyncPayload(request, static_cast<SyncType>(0), out), Error::Invalid);
	rejected(EncodeBufferInfo({1, 24, 25}, out), Error::Invalid);
	rejected(EncodeDataInfo({1, UINT64_MAX}, out), Error::LimitExceeded);
	rejected(EncodeBufferInfo({1, UINT64_MAX, 0}, out), Error::LimitExceeded);
	rejected(EncodeInvoke320({1, 11, 0, 0, 0, 0}, out), Error::LimitExceeded);
	rejected(EncodeReply232({-1, 11, 0}, out), Error::LimitExceeded);
	rejected(EncodeReceiveInfo({UINT64_MAX, 1, 1, 1}, out), Error::LimitExceeded);
	small = {}; small.max_frame_bytes = 15;
	rejected(EncodeDataInfo({}, out, small), Error::LimitExceeded);
	rejected(EncodeBufferInfo({}, out, small), Error::LimitExceeded);
	rejected(EncodeInvoke320({}, out, small), Error::LimitExceeded);
	rejected(EncodeReply232({}, out, small), Error::LimitExceeded);
	rejected(EncodeReceiveInfo({}, out, small), Error::LimitExceeded);
	Connect connect; connect.queue_sizes.assign(33, 1);
	rejected(EncodeConnectPayload(connect, out), Error::LimitExceeded);
	connect = {}; connect.argument = {nullptr, 1};
	rejected(EncodeConnectPayload(connect, out), Error::Invalid);
	connect.argument = {reinterpret_cast<const uint8_t*>(1), UINT64_MAX};
	rejected(EncodeConnectPayload(connect, out), Error::Overflow);
	connect = {}; small = {}; small.max_frame_bytes = 0x153;
	rejected(EncodeConnectPayload(connect, out, small), Error::LimitExceeded);
	connect.sync_count = 17; rejected(EncodeConnectPayload(connect, out), Error::LimitExceeded);
	connect = {}; connect.async_count = 17; rejected(EncodeConnectPayload(connect, out), Error::LimitExceeded);
	connect = {}; connect.event_count = 17; rejected(EncodeConnectPayload(connect, out), Error::LimitExceeded);
	connect = {}; connect.sync_input_limit = UINT64_MAX;
	rejected(EncodeConnectPayload(connect, out), Error::LimitExceeded);
	connect = {}; connect.argument = {data,4}; small = {}; small.max_argument_bytes = 3;
	rejected(EncodeConnectPayload(connect, out, small), Error::LimitExceeded);
	connect = {}; connect.queue_sizes = {4,4}; small = {}; small.max_queue_total_bytes = 7;
	rejected(EncodeConnectPayload(connect, out, small), Error::LimitExceeded);
	// Real checked-add overflow, with caller-expanded HOST policy budgets.
	connect.queue_sizes = {UINT64_MAX, 1}; small.max_channel_bytes = UINT64_MAX; small.max_queue_total_bytes = UINT64_MAX;
	rejected(EncodeConnectPayload(connect, out, small), Error::Overflow);
}

int main() {
	ExactGetStatusFrame(); PackedInputsHaveNoPadding(); ExactDescriptors();
	ConnectOffsetsAndArguments(); RejectionNeverPublishesPartialOutput();
	std::cout << "IPMI wire codec tests passed\n";
}
