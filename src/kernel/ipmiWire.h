#pragma once

// Host-only encoders for the verified native IPMI layouts. This is NOT a
// production gateway: no registration, syscall, Core, guest-memory access,
// native callbacks, identity translation or recovery-result synthesis.
#include <cstdint>
#include <vector>

namespace Libs::LibKernel::IpmiWire {

using Bytes = std::vector<uint8_t>;
// Addresses in wire descriptors are opaque numeric values, never dereferenced.
struct DataInfo { uint64_t pointer = 0, size = 0; };
struct BufferInfo { uint64_t pointer = 0, capacity = 0, written = 0; };
struct Invoke {
	uint32_t method = 0, input_count = 0, output_count = 0;
	uint64_t inputs = 0, outputs = 0, method_result = 0;
};
struct Reply {
	int32_t method_result = 0;
	uint32_t output_count = 0;
	uint64_t outputs = 0;
};
struct ReceiveInfo {
	uint64_t payload_size = 0;
	uint32_t type = 0;
	int32_t kid = 0;
	uint64_t handler = 0;
};

// Actual readable HOST bytes supplied by the adapter, not a guest address.
// The storage must remain valid and unmodified for the duration of encoding.
struct ByteView { const uint8_t* data = nullptr; uint64_t size = 0; };
enum class SyncType : uint32_t { Request = 0x41, Inline = 0x8041 };
struct SyncRequest {
	uint64_t native_session = 0;
	int32_t caller_pid = 0;
	uint32_t method = 0;
	std::vector<ByteView> inputs;
	std::vector<uint64_t> output_capacities;
};
struct Connect {
	int32_t client_pid = 0;
	uint32_t sync_count = 0;
	uint64_t sync_input_limit = 0, sync_output_limit = 0;
	uint32_t async_count = 0;
	uint64_t async_input_limit = 0, async_output_limit = 0;
	uint32_t event_count = 0;
	std::vector<uint64_t> queue_sizes;
	ByteView argument;
};

// Explicit HOST resource budgets, not asserted firmware validation rules.
// Relaxing these does not relax the u32 length/capacity and 32-queue wire bounds.
struct Limits {
	uint32_t max_inputs = 10, max_outputs = 10;
	uint32_t max_sync_count = 16, max_async_count = 16, max_events = 16;
	uint64_t max_input_bytes = 1024 * 1024, max_output_bytes = 1024 * 1024;
	uint64_t max_channel_bytes = 1024 * 1024, max_queue_total_bytes = 16 * 1024 * 1024;
	// 13.60 server registration accepts argument buffers through 0x2000;
	// ShellCore's native SceNpMgrIpc config uses that size (cca620..cca726).
	uint64_t max_argument_bytes = 8192, max_frame_bytes = 2 * 1024 * 1024;
};
// Host codec results only: never convert these directly to guest errno/results.
enum class Error { Ok, Invalid, Overflow, LimitExceeded, NoMemory };

// On EVERY failure, out remains byte-for-byte unchanged. All validation and
// checked arithmetic precede allocation/copy; output is published by swap.
Error EncodeDataInfo(const DataInfo& data, Bytes& out, const Limits& limits = {});
Error EncodeBufferInfo(const BufferInfo& data, Bytes& out, const Limits& limits = {});
Error EncodeInvoke320(const Invoke& invoke, Bytes& out, const Limits& limits = {});
Error EncodeReply232(const Reply& reply, Bytes& out, const Limits& limits = {});
Error EncodeReceiveInfo(const ReceiveInfo& receive, Bytes& out, const Limits& limits = {});
Error EncodeSyncPayload(const SyncRequest& request, SyncType type, Bytes& out, const Limits& limits = {});
Error EncodeConnectPayload(const Connect& connect, Bytes& out, const Limits& limits = {});

} // namespace Libs::LibKernel::IpmiWire
