#include "kernel/ipmiGateway.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

using namespace Libs::LibKernel;
using namespace Ipmi;
using namespace IpmiGateway;
#define CHECK(expr) do { if (!(expr)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #expr "\n"; std::abort(); } } while (false)

struct Fixture {
	static constexpr uint64_t Descriptor = 0x100, Output = 0x200;
	Core core;
	Identity id {200, 7}, server {100, 1}, worker {100, 2};
	DiagnosticCaller caller {{200, 7}, {}, {}};
	Bytes memory = Bytes(0x1000, 0xcc);
	std::vector<std::pair<uint64_t, size_t>> reads, writes;
	std::vector<char> order;
	std::vector<DiagnosticEvent> events, unknown;
	uint64_t fail_read = UINT64_MAX, fail_write = UINT64_MAX;
	size_t partial_write = 0;
	int error = 0;
	bool enabled = true;
	Handle lookup = 0;
	std::function<void()> on_submit;
	std::unique_ptr<Gateway> gateway;

	Fixture() {
		caller.process_name.fill('P'); caller.thread_name.fill('T');
		std::fill_n(memory.begin() + Descriptor, 64, 0);
		Put(0, UINT32_MAX, 4);
		Reset();
	}
	void Put(size_t offset, uint64_t value, size_t size = 8, uint64_t address = Descriptor) {
		for (size_t n = 0; n < size; ++n) { memory[address + offset + n] = static_cast<uint8_t>(value >> (n * 8)); }
	}
	uint32_t ResultWord() const {
		uint32_t value = 0;
		for (size_t n = 0; n < 4; ++n) { value |= uint32_t(memory[Output + n]) << (n * 8); }
		return value;
	}
	Callbacks Hooks() {
		Callbacks cb;
		cb.identity = [&] { return id; };
		cb.set_errno = [&](int e) { error = e; };
		cb.copy_in = [&](uint64_t address, void* out, size_t size) {
			reads.emplace_back(address, size); order.push_back('I');
			if (address > memory.size() || size > memory.size() - address) { return 14; }
			if (fail_read >= address && fail_read - address < size) {
				std::memcpy(out, memory.data() + address, static_cast<size_t>(fail_read - address));
				return 14;
			}
			std::memcpy(out, memory.data() + address, size); return 0;
		};
		cb.copy_out = [&](uint64_t address, const void* in, size_t size) {
			writes.emplace_back(address, size); order.push_back('O');
			if (address > memory.size() || size > memory.size() - address) { return 14; }
			if (address == fail_write) {
				std::memcpy(memory.data() + address, in, std::min(size, partial_write)); return 14;
			}
			std::memcpy(memory.data() + address, in, size); return 0;
		};
		cb.diagnostic_caller = [&] { order.push_back('C'); return caller; };
		cb.diagnostic_enabled = [&] { order.push_back('E'); return enabled; };
		cb.diagnostic_submit = [&](const DiagnosticEvent& event) {
			order.push_back('T'); events.push_back(event);
			// Reenter Core from the collector: the diagnostic lookup released its lock.
			CHECK(core.TraceServerName(lookup) == event.server_name || event.selector_input == 0x1f02);
			if (on_submit) { on_submit(); }
		};
		cb.diagnostic_unknown = [&](const DiagnosticEvent& event) { order.push_back('U'); unknown.push_back(event); };
		cb.cooperate = [] { return Interrupted; };
		cb.authorize_connect = [](Identity, const ConnectionInfo&) { return 0; };
		return cb;
	}
	void Reset(size_t capacity = 1, IpmiWire::SyncType type = IpmiWire::SyncType::Inline) {
		gateway = std::make_unique<Gateway>(core, Hooks(), IpmiWire::Limits {}, type, capacity);
	}
	int Call(uint32_t selector = 0x1f00, uint64_t size = 64, uint64_t address = Descriptor,
	         uint64_t tag = UINT64_C(0x123456789abcdef0), uint64_t output = Output) {
		int32_t signed_selector;
		std::memcpy(&signed_selector, &selector, sizeof(selector));
		error = 0;
		return gateway->Call(0x10, signed_selector, output, address, size, tag);
	}
	void ClearObservations() { reads.clear(); writes.clear(); order.clear(); events.clear(); unknown.clear(); }
};

static void SizeAndCopyPrecedence() {
	Fixture f;
	for (uint64_t size: {UINT64_C(65), UINT64_MAX}) {
		f.ClearObservations();
		CHECK(f.Call(0x1f00, size, 0) == 0 && f.ResultWord() == 0x800e0001u);
		CHECK(f.reads.empty() && f.order == std::vector<char>({'O'}));
		CHECK(f.writes.size() == 1 && f.writes[0].second == 4);
	}
	f.ClearObservations(); f.fail_write = Fixture::Output;
	CHECK(f.Call(0x1f00, 65, 0) == -1 && f.error == 14 && f.reads.empty());
	f.fail_write = UINT64_MAX;
	for (uint64_t size: {UINT64_C(0), UINT64_C(1), UINT64_C(63), UINT64_C(64)}) {
		for (uint64_t address: {UINT64_C(0), Fixture::Descriptor}) {
			if (size == 64 && address) { continue; }
			f.ClearObservations(); f.Put(0, 0xabcdef01, 4, Fixture::Output);
			CHECK(f.Call(0x1f00, size, address) == -1 && f.error == Invalid);
			CHECK(f.order.empty() && f.ResultWord() == 0xabcdef01);
		}
	}
	// Even an unused last byte must be copied before any snapshot/observation.
	f.ClearObservations(); f.fail_read = Fixture::Descriptor + 63;
	CHECK(f.Call() == -1 && f.error == 14 && f.ResultWord() == 0xabcdef01);
	CHECK(f.order == std::vector<char>({'I'}) && f.reads.size() == 1 && f.reads[0].second == 64);
	f.fail_read = UINT64_MAX;
	f.ClearObservations();
	CHECK(f.Call(0x1f00, 64, UINT64_MAX - 31) == -1 && f.error == 14);
	CHECK(f.events.empty() && f.writes.empty());
}

static void SelectorsAndTrustedSnapshot() {
	Fixture f;
	CHECK(f.core.RegisterServer(f.server, "SceDiagnostic", 0x1234, f.lookup).syscall_errno == Ok);
	f.Put(0, f.lookup, 4);
	const uint64_t numeric = UINT64_C(0xffff800012345678); // Unmapped, never dereferenced.
	f.Put(8, numeric); f.Put(0x10, 0xf1234567, 4);
	CHECK(f.Call() == 0 && f.ResultWord() == 0);
	const auto first = f.events.back();
	CHECK(first.selector == 0x1f00 && first.handle == f.lookup && first.a == 0xf1234567 && first.b == 0 && first.p == numeric);
	CHECK(first.caller.identity == f.caller.identity && first.caller.process_name == f.caller.process_name &&
	      first.caller.thread_name == f.caller.thread_name);
	CHECK(f.reads.size() == 1 && f.reads[0].first == Fixture::Descriptor && f.reads[0].second == 64);
	CHECK(f.order == std::vector<char>({'I','C','E','T','O'}));
	for (uint32_t selector: {0x1f01u, 0x1f03u, 0x1f05u}) {
		f.Put(8, 0x80550005, 4);
		CHECK(f.Call(selector) == 0 && f.ResultWord() == 0);
		const auto& event = f.events.back();
		CHECK(event.selector == selector && event.handle == f.lookup && event.a == 0x80550005 && event.b == 0 && event.p == 0);
	}
	f.Put(8, 0x87654321, 4); f.Put(0x10, numeric); f.Put(0x18, 0xabcdef98, 4);
	CHECK(f.Call(0x1f04) == 0);
	CHECK(f.events.back().a == 0x87654321 && f.events.back().b == 0xabcdef98 && f.events.back().p == numeric);
	CHECK(f.events.back().handle == f.lookup && f.events.back().selector == 0x1f04);
	CHECK(f.unknown.empty());
	std::fill_n(f.memory.begin() + Fixture::Descriptor + 8, 25, 'X');
	f.memory[Fixture::Descriptor + 33] = 'Y'; f.Put(0x24, 0x80020016, 4);
	CHECK(f.Call(0x1f02) == 0 && f.ResultWord() == 0);
	const auto& inline_event = f.events.back();
	CHECK(inline_event.handle == 0xffff && inline_event.a == 0x80020016 && inline_event.b == 0 && inline_event.p == 0);
	CHECK(std::all_of(inline_event.server_name.begin(), inline_event.server_name.end(), [](char c) { return c == 'X'; }));
	CHECK(inline_event.caller.process_name == f.caller.process_name);
	f.memory[Fixture::Descriptor + 10] = 0;
	CHECK(f.Call(0x1f02) == 0);
	CHECK(f.events.back().server_name[0] == 'X' && f.events.back().server_name[1] == 'X');
	CHECK(std::all_of(f.events.back().server_name.begin() + 2, f.events.back().server_name.end(), [](char c) { return c == 0; }));
	f.memory[Fixture::Descriptor + 8] = 0;
	CHECK(f.Call(0x1f02) == 0);
	CHECK(std::all_of(f.events.back().server_name.begin(), f.events.back().server_name.end(), [](char c) { return c == 0; }));
	// No aliasing of the collector's retained snapshot with later caller updates.
	f.caller.process_name.fill('Q'); CHECK(first.caller.process_name[0] == 'P');
}

static void UnknownDisabledAndTags() {
	Fixture f;
	CHECK(f.Call(0x00011f00) == 0 && f.ResultWord() == 0);
	CHECK(f.events.size() == 1 && f.unknown.size() == 1);
	const auto& event = f.events.back();
	CHECK(event.selector_input == 0x00011f00 && event.selector == 0x1f00 && event.handle == 0xffff &&
	      event.a == 0 && event.b == 0 && event.p == UINT64_MAX);
	f.ClearObservations();
	CHECK(f.Call(0x80009f00) == 0); // Low16 sign bit filters append, not fallback observation.
	CHECK(f.events.empty() && f.unknown.size() == 1 && f.unknown[0].selector_input == 0x80009f00);
	CHECK(f.order == std::vector<char>({'I','C','U','O'}));
	f.ClearObservations(); f.enabled = false;
	CHECK(f.Call() == 0 && f.events.empty() && f.unknown.empty());
	CHECK(f.Call(0x1234) == 0 && f.events.empty() && f.unknown.size() == 1);
	auto hooks = f.Hooks(); hooks.diagnostic_submit = {}; hooks.diagnostic_enabled = {}; hooks.diagnostic_caller = {};
	f.gateway = std::make_unique<Gateway>(f.core, hooks);
	CHECK(f.Call() == 0); // Absent collector is disabled success.
	hooks.diagnostic_submit = f.Hooks().diagnostic_submit;
	hooks.diagnostic_enabled = [] { return true; };
	f.gateway = std::make_unique<Gateway>(f.core, hooks);
	CHECK(f.Call() == 0 && f.events.back().caller.identity == f.id);
	CHECK(f.events.back().caller.process_name[0] == 0 && f.events.back().caller.thread_name[0] == 0);
	f.Reset(0, static_cast<IpmiWire::SyncType>(0)); // Diagnostics need no sync wire mode or Pending budget.
	f.enabled = true;
	for (uint64_t tag: {UINT64_C(0), NativeDefaultTag, UINT64_MAX, UINT64_C(0xfeedface12345678)}) {
		CHECK(f.Call(0x1f00, 64, Fixture::Descriptor, tag) == 0);
	}
	f.Reset();
	// The ordinary gateway's tag policy remains in place.
	f.Put(0, 1);
	CHECK(f.gateway->Call(0x202, 123, Fixture::Output, Fixture::Descriptor, 24, 0xfeedface) == -1 && f.error == Unsupported);
}

static void ServerLookupLifetime() {
	Fixture f;
	Handle original = 0, client = 0, session = 0;
	CHECK(f.core.RegisterServer(f.server, "SceDiagnostic", 1, original).syscall_errno == Ok);
	f.lookup = original; f.Put(0, original, 4);
	CHECK(f.Call() == 0 && f.events.back().server_name[0] == 'S'); // Caller PID200, owner PID100.
	CHECK(f.core.RegisterClient(f.id, "SceDiagnostic", Config {}, client).syscall_errno == Ok);
	Ticket ticket; CHECK(f.core.Connect(f.id, client, ticket).syscall_errno == Ok);
	Dispatch dispatch; CHECK(f.core.Receive(f.worker, original, dispatch).syscall_errno == Ok);
	CHECK(f.core.CreateSession(f.worker, 0x1234, 0, session).syscall_errno == Ok);
	const std::array<char, 25> empty_name {};
	for (Handle handle: {client, session, original + 0x10000u, Handle(0), Handle(UINT32_MAX)}) {
		f.lookup = 0; f.Put(0, handle, 4);
		CHECK(f.Call() == 0 && f.events.back().server_name == empty_name);
		CHECK(f.events.back().handle == static_cast<uint16_t>(handle));
	}
	// Independent server retirement: a stale handle must not find its replacement.
	Handle retired = 0, replacement = 0;
	CHECK(f.core.RegisterServer(f.server, "SceRetired", 1, retired).syscall_errno == Ok);
	CHECK(f.core.DestroyServer(f.server, retired).syscall_errno == Ok);
	CHECK(f.core.RegisterServer(f.server, "SceRetired", 1, replacement).syscall_errno == Ok && replacement != retired);
	f.Put(0, retired, 4); CHECK(f.Call() == 0 && f.events.back().server_name[0] == 0);
	f.lookup = replacement; f.Put(0, replacement, 4);
	CHECK(f.Call() == 0 && f.events.back().server_name[0] == 'S');
	CHECK(f.Call(0x00011f00) == 0 && f.unknown.back().server_name == f.core.TraceServerName(replacement));
}

static void FinalCopyFaultAndNoRetention() {
	Fixture f;
	f.Reset(0);
	f.fail_write = Fixture::Output; f.partial_write = 2;
	CHECK(f.Call() == -1 && f.error == 14 && f.events.size() == 1);
	CHECK(f.ResultWord() == 0xcccc0000 && f.writes.size() == 1 && f.writes[0].second == 4);
	CHECK(f.memory[Fixture::Output - 1] == 0xcc && f.memory[Fixture::Output + 4] == 0xcc);
	CHECK(f.order == std::vector<char>({'I','C','E','T','O'}));
	f.Put(0x10, 99, 4);
	CHECK(f.Call() == -1 && f.events.size() == 2 && f.events.back().a == 99 && f.reads.size() == 2);
	f.fail_write = UINT64_MAX;
	CHECK(f.Call() == 0 && f.events.size() == 3 && f.ResultWord() == 0);
	CHECK(f.Call(0x1f00, 64, Fixture::Descriptor, 0, UINT64_MAX) == -1 && f.error == 14 && f.events.size() == 4);
	// A collector may reenter the gateway; there is no diagnostic active/cache key.
	bool nested = false;
	f.on_submit = [&] { if (!nested) { nested = true; CHECK(f.Call(0x1f01) == 0); } };
	CHECK(f.Call() == 0 && nested && f.events.size() == 6);
	f.on_submit = {};
	// A suspended transport transaction survives diagnostics on the same identity.
	f.Reset(1); Handle server = 0, client = 0, session = 0;
	CHECK(f.core.RegisterServer(f.server, "ScePending", 1, server).syscall_errno == Ok);
	CHECK(f.core.RegisterClient(f.id, "ScePending", Config {}, client).syscall_errno == Ok);
	constexpr uint64_t connect_descriptor = 0x300, connect_result = 0x380, method = 0x390;
	std::fill_n(f.memory.begin() + connect_descriptor, 32, 0);
	f.Put(0x10, method, 8, connect_descriptor);
	CHECK(f.gateway->Call(0x400, client, connect_result, connect_descriptor, 32, 0) == -1 && f.error == Interrupted);
	CHECK(f.Call() == 0);
	Dispatch dispatch; CHECK(f.core.Receive(f.worker, server, dispatch).syscall_errno == Ok);
	CHECK(f.core.CreateSession(f.worker, 0x1234, 0, session).syscall_errno == Ok);
	CHECK(f.core.Accept(f.worker, client, 0).syscall_errno == Ok);
	CHECK(f.gateway->Call(0x400, client, connect_result, connect_descriptor, 32, 0) == 0);
	CHECK(f.core.Receive(f.worker, server, dispatch).syscall_errno == WouldBlock);
}

int main() {
	SizeAndCopyPrecedence(); SelectorsAndTrustedSnapshot(); UnknownDisabledAndTags();
	ServerLookupLifetime(); FinalCopyFaultAndNoRetention();
	std::cout << "IPMI diagnostic tests passed\n";
}
