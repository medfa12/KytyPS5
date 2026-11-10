// Parent supplies rngSysctl/errno/symbolDatabase headers and declarations for
// Libs::ShellHits::KernelBoot11::InitShellHitsKernelBoot11_1 and
// Libs::LibcInternal::InitLibcInternal_1.
namespace NumericSysctlTest {
namespace Rng = Libs::RngSysctl;
namespace Posix = Libs::Posix;
constexpr const char* Name = "NumericSysctl";
constexpr int Fault = 14, Invalid = 22, NoSys = 78, NotDir = 20, Permission = 1, NoDevice = 6;
constexpr int ErrnoSentinel = 0x3456;
constexpr uint32_t Known[] = {1, 37};

struct Fixture {
	char order[32] {};
	size_t steps = 0, mib_copied = 0, output_range = 0, input_range = 0, generated = 0, copied = 0;
	bool bad_mib = false, bad_length = false, bad_output = false, bad_input = false;
	bool bad_entropy = false, bad_copy = false, bad_store = false;
	void Step(char c) noexcept { order[steps++] = c; }
	static Fixture& Self(void* p) noexcept { return *static_cast<Fixture*>(p); }
	static bool Mib(void* p, const void* src, void* dst, size_t n) noexcept {
		auto& f = Self(p); f.Step('m'); if (f.bad_mib) { return false; }
		std::memcpy(dst, src, n); f.mib_copied = n; return true;
	}
	static bool Length(void* p, const size_t* src, size_t& n) noexcept {
		auto& f = Self(p); f.Step('r'); if (f.bad_length) { return false; }
		std::memcpy(&n, src, sizeof(n)); return true;
	}
	static bool Validate(void* p, const void*, size_t n, bool write) noexcept {
		auto& f = Self(p); f.Step(write ? 'o' : 'i');
		(write ? f.output_range : f.input_range) = n;
		return !(write ? f.bad_output : f.bad_input);
	}
	static bool Copy(void* p, void* dst, const void* src, size_t n) noexcept {
		auto& f = Self(p); f.Step('c'); f.copied = f.bad_copy ? (std::min)(n, size_t {7}) : n;
		std::memcpy(dst, src, f.copied); return !f.bad_copy;
	}
	static bool Store(void* p, size_t* dst, size_t n) noexcept {
		auto& f = Self(p); f.Step('s');
		if (f.bad_store) { return false; }
		std::memcpy(dst, &n, sizeof(n)); return true;
	}
	static bool Entropy(void* p, uint8_t* dst, size_t n) noexcept {
		auto& f = Self(p); f.Step('e'); f.generated = n;
		for (size_t i = 0; i < n; ++i) { dst[i] = static_cast<uint8_t>(i + 1); }
		return !f.bad_entropy;
	}
	Rng::NumericCallbacks Callbacks() { return {this, Mib, Length, Validate, Copy, Store, Entropy}; }
	int Call(const void* mib, uint32_t count, void* output, size_t* length, const void* input = nullptr, size_t in_size = 0) {
		*Posix::GetErrorAddr() = ErrnoSentinel;
		return Rng::NumericRun(mib, count, output, length, input, in_size, Callbacks());
	}
	void Expect(int result, int expected, const char* sequence, const char* label) {
		Check(Name, result == expected && *Posix::GetErrorAddr() == ErrnoSentinel &&
		                std::strcmp(order, sequence) == 0, label);
	}
};

void Written(const std::array<uint8_t, 1040>& bytes, size_t count) {
	for (size_t i = 0; i < bytes.size(); ++i) {
		const uint8_t expected = i >= 8 && i < 8 + count ? static_cast<uint8_t>(i - 7) : uint8_t {0xa5};
		Check(Name, bytes[i] == expected, "synthetic output extent and canaries");
	}
}

void Core() {
	for (size_t capacity : {size_t {1}, size_t {63}, size_t {64}, size_t {255}, size_t {256}, size_t {257}, size_t {1024}}) {
		Fixture f; std::array<uint8_t, 1040> output; output.fill(0xa5); size_t length = capacity;
		const auto count = (std::min)(capacity, size_t {256});
		f.Expect(f.Call(Known, 2, output.data() + 8, &length), 0, "mroecs", "arandom capacity/order");
		Check(Name, f.output_range == capacity && f.generated == count && f.copied == count && length == count,
		      "full advertised validation and bounded count");
		Written(output, count);
	}
	{
		Fixture f; size_t length = 0; uint8_t output = 0xa5;
		f.Expect(f.Call(Known, 2, &output, &length), NoDevice, "mro", "zero capacity");
		Check(Name, output == 0xa5 && length == 0, "zero capacity mutated output");
	}
	{
		Fixture f; uint8_t output = 0xa5;
		f.Expect(f.Call(Known, 2, &output, nullptr), NoDevice, "mo", "null length implies zero");
		Check(Name, output == 0xa5, "null length output changed");
	}
	{
		Fixture f; f.Expect(f.Call(Known, 2, nullptr, nullptr), NoDevice, "m", "both outputs null");
	}
	{
		Fixture f; size_t length = 0;
		f.Expect(f.Call(Known, 2, nullptr, &length), NoDevice, "mr", "null output and zero capacity");
		Check(Name, length == 0, "zero-capacity query stored length");
	}
	for (size_t initial : {size_t {1}, size_t {64}, size_t {257}}) {
		Fixture f; size_t length = initial;
		f.Expect(f.Call(Known, 2, nullptr, &length), 0, "mres", "null output count");
		Check(Name, length == (std::min)(initial, size_t {256}) && f.generated == length,
		      "null output did not return requested bounded count");
	}
	for (uint32_t n : {uint32_t {0}, uint32_t {1}, uint32_t {25}, UINT32_MAX}) {
		Fixture f; f.Expect(f.Call(nullptr, n, nullptr, nullptr), Invalid, "", "MIB count must fail before copy");
	}
	{
		Fixture f; f.Expect(f.Call(nullptr, 2, nullptr, nullptr), Fault, "", "null MIB");
	}
	{
		Fixture f; f.bad_mib = true; f.bad_length = true;
		f.Expect(f.Call(reinterpret_cast<void*>(1), 2, nullptr, reinterpret_cast<size_t*>(1)), Fault, "m", "MIB copy precedes length");
	}
	const uint32_t unknown[] = {1, 99};
	const uint32_t extra[] = {1, 37, 1};
	{
		Fixture f; std::array<uint32_t, 24> longest {}; longest[0] = 1; longest[1] = 37;
		size_t length = 64;
		f.Expect(f.Call(longest.data(), 24, nullptr, &length), NotDir, "mr", "maximum valid MIB length");
		Check(Name, f.mib_copied == 96 && length == 64, "maximum MIB must copy all96 bytes before dispatch");
	}
	{
		Fixture f; size_t length = 64; uint8_t input = 0;
		f.Expect(f.Call(extra, 3, nullptr, &length, &input, 1), NotDir, "mri", "trailing component precedes write permission");
		Check(Name, length == 64, "trailing component changed length");
	}
	{
		Fixture f; std::array<uint8_t, 1040> output; output.fill(0xa5); size_t length = 64;
		f.Expect(f.Call(unknown, 2, output.data() + 8, &length), NoSys, "mro", "unsupported valid MIB");
		Written(output, 0); Check(Name, length == 64, "unsupported MIB changed length");
	}
	{
		Fixture f; size_t length = 64; uint8_t output[64] {};
		f.Expect(f.Call(extra, 3, output, &length), NotDir, "mro", "extra element after known leaf");
	}
	for (size_t n : {size_t {0}, size_t {1}}) {
		Fixture f; uint8_t input = 0; size_t length = 64; uint8_t output[64] {};
		f.Expect(f.Call(Known, 2, output, &length, &input, n), Permission, "mroi", "new input permission after validation");
		Check(Name, f.input_range == n && length == 64, "permission rejection changed length");
	}
	for (int failure = 0; failure < 6; ++failure) {
		Fixture f; std::array<uint8_t, 1040> output; output.fill(0xa5); size_t length = 64; uint8_t input = 0;
		f.bad_length = failure == 0; f.bad_output = failure == 1; f.bad_input = failure == 2;
		f.bad_entropy = failure == 3; f.bad_copy = failure == 4; f.bad_store = failure == 5;
		const char* order[] = {"mr", "mro", "mroi", "mroe", "mroec", "mroecs"};
		f.Expect(f.Call(Known, 2, output.data() + 8, &length, failure == 2 ? &input : nullptr, 1),
		         failure == 3 ? NoDevice : Fault, order[failure], "injected fault ordering");
		Check(Name, length == 64, "failure stored length"); Written(output, failure == 4 ? 7 : failure == 5 ? 64 : 0);
	}
	{
		Fixture f; size_t length = 64; uint8_t input = 0; f.bad_input = true;
		f.Expect(f.Call(unknown, 2, nullptr, &length, &input, 1), Fault, "mri", "bad input precedes unsupported dispatch");
	}
	{
		Fixture f; f.bad_length = true;
		f.Expect(f.Call(unknown, 2, nullptr, reinterpret_cast<size_t*>(1)), Fault, "mr", "bad length precedes unsupported dispatch");
	}
	{
		Fixture f; size_t length = 64; uint8_t input = 0;
		f.Expect(f.Call(unknown, 2, nullptr, &length, &input, 1), NoSys, "mri", "unsupported dispatch precedes permission");
	}
	{
		Fixture f; std::array<size_t, 8> storage {};
		auto* length = storage.data(); *length = 64;
		f.Expect(f.Call(Known, 2, storage.data(), length), 0, "mroecs", "aliased length stored last");
		Check(Name, *length == 64, "aliased length not stored last");
		const auto* bytes = reinterpret_cast<const uint8_t*>(storage.data());
		for (size_t i = sizeof(size_t); i < sizeof(storage); ++i) { Check(Name, bytes[i] == static_cast<uint8_t>(i + 1), "alias output suffix"); }
	}
}

void Bindings() {
	using Function = int (KYTY_SYSV_ABI *)(const void*, uint32_t, void*, size_t*, const void*, size_t);
	for (bool reverse : {false, true}) {
		Loader::SymbolDatabase db;
		if (reverse) { Libs::LibcInternal::InitLibcInternal_1(&db); Libs::ShellHits::KernelBoot11::InitShellHitsKernelBoot11_1(&db); }
		else { Libs::ShellHits::KernelBoot11::InitShellHitsKernelBoot11_1(&db); Libs::LibcInternal::InitLibcInternal_1(&db); }
		const auto* kernel = db.Find({"DFmMT80xcNI", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
		const auto* libc = db.Find({"DFmMT80xcNI", "LibcInternal", 1, "LibcInternal", 1, 1, Loader::SymbolType::Func});
		const auto* fallback = db.FindByNid("DFmMT80xcNI", Loader::SymbolType::Func);
		const auto expected = reinterpret_cast<uintptr_t>(&Rng::NumericSysctl);
		Check(Name, kernel && libc && fallback && kernel->vaddr == expected && libc->vaddr == expected && fallback->vaddr == expected &&
		                kernel->dbg_name == "RngSysctl::NumericSysctl" && libc->dbg_name == "RngSysctl::NumericSysctl" &&
		                fallback->dbg_name == "RngSysctl::NumericSysctl",
		      "both providers and NID fallback must bind numeric wrapper in either order");
		auto function = reinterpret_cast<Function>(fallback->vaddr);
		uint8_t output[64] {}; size_t length = sizeof(output); *Posix::GetErrorAddr() = ErrnoSentinel;
		Check(Name, function(Known, 2, output, &length, nullptr, 0) == 0 && length == 64 &&
		                *Posix::GetErrorAddr() == ErrnoSentinel, "resolved SYSV numeric wrapper call");
	}
}

void Production() {
	for (size_t capacity : {size_t {1}, size_t {63}, size_t {64}, size_t {255}, size_t {256}, size_t {257}, size_t {1024}}) {
		std::array<uint8_t, 1040> output; output.fill(0xa5); size_t length = capacity;
		*Posix::GetErrorAddr() = ErrnoSentinel;
		Check(Name, Rng::NumericSysctlRaw(Known, 2, output.data() + 8, &length, nullptr, 0) == 0 &&
		                length == (std::min)(capacity, size_t {256}) && *Posix::GetErrorAddr() == ErrnoSentinel,
		      "real entropy raw count and errno");
		for (size_t i = 0; i < output.size(); ++i) {
			if (i < 8 || i >= 8 + length) { Check(Name, output[i] == 0xa5, "real entropy output overrun"); }
		}
	}
	size_t length = 257; *Posix::GetErrorAddr() = ErrnoSentinel;
	Check(Name, Rng::NumericSysctlRaw(Known, 2, nullptr, &length, nullptr, 0) == 0 && length == 256 &&
	                *Posix::GetErrorAddr() == ErrnoSentinel, "raw null output");
	Check(Name, Rng::NumericSysctlRaw(Known, 2, nullptr, nullptr, nullptr, 0) == NoDevice &&
	                *Posix::GetErrorAddr() == ErrnoSentinel, "raw null length errno preservation");
	length = 64; std::array<uint8_t, 64> output {};
	Check(Name, Rng::NumericSysctl(Known, 2, output.data(), &length, nullptr, 0) == 0 &&
	                length == 64 && *Posix::GetErrorAddr() == ErrnoSentinel, "POSIX success preserves errno");
	Check(Name, Rng::NumericSysctl(nullptr, 2, output.data(), &length, nullptr, 0) == -1 &&
	                *Posix::GetErrorAddr() == Fault, "POSIX failure sets errno");
	*Posix::GetErrorAddr() = ErrnoSentinel;
	Check(Name, Rng::NumericSysctlRaw(reinterpret_cast<void*>(1), 2, output.data(), &length, nullptr, 0) == Fault &&
	                *Posix::GetErrorAddr() == ErrnoSentinel, "raw unmapped MIB preserves errno");
#ifdef _WIN32
	struct Pages {
		uint8_t* data = nullptr; size_t page = 0;
		Pages() { SYSTEM_INFO info {}; GetSystemInfo(&info); page = info.dwPageSize;
			data = static_cast<uint8_t*>(VirtualAlloc(nullptr, 3 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)); }
		~Pages() { if (data) { VirtualFree(data, 0, MEM_RELEASE); } }
	} pages;
	Check(Name, pages.data != nullptr, "host fault fixture allocation");
	DWORD prior = 0;
	Check(Name, VirtualProtect(pages.data + 2 * pages.page, pages.page, PAGE_NOACCESS, &prior) != 0, "guard page setup");
	std::array<uint32_t, 24> longest {}; longest[0] = 1; longest[1] = 37;
	auto* full_mib = pages.data + 2 * pages.page - sizeof(longest);
	std::memcpy(full_mib, longest.data(), sizeof(longest)); length = 64;
	Check(Name, Rng::NumericSysctlRaw(full_mib, 24, output.data(), &length, nullptr, 0) == NotDir && length == 64,
	      "maximum96-byte MIB at guard boundary must reach leaf dispatch");
	Check(Name, Rng::NumericSysctlRaw(full_mib + 4, 24, output.data(), &length, nullptr, 0) == Fault && length == 64,
	      "maximum MIB crossing guard must fail copy before dispatch");
	auto* cross_mib = pages.data + 2 * pages.page - 4;
	std::memcpy(cross_mib, Known, 4); length = 64;
	Check(Name, Rng::NumericSysctlRaw(cross_mib, 2, output.data(), &length, nullptr, 0) == Fault && length == 64,
	      "cross-guard MIB rejected before output");
	auto* tail = pages.data + 2 * pages.page - 64; std::memset(tail, 0xa5, 64);
	length = 128;
	Check(Name, Rng::NumericSysctlRaw(Known, 2, tail, &length, nullptr, 0) == Fault && length == 128 &&
	                std::all_of(tail, tail + 64, [](uint8_t x) { return x == 0xa5; }), "advertised output crosses guard");
	auto* readonly_length = reinterpret_cast<size_t*>(pages.data); *readonly_length = 64;
	Check(Name, VirtualProtect(pages.data, pages.page, PAGE_READONLY, &prior) != 0, "read-only length setup");
	output.fill(0xa5);
	Check(Name, Rng::NumericSysctlRaw(Known, 2, output.data(), readonly_length, nullptr, 0) == Fault &&
	                *readonly_length == 64, "late read-only length store fault");
#endif
}
} // namespace NumericSysctlTest

void TestNumericSysctl() {
	NumericSysctlTest::Core();
	NumericSysctlTest::Production();
	NumericSysctlTest::Bindings();
	std::printf("[host]    %-48s ok\n", NumericSysctlTest::Name);
}
