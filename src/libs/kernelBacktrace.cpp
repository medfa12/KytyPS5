#include "libs/kernelBacktrace.h"

#ifdef _WIN32
#include "common/logging/log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Libs::KernelBacktrace {
namespace {

uint64_t ReadLittleEndian(const uint8_t* bytes) noexcept {
	uint64_t value = 0;
	for (size_t i = 0; i < 8; ++i) {
		value |= static_cast<uint64_t>(bytes[i]) << (i * 8);
	}
	return value;
}

} // namespace

Capture Collect(uint64_t first_frame, const Reader& reader, size_t max_frames) noexcept {
	Capture result;
	if (max_frames > MaxFrames) { max_frames = MaxFrames; }
	std::array<uint64_t, MaxFrames> visited {};
	uint64_t frame = first_frame;
	while (frame != 0) {
		result.stop_frame = frame;
		if (reader.read == nullptr) {
			result.stop = Stop::InvalidReader;
			return result;
		}
		for (size_t i = 0; i < result.count; ++i) {
			if (visited[i] == frame) {
				result.stop = Stop::Cycle;
				return result;
			}
		}
		if (result.count == max_frames) {
			result.stop = Stop::Limit;
			return result;
		}
		if (frame > UINT64_MAX - (FrameBytes - 1)) {
			result.stop = Stop::AddressOverflow;
			return result;
		}
		std::array<uint8_t, FrameBytes> bytes {};
		size_t copied = 0;
		result.requested_bytes += bytes.size();
		if (!reader.read(reader.context, frame, bytes.data(), bytes.size(), copied) || copied != bytes.size()) {
			result.stop = Stop::ReadFailed;
			return result;
		}
		visited[result.count] = frame;
		result.pcs[result.count++] = ReadLittleEndian(bytes.data() + 8);
		frame = ReadLittleEndian(bytes.data());
	}
	result.stop_frame = 0;
	return result;
}

Prefix ReadPrefix(uint64_t address, const Reader& reader) noexcept {
	Prefix result;
	if (address == 0) {
		result.stop = PrefixStop::NullAddress;
		return result;
	}
	if (reader.read == nullptr) {
		result.stop = PrefixStop::InvalidReader;
		return result;
	}
	for (size_t i = 0; i < MaxPrefixBytes; ++i) {
		if (address > UINT64_MAX - i) {
			result.stop = PrefixStop::AddressOverflow;
			return result;
		}
		char byte = 0;
		size_t copied = 0;
		if (!reader.read(reader.context, address + i, &byte, 1, copied) || copied != 1) {
			result.stop = PrefixStop::ReadFailed;
			return result;
		}
		if (byte == '\0') { return result; }
		result.text[result.length++] = byte;
	}
	result.stop = PrefixStop::Limit;
	return result;
}

} // namespace Libs::KernelBacktrace

#ifdef _WIN32
namespace Libs::LibKernel {
namespace {
namespace Backtrace = Libs::KernelBacktrace;
static_assert(sizeof(uintptr_t) == sizeof(uint64_t));

struct PreserveLastError {
	DWORD value = GetLastError();
	~PreserveLastError() { SetLastError(value); }
};

bool ReadLocalMemory(void*, uint64_t address, void* destination, size_t size, size_t& copied) noexcept {
	const PreserveLastError preserve;
	SIZE_T bytes = 0;
	const BOOL success = ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), destination, size, &bytes);
	copied = bytes;
	return success != FALSE && bytes == size;
}

const char* StopText(Backtrace::Stop stop) noexcept {
	switch (stop) {
		case Backtrace::Stop::Complete: return "null frame link";
		case Backtrace::Stop::ReadFailed: return "read error (failed or short frame read)";
		case Backtrace::Stop::Cycle: return "cycle detected";
		case Backtrace::Stop::AddressOverflow: return "frame address overflow";
		case Backtrace::Stop::Limit: return "512-frame / 8192-byte read limit";
		case Backtrace::Stop::InvalidReader: return "reader unavailable";
	}
	return "unknown stop";
}

const char* PrefixStopText(Backtrace::PrefixStop stop) noexcept {
	switch (stop) {
		case Backtrace::PrefixStop::Complete: return "complete";
		case Backtrace::PrefixStop::NullAddress: return "unavailable (null address)";
		case Backtrace::PrefixStop::ReadFailed: return "truncated/unavailable: read error";
		case Backtrace::PrefixStop::AddressOverflow: return "truncated: address overflow";
		case Backtrace::PrefixStop::Limit: return "truncated: no terminator in 128 bytes";
		case Backtrace::PrefixStop::InvalidReader: return "unavailable: reader missing";
	}
	return "unknown stop";
}

} // namespace

extern "C" void __attribute__((noinline, force_align_arg_pointer)) KYTY_SYSV_ABI
KytyKernelBacktraceFromGuest(const char* prefix, uint64_t guest_frame, uint64_t caller_pc) {
	const PreserveLastError preserve; // Logging must not leak changes either.
	if (Log::IsSilent()) { return; }
	const Backtrace::Reader reader {nullptr, ReadLocalMemory};
	// The actual entry caller consumes one of the total 512 presentation slots.
	const auto capture = Backtrace::Collect(guest_frame, reader, Backtrace::MaxFrames - 1);
	const auto label = Backtrace::ReadPrefix(reinterpret_cast<uintptr_t>(prefix), reader);
	LOGF("[%s] backtrace:\n", label.text.data());
	if (label.stop != Backtrace::PrefixStop::Complete) {
		LOGF("  prefix %s\n", PrefixStopText(label.stop));
	}
	const size_t total = capture.count + 1;
	for (size_t i = 0; i < total; ++i) {
		if (total > 2 * Backtrace::PresentationEdge && i == Backtrace::PresentationEdge) {
			LOGF("  ... %zu captured frames omitted ...\n", total - 2 * Backtrace::PresentationEdge);
			i = total - Backtrace::PresentationEdge;
		}
		if (i == 0) {
			LOGF("  caller 0x%016llx\n", static_cast<unsigned long long>(caller_pc));
		} else {
			LOGF("  #%zu 0x%016llx\n", i, static_cast<unsigned long long>(capture.pcs[i - 1]));
		}
	}
	if (capture.stop != Backtrace::Stop::Complete) {
		LOGF("  backtrace truncated after %zu frames: %s at 0x%016llx\n", total, StopText(capture.stop),
		     static_cast<unsigned long long>(capture.stop_frame));
	} else if (capture.count == 0) {
		LOGF("  no further frames (null frame link)\n");
	}
}

// Windows unwind prologues do not promise the SysV RBP-chain layout. Capture
// these two actual guest entry values before ANY compiler-generated prologue.
// Only SysV caller-saved RSI/RDX change; the tail call preserves the original
// return address and lets the ordinary helper realign the stack for host calls.
void __attribute__((naked)) KYTY_SYSV_ABI KernelBacktrace(const char*) {
	__asm__("movq %rbp, %rsi\n\t"
	        "movq (%rsp), %rdx\n\t"
	        "jmp KytyKernelBacktraceFromGuest\n\t");
}

} // namespace Libs::LibKernel
#endif
