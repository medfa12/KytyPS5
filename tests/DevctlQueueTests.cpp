#include "kernel/devctlQueue.h"
#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>

using Libs::LibKernel::FileSystem::Devctl::Queue;

int main() {
	Queue q;
	bool closed = false;
	char out[32] {};
	size_t n = 99;
	assert(q.Read(out, sizeof(out), &n, true, closed) == Queue::ReadResult::Empty && n == 99);
	assert(!q.Publish("") && !q.Publish("no newline") && !q.Publish(std::string(1025, 'x')));
	assert(q.Publish("first\n") && q.Publish("second\n"));
	assert(q.Read(out, 3, &n, true, closed) == Queue::ReadResult::Data && n == 3 && std::string(out, n) == "fir");
	assert(q.Read(out, sizeof(out), &n, true, closed) == Queue::ReadResult::Data && std::string(out, n) == "second\n");
	for (size_t i = 0; i < Queue::MaxRecords; ++i) { assert(q.Publish("x\n")); }
	assert(!q.Publish("overflow\n"));
	for (size_t i = 0; i < Queue::MaxRecords; ++i) { assert(q.Read(out, sizeof(out), &n, true, closed) == Queue::ReadResult::Data); }
	auto waiter = std::async(std::launch::async, [&] { char b[8]; size_t size = 0; return q.Read(b, sizeof(b), &size, false, closed); });
	q.Close(closed);
	assert(waiter.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
	assert(waiter.get() == Queue::ReadResult::Closed);
	// Two descriptor-like handles can retain a queue independently; closing
	// one must not invalidate the other or consume its next record.
	auto shared = std::make_shared<Queue>();
	auto duplicate = shared;
	bool first_closed = false, second_closed = false;
	shared->Close(first_closed);
	shared.reset();
	assert(duplicate->Publish("transition\n"));
	assert(duplicate->Read(out, sizeof(out), &n, true, second_closed) == Queue::ReadResult::Data);
	assert(std::string(out, n) == "transition\n");
	duplicate->Close(second_closed);
}
