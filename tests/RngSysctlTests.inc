// Parent includes libs/rngSysctl.h, libs/errno.h, loader/symbolDatabase.h,
// standard array/cstring/thread headers, and declares Libs::ShellHits::InitShellHits_1.
namespace RngSysctlTest {
namespace Rng = Libs::RngSysctl;
namespace Posix = Libs::Posix;
constexpr const char* Name = "RngSysctl";

struct Fixture {
	char order[32] {};
	size_t steps = 0, validated_output = 0, validated_input = 0, copied = 0;
	bool bad_read = false, bad_output = false, bad_input = false;
	bool bad_copy = false, bad_store = false, bad_entropy = false;
	void Step(char operation) noexcept { order[steps++] = operation; }
	static Fixture& Self(void* context) noexcept { return *static_cast<Fixture*>(context); }
	static bool Read(void* context, const size_t* source, size_t& value) noexcept {
		auto& f = Self(context); f.Step('r');
		if (f.bad_read) { return false; }
		std::memcpy(&value, source, sizeof(value)); return true;
	}
	static bool Validate(void* context, const void*, size_t size, bool write) noexcept {
		auto& f = Self(context); f.Step(write ? 'o' : 'i');
		(write ? f.validated_output : f.validated_input) = size;
		return !(write ? f.bad_output : f.bad_input);
	}
	static bool Copy(void* context, void* output, const void* input, size_t size) noexcept {
		auto& f = Self(context); f.Step('c');
		f.copied = f.bad_copy ? (std::min)(size, size_t {7}) : size;
		std::memcpy(output, input, f.copied); return !f.bad_copy;
	}
	static bool Store(void* context, size_t* output, size_t value) noexcept {
		auto& f = Self(context); f.Step('s');
		if (f.bad_store) { return false; }
		std::memcpy(output, &value, sizeof(value)); return true;
	}
	static bool Entropy(void* context, Rng::Bytes& bytes) noexcept {
		auto& f = Self(context); f.Step('e');
		for (size_t i = 0; i < bytes.size(); ++i) { bytes[i] = static_cast<uint8_t>(i + 1); }
		return !f.bad_entropy; // Controlled bytes exist only in this injected fixture.
	}
	Rng::Callbacks Callbacks() { return {this, Read, Validate, Copy, Store, Entropy}; }
	int Call(void* output, size_t* length, const void* input = nullptr, size_t input_size = 0) {
		*Posix::GetErrorAddr() = 0x1234;
		return Rng::Run(output, length, input, input_size, Callbacks());
	}
	void Expect(int result, int error, const char* sequence) {
		Check(Name, result == (error == 0 ? 0 : -1), "POSIX result");
		Check(Name, *Posix::GetErrorAddr() == (error == 0 ? 0x1234 : error), "errno status/preservation");
		Check(Name, std::strcmp(order, sequence) == 0, "read/validate/entropy/copy/store ordering");
	}
};

void Written(const std::array<uint8_t, 160>& output, size_t count) {
	for (size_t i = 0; i < output.size(); ++i) {
		const auto expected = i >= 8 && i < 8 + count ? static_cast<uint8_t>(i - 7) : uint8_t {0xa5};
		Check(Name, output[i] == expected, "exact output extent / canaries");
	}
}

void Core() {
	for (size_t capacity: {0u, 1u, 16u, 40u, 63u, 64u, 65u, 128u}) {
		Fixture f;
		std::array<uint8_t, 160> output; output.fill(0xa5);
		size_t length = capacity;
		const auto count = (std::min)(capacity, Rng::ResultSize);
		f.Expect(f.Call(output.data() + 8, &length, nullptr, SIZE_MAX), capacity < 64 ? 12 : 0,
		         capacity == 0 ? "roes" : "roecs");
		Check(Name, length == count && f.validated_output == capacity, "full advertised range / final length");
		Written(output, count);
	}
	for (size_t initial: {size_t {0}, size_t {32}, size_t {128}, SIZE_MAX}) {
		Fixture f; size_t length = initial;
		f.Expect(f.Call(nullptr, &length), 0, "res");
		Check(Name, length == 64, "size query must generate and report64");
	}
	{
		Fixture f; f.Expect(f.Call(nullptr, nullptr, nullptr, SIZE_MAX), 0, "e");
	}
	{
		Fixture f; uint8_t output = 0xa5;
		f.Expect(f.Call(&output, nullptr), 12, "oe");
		Check(Name, output == 0xa5, "null length implies zero capacity");
	}
	for (int failure = 0; failure < 6; ++failure) {
		Fixture f; std::array<uint8_t, 160> output; output.fill(0xa5); size_t length = 128;
		f.bad_read = failure == 0; f.bad_output = failure == 1; f.bad_input = failure == 2;
		f.bad_entropy = failure == 3; f.bad_copy = failure == 4; f.bad_store = failure == 5;
		const char* order[] = {"r", "ro", "roi", "roe", "roec", "roecs"};
		f.Expect(f.Call(output.data() + 8, &length, failure <= 2 ? output.data() : nullptr, 1),
		         failure == 3 ? 5 : 14, order[failure]);
		Check(Name, length == 128, "failure changed caller length");
		Written(output, failure == 4 ? 7 : failure == 5 ? 64 : 0);
	}
	for (size_t newlen: {0u, 1u}) {
		Fixture f; uint8_t input = 0; size_t length = 123;
		f.Expect(f.Call(nullptr, &length, &input, newlen), 1, "ri");
		Check(Name, length == 123 && f.validated_input == newlen, "write rejection must preserve length");
	}
	{
		Fixture f; std::array<uint8_t, 160> output; output.fill(0xa5); size_t length = 128;
		f.Expect(f.Call(output.data() + 8, &length, output.data(), 1), 1, "roi");
		Written(output, 0); Check(Name, length == 128, "read/write EPERM must precede entropy/output");
	}
	{
		Fixture f; size_t length = 128; f.bad_entropy = true;
		f.Expect(f.Call(nullptr, &length), 5, "re");
		Check(Name, length == 128, "failed query changed length");
	}
	{
		Fixture f; uint8_t output = 0xa5; size_t length = 0; f.bad_entropy = true;
		f.Expect(f.Call(&output, &length), 5, "roe"); // EIO precedes zero-capacity ENOMEM.
	}
	{
		Fixture f; std::array<uint8_t, 160> output; output.fill(0xa5); size_t length = 16; f.bad_store = true;
		f.Expect(f.Call(output.data() + 8, &length), 14, "roecs"); // Store fault overrides short-buffer ENOMEM.
		Written(output, 16); Check(Name, length == 16, "failed final store changed length");
	}
	{
		Fixture f; struct { uint64_t before = 0x1234; std::array<size_t, 8> data {}; uint64_t after = 0x5678; } alias;
		alias.data[0] = 64;
		f.Expect(f.Call(alias.data.data(), alias.data.data()), 0, "roecs");
		Check(Name, alias.data[0] == 64 && alias.before == 0x1234 && alias.after == 0x5678, "alias length stored last");
		const auto* bytes = reinterpret_cast<const uint8_t*>(alias.data.data());
		for (size_t i = 8; i < 64; ++i) { Check(Name, bytes[i] == i + 1, "alias output suffix"); }
	}
	{
		Fixture f;
		struct { uint64_t before = 1; Rng::Bytes data {}; uint64_t after = 2; } temporary;
		struct { uint64_t before = 3; std::array<uint8_t, 40> data {}; uint64_t after = 4; } seed;
		size_t length = 64;
		f.Expect(f.Call(temporary.data.data(), &length), 0, "roecs");
		std::memcpy(seed.data.data(), temporary.data.data(), seed.data.size());
		for (size_t i = 0; i < 40; ++i) { Check(Name, seed.data[i] == i + 1, "SSL seed must include bytes16..39"); }
		Check(Name, temporary.before == 1 && temporary.after == 2 && seed.before == 3 && seed.after == 4,
		      "SSL64-byte temporary /40-byte seed canaries");
	}
	for (size_t capacity: {Rng::MaxValidatedRange + 1, SIZE_MAX}) {
		Fixture f; uint8_t output = 0; size_t length = capacity;
		f.Expect(f.Call(&output, &length), 14, "r");
		Check(Name, length == capacity, "oversized host range changed length");
	}
	{
		Fixture f; size_t length = 64;
		f.Expect(f.Call(reinterpret_cast<void*>(UINTPTR_MAX - 31), &length), 14, "r");
	}
	{
		Fixture f; uint8_t input = 0;
		f.Expect(f.Call(nullptr, nullptr, &input, SIZE_MAX), 14, "");
	}
}

using Sysctl = int (KYTY_SYSV_ABI *)(const char*, void*, size_t*, void*, size_t);
Sysctl Resolve(Loader::SymbolDatabase& symbols) {
	Libs::ShellHits::InitShellHits_1(&symbols);
	const auto* exact = symbols.Find({"MhC53TKmjVA", "libkernel", 1, "libkernel", 1, 1, Loader::SymbolType::Func});
	const auto* fallback = symbols.FindByNid("MhC53TKmjVA", Loader::SymbolType::Func);
	Check(Name, exact != nullptr && fallback != nullptr && exact->vaddr != 0 && exact->vaddr == fallback->vaddr &&
	            exact->dbg_name == "Kernel::SysctlByName", "production exact/NID sysctl binding");
	return reinterpret_cast<Sysctl>(exact->vaddr);
}

void Production() {
	Loader::SymbolDatabase symbols;
	const auto sysctl = Resolve(symbols);
	constexpr const char* knob = "kern.rng_pseudo";
	for (size_t capacity: {0u, 1u, 16u, 40u, 63u, 64u, 65u, 128u}) {
		std::array<uint8_t, 160> output; output.fill(0xa5); size_t length = capacity;
		*Posix::GetErrorAddr() = 0x4567;
		Check(Name, sysctl(knob, output.data() + 8, &length, nullptr, 0) == (capacity < 64 ? -1 : 0), "host entropy path status");
		Check(Name, length == (std::min)(capacity, size_t {64}) && *Posix::GetErrorAddr() == (capacity < 64 ? 12 : 0x4567),
		      "host length/errno");
		for (size_t i = 0; i < output.size(); ++i) {
			if (i < 8 || i >= 8 + length) { Check(Name, output[i] == 0xa5, "host copy exceeded result extent"); }
		}
	}
	size_t query = 0;
	CheckOk(Name, sysctl(knob, nullptr, &query, nullptr, SIZE_MAX), "production size query");
	Check(Name, query == 64, "production query size");
	CheckOk(Name, sysctl(knob, nullptr, nullptr, nullptr, SIZE_MAX), "production both-null query");
	uint8_t output = 0xa5;
	Check(Name, sysctl(knob, &output, nullptr, nullptr, 0) == -1 && *Posix::GetErrorAddr() == 12 && output == 0xa5,
	      "production null length");
	Check(Name, sysctl(knob, nullptr, reinterpret_cast<size_t*>(1), nullptr, 0) == -1 && *Posix::GetErrorAddr() == 14,
	      "checked length adapter (native userspace fault caveat)");
	query = SIZE_MAX;
	Check(Name, sysctl(knob, &output, &query, nullptr, 0) == -1 && *Posix::GetErrorAddr() == 14 && query == SIZE_MAX,
	      "production bounded range policy");
	int* main_errno = Posix::GetErrorAddr(); *main_errno = 0x6789;
	bool independent = false;
	std::thread worker([&] {
		uint8_t input = 0;
		int* local_errno = Posix::GetErrorAddr(); *local_errno = 0x789a;
		const int result = sysctl(knob, nullptr, nullptr, &input, 0);
		independent = local_errno != main_errno && result == -1 && *local_errno == 1;
	});
	worker.join();
	Check(Name, independent && *main_errno == 0x6789, "guest errno must be thread-local");
#ifdef _WIN32
	struct Pages {
		uint8_t* data = nullptr; size_t page = 0;
		explicit Pages(size_t count = 3) {
			SYSTEM_INFO info {}; GetSystemInfo(&info); page = info.dwPageSize;
			data = static_cast<uint8_t*>(VirtualAlloc(nullptr, count * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
			Check(Name, data != nullptr, "host fault fixture allocation");
		}
		~Pages() { VirtualFree(data, 0, MEM_RELEASE); }
	} pages;
	auto* length = reinterpret_cast<size_t*>(pages.data);
	auto* tail = pages.data + 2 * pages.page - 64;
	DWORD previous = 0;
	Check(Name, VirtualProtect(pages.data + 2 * pages.page, pages.page, PAGE_NOACCESS, &previous) != 0, "output guard page");
	std::memset(tail, 0xa5, 64); *length = 128;
	Check(Name, sysctl(knob, tail, length, nullptr, 0) == -1 && *Posix::GetErrorAddr() == 14 && *length == 128,
	      "validate advertised128 bytes before copying64");
	for (size_t i = 0; i < 64; ++i) { Check(Name, tail[i] == 0xa5, "preflight fault copied output"); }
	*length = 64;
	CheckOk(Name, sysctl(knob, tail, length, nullptr, 0), "exact64 at guard boundary");
	*length = 128;
	Check(Name, VirtualProtect(pages.data, pages.page, PAGE_READONLY, &previous) != 0, "read-only length");
	std::array<uint8_t, 128> writable {};
	Check(Name, sysctl(knob, writable.data(), length, nullptr, 0) == -1 && *Posix::GetErrorAddr() == 14 && *length == 128,
	      "late read-only length fault");
	Pages fragmented(Rng::MaxValidationRegions + 1);
	for (size_t i = 1; i < Rng::MaxValidationRegions + 1; i += 2) {
		Check(Name, VirtualProtect(fragmented.data + i * fragmented.page, fragmented.page, PAGE_READONLY, &previous) != 0,
		      "fragmented validation fixture");
	}
	Check(Name, sysctl(knob, nullptr, nullptr, fragmented.data, fragmented.page * Rng::MaxValidationRegions) == -1 &&
	            *Posix::GetErrorAddr() == 1, "exact region budget must reach read-only knob rejection");
	Check(Name, sysctl(knob, nullptr, nullptr, fragmented.data, fragmented.page * (Rng::MaxValidationRegions + 1)) == -1 &&
	            *Posix::GetErrorAddr() == 14, "excess mapping traversal must fail within budget");
#endif
}
} // namespace RngSysctlTest

void TestRngSysctl() {
	RngSysctlTest::Core();
	RngSysctlTest::Production();
}
