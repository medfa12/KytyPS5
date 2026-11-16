#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

// A bounded stream of real device transitions. The owner of a modeled device
// may publish only after that device has actually transitioned; an empty
// system publishes nothing. Queue limits are emulator policy, not PS5 claims.
namespace Libs::LibKernel::FileSystem::Devctl {
class Queue {
public:
	static constexpr size_t MaxRecords = 256;
	static constexpr size_t MaxRecordBytes = 1024;
	enum class ReadResult { Data, Empty, Closed };

	bool Publish(std::string_view record) {
		if (record.empty() || record.size() > MaxRecordBytes || record.back() != '\n' ||
		    record.find('\0') != std::string_view::npos) { return false; }
		std::lock_guard lock(m_mutex);
		if (m_records.size() == MaxRecords) { return false; }
		m_records.emplace_back(record);
		m_ready.notify_all();
		return true;
	}
	ReadResult Read(void* out, size_t capacity, size_t* bytes, bool nonblocking, const bool& closed) {
		std::unique_lock lock(m_mutex);
		m_ready.wait(lock, [&] { return closed || nonblocking || !m_records.empty(); });
		if (closed) { return ReadResult::Closed; }
		if (m_records.empty()) { return ReadResult::Empty; }
		const auto count = (std::min)(capacity, m_records.front().size());
		std::memcpy(out, m_records.front().data(), count);
		m_records.pop_front(); // record boundary: a short read discards its tail.
		*bytes = count;
		return ReadResult::Data;
	}
	void Close(bool& closed) {
		std::lock_guard lock(m_mutex);
		closed = true;
		m_ready.notify_all();
	}
private:
	std::mutex m_mutex;
	std::condition_variable m_ready;
	std::deque<std::string> m_records;
};
} // namespace Libs::LibKernel::FileSystem::Devctl
