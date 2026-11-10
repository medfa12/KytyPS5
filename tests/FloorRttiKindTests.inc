// Include in VirtualMemoryAllocationTests.cpp's fixture namespace; dispatch
// TestFloorRttiKind() in a dedicated process after InitSubsystems(). Add this
// top-level declaration before including:
// namespace Libs::ShellCoreFloor::LibcFloor {
// void InitShellCoreFloorLibc_1(Loader::SymbolDatabase*);
// }
// Exercises the actual registered __dynamic_cast HLE, without firmware or a
// test-only production entry point. Existing fixture headers suffice.
void TestFloorRttiKind() {
	constexpr const char* test = "FloorRttiKind";
	using Mode = Common::VirtualMemory::Mode;
	using namespace Libs::LibKernel::Memory;
	Loader::SymbolDatabase symbols;
	Libs::ShellCoreFloor::LibcFloor::InitShellCoreFloorLibc_1(&symbols);
	const auto* symbol = symbols.FindByNid("hMAe+TWS9mQ", Loader::SymbolType::Func);
	Check(test, symbol != nullptr && symbol->vaddr != 0, "resolve actual dynamic_cast export");
	const auto cast = reinterpret_cast<uint64_t(KYTY_SYSV_ABI *)(uint64_t, uint64_t, uint64_t, uint64_t)>(symbol->vaddr);
	auto addr = [](const auto* p) { return reinterpret_cast<uint64_t>(p); };
	struct Ti { uint64_t vptr; uint64_t name; uint64_t data[33] {}; };
	// Each address point is +16, with offset-to-top 0 and an identifying RTTI
	// record at -8, matching native libc's Itanium vtable headers.
	Ti class_meta {0, addr("N10__cxxabiv117__class_type_infoE")};
	Ti si_meta {0, addr("N10__cxxabiv120__si_class_type_infoE")};
	Ti vmi_meta {0, addr("N10__cxxabiv121__vmi_class_type_infoE")};
	Ti unknown_meta {0, addr("N7Fixture11UnknownKindE")};
	std::array<uint64_t, 5> class_vt {0, addr(&class_meta)};
	std::array<uint64_t, 5> si_vt {0, addr(&si_meta)};
	std::array<uint64_t, 5> vmi_vt {0, addr(&vmi_meta)};
	std::array<uint64_t, 5> unknown_vt {0, addr(&unknown_meta)};
	Ti base {addr(class_vt.data() + 2), addr("N7Fixture4BaseE")};
	Ti other {addr(class_vt.data() + 2), addr("N7Fixture5OtherE")};
	Ti hidden {addr(class_vt.data() + 2), addr("N7Fixture6HiddenE")};
	Ti unrelated {addr(class_vt.data() + 2), addr("N7Fixture9UnrelatedE")};
	Ti single {addr(si_vt.data() + 2), addr("N7Fixture6SingleE")};
	single.data[0] = addr(&base);
	Ti multiple {addr(vmi_vt.data() + 2), addr("N7Fixture8MultipleE")};
	multiple.data[0] = uint64_t{3} << 32;
	multiple.data[1] = addr(&single); multiple.data[2] = (0x18 << 8) | 2;
	multiple.data[3] = addr(&hidden); multiple.data[4] = (0x30 << 8); // nonpublic
	multiple.data[5] = addr(&other); multiple.data[6] = (0x40 << 8) | 2;
	std::array<uint64_t, 5> object_vt {};
	std::array<uint64_t, 32> object {};
	object[0] = addr(object_vt.data() + 2);
	auto probe = [&](Ti& dynamic, Ti& destination) {
		object_vt[1] = addr(&dynamic);
		return cast(addr(object.data()), addr(&base), addr(&destination), 0);
	};
	Check(test, probe(base, base) == addr(object.data()), "plain class exact identity");
	base.data[0] = addr(&other); // readable garbage after a plain class record
	Check(test, probe(base, other) == 0, "plain class must not read an SI payload");
	Check(test, probe(single, base) == addr(object.data()), "SI public offset-zero base");
	Check(test, probe(multiple, base) == addr(object.data()) + 0x18, "VMI plus SI retains nonzero public offset");
	Check(test, probe(multiple, other) == addr(object.data()) + 0x40, "later VMI public base offset");
	Check(test, probe(multiple, hidden) == 0 && probe(multiple, unrelated) == 0, "private and unrelated targets fail");
	Check(test, cast(0, addr(&base), addr(&base), 0) == 0, "null object fails");

	constexpr uint64_t collision_address = uint64_t{3} << 32;
	constexpr uint64_t page = 0x4000;
	struct Cleanup {
		uint64_t collision = 0;
		uint64_t guarded = 0;
		~Cleanup() {
			if (guarded) { FreeGuestMemory(guarded, 2 * page); }
			if (collision) { FreeGuestMemory(collision, page); }
		}
	} cleanup;
	cleanup.collision = AllocateProgramMemoryAt(collision_address, page, Mode::ReadWrite, "rtti-count-collision");
	Check(test, cleanup.collision == collision_address, "fixture requires the exact readable count<<32 address");
	// This is deliberately valid but UNRELATED RTTI at the mistaken SI base
	// pointer. The old walker immediately recursed here, losing all VMI bases.
	auto* collision = reinterpret_cast<Ti*>(collision_address);
	*collision = unrelated;
	Check(test, probe(multiple, base) == addr(object.data()) + 0x18 &&
	            probe(multiple, other) == addr(object.data()) + 0x40,
	      "readable unrelated RTTI at count<<32 must not hide public VMI bases");
	Check(test, probe(multiple, unrelated) == 0, "collision RTTI must not invent a base relationship");
	collision->name = addr("N7Fixture4BaseE");
	Check(test, probe(multiple, base) == addr(object.data()) + 0x18,
	      "even matching collision names cannot replace the real nonzero base offset");

	const Ti valid_multiple = multiple;
	for (uint64_t header: {uint64_t{0}, uint64_t{17} << 32, (uint64_t{3} << 32) | 1,
	                      (uint64_t{3} << 32) | 2, (uint64_t{3} << 32) | 4}) {
		multiple.data[0] = header;
		Check(test, probe(multiple, base) == 0, "invalid count or unsupported VMI flags rejected");
	}
	multiple = valid_multiple;
	for (uint64_t flags: {uint64_t{3}, uint64_t{6}, UINT64_MAX}) {
		multiple.data[2] = flags;
		Check(test, probe(multiple, base) == 0, "virtual, unknown or negative base layout rejected");
	}
	multiple = valid_multiple;
	multiple.data[5] = addr(&base); // second public nonvirtual occurrence
	Check(test, probe(multiple, base) == 0, "ambiguous repeated public target rejected");
	multiple = valid_multiple;
	Ti unknown {addr(unknown_vt.data() + 2), base.name};
	Check(test, probe(unknown, base) == 0, "unknown kind cannot bypass validation by matching the target name");
	Ti cycle {addr(si_vt.data() + 2), addr("N7Fixture5CycleE")};
	cycle.data[0] = addr(&cycle);
	Check(test, probe(cycle, base) == 0, "cyclic SI metadata bounded by recursion limit");
	class_vt[0] = 8;
	Check(test, probe(base, base) == 0, "noncanonical type-info vtable offset-to-top rejected");
	class_vt[0] = 0;

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	// FloorRangeOk's fault containment is Windows-specific. Keep guard-page
	// probes here rather than claiming the non-Windows floor can safely read
	// arbitrary invalid guest addresses.
	cleanup.guarded = AllocateRuntimeMemory(0, 2 * page, Mode::ReadWrite, "rtti-guard");
	Check(test, cleanup.guarded != 0, "allocate bounded RTTI fixture");
	Check(test, Common::VirtualMemory::Protect(cleanup.guarded + page, page, Mode::NoAccess), "guard RTTI tail");
	auto* short_vmi = reinterpret_cast<uint64_t*>(cleanup.guarded + page - 24);
	short_vmi[0] = multiple.vptr; short_vmi[1] = multiple.name; short_vmi[2] = uint64_t{3} << 32;
	object_vt[1] = addr(short_vmi);
	Check(test, cast(addr(object.data()), addr(&base), addr(&base), 0) == 0, "truncated VMI base array rejected");
	auto* short_si = reinterpret_cast<uint64_t*>(cleanup.guarded + page - 16);
	short_si[0] = single.vptr; short_si[1] = single.name;
	object_vt[1] = addr(short_si);
	Check(test, cast(addr(object.data()), addr(&base), addr(&base), 0) == 0, "truncated SI base field rejected");
	short_si[0] = base.vptr; short_si[1] = base.name;
	Check(test, cast(addr(object.data()), addr(&base), addr(&base), 0) == addr(object.data()) &&
	            cast(addr(object.data()), addr(&base), addr(&other), 0) == 0,
	      "plain class needs only its 16-byte header, never a base payload beyond the guard");
	Ti bad = single;
	bad.data[0] = cleanup.guarded + page;
	Check(test, probe(bad, base) == 0, "unreadable base type-info rejected");
	bad = base; bad.vptr = 8;
	Check(test, probe(bad, base) == 0, "underflowing vtable header rejected");
	bad = base; bad.vptr = cleanup.guarded + page + 16;
	Check(test, probe(bad, base) == 0, "unreadable vtable header rejected");
	bad = base; bad.name = cleanup.guarded + page - 32;
	std::memset(reinterpret_cast<void*>(bad.name), 'x', 32);
	Check(test, probe(bad, base) == 0, "unterminated name at guard boundary rejected");
#endif
}
