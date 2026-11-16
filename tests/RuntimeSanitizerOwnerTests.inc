// Included by runtimeLinker.cpp in Loader. Real registered SysV query targets,
// exercised through production HostCallReturnThunk/RuntimeGuestExecution, not an
// override of the returning-export policy (the parent owns those exact pairs).
namespace RuntimeSanitizerOwnerFixture {
constexpr std::array<const char*, 2> Nids {"py6L8jiVAN8", "bnZxYgAFeA0"};
constexpr std::array<const char*, 2> Expressions {"LibKernel::KernelGetSanitizerMallocReplaceExternal",
    "LibKernel::KernelGetSanitizerNewReplaceExternal"};
constexpr std::array<uint64_t, 2> Sizes {112, 104};
constexpr std::array<ApplicationSanitizerReplacement, 2> Kinds {
    ApplicationSanitizerReplacement::Malloc, ApplicationSanitizerReplacement::New};
using Query = uint64_t(KYTY_SYSV_ABI*)();
struct Invocation {
	uint64_t gateway = 0;
	uint64_t target = 0;
	RuntimeGuestExecution* execution = nullptr;
	uint64_t observed = 0;
	unsigned continued = 0;
	unsigned destroyed = 0;
};
static uint64_t KYTY_SYSV_ABI GuestQuery(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	const auto result = reinterpret_cast<Query>(call->gateway)();
	++call->continued;
	return result;
}
static uint64_t KYTY_SYSV_ABI GuestQueryWithArgument(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	const auto result = reinterpret_cast<uint64_t(KYTY_SYSV_ABI*)(void*)>(call->gateway)(opaque);
	++call->continued;
	return result;
}
static uint64_t KYTY_SYSV_ABI StopAfterRegisteredQuery(void* opaque) {
	auto* call = static_cast<Invocation*>(opaque);
	struct Pin { Invocation* call; ~Pin() { ++call->destroyed; } } pin {call};
	call->observed = reinterpret_cast<Query>(call->target)();
	call->execution->RequestStop();
	return call->observed; // Host RAII must finish before the outer gateway aborts.
}
static std::array<uint64_t, 14> Words(uint64_t address, uint64_t size) {
	std::array<uint64_t, 14> words {};
	std::memcpy(words.data(), reinterpret_cast<const void*>(address), size);
	return words;
}
static bool Inactive(uint64_t address, unsigned kind) {
	if (!address) { return false; }
	const auto words = Words(address, Sizes[kind]);
	if (words[0] != Sizes[kind]) { return false; }
	for (uint64_t i = 1; i < Sizes[kind] / sizeof(uint64_t); ++i) {
		if (words[i] != 0) { return false; }
	}
	return true;
}
}

bool RuntimeLinker::TestRuntimeSanitizerOwner(std::string* failure) {
	using namespace RuntimeSanitizerOwnerFixture;
	const auto check = [&](bool ok, const char* message) {
		if (!ok && failure) { *failure = message; }
		return ok;
	};
	auto& linker = *Common::Singleton<RuntimeLinker>::Instance();
	Program programs[7]; // A, B, private A/B, session, relaunch A, unavailable/stopped-before-first-query
	std::vector<std::unique_ptr<HostCallReturnThunk>> gateways;
	struct Cleanup {
		RuntimeLinker& linker;
		Program* programs;
		Program* previous = RuntimeLinker::GetCurrentProgram();
		std::vector<Program*> original;
		~Cleanup() {
			RuntimeLinker::SetCurrentProgram(previous);
			for (unsigned i = 0; i != 7; ++i) {
				if (auto execution = GetApplicationExecution(&programs[i])) { execution->RequestStop(); }
			}
			linker.m_programs = std::move(original);
		}
	} cleanup {linker, programs};
	cleanup.original = linker.m_programs;
	Libs::InitLibKernel_1(linker.Symbols());
	std::array<uint64_t, 2> targets {};
	for (unsigned kind = 0; kind != 2; ++kind) {
		const auto* registered = linker.Symbols()->FindRegistration(
		    {Nids[kind], "libkernel", 1, "libkernel", 1, 1, SymbolType::Func}, Expressions[kind]);
		if (!check(registered != nullptr, "exact production sanitizer-query registration must exist")) { return false; }
		targets[kind] = registered->vaddr;
	}
	for (unsigned i = 0; i != 7; ++i) {
		auto& program = programs[i];
		program.rt = &linker;
		program.unique_id = 989700 + i;
		program.file_name = i == 0 || i == 5 ? "SanitizerApplicationA.bin" :
		    i == 1 ? "SanitizerApplicationB.bin" : i == 4 ? "SanitizerShell.bin" : "private-sanitizer.prx";
		program.application_owner = i == 0 || i == 1 || i == 5 || i == 6 ? &program :
		    i == 2 ? &programs[0] : i == 3 ? &programs[1] : nullptr;
		if ((i == 0 || i == 1 || i == 5) &&
		    !check(InitializeApplicationExecution(&program), "initialize actual sanitizer-query owner")) { return false; }
		linker.m_programs.push_back(&program);
	}
	RuntimeLinker::SetCurrentProgram(&programs[4]);
	uint64_t session[2] {};
	std::array<uint64_t, 14> session_words[2];
	for (unsigned kind = 0; kind != 2; ++kind) {
		session[kind] = reinterpret_cast<Query>(targets[kind])();
		if (!check(Inactive(session[kind], kind) && reinterpret_cast<Query>(targets[kind])() == session[kind],
		           "session query must retain its original stable sized host table with null hooks")) { return false; }
		session_words[kind] = Words(session[kind], Sizes[kind]);
	}
	const auto gateway = [&](unsigned owner, uint64_t target) {
		const auto execution = programs[owner].application_execution;
		auto thunk = HostCallReturnThunk::Create(execution->Boundary(), target, execution.get(),
		    RuntimeGuestExecution::EnterHostCall, RuntimeGuestExecution::LeaveHostCall, HostCallAbi::BaselineSysV);
		if (!thunk) { return uint64_t {0}; }
		const auto address = thunk->GetAddress();
		gateways.push_back(std::move(thunk));
		return address;
	};
	uint64_t addresses[6][2] {};
	uint64_t query_gateways[2][2] {};
	for (unsigned owner = 0; owner != 2; ++owner) {
		RuntimeLinker::SetCurrentProgram(&programs[owner]);
		// B asks for new first: neither query may rely on the other having run.
		for (unsigned index = 0; index != 2; ++index) {
			const auto kind = owner == 1 ? 1 - index : index;
			query_gateways[owner][kind] = gateway(owner, targets[kind]);
			if (!check(query_gateways[owner][kind] != 0, "create real owner-bound query return gateway")) { return false; }
			Invocation call {query_gateways[owner][kind]};
			const auto result = programs[owner].application_execution->Invoke(GuestQuery, &call);
			addresses[owner][kind] = result.call.value;
			if (!check(result.Returned() && call.continued == 1 && Inactive(addresses[owner][kind], kind) &&
			               addresses[owner][kind] != session[kind] && programs[owner].application_import_data &&
			               programs[owner].application_execution->Outstanding() == 0 &&
			               HostCallReturnThunk::CurrentDepth() == 0 && GuestCallBoundary::CurrentDepth() == 0,
			           "registered SysV query must return a sized null-hook guest table through its production gateway")) { return false; }
			void* begin = nullptr; void* end = nullptr; int prot = -1;
			if (!check(Libs::LibKernel::Memory::KernelQueryMemoryProtection(reinterpret_cast<void*>(addresses[owner][kind]),
			               &begin, &end, &prot) == 0 && prot == 3 &&
			               addresses[owner][kind] + Sizes[kind] <= reinterpret_cast<uint64_t>(end),
			           "query tables must have real guest RW backing, never executable or raw host storage")) { return false; }
			const auto lifetime = programs[owner].application_execution->Owner();
			for (uint64_t byte = 0; byte < Sizes[kind]; ++byte) {
				if (!check(!programs[owner].application_import_data->ContainsObjectAddress(lifetime, addresses[owner][kind] + byte),
				           "API-returned table bytes must never join the audited ELF import-object whitelist")) { return false; }
			}
			SymbolRecord object {SymbolDatabase::GenerateName({Nids[kind], "libkernel", 1, "libkernel", 1, 1, SymbolType::Object}),
			    Expressions[kind], addresses[owner][kind]};
			if (!check(!ApplicationImportData::Supports(*linker.Symbols(), Nids[kind], object),
			           "a query function identity must not be reinterpreted as a data import")) { return false; }
			RuntimeLinker::SetCurrentProgram(&programs[owner + 2]);
			Invocation module {query_gateways[owner][kind]};
			const auto shared = programs[owner].application_execution->Invoke(GuestQuery, &module);
			addresses[owner + 2][kind] = shared.call.value;
			if (!check(shared.Returned() && shared.call.value == addresses[owner][kind] && module.continued == 1,
			           "every private module must observe its executable owner's exact stable table")) { return false; }
			RuntimeLinker::SetCurrentProgram(&programs[owner]);
		}
	}
	for (unsigned kind = 0; kind != 2; ++kind) {
		if (!check(addresses[0][kind] != addresses[1][kind], "concurrent owners must not share sanitizer query tables")) { return false; }
		const uint64_t value = 0x1234567800000000ULL + kind;
		std::memcpy(reinterpret_cast<void*>(addresses[2][kind] + sizeof(uint64_t)), &value, sizeof(value));
		RuntimeLinker::SetCurrentProgram(&programs[0]);
		Invocation repeat {query_gateways[0][kind]};
		const auto result = programs[0].application_execution->Invoke(GuestQuery, &repeat);
		if (!check(result.Returned() && result.call.value == addresses[0][kind] &&
		               Words(addresses[0][kind], Sizes[kind])[1] == value && Inactive(addresses[1][kind], kind) &&
		               Words(session[kind], Sizes[kind]) == session_words[kind],
		           "module writes must persist only in A's own table, without resetting it or affecting B/session")) { return false; }
	}
	if (!check(addresses[0][0] != addresses[0][1], "malloc and new must have separate owner-local table storage")) { return false; }
	RuntimeLinker::SetCurrentProgram(&programs[6]);
	for (auto target: targets) {
		if (!check(reinterpret_cast<Query>(target)() == 0 && !programs[6].application_import_data,
		           "a shared owner without execution admission must not fall back to a session table")) { return false; }
	}
	if (!check(InitializeApplicationExecution(&programs[6]), "initialize stopped-before-first-query owner")) { return false; }
	programs[6].application_execution->RequestStop();
	for (auto target: targets) {
		if (!check(reinterpret_cast<Query>(target)() == 0 && !programs[6].application_import_data,
		           "stop before first query must deny allocation/publication rather than expose global data")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&programs[0]);
	const auto lifetime = programs[0].application_execution->Owner();
	Invocation stopping;
	stopping.execution = programs[0].application_execution.get();
	stopping.target = targets[0];
	stopping.gateway = gateway(0, reinterpret_cast<uint64_t>(StopAfterRegisteredQuery));
	if (!check(stopping.gateway != 0, "create stop-after-query host gateway")) { return false; }
	const auto stopped = programs[0].application_execution->Invoke(GuestQueryWithArgument, &stopping);
	if (!check(stopped.stopped && !stopped.Returned() && !stopped.failure && stopping.observed == addresses[0][0] &&
	               stopping.continued == 0 && stopping.destroyed == 1 && programs[0].application_execution->Outstanding() == 0 &&
	               HostCallReturnThunk::CurrentDepth() == 0 && GuestCallBoundary::CurrentDepth() == 0,
	           "stop after actual query must finish host RAII and abort at the gateway before publishing a guest return")) { return false; }
	for (unsigned kind = 0; kind != 2; ++kind) {
		RuntimeLinker::SetCurrentProgram(&programs[2]);
		if (!check(reinterpret_cast<Query>(targets[kind])() == 0 &&
		               programs[0].application_import_data->SanitizerTable(lifetime, Kinds[kind]) == addresses[0][kind] &&
		               Words(addresses[0][kind], Sizes[kind])[1] == 0x1234567800000000ULL + kind,
		           "stopped module queries must deny publication while previously copied owner tables stay pinned")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&programs[5]);
	if (!check(linker.GetApplicationSanitizerReplacement(static_cast<ApplicationSanitizerReplacement>(99)) == 0 &&
	               !programs[5].application_import_data,
	           "an unknown table kind must not allocate or publish arbitrary store bytes")) { return false; }
	for (unsigned kind = 0; kind != 2; ++kind) {
		Invocation relaunch {gateway(5, targets[kind])};
		if (!check(relaunch.gateway != 0, "create relaunch query gateway")) { return false; }
		const auto result = programs[5].application_execution->Invoke(GuestQuery, &relaunch);
		addresses[5][kind] = result.call.value;
		if (!check(result.Returned() && Inactive(result.call.value, kind) && result.call.value != addresses[0][kind] &&
		               result.call.value != addresses[1][kind] && result.call.value != session[kind] &&
		               programs[5].application_execution->Owner().token != lifetime.token &&
		               programs[0].application_import_data->SanitizerTable(programs[5].application_execution->Owner(), Kinds[kind]) == 0,
		           "same-file relaunch must get fresh null-hook tables and reject the stopped owner's lifetime")) { return false; }
	}
	RuntimeLinker::SetCurrentProgram(&programs[4]);
	for (unsigned kind = 0; kind != 2; ++kind) {
		if (!check(reinterpret_cast<Query>(targets[kind])() == session[kind] && Words(session[kind], Sizes[kind]) == session_words[kind],
		           "all shared queries, writes, stops and relaunches must leave the session query result unchanged")) { return false; }
	}
	for (auto& program: programs) {
		if (program.application_execution && !check(program.application_execution->Outstanding() == 0 &&
		    !program.application_execution->Failure(), "query fixture must leave every owner balanced and unfailed")) { return false; }
	}
	return true;
}
