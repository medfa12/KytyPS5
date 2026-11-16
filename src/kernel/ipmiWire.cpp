#include "kernel/ipmiWire.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>

namespace Libs::LibKernel::IpmiWire {
namespace {
bool Add(uint64_t& total, uint64_t amount) {
	if (amount > UINT64_MAX - total) { return false; }
	total += amount;
	return true;
}
void U32(Bytes& bytes, size_t offset, uint32_t value) {
	for (size_t i = 0; i < 4; ++i) { bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i)); }
}
void U64(Bytes& bytes, size_t offset, uint64_t value) {
	for (size_t i = 0; i < 8; ++i) { bytes[offset + i] = static_cast<uint8_t>(value >> (8 * i)); }
}
Error Count(size_t count, uint32_t budget) {
	if (count > UINT32_MAX) { return Error::Overflow; }
	return count > budget ? Error::LimitExceeded : Error::Ok;
}
Error View(ByteView bytes, uint64_t budget) {
	if (bytes.size > UINT32_MAX || bytes.size > SIZE_MAX) { return Error::Overflow; }
	if (bytes.size > budget) { return Error::LimitExceeded; }
	if (bytes.size != 0 && bytes.data == nullptr) { return Error::Invalid; }
	return Error::Ok;
}
template <typename Write>
Error Publish(uint64_t size, Bytes& out, const Limits& limits, Write write) {
	if (size > SIZE_MAX) { return Error::Overflow; }
	if (size > limits.max_frame_bytes) { return Error::LimitExceeded; }
	try {
		Bytes encoded(static_cast<size_t>(size), 0);
		write(encoded);
		out.swap(encoded);
		return Error::Ok;
	} catch (const std::length_error&) {
		return Error::Overflow;
	} catch (const std::bad_alloc&) {
		return Error::NoMemory;
	}
}
} // namespace

Error EncodeDataInfo(const DataInfo& data, Bytes& out, const Limits& limits) {
	if (data.size > limits.max_input_bytes) { return Error::LimitExceeded; }
	return Publish(0x10, out, limits, [&](Bytes& b) { U64(b, 0, data.pointer); U64(b, 8, data.size); });
}
Error EncodeBufferInfo(const BufferInfo& data, Bytes& out, const Limits& limits) {
	if (data.written > data.capacity) { return Error::Invalid; }
	if (data.capacity > limits.max_output_bytes) { return Error::LimitExceeded; }
	return Publish(0x18, out, limits, [&](Bytes& b) {
		U64(b, 0, data.pointer); U64(b, 8, data.capacity); U64(b, 0x10, data.written);
	});
}
Error EncodeInvoke320(const Invoke& invoke, Bytes& out, const Limits& limits) {
	if (invoke.input_count > limits.max_inputs || invoke.output_count > limits.max_outputs) {
		return Error::LimitExceeded;
	}
	return Publish(0x30, out, limits, [&](Bytes& b) {
		U32(b, 0, invoke.method); U32(b, 4, invoke.input_count); U32(b, 8, invoke.output_count);
		U64(b, 0x10, invoke.inputs); U64(b, 0x18, invoke.outputs); U64(b, 0x20, invoke.method_result);
		U32(b, 0x28, 1);
	});
}
Error EncodeReply232(const Reply& reply, Bytes& out, const Limits& limits) {
	if (reply.output_count > limits.max_outputs) { return Error::LimitExceeded; }
	return Publish(0x18, out, limits, [&](Bytes& b) {
		U32(b, 0, static_cast<uint32_t>(reply.method_result)); U32(b, 4, reply.output_count);
		U64(b, 8, reply.outputs); U32(b, 0x10, 1);
	});
}
Error EncodeReceiveInfo(const ReceiveInfo& receive, Bytes& out, const Limits& limits) {
	if (receive.payload_size > limits.max_frame_bytes) { return Error::LimitExceeded; }
	return Publish(0x18, out, limits, [&](Bytes& b) {
		U64(b, 0, receive.payload_size); U32(b, 8, receive.type);
		U32(b, 0xc, static_cast<uint32_t>(receive.kid)); U64(b, 0x10, receive.handler);
	});
}

Error EncodeSyncPayload(const SyncRequest& request, SyncType type, Bytes& out, const Limits& limits) {
	if (type != SyncType::Request && type != SyncType::Inline) { return Error::Invalid; }
	if (const auto error = Count(request.inputs.size(), limits.max_inputs); error != Error::Ok) { return error; }
	if (const auto error = Count(request.output_capacities.size(), limits.max_outputs); error != Error::Ok) { return error; }
	uint64_t size = 0x18, input_total = 0, output_total = 0;
	for (const auto& input: request.inputs) {
		if (const auto error = View(input, limits.max_input_bytes); error != Error::Ok) { return error; }
		if (!Add(input_total, input.size) || !Add(size, 4) || !Add(size, input.size)) { return Error::Overflow; }
		if (input_total > limits.max_input_bytes) { return Error::LimitExceeded; }
	}
	for (const auto capacity: request.output_capacities) {
		if (capacity > UINT32_MAX) { return Error::Overflow; }
		if (!Add(output_total, capacity) || !Add(size, 4)) { return Error::Overflow; }
		if (output_total > limits.max_output_bytes) { return Error::LimitExceeded; }
	}
	if (type == SyncType::Inline && !Add(size, output_total)) { return Error::Overflow; }
	return Publish(size, out, limits, [&](Bytes& b) {
		U64(b, 0, request.native_session); U32(b, 8, static_cast<uint32_t>(request.caller_pid));
		U32(b, 0xc, request.method); U32(b, 0x10, static_cast<uint32_t>(request.inputs.size()));
		U32(b, 0x14, static_cast<uint32_t>(request.output_capacities.size()));
		size_t offset = 0x18;
		for (const auto& input: request.inputs) {
			U32(b, offset, static_cast<uint32_t>(input.size)); offset += 4;
			if (input.size != 0) { std::memcpy(b.data() + offset, input.data, static_cast<size_t>(input.size)); }
			offset += static_cast<size_t>(input.size);
		}
		for (const auto capacity: request.output_capacities) { U32(b, offset, static_cast<uint32_t>(capacity)); offset += 4; }
		// Inline output workspace is reserved, initialized HOST storage, not a
		// reply/status. Only the future native producer may supply those results.
	});
}

Error EncodeConnectPayload(const Connect& connect, Bytes& out, const Limits& limits) {
	if (connect.queue_sizes.size() > 32 || connect.sync_count > limits.max_sync_count ||
	    connect.async_count > limits.max_async_count || connect.event_count > limits.max_events) {
		return Error::LimitExceeded;
	}
	for (const auto size: {connect.sync_input_limit, connect.sync_output_limit,
	                     connect.async_input_limit, connect.async_output_limit}) {
		if (size > limits.max_channel_bytes) { return Error::LimitExceeded; }
	}
	uint64_t queue_total = 0;
	for (const auto size: connect.queue_sizes) {
		if (!Add(queue_total, size)) { return Error::Overflow; }
		if (size > limits.max_channel_bytes || queue_total > limits.max_queue_total_bytes) { return Error::LimitExceeded; }
	}
	if (const auto error = View(connect.argument, limits.max_argument_bytes); error != Error::Ok) { return error; }
	uint64_t size = 0x154;
	if (!Add(size, connect.argument.size)) { return Error::Overflow; }
	return Publish(size, out, limits, [&](Bytes& b) {
		U32(b, 0, static_cast<uint32_t>(connect.client_pid)); U32(b, 8, connect.sync_count);
		U64(b, 0x10, connect.sync_input_limit); U64(b, 0x18, connect.sync_output_limit);
		U32(b, 0x20, connect.async_count); U64(b, 0x28, connect.async_input_limit);
		U64(b, 0x30, connect.async_output_limit); U32(b, 0x38, connect.event_count);
		U32(b, 0x40, static_cast<uint32_t>(connect.queue_sizes.size()));
		for (size_t i = 0; i < connect.queue_sizes.size(); ++i) { U64(b, 0x48 + i * 8, connect.queue_sizes[i]); }
		U64(b, 0x148, 0); // verified memorySize field
		U32(b, 0x150, static_cast<uint32_t>(connect.argument.size));
		if (connect.argument.size != 0) {
			std::memcpy(b.data() + 0x154, connect.argument.data, static_cast<size_t>(connect.argument.size));
		}
	});
}

} // namespace Libs::LibKernel::IpmiWire
