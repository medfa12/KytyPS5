#include "loader/runtimeChildProtocol.h"

#include <charconv>
#include <algorithm>
#include <utility>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Loader::RuntimeChild {
std::string EncodeArguments(const ChildArguments& a) {
	return std::to_string(a.receive) + ":" + std::to_string(a.send) + ":" + std::to_string(a.key.app) + ":" +
	    std::to_string(a.key.sequence) + ":" + std::to_string(a.key.nonce_lo) + ":" + std::to_string(a.key.nonce_hi) + ":" + std::to_string(a.input) +
	    (a.lease ? ":" + std::to_string(a.lease) + ":" + std::to_string(a.lease_volume) + ":" + std::to_string(a.lease_file) : "");
}
bool DecodeArguments(const std::string& text, ChildArguments* out) {
	if (!out || text.size() > 240) { return false; }
	const unsigned count = std::count(text.begin(), text.end(), ':') == 9 ? 10 : 7;
	uint64_t words[10] {}; size_t at = 0;
	for (unsigned i = 0; i != count; ++i) {
		const auto end = i == count - 1 ? text.size() : text.find(':', at);
		if (end == std::string::npos || end == at) { return false; }
		const auto parsed = std::from_chars(text.data() + at, text.data() + end, words[i]);
		if (parsed.ec != std::errc() || parsed.ptr != text.data() + end) { return false; }
		at = end + 1;
	}
	ChildArguments result {{static_cast<uint32_t>(words[2]), 0, words[3], words[4], words[5]}, words[0], words[1], words[6]};
	result.lease = words[7]; result.lease_volume = words[8]; result.lease_file = words[9];
	if (count == 10 && (!result.lease || result.lease == result.receive || result.lease == result.send || result.lease == result.input)) { return false; }
	if (words[2] > INT32_MAX || !result.key.Valid() || !result.receive || !result.send || result.receive == result.send) { return false; }
	*out = result; return true;
}
bool ValidMessage(const Message& m, const Key& expected) {
	return m.magic == 0x4b435348 && m.version == 1 && m.bytes == sizeof(Message) && m.key == expected &&
	    m.serial && m.kind >= Kind::Hello && m.kind <= Kind::LeaseAck;
}
Channel::~Channel() { Reset(); }
Channel::Channel(Channel&& other) noexcept: receive_(std::exchange(other.receive_, 0)), send_(std::exchange(other.send_, 0)) {}
Channel& Channel::operator=(Channel&& other) noexcept {
	if (this != &other) { Reset(); receive_ = std::exchange(other.receive_, 0); send_ = std::exchange(other.send_, 0); }
	return *this;
}
void Channel::Reset() noexcept {
#if defined(_WIN32)
	if (receive_) { CloseHandle(reinterpret_cast<HANDLE>(receive_)); }
	if (send_) { CloseHandle(reinterpret_cast<HANDLE>(send_)); }
#endif
	receive_ = send_ = 0;
}
bool Channel::Adopt(uint64_t receive, uint64_t send) {
	if (receive_ || send_ || !receive || !send || receive == send) { return false; }
#if defined(_WIN32)
	if (GetFileType(reinterpret_cast<HANDLE>(receive)) != FILE_TYPE_PIPE || GetFileType(reinterpret_cast<HANDLE>(send)) != FILE_TYPE_PIPE) { return false; }
	receive_ = receive; send_ = send; return true;
#else
	return false;
#endif
}
ReadResult Channel::Read(Message* out) {
#if defined(_WIN32)
	if (!receive_ || !out) { return ReadResult::Invalid; }
	DWORD available = 0;
	if (!PeekNamedPipe(reinterpret_cast<HANDLE>(receive_), nullptr, 0, nullptr, &available, nullptr)) { return ReadResult::Closed; }
	if (available < sizeof(Message)) { return ReadResult::Pending; }
	DWORD read = 0;
	return ReadFile(reinterpret_cast<HANDLE>(receive_), out, sizeof(*out), &read, nullptr) && read == sizeof(*out)
	    ? ReadResult::Received : ReadResult::Closed;
#else
	return ReadResult::Invalid;
#endif
}
bool Channel::Write(const Message& message) {
#if defined(_WIN32)
	DWORD written = 0;
	return send_ && WriteFile(reinterpret_cast<HANDLE>(send_), &message, sizeof(message), &written, nullptr) && written == sizeof(message);
#else
	return false;
#endif
}
}
