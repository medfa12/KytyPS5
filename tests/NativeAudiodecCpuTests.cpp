// Offline execution of the actual dispatcher and DDP provider. No SDL/device.
// Prepare images with prepare_native_audiodec.py; pass directory and an E-AC-3
// fixture (independent 5.1 frames, 48 kHz). Third argument "stub" is the old
// unresolved-import negative control, which MUST fail the PCM contract.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#define ABI __attribute__((sysv_abi))
static void Check(bool ok, const char* why) {
	if (!ok) { std::fprintf(stderr, "NativeAudiodecCpuTests: %s\n", why); std::exit(1); }
}
#ifndef NATIVE_AUDIODEC_PRODUCTION
static uint64_t guard = 0x123456789abcdef0ull;
static uint32_t need_flag = 0;
static uintptr_t ddp_base = 0;
static LONG WINAPI Fault(EXCEPTION_POINTERS* info) {
	std::fprintf(stderr, "native exception=%08lx ddp_rva=%llx address=%llx\n",
	             info->ExceptionRecord->ExceptionCode,
	             static_cast<unsigned long long>(info->ContextRecord->Rip - ddp_base),
	             static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
	return EXCEPTION_EXECUTE_HANDLER;
}
static int ABI Ps4() { return 0; }
static int ABI Sdk(uint32_t* value) { *value = 0x09000000; return 0; }
static uint32_t ABI SdkValue() { return 0x09000000; }
static void ABI Unexpected() { Check(false, "unexpected native dependency executed"); }
static void* ABI Copy(void* d, const void* s, size_t n) { return std::memcpy(d, s, n); }
static void* ABI Move(void* d, const void* s, size_t n) { return std::memmove(d, s, n); }
static void* ABI Set(void* d, int c, size_t n) { return std::memset(d, c, n); }
static int ABI CopyS(void* d, size_t capacity, const void* s, size_t n) {
	if (n > capacity) return 34;
	std::memcpy(d, s, n); return 0;
}
static int ABI Compare(const void* a, const void* b, size_t n) { return std::memcmp(a, b, n); }
static char* ABI StringCopy(char* d, const char* s, size_t n) { return std::strncpy(d, s, n); }
static float ABI Frexp(float v, int* exponent) { return std::frexp(v, exponent); }
static int ABI Print(char* dst, const char* format, uintptr_t a, uintptr_t b, uintptr_t c, uintptr_t d) {
	const uintptr_t args[] = {a, b, c, d};
	unsigned arg = 0;
	char* start = dst;
	for (; *format; ++format) {
		if (*format != '%') { *dst++ = *format; continue; }
		++format;
		if (*format == 's') {
			Check(arg < 4, "too many native diagnostic arguments");
			const char* s = reinterpret_cast<const char*>(args[arg++]);
			while (*s) *dst++ = *s++;
		} else if (*format == 'd' || *format == 'u') {
			char fmt[] = {'%', *format, 0};
			Check(arg < 4, "too many native diagnostic arguments");
			dst += std::sprintf(dst, fmt, static_cast<int>(args[arg++]));
		} else if (*format == '%') { *dst++ = '%'; }
		else { Check(false, "unsupported native diagnostic format"); }
	}
	*dst = 0;
	return static_cast<int>(dst - start);
}

#endif
static std::vector<uint8_t> Read(const std::filesystem::path& path) {
	std::ifstream file(path, std::ios::binary);
	Check(bool(file), "fixture missing");
	return {std::istreambuf_iterator<char>(file), {}};
}
#ifndef NATIVE_AUDIODEC_PRODUCTION
struct Image {
	uint8_t* base;
	std::filesystem::path prefix;
	std::unordered_map<std::string, uintptr_t> exports;
	Image(const std::filesystem::path& directory, const char* module): prefix(directory / module) {
		const auto bytes = Read(prefix.string() + ".image");
		base = static_cast<uint8_t*>(VirtualAlloc(nullptr, bytes.size(), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		Check(base != nullptr, "image allocation failed");
		std::memcpy(base, bytes.data(), bytes.size());
		std::ifstream file(prefix.string() + ".exports");
		std::string name; size_t offset;
		while (file >> name >> offset) exports[name] = reinterpret_cast<uintptr_t>(base + offset);
	}
	~Image() { VirtualFree(base, 0, MEM_RELEASE); }
	void Relocate(const std::unordered_map<std::string, uintptr_t>& imports) {
		std::ifstream file(prefix.string() + ".relocs");
		size_t offset; unsigned type; int64_t addend; std::string name;
		while (file >> offset >> type >> addend >> name) {
			uintptr_t value;
			if (type == 8) value = reinterpret_cast<uintptr_t>(base) + addend;
			else {
				const auto it = imports.find(name);
				if (it == imports.end()) std::fprintf(stderr, "Missing test import: %s\n", name.c_str());
				Check(it != imports.end(), "unmapped native import");
				value = it->second + addend;
			}
			std::memcpy(base + offset, &value, sizeof(value));
		}
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
	}
};
#endif
struct Buffer { uint32_t size = 24; void* data = nullptr; uint32_t bytes = 0; };
struct Control { void* param; void* bsi; Buffer* au; Buffer* pcm; };
using Call = int (ABI *)(Control*, Buffer*, uint32_t);
static void Put32(std::vector<uint8_t>& v, size_t at, uint32_t value) { std::memcpy(v.data() + at, &value, 4); }

#ifdef NATIVE_AUDIODEC_PRODUCTION
static void RunDecoderContract(Call query, Call init, Call decode, const char* fixture) {
#else
int main(int argc, char** argv) {
	Check(argc >= 3, "expected image directory and encoded fixture");
	Image ddp(argv[1], "libSceAudiodecCpuDdp"), cpu(argv[1], "libSceAudiodecCpu");
	ddp_base = reinterpret_cast<uintptr_t>(ddp.base);
	SetUnhandledExceptionFilter(Fault);
	std::unordered_map<std::string, uintptr_t> imports {
		{"f7uOxY9mM1U", reinterpret_cast<uintptr_t>(&guard)},
		{"ZT4ODD2Ts9o", reinterpret_cast<uintptr_t>(&need_flag)},
		{"Yrwoq3bti3c", reinterpret_cast<uintptr_t>(&Ps4)},
		{"nQ5AHlyJghQ", reinterpret_cast<uintptr_t>(&Ps4)},
		{"GGeRJk1XdWc", reinterpret_cast<uintptr_t>(&Sdk)},
		{"WB66evu8bsU", reinterpret_cast<uintptr_t>(&SdkValue)},
		{"Ou3iL1abvng", reinterpret_cast<uintptr_t>(&Unexpected)},
		{"H2e8t5ScQGc", reinterpret_cast<uintptr_t>(&Unexpected)},
		{"8zTFvBIAIN8", reinterpret_cast<uintptr_t>(&Set)},
		{"Q3VBxCXhUHs", reinterpret_cast<uintptr_t>(&Copy)},
		{"NFLs+dRJGNg", reinterpret_cast<uintptr_t>(&CopyS)},
		{"+P6FRGH4LfA", reinterpret_cast<uintptr_t>(&Move)},
		{"5TjaJwkLWxE", reinterpret_cast<uintptr_t>(&Compare)},
		{"6sJWiWSRuqk", reinterpret_cast<uintptr_t>(&StringCopy)},
		{"aaDMGGkXFxo", reinterpret_cast<uintptr_t>(&Frexp)},
		{"tcVi5SivF7Q", reinterpret_cast<uintptr_t>(&Print)},
		{"xqlFMfkztKg", ddp.exports.at("xqlFMfkztKg")},
	};
	// Dispatcher tables for codecs not under test. Calling them would fault;
	// no successful decoder substitutes are supplied.
	for (auto name: {"ijtKzWkV5h0", "k0Gp2cXDeqU", "31BlqiSZ3fw", "Psa+mnQ+wRg",
	                 "Hnrc4jUGrL4", "nR0nX7p2jFo", "lYA31T9O1KU", "zB0C+z6bevw", "UcpWxSTVJos"}) imports[name] = 0;
	ddp.Relocate(imports); cpu.Relocate(imports);
	const auto query = reinterpret_cast<Call>(cpu.exports.at("ktD2w3D4G2U"));
	const auto init = reinterpret_cast<Call>(cpu.exports.at("hdFsxo3MFu8"));
	const auto decode = reinterpret_cast<Call>(cpu.exports.at("lSVTiWV5wLc"));
	const char* fixture = argv[2];
#endif
	std::vector<uint8_t> param(0x2d48), bsi(0xa6b4);
	Put32(param, 0, 0x2d48); Put32(bsi, 0, 0xa6b4);
	Put32(param, 4, 32); Put32(param, 8, 0x07010200); param[12] = 6; Put32(param, 16, 100);
	Put32(param, 0xf0, 2); Put32(param, 0xf4, 1); Put32(param, 0xf8, 1);
	const uint32_t channels[] = {0, 2, 1, 5, 3, 4, 6, 7};
	for (unsigned c = 0; c < 8; ++c) Put32(param, 0x10c + 4*c, channels[c]);
	Put32(param, 0x2d00, 1); Put32(param, 0x2d04, 1); Put32(param, 0x2d08, 1);
	Put32(param, 0x2d10, uint32_t(-1)); Put32(param, 0x2d1c, 6);
	Put32(param, 0x2d3c, 1); Put32(param, 0x2d40, 5);
	Buffer resource, au, pcm;
	Control control {param.data(), bsi.data(), &au, &pcm};
	Check(query(&control, &resource, 0x1007) == 0 && resource.bytes != 0, "native query failed");
	std::vector<uint8_t> work(resource.bytes + 32);
	resource.data = work.data();
	const int initialized = init(&control, &resource, 0x1007);
	if (initialized) std::fprintf(stderr, "init=%08x\n", unsigned(initialized));
	Check(initialized == 0, "native init failed");
	const auto encoded = Read(fixture);
	size_t position = 0; unsigned frames = 0; double energy = 0;
	uint64_t output_frames = 0;
	const double frequencies[] = {440, 550, 660, 80, 770, 880};
	double sine[6][6] {}, cosine[6][6] {};
	while (position + 6 <= encoded.size()) {
		Check(encoded[position] == 0x0b && encoded[position+1] == 0x77, "expected E-AC-3 sync");
		const size_t bytes = ((((encoded[position+2] & 7) << 8) | encoded[position+3]) + 1) * 2;
		Check(position + bytes <= encoded.size(), "truncated fixture frame");
		std::vector<float> output(40960 / 4 + 2, std::nanf(""));
		au.data = const_cast<uint8_t*>(encoded.data() + position); au.bytes = static_cast<uint32_t>(bytes);
		pcm.data = output.data() + 1; pcm.bytes = 40960;
#ifdef NATIVE_AUDIODEC_PRODUCTION
		const auto result = decode(&control, &resource, 0x1007);
#else
		const auto result = argc > 3 ? 0 : decode(&control, &resource, 0x1007);
#endif
		if (result) std::fprintf(stderr, "decode=%08x\n", unsigned(result));
		Check(result == 0, "native decode failed");
		std::printf("frame=%u consumed=%u produced=%u\n", frames, au.bytes, pcm.bytes);
		Check(pcm.bytes == 1536 * 6 * 4 || (frames == 0 && pcm.bytes == 0),
		      "decoder published capacity instead of actual PCM length");
		Check(std::isnan(output.front()) && std::isnan(output.back()), "decoder overwrote canary");
		for (size_t i = 0; i < pcm.bytes / 4; ++i) {
			const float sample = output[1+i];
			Check(std::isfinite(sample) && std::fabs(sample) < 0.5f, "invalid decoded sample range");
			energy += double(sample) * sample;
			for (unsigned tone = 0; tone < 6; ++tone) {
				const double phase = 6.283185307179586 * frequencies[tone] * (output_frames + i / 6) / 48000;
				sine[i % 6][tone] += sample * std::sin(phase);
				cosine[i % 6][tone] += sample * std::cos(phase);
			}
		}
		output_frames += pcm.bytes / 24;
		position += bytes; ++frames;
	}
	Check(frames >= 3 && energy > 1, "decoder did not produce real audio");
	for (unsigned channel = 0; channel < 6; ++channel) {
		const auto power = [&](unsigned tone) { return sine[channel][tone] * sine[channel][tone] + cosine[channel][tone] * cosine[channel][tone]; };
		for (unsigned tone = 0; tone < 6; ++tone)
			if (tone != channel) Check(power(channel) > 2 * power(tone), "decoded channel order/stride or tone content is wrong");
	}
	std::printf("NativeAudiodecCpuTests: %u frames decoded, finite nonzero 5.1 PCM; all cases passed\n", frames);
}
