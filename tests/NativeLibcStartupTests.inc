// Parent includes loader/nativeLibcStartup.h plus array/algorithm/cstdint.
// Host-only fixture: no firmware, RuntimeLinker or live provider is required.
// State calls are serialized by this single test thread; no concurrent-access
// safety is claimed. Controlled callbacks deliberately exercise reentry.
namespace NativeLibcStartupTest {
namespace Bridge = Loader::NativeLibcStartup;
using Bridge::Phase;
using Bridge::Result;
constexpr const char* Name = "NativeLibcStartup";
constexpr int32_t Owner = 770001;

unsigned calls = 0, order = 0;
int diagnostic = 0;
Bridge::State* active = nullptr;
Bridge::PreparedCall* active_call = nullptr;
bool reenter = false;

void KYTY_SYSV_ABI ControlledLeaf() {
	++calls;
	diagnostic = 0; // Fixture sentinel only; no production gate/global writes.
	Check(Name, order == 1 && active != nullptr && active_call != nullptr, "callback order/context");
	order = 2;
	Check(Name, active->GetPhase() == Phase::Invoking && !active->InvocationCompleted(), "completion published before return");
	if (reenter) {
		Bridge::PreparedCall nested;
		Check(Name, active->Prepare(nested) == Result::Invoking, "reentrant preparation accepted");
		Check(Name, active_call->Invoke() == Result::AlreadyInvoked, "reentrant ticket invoked twice");
		Check(Name, active->Complete(*active_call) == Result::NotReturned, "callback published its own completion");
		Check(Name, active->RecordStartupAdmission() == Result::Invoking && !active->StartupAdmitted(), "startup admitted in flight");
		Check(Name, active->Bind(Owner + 1, ControlledLeaf, true) == Result::Invoking, "replacement admitted in flight");
		Check(Name, active->InvalidateProvider(Owner) == Result::Invoking, "unload admitted in flight");
	}
}

void KYTY_SYSV_ABI OtherLeaf() { ++calls; diagnostic = -1; }

void Profile() {
	constexpr std::array<uint8_t, 32> expected {0xb4, 0xf5, 0xd9, 0xb1, 0x55, 0x59, 0x94, 0xc5,
	    0xeb, 0xae, 0xbd, 0x82, 0x91, 0x8d, 0x56, 0x0a, 0x3b, 0xf8, 0x29, 0x62,
	    0xa1, 0x71, 0xa1, 0x61, 0x4e, 0x75, 0x51, 0x93, 0x9e, 0x94, 0x33, 0x66};
	Check(Name, Bridge::LeafSize == 98 && Bridge::LeafRva == 0x63f0, "audited leaf shape");
	std::array<uint8_t, Bridge::LeafSize + 16> image {};
	for (size_t i = 0; i < Bridge::LeafSize; ++i) { image[8 + i] = static_cast<uint8_t>(i); }
	Check(Name, Bridge::MatchesLeafDigest(image, 8, expected), "exact readable profile");
	Check(Name, !Bridge::MatchesLeafDigest({}, 0, expected) && !Bridge::MatchesLeafDigest(image, SIZE_MAX, expected) &&
	            !Bridge::MatchesLeafDigest(image, image.size(), expected) && !Bridge::MatchesLeafDigest(image, 9, expected), "profile bounds/offset");
	for (size_t length = 0; length < Bridge::LeafSize; ++length) {
		Check(Name, !Bridge::MatchesLeafDigest(std::span<const uint8_t>(image.data() + 8, length), 0, expected), "truncated profile accepted");
	}
	for (size_t i = 0; i < Bridge::LeafSize; ++i) {
		image[8 + i] ^= 1;
		Check(Name, !Bridge::MatchesLeafDigest(image, 8, expected), "changed leaf byte accepted");
		image[8 + i] ^= 1;
	}
	image[0] = 0xff; image.back() = 0xaa;
	Check(Name, Bridge::MatchesLeafDigest(image, 8, expected), "leaf profile must not claim whole-image matching");
	Check(Name, !Bridge::MatchesLeafProfile(image, 8), "synthetic bytes matched production profile");
}

void DisabledAndValidation() {
	for (const char* value: {"", "0", "true", "01", "1 ", " 1", "2"}) {
		Bridge::State state(value); Bridge::PreparedCall call;
		Check(Name, !state.Enabled() && state.Bind(Owner, ControlledLeaf, true) == Result::Disabled &&
		            state.Prepare(call) == Result::Disabled && state.Complete(call) == Result::Disabled,
		      "only exact environment1 may enable bridge");
		Check(Name, call.Invoke() == Result::InvalidInvocation && state.GetPhase() == Phase::Fresh && state.Owner() == -1,
		      "off path armed a callback");
		Check(Name, state.RecordStartupAdmission() == Result::Disabled && state.StartupAdmitted(), "off-path startup history");
		Check(Name, state.InvalidateProvider(Owner) == Result::Disabled, "disabled invalidation");
	}
	Bridge::State default_off;
	Check(Name, !default_off.Enabled() && calls == 0, "default-off called native code");
	Bridge::State state("1"); Bridge::PreparedCall call;
	Check(Name, state.Enabled() && state.Prepare(call) == Result::NotBound, "fresh unbound state");
	Check(Name, state.Bind(-1, ControlledLeaf, true) == Result::InvalidOwner, "invalid owner accepted");
	Check(Name, state.Bind(Owner, nullptr, true) == Result::NullEndpoint, "null endpoint accepted");
	Check(Name, state.Bind(Owner, ControlledLeaf, false) == Result::UnverifiedProfile, "unverified provider/profile accepted");
	Check(Name, state.GetPhase() == Phase::Fresh && state.Owner() == -1 && calls == 0, "validation failure armed bridge");
}

void OnceAndReentry() {
	Bridge::State state("1"); Bridge::PreparedCall call;
	Check(Name, state.Bind(Owner, ControlledLeaf, true) == Result::Ok && state.Owner() == Owner, "bind stable owner");
	Check(Name, state.Bind(Owner, ControlledLeaf, true) == Result::AlreadyBound, "duplicate bind");
	Check(Name, state.Bind(Owner, OtherLeaf, true) == Result::ReplacementRejected &&
	            state.Bind(Owner + 1, ControlledLeaf, true) == Result::ReplacementRejected, "replacement before prepare");
	Check(Name, state.Prepare(call) == Result::Ok && state.GetPhase() == Phase::Invoking, "prepare closes admission");
	Check(Name, state.Complete(call) == Result::NotReturned && !state.InvocationCompleted(), "premature completion");
	Bridge::State foreign("1");
	Check(Name, foreign.Complete(call) == Result::InvalidInvocation, "foreign completion accepted");
	active = &state; active_call = &call; reenter = true; order = 1; diagnostic = 91;
	Check(Name, call.Invoke() == Result::Ok && calls == 1 && order == 2 && diagnostic == 0, "one actual typed callback");
	Check(Name, state.GetPhase() == Phase::Invoking && !state.InvocationCompleted(), "completion bypassed external publication");
	Check(Name, state.Complete(call) == Result::Ok && state.InvocationCompleted() && state.GetPhase() == Phase::Completed,
	      "publish completion after return");
	order = 3; diagnostic = 123;
	Bridge::PreparedCall repeat;
	Check(Name, state.Prepare(repeat) == Result::AlreadyCompleted && call.Invoke() == Result::AlreadyInvoked &&
	            state.Complete(call) == Result::AlreadyCompleted && state.Bind(Owner, ControlledLeaf, true) == Result::AlreadyCompleted,
	      "repeat/rearm accepted");
	Check(Name, calls == 1 && diagnostic == 123 && order == 3, "repeat reset diagnostic sentinel");
	Check(Name, state.RecordStartupAdmission() == Result::Ok && state.StartupAdmitted(), "startup after bridge");
	Check(Name, state.InvalidateProvider(Owner + 1) == Result::UnrelatedProvider && state.GetPhase() == Phase::Completed,
	      "unrelated provider invalidated selection");
	Check(Name, state.InvalidateProvider(Owner) == Result::Ok && state.GetPhase() == Phase::Invalidated &&
	            state.Owner() == Owner && state.StartupAdmitted() && state.InvocationCompleted(), "unload erased history");
	Check(Name, state.Prepare(repeat) == Result::Invalidated && state.Bind(Owner, ControlledLeaf, true) == Result::Invalidated &&
	            state.Bind(Owner + 1, OtherLeaf, true) == Result::ReplacementRejected, "unload/reload rearmed callback");
	Check(Name, calls == 1 && diagnostic == 123, "unload/replacement invoked callback");
	active = nullptr; active_call = nullptr; reenter = false;
}

void AdmissionAndUnload() {
	Bridge::State early("1"); Bridge::PreparedCall early_call;
	Check(Name, early.RecordStartupAdmission() == Result::Ok, "record earlier initializer/entry");
	Check(Name, early.InvalidateProvider(Owner + 5) == Result::UnrelatedProvider && early.StartupAdmitted(),
	      "unrelated unload erased earlier admission history");
	Check(Name, early.Bind(Owner, ControlledLeaf, true) == Result::StartupAlreadyAdmitted &&
	            early.Prepare(early_call) == Result::StartupAlreadyAdmitted, "late opt-in accepted after earlier provider unloaded");
	Bridge::State bound("1"); Bridge::PreparedCall late;
	Check(Name, bound.Bind(Owner, ControlledLeaf, true) == Result::Ok, "bind before admission");
	Check(Name, bound.RecordStartupAdmission() == Result::Ok && bound.Prepare(late) == Result::StartupAlreadyAdmitted,
	      "admission between bind and prepare ignored");
	Bridge::State unloaded("1"); Bridge::PreparedCall stale;
	Check(Name, unloaded.Bind(Owner, ControlledLeaf, true) == Result::Ok && unloaded.InvalidateProvider(Owner) == Result::Ok,
	      "invalidate uncalled provider before unmap");
	Check(Name, unloaded.Prepare(stale) == Result::Invalidated && stale.Invoke() == Result::InvalidInvocation &&
	            unloaded.Bind(Owner, OtherLeaf, true) == Result::Invalidated &&
	            unloaded.Bind(Owner + 1, OtherLeaf, true) == Result::ReplacementRejected, "stale callback/replacement admitted");
	Check(Name, !unloaded.InvocationCompleted() && calls == 1 && diagnostic == 123, "invalidated provider called");
}

void RawExport() {
	char names[] = "h8jq9ee4h5c#C#A\0h8jq9ee4h5c#D#A";
	std::array<Loader::Elf64_Sym, 2> symbols {};
	auto& symbol = symbols[0];
	symbol.st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_FUNC;
	symbol.st_shndx = 3; symbol.st_value = Bridge::LeafRva; symbol.st_size = Bridge::LeafSize;
	symbols[1] = symbol;
	Loader::DynamicInfo info;
	info.str_table = names; info.str_table_size = sizeof(names);
	info.symbol_table = symbols.data(); info.symbol_table_entry_size = sizeof(symbol);
	info.symbol_table_total_size = sizeof(symbol);
	info.export_libs.push_back({"C", 1, "libSceLibcInternal"});
	info.export_modules.push_back({"A", 1, 1, "libSceLibcInternal"});
	Check(Name, Loader::TestNativeLibcRawExport(info), "unique exact raw export rejected");
	info.symbol_table_total_size = sizeof(symbols);
	Check(Name, !Loader::TestNativeLibcRawExport(info), "duplicate last-wins export accepted");
	symbols[1].st_name = sizeof("h8jq9ee4h5c#C#A");
	Check(Name, !Loader::TestNativeLibcRawExport(info), "duplicate NID with different raw library accepted");
	info.symbol_table_total_size = sizeof(symbol);
	info.export_libs[0].name = "LibcInternal";
	Check(Name, !Loader::TestNativeLibcRawExport(info), "normalized library alias accepted as raw identity");
	info.export_libs[0].name = "libSceLibcInternal";
	info.export_modules[0].version_minor = 2;
	Check(Name, !Loader::TestNativeLibcRawExport(info), "wrong module version accepted");
	info.export_modules[0].version_minor = 1;
	info.export_libs.push_back(info.export_libs[0]);
	Check(Name, !Loader::TestNativeLibcRawExport(info), "ambiguous raw library ID accepted");
	info.export_libs.pop_back();
	for (unsigned bad = 0; bad < 6; ++bad) {
		const auto saved = symbol;
		switch (bad) {
			case 0: symbol.st_value++; break;
			case 1: symbol.st_size--; break;
			case 2: symbol.st_info = (Loader::STB_WEAK << 4) | Loader::STT_FUNC; break;
			case 3: symbol.st_info = (Loader::STB_GLOBAL << 4) | Loader::STT_OBJECT; break;
			case 4: symbol.st_shndx = 0; break;
			case 5: symbol.st_name = sizeof(names); break;
		}
		Check(Name, !Loader::TestNativeLibcRawExport(info), "invalid raw definition accepted");
		symbol = saved;
	}
	info.str_table_size = 14; // Excludes the first terminating NUL.
	Check(Name, !Loader::TestNativeLibcRawExport(info), "unbounded symbol name accepted");
}

void SerializedCompetition() {
	Bridge::State state("1"); Bridge::PreparedCall call;
	Common::Mutex mutex;
	{
		Common::LockGuard lock(mutex);
		Check(Name, state.Bind(Owner, OtherLeaf, true) == Result::Ok && state.Prepare(call) == Result::Ok,
		      "prepare serialized competition");
	}
	bool blocked = false;
	std::thread contender([&] {
		Common::LockGuard lock(mutex);
		Bridge::PreparedCall competing;
		blocked = state.Prepare(competing) == Result::Invoking && state.RecordStartupAdmission() == Result::Invoking &&
		          state.InvalidateProvider(Owner) == Result::Invoking && !state.StartupAdmitted();
	});
	contender.join();
	Check(Name, blocked && call.Invoke() == Result::Ok, "competing start/unload crossed the prepared interval");
	{
		Common::LockGuard lock(mutex);
		Check(Name, state.Complete(call) == Result::Ok && state.RecordStartupAdmission() == Result::Ok,
		      "post-call admission not serialized");
	}
}

void FileIdentity() {
	auto bytes = AuthorityMetadataTest::Plain();
	std::fill(bytes.begin() + 0x78, bytes.begin() + 0xb8, 0);
	constexpr uint8_t digest[32] {
	    0x75, 0x50, 0x77, 0xdd, 0x60, 0xf1, 0x6c, 0xea, 0xa9, 0x91, 0x3e, 0xf3, 0xfc, 0x80, 0x03, 0x18,
	    0x0e, 0x5b, 0x9b, 0xa0, 0x30, 0xd0, 0x18, 0x77, 0xe3, 0xea, 0x4d, 0x61, 0x6c, 0x47, 0x1a, 0xf9,
	};
	const auto path = std::filesystem::temp_directory_path() /
	    ("kyty-libc-identity-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".elf");
	struct Remove { std::filesystem::path path; ~Remove() { std::error_code e; std::filesystem::remove(path, e); } } cleanup {path};
	{
		Common::File output;
		Check(Name, output.Create(path), "create hash fixture");
		uint32_t written = 0;
		output.Write(bytes.data(), static_cast<uint32_t>(bytes.size()), &written);
		Check(Name, written == bytes.size(), "write hash fixture");
	}
	Loader::Elf64 image;
	image.Open(path);
	Check(Name, image.IsValid() && image.MatchesFileSha256(digest, bytes.size()), "same-open-file SHA256 mismatch");
	Check(Name, !image.MatchesFileSha256(digest, bytes.size() - 1), "hash byte bound ignored");
	uint8_t wrong[32] {};
	Check(Name, !image.MatchesFileSha256(wrong, bytes.size()), "wrong file hash accepted");
	std::array<uint8_t, 16> segment {};
	uint32_t read = 0;
	image.LoadSegment(reinterpret_cast<uint64_t>(segment.data()), 0x200, segment.size(), &read);
	Check(Name, read == segment.size() && image.MatchesFileSha256(digest, bytes.size()), "hashing disturbed ELF backing reads");
}
} // namespace NativeLibcStartupTest

void TestNativeLibcStartup() {
	using namespace NativeLibcStartupTest;
	calls = 0; order = 0; diagnostic = 0; active = nullptr; active_call = nullptr; reenter = false;
	Profile();
	DisabledAndValidation();
	OnceAndReentry();
	AdmissionAndUnload();
	RawExport();
	SerializedCompetition();
	FileIdentity();
}
