// Production export integration: resource admission -> real direct allocation
// -> decoder creation -> actual synthetic AVC/HEVC frame -> destruction.
#include "fixtures/esvm-nal/packets.h"

namespace VideoDec2ResourceTests {
using namespace Libs::VideoDec2;
namespace Memory = Libs::LibKernel::Memory;
constexpr const char* Test = "VideoDec2Resources";

struct Api {
	Loader::SymbolDatabase symbols;
	using Query = KYTY_SYSV_ABI int (*)(const Videodec2DecoderConfigInfo*, Videodec2DecoderMemoryInfo*);
	using Create = KYTY_SYSV_ABI int (*)(const Videodec2DecoderConfigInfo*, const Videodec2DecoderMemoryInfo*, void**);
	using Unary = KYTY_SYSV_ABI int (*)(void*);
	using Decode = KYTY_SYSV_ABI int (*)(void*, const Videodec2InputData*, Videodec2FrameBuffer*, Videodec2OutputInfo*);
	using Flush = KYTY_SYSV_ABI int (*)(void*, Videodec2FrameBuffer*, Videodec2OutputInfo*);
	using Picture = KYTY_SYSV_ABI int (*)(const Videodec2OutputInfo*, void*, void*);
	using QueryCompute = KYTY_SYSV_ABI int (*)(Videodec2ComputeMemoryInfo*);
	using AllocateCompute = KYTY_SYSV_ABI int (*)(const Videodec2ComputeConfigInfo*, const Videodec2ComputeMemoryInfo*, void**);
	template<class F> F Function(const char* nid) {
		const auto* record = symbols.Find({nid, "Videodec2", 1, "Videodec2", 1, 1, Loader::SymbolType::Func});
		Check(Test, record != nullptr, "production Videodec2 export resolves");
		return reinterpret_cast<F>(record->vaddr);
	}
	Api() { InitVideoDec2_1(&symbols); }
};

struct DirectMemory {
	int64_t physical = -1;
	void* address = nullptr;
	size_t size;
	explicit DirectMemory(size_t requested): size((requested + 0x3fff) & ~size_t(0x3fff)) {
		Check(Test, size != 0, "query supplies a nonzero direct-memory requirement");
		CheckOk(Test, Memory::KernelAllocateMainDirectMemory(size, 0x4000, 12, &physical), "allocate vdec dmem");
		CheckOk(Test, Memory::KernelMapDirectMemory(&address, size, 0x32, 0, physical, 0x4000), "map vdec dmem");
	}
	~DirectMemory() {
		CheckOk(Test, Memory::KernelMunmap(reinterpret_cast<uint64_t>(address), size), "unmap vdec dmem");
		CheckOk(Test, Memory::KernelReleaseDirectMemory(physical, size), "release vdec dmem");
	}
};

void Run() {
	Api api;
	const auto query = api.Function<Api::Query>("qqMCwlULR+E");
	const auto create = api.Function<Api::Create>("CNNRoRYd8XI");
	const auto destroy = api.Function<Api::Unary>("jwImxXRGSKA");
	const auto decode = api.Function<Api::Decode>("852F5+q6+iM");
	const auto flush = api.Function<Api::Flush>("l1hXwscLuCY");
	const auto picture = api.Function<Api::Picture>("NtXRa3dRzU0");
	// These return before inspecting the deliberately unreadable descriptors.
	// Opt-in result tracing must not turn an ordinary API error into a host fault.
	Check(Test, query(reinterpret_cast<const Videodec2DecoderConfigInfo*>(1), nullptr) ==
	            static_cast<int>(0x811d0102u), "query trace preserves early pointer rejection");
	Check(Test, create(reinterpret_cast<const Videodec2DecoderConfigInfo*>(1), nullptr, nullptr) ==
	            static_cast<int>(0x811d0102u), "create trace preserves early pointer rejection");
	Check(Test, decode(nullptr, reinterpret_cast<const Videodec2InputData*>(1),
	                   reinterpret_cast<Videodec2FrameBuffer*>(1), reinterpret_cast<Videodec2OutputInfo*>(1)) ==
	            static_cast<int>(0x811d0103u), "decode trace does not read untouched output");
	Check(Test, flush(nullptr, reinterpret_cast<Videodec2FrameBuffer*>(1), reinterpret_cast<Videodec2OutputInfo*>(1)) ==
	            static_cast<int>(0x811d0103u), "flush trace does not read untouched output");
	// Exact AVC query config from Netflix helper RVA 0x1506d44.
	Videodec2DecoderConfigInfo config {sizeof(config), 0x12384, 1, 100, 40, -1, -1,
	                                 -1, 1, nullptr, 0, -1, true};
	Videodec2DecoderMemoryInfo requirements {sizeof(requirements)};
	CheckOk(Test, query(&config, &requirements), "private Vdec resource query with no compute queue");
	std::thread worker([&] {
		Videodec2DecoderMemoryInfo worker_memory {sizeof(worker_memory)};
		CheckOk(Test, query(&config, &worker_memory), "query on worker without PRINT_NAME initialization");
		Check(Test, worker_memory.gpu_memory_size == requirements.gpu_memory_size,
		      "worker result trace does not change requirements");
	});
	worker.join();
	int64_t invalid_physical = -1;
	Check(Test, Memory::KernelAllocateMainDirectMemory(0, 0x4000, 12, &invalid_physical) ==
	            Libs::LibKernel::KERNEL_ERROR_EINVAL && invalid_physical == -1,
	      "zero-length kernel allocation remains invalid");
	config.resource_type = 0x12385;
	Check(Test, query(&config, &requirements) == static_cast<int>(0x811d0203u), "unknown resource rejected");
	config.resource_type = 0x12384;
	config.decode_input_queue_depth = 0;
	Check(Test, query(&config, &requirements) == static_cast<int>(0x811d0206u), "private resource retains config validation");
	config.decode_input_queue_depth = 1;
	config.codec_type = 0;
	Check(Test, query(&config, &requirements) == static_cast<int>(0x811d0204u), "unknown codec is not made successful");
	config.codec_type = 1;

	for (const uint32_t resource: {0x12384u, 1u}) {
		for (const uint32_t codec: {1u, 0xee049u}) {
			config.resource_type = resource;
			config.codec_type = codec;
			config.profile = codec == 1 ? 100 : 2;
			config.max_level = codec == 1 ? 40 : 153;
			config.max_frame_width = codec == 1 ? -1 : 4096;
			config.max_frame_height = codec == 1 ? -1 : 2176;
			config.compute_queue = nullptr;
			CheckOk(Test, query(&config, &requirements), "query both decoder resource paths");
			DirectMemory gpu(requirements.gpu_memory_size), shared(requirements.cpu_gpu_memory_size);
			std::vector<uint8_t> cpu(requirements.cpu_memory_size), pixels(requirements.max_frame_buffer_size, 0);
			auto memory = requirements;
			memory.cpu_memory = cpu.data();
			memory.gpu_memory = gpu.address;
			memory.cpu_gpu_memory = shared.address;
			void* decoder = nullptr;
			Videodec2ComputeMemoryInfo compute {sizeof(compute)};
			std::vector<uint8_t> compute_storage;
			if (resource == 1) {
				Check(Test, create(&config, &memory, &decoder) == static_cast<int>(0x811d0110u) && !decoder,
				      "compute resource still requires a queue at creation");
				CheckOk(Test, api.Function<Api::QueryCompute>("RnDibcGCPKw")(&compute), "query compute memory");
				compute_storage.resize(compute.cpu_gpu_memory_size);
				compute.cpu_gpu_memory = compute_storage.data();
				Videodec2ComputeConfigInfo compute_config {sizeof(compute_config)};
				CheckOk(Test, api.Function<Api::AllocateCompute>("eD+X2SmxUt4")(&compute_config, &compute, &config.compute_queue),
				        "allocate caller-owned compute queue");
			}
			auto short_memory = memory;
			short_memory.gpu_memory_size = 0;
			Check(Test, create(&config, &short_memory, &decoder) == static_cast<int>(0x811d0104u) && !decoder,
			      "resource admission does not waive required memory");
			CheckOk(Test, create(&config, &memory, &decoder), "create real decoder with resource-specific queue ownership");
			Check(Test, decoder != nullptr, "real decoder instance returned");
			std::vector<uint8_t> access_unit;
			auto nal = [&](const auto& bytes) {
				access_unit.insert(access_unit.end(), {0, 0, 0, 1});
				access_unit.insert(access_unit.end(), bytes.begin(), bytes.end());
			};
			if (codec == 1) {
				nal(EsvmNalFixture::AvcSps); nal(EsvmNalFixture::AvcPps); nal(EsvmNalFixture::AvcIdr);
			} else {
				nal(EsvmNalFixture::HevcVps); nal(EsvmNalFixture::HevcSps);
				nal(EsvmNalFixture::HevcPps); nal(EsvmNalFixture::HevcIdr);
			}
			Videodec2InputData input {sizeof(input), access_unit.data(), access_unit.size(), 12345, 12000, 67890};
			Videodec2FrameBuffer frame {sizeof(frame), pixels.data(), pixels.size()};
			Videodec2OutputInfo output {sizeof(output)};
			// Private storage cannot be resubmitted after acceptance. Drain the
			// single AU; public caller-owned receive storage can be reused.
			bool accepted = false;
			for (int submitted = 0; submitted < 4 && !output.is_valid; ++submitted) {
				CheckOk(Test, decode(decoder, &input, &frame, &output), "decode synthetic red frame through export");
				accepted = accepted || frame.is_accepted;
				if (resource == 0x12384 && frame.is_accepted && !output.is_valid) {
					CheckOk(Test, flush(decoder, &frame, &output), "drain accepted private storage");
					break;
				}
			}
			Check(Test, output.is_valid && accepted && output.frame_width == 64 && output.frame_height == 48 &&
			            output.codec_type == codec && output.frame_buffer == pixels.data(), "actual 64x48 decoded output");
			const size_t uv = output.frame_pitch * output.frame_height;
			Check(Test, pixels[0] >= 75 && pixels[0] <= 90 && pixels[uv] >= 80 && pixels[uv] <= 100 && pixels[uv + 1] >= 230,
			      "decoded NV12 contains red pixels, not fabricated frame readiness");
			if (codec == 1) {
				Decoder::AvcPictureInfo info {};
				info.this_size = sizeof(info);
				CheckOk(Test, picture(&output, &info, nullptr), "read real decoded picture metadata");
				Check(Test, info.is_valid && info.pts_data == input.pts_data && info.attached_data == input.attached_data,
				      "picture metadata belongs to submitted AU");
			}
			CheckOk(Test, flush(decoder, &frame, &output), "flush output trace preserves real API result");
			CheckOk(Test, destroy(decoder), "destroy real decoder");
			Check(Test, destroy(decoder) == static_cast<int>(0x811d0103u), "destroy retires decoder handle");
			if (resource == 0x12384 && codec == 0xee049) {
				// This actual HEVC AU delays its first picture. Private storage is
				// owned at submission, then returned independently of flush's buffer.
				const auto reset = api.Function<Api::Unary>("wJXikG6QFN8");
				std::vector<uint8_t> scratch(pixels.size(), 0x5a);
				Videodec2FrameBuffer other {sizeof(other), scratch.data(), scratch.size()};
				CheckOk(Test, create(&config, &memory, &decoder), "create delayed ownership decoder");
				std::fill(pixels.begin(), pixels.end(), 0xa5);
				CheckOk(Test, decode(decoder, &input, &frame, &output), "submit first delayed HEVC AU");
				Check(Test, frame.is_accepted && !output.is_valid && output.picture_count == 0,
				      "private first-AU storage retained before picture availability");
				Check(Test, std::all_of(pixels.begin(), pixels.end(), [](auto v) { return v == 0xa5; }),
				      "acceptance alone does not fabricate pixels");
				CheckOk(Test, flush(decoder, &other, &output), "drain delayed first AU without a second AU");
				Check(Test, output.is_valid && output.frame_buffer == pixels.data() && !other.is_accepted,
				      "flush returns retained AU storage and does not accept scratch storage");
				Decoder::PictureInfo delayed_info {};
				Check(Test, Decoder::GetPictureInfo(output.frame_buffer, &delayed_info) &&
				            delayed_info.pts == input.pts_data && delayed_info.attached_data == input.attached_data,
				      "delayed output retains original AU metadata");
				const auto delayed_uv = output.frame_pitch * output.frame_height;
				Check(Test, pixels[0] >= 75 && pixels[0] <= 90 && pixels[delayed_uv + 1] >= 230 &&
				            std::all_of(scratch.begin(), scratch.end(), [](auto v) { return v == 0x5a; }),
				      "delayed pixels written to retained storage only");
				CheckOk(Test, flush(decoder, &other, &output), "drain ends without duplicate output");
				Check(Test, !output.is_valid && !other.is_accepted, "no duplicate picture or scratch ownership");
				CheckOk(Test, reset(decoder), "reset before delayed multi-AU sequence");
				std::array<std::vector<uint8_t>, 4> owned;
				std::array<unsigned, 4> seen {};
				for (auto& storage: owned) storage.assign(256 * 48 * 3 / 2, 0xa5);
				auto verify_owned = [&] {
					if (!output.is_valid) return;
					Decoder::PictureInfo info {};
					Check(Test, Decoder::GetPictureInfo(output.frame_buffer, &info), "returned picture has metadata");
					const auto index = info.attached_data - 900;
					Check(Test, index < owned.size(), "picture belongs to one submitted AU");
					if (index >= owned.size()) return;
					Check(Test, output.frame_buffer == owned[index].data() && info.pts == 1000 + index &&
					            ++seen[index] == 1 && owned[index][0] >= 75 && owned[index][0] <= 90,
					      "each delayed picture returns its own AU storage and timestamp exactly once");
				};
				for (size_t n = 0; n < owned.size(); ++n) {
					auto queued_input = input;
					queued_input.pts_data = 1000 + n;
					queued_input.attached_data = 900 + n;
					Videodec2FrameBuffer queued {sizeof(queued), owned[n].data(), owned[n].size()};
					CheckOk(Test, decode(decoder, &queued_input, &queued, &output), "submit distinct private storage");
					Check(Test, queued.is_accepted, "current storage ownership independent of older picture return");
					verify_owned();
				}
				for (size_t n = 0; n <= owned.size(); ++n) {
					CheckOk(Test, flush(decoder, &other, &output), "drain all submitted private pictures");
					if (!output.is_valid) break;
					verify_owned();
				}
				Check(Test, std::all_of(seen.begin(), seen.end(), [](auto n) { return n == 1; }),
				      "all four delayed AUs delivered without buffer substitution");
				CheckOk(Test, reset(decoder), "reset after private EOS");
				CheckOk(Test, decode(decoder, &input, &frame, &output), "retain a buffer before reset");
				Check(Test, frame.is_accepted && !output.is_valid, "second lifecycle delays first picture");
				CheckOk(Test, reset(decoder), "reset discards delayed AU and its storage token");
				std::fill(pixels.begin(), pixels.end(), 0xa5);
				input.pts_data += 100;
				CheckOk(Test, decode(decoder, &input, &other, &output), "submit fresh storage after reset");
				Check(Test, other.is_accepted && !output.is_valid, "fresh storage accepted independently");
				CheckOk(Test, flush(decoder, &frame, &output), "drain new lifecycle into its own storage");
				Check(Test, output.is_valid && output.frame_buffer == scratch.data() && !frame.is_accepted &&
				            std::all_of(pixels.begin(), pixels.end(), [](auto v) { return v == 0xa5; }),
				      "reset prevents delayed writes to retired storage");
				CheckOk(Test, reset(decoder), "reset before retained-buffer destruction");
				CheckOk(Test, decode(decoder, &input, &frame, &output), "retain storage before destroy");
				CheckOk(Test, destroy(decoder), "destroy drops retained storage without writes");
				Check(Test, std::all_of(pixels.begin(), pixels.end(), [](auto v) { return v == 0xa5; }),
				      "destroy does not write a delayed picture");
			}
			if (config.compute_queue) {
				CheckOk(Test, api.Function<Api::Unary>("UvtA3FAiF4Y")(config.compute_queue), "release caller-owned compute queue");
			}
		}
	}
	// Exercise the bounded error diagnostic without decoding or mutating state.
	for (int i = 0; i < 20; ++i) {
		Check(Test, query(nullptr, nullptr) == static_cast<int>(0x811d0102u),
		      "bounded tracing preserves repeated API errors");
	}
	std::puts("Videodec2 resource admission, direct memory, real AVC/HEVC pixels and lifecycle passed");
}
} // namespace VideoDec2ResourceTests
