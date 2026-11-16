#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace Loader::RuntimeChild {
// HOST-only fixed wire ABI. No guest pointers, STL records or callback addresses.
struct Key {
	uint32_t app = 0, reserved = 0;
	uint64_t sequence = 0, nonce_lo = 0, nonce_hi = 0;
	bool operator==(const Key&) const = default;
	bool Valid() const { return app && app <= INT32_MAX && !reserved && sequence && (nonce_lo || nonce_hi); }
};
enum class Kind : uint32_t { Hello = 1, Entry, Window, Present, Failure, Focus, FocusAck, Close, LeaseAck };
enum Capability : uint64_t { EmbeddedWindow = 1, BrokeredInput = 2, FocusAudio = 4, ComposedFrames = 8 };
struct Message {
	uint32_t magic = 0x4b435348, version = 1, bytes = 104;
	Kind kind = Kind::Hello;
	Key key;
	uint64_t serial = 0;
	std::array<int64_t, 6> value {};
};
static_assert(sizeof(Message) == 104);
struct ChildArguments {
	Key key;
	uint64_t receive = 0, send = 0, input = 0;
	uint64_t lease = 0, lease_volume = 0, lease_file = 0;
};
std::string EncodeArguments(const ChildArguments& args);
bool DecodeArguments(const std::string& text, ChildArguments* out);
bool ValidMessage(const Message& message, const Key& expected);

// One writer at a time, one reader; ownership of both pipe handles is explicit.
// Fixed writes are <=104 bytes; the protocol allows only one unacked focus and
// one close command, keeping parent writes within the fresh 4096-byte pipe.
enum class ReadResult { Pending, Received, Closed, Invalid };
class Channel {
public:
	Channel() = default;
	~Channel();
	Channel(const Channel&) = delete;
	Channel& operator=(const Channel&) = delete;
	Channel(Channel&& other) noexcept;
	Channel& operator=(Channel&& other) noexcept;
	bool Adopt(uint64_t receive, uint64_t send);
	ReadResult Read(Message* out);
	bool Write(const Message& message);
	void Reset() noexcept;
private:
	uint64_t receive_ = 0, send_ = 0;
};
}
