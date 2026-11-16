// Device-free actual loader/registry/module startup/guest pthread integration.
// Usage: ProductionAudiodecTests importer.elf firmware-root tones.eac3
// Empty firmware root tests missing dispatcher; a root containing only DDP tests
// wrong-prefix rejection. Each scenario runs in a fresh process.
#include "common/threads.h"
#include "common/singleton.h"
#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/subsystems.h"
#include "common/virtualMemory.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kernel/fileSystem.h"
#include "libs/libs.h"
#include "loader/runtimeLinker.h"
#include "loader/symbolDatabase.h"
#include <limits>

// Share the PCM contract, excluding ALL standalone mapper and helper bindings.
#define NATIVE_AUDIODEC_PRODUCTION
#include "NativeAudiodecCpuTests.cpp"

struct Integration {
	Loader::RuntimeLinker* linker;
	Loader::Program* importer;
	const char* fixture;
};
static void* KYTY_SYSV_ABI DecodeWorker(void* argument) {
	auto& test = *static_cast<Integration*>(argument);
	Libs::LibKernel::Memory::VirtualQueryInfo stack {};
	Check(Libs::LibKernel::Memory::KernelVirtualQuery(&stack, 0, &stack, sizeof(stack)) == 0 &&
	      stack.is_stack && stack.is_committed, "decoder worker is not on a committed production guest stack");
	std::printf("ProductionAudiodecTests: committed guest stack bytes=%llu\n",
	            static_cast<unsigned long long>(stack.end - stack.start));
	const auto* helper = test.linker->Symbols()->FindByNid("aaDMGGkXFxo", Loader::SymbolType::Func);
	Check(helper != nullptr, "production frexpf registration absent");
	using FrexpCall = float (KYTY_SYSV_ABI *)(float, int*);
	const auto frexp_call = reinterpret_cast<FrexpCall>(helper->vaddr);
	int exponent = 123;
	Check(frexp_call(-6.0f, &exponent) == -0.75f && exponent == 3, "frexpf mixed float/pointer ABI");
	Check(frexp_call(std::numeric_limits<float>::denorm_min(), &exponent) == 0.5f && exponent == -148,
	      "frexpf subnormal normalization");
	test.linker->StartAllModules();
	for (auto* program: test.linker->GetPrograms()) {
		if (program != test.importer) {
			Check(program->initializer_returned && program->initializer_result >= 0 && program->started,
			      "native module initializer failed");
		}
	}
	std::puts("ProductionAudiodecTests: native module startup returned");
	const auto* slots = reinterpret_cast<const uint64_t*>(test.importer->base_vaddr + 0xc00);
	RunDecoderContract(reinterpret_cast<Call>(slots[0]), reinterpret_cast<Call>(slots[1]),
	                   reinterpret_cast<Call>(slots[2]), test.fixture);
	return reinterpret_cast<void*>(1);
}

int main(int argc, char** argv) {
	Check(argc == 4, "expected importer, firmware root, encoded fixture");
	using namespace Libs::LibKernel;
	static Common::Subsystems subsystems;
	Common::VirtualMemory::Init();
	Common::InitializeThreads();
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options;
	options.printf_direction = Config::LogDirection::Console;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Memory::Lifecycle>();
	subsystems.Initialize<FileSystem::Lifecycle>();
	Initialize();
	PthreadInitSelfForMainThread();
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());
	const std::filesystem::path root(argv[2]);
	FileSystem::Mount(root / "system_b/common", "/common", true);
	FileSystem::Mount(root / "system_b", "/system", true);
	auto* importer = rt->LoadProgram(argv[1]);
	Check(importer != nullptr, "actual importer mapping failed");
	rt->EnsureImportProviders();
	rt->RelocateAll();
	Check(rt->GetPrograms().size() == 3, "expected importer, CPU and DDP from two discovery passes");
	auto* cpu = rt->FindProgramByFileName(root / "system_b/common/lib/libSceAudiodecCpu.sprx");
	auto* ddp = rt->FindProgramByFileName(root / "system_b/common/lib/libSceAudiodecCpuDdp.sprx");
	Check(cpu && ddp && ddp->fail_if_global_not_resolved, "missing native providers/strict DDP policy");
	const auto relocations = (ddp->dynamic_info->rela_table_total_size + ddp->dynamic_info->jmprela_table_size) / 24;
	for (size_t i = 0; i < relocations; ++i) {
		Check((ddp->rela_bits[i / 8] & (1u << (i % 8))) != 0, "DDP relocation closure incomplete");
	}
	const auto* slots = reinterpret_cast<const uint64_t*>(importer->base_vaddr + 0xc00);
	const char* nids[] = {"ktD2w3D4G2U", "hdFsxo3MFu8", "lSVTiWV5wLc", "hAS5WH6hxrE"};
	for (unsigned i = 0; i < 4; ++i) {
		const auto* symbol = cpu->export_symbols->FindByNid(nids[i], Loader::SymbolType::Func);
		std::printf("API %s GOT=%llx export=%llx\n", nids[i], static_cast<unsigned long long>(slots[i]),
		            static_cast<unsigned long long>(symbol ? symbol->vaddr : 0));
		Loader::SymbolResolve query {nids[i], "libSceAudiodecCpu", 1, "libSceAudiodecCpu", 1, 1, Loader::SymbolType::Func};
		Check(symbol && slots[i] == Loader::TraceWrapImport(symbol->vaddr, Loader::SymbolDatabase::GenerateName(query)),
		      "import GOT does not point through production trace thunk to actual CPU export");
	}
	std::puts("ProductionAudiodecTests: two-pass discovery and actual API relocations passed");
	Integration test {rt, importer, argv[3]};
	Pthread worker = nullptr;
	Check(PthreadCreate(&worker, nullptr, DecodeWorker, &test, "audiodec-integration") == 0, "guest pthread creation failed");
	void* result = nullptr;
	Check(PthreadJoin(worker, &result) == 0 && result == reinterpret_cast<void*>(1), "guest decoder worker failed");
	std::puts("ProductionAudiodecTests: production integration passed (no audio device)");
	std::fflush(stdout);
	std::_Exit(0); // No global emulator teardown/device lifecycle in this fixture.
}
