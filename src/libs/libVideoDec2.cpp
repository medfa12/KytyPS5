#include "common/abi.h"
#include "libs/errno.h"
#include "libs/libs.h"
#include "libs/videoDec2Decoder.h"
#include "libs/videoDec2Types.h"
#include "loader/symbolDatabase.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace Libs {

LIB_VERSION("Videodec2", 1, "Videodec2", 1, 1);

namespace VideoDec2 {

constexpr int32_t VIDEODEC2_ERROR_API_FAIL             = -2128805632; // 0x811d0100
constexpr int32_t VIDEODEC2_ERROR_STRUCT_SIZE          = -2128805631; // 0x811d0101
constexpr int32_t VIDEODEC2_ERROR_ARGUMENT_POINTER     = -2128805630; // 0x811d0102
constexpr int32_t VIDEODEC2_ERROR_DECODER_INSTANCE     = -2128805629; // 0x811d0103
constexpr int32_t VIDEODEC2_ERROR_MEMORY_SIZE          = -2128805628; // 0x811d0104
constexpr int32_t VIDEODEC2_ERROR_MEMORY_POINTER       = -2128805627; // 0x811d0105
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_SIZE    = -2128805626; // 0x811d0106
constexpr int32_t VIDEODEC2_ERROR_FRAME_BUFFER_POINTER = -2128805625; // 0x811d0107
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_SIZE     = -2128805619; // 0x811d010d
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT_POINTER  = -2128805618; // 0x811d010e
constexpr int32_t VIDEODEC2_ERROR_OUTPUT_INFO          = -2128805617; // 0x811d010f
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE        = -2128805616; // 0x811d0110
constexpr int32_t VIDEODEC2_ERROR_CONFIG_INFO          = -2128805376; // 0x811d0200
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_PIPE_ID      = -2128805375; // 0x811d0201
constexpr int32_t VIDEODEC2_ERROR_COMPUTE_QUEUE_ID     = -2128805374; // 0x811d0202
constexpr int32_t VIDEODEC2_ERROR_RESOURCE_TYPE        = -2128805373; // 0x811d0203
constexpr int32_t VIDEODEC2_ERROR_CODEC_TYPE           = -2128805372; // 0x811d0204
constexpr int32_t VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH    = -2128805370; // 0x811d0206
constexpr int32_t VIDEODEC2_ERROR_DPB_FRAME_COUNT      = -2128805367; // 0x811d0209
constexpr int32_t VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT   = -2128805366; // 0x811d020a
constexpr int32_t VIDEODEC2_ERROR_ACCESS_UNIT          = -2128805119; // 0x811d0301
constexpr int32_t VIDEODEC2_ERROR_OVERSIZE_DECODE      = -2128805118; // 0x811d0302

constexpr uint32_t VIDEODEC2_RESOURCE_TYPE_COMPUTE = 1;
// Regular 13.60 translates this private resource to VdecCore mode 8.
// Unlike COMPUTE it does not require a caller-owned compute queue.
constexpr uint32_t VIDEODEC2_RESOURCE_TYPE_VDEC = 0x12384;
constexpr size_t   VIDEODEC2_MIN_MEMORY_SIZE       = 16ull * 1024ull * 1024ull;
constexpr uint32_t VIDEODEC2_FRAME_FORMAT_DEFAULT  = 0;

using Videodec2AvcPictureInfo = Decoder::AvcPictureInfo;

using DecoderState = Decoder::Instance;

enum class TraceOperation : size_t { Query, Create, Decode, Flush };
static constexpr const char* TraceOperationNames[] = {"query", "create", "decode", "flush"};

static bool TakeTraceRecord(TraceOperation operation, int32_t result) {
	// Independent of PRINT_NAME's thread-local initialization: apps call these
	// exports from decoder workers, not necessarily the library-registration thread.
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_MEDIA_TRACE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	if (!enabled) { return false; }
	static std::atomic_uint counts[4][2] {};
	const bool error = result != OK;
	const unsigned limit = error ? 16u : operation >= TraceOperation::Decode ? 64u : 32u;
	const auto count = counts[static_cast<size_t>(operation)][error].fetch_add(1, std::memory_order_relaxed);
	if (count == limit) {
		LOGF("Videodec2 result: operation=%s trace-cap=%u errors=%u\n",
		     TraceOperationNames[static_cast<size_t>(operation)], limit, unsigned(error));
	}
	return count < limit;
}

static void TraceMemoryResult(TraceOperation operation, int32_t result,
                             const Videodec2DecoderConfigInfo* config,
                             const Videodec2DecoderMemoryInfo* memory, void* decoder) {
	if (!TakeTraceRecord(operation, result)) { return; }
	// Those errors return before all descriptor sizes have been checked. Do not
	// dereference a pointer the API itself deliberately never inspected.
	const bool config_known = result != VIDEODEC2_ERROR_ARGUMENT_POINTER && result != VIDEODEC2_ERROR_STRUCT_SIZE;
	const bool memory_known = config_known && (operation == TraceOperation::Create || result == OK);
	LOGF("Videodec2 result: operation=%s result=%08x config=%u resource=%08x codec=%u "
	     "profile=%u level=%u width=%d height=%d depth=%u memory=%s cpu=%llu gpu=%llu shared=%llu "
	     "frame=%llu alignment=%u decoder=%p\n",
	     TraceOperationNames[static_cast<size_t>(operation)], unsigned(result), unsigned(config_known),
	     config_known ? config->resource_type : 0, config_known ? config->codec_type : 0,
	     config_known ? config->profile : 0, config_known ? config->max_level : 0,
	     config_known ? config->max_frame_width : 0, config_known ? config->max_frame_height : 0,
	     config_known ? config->decode_input_queue_depth : 0,
	     !memory_known ? "unobserved" : operation == TraceOperation::Query ? "returned" : "supplied",
	     static_cast<unsigned long long>(memory_known ? memory->cpu_memory_size : 0),
	     static_cast<unsigned long long>(memory_known ? memory->gpu_memory_size : 0),
	     static_cast<unsigned long long>(memory_known ? memory->cpu_gpu_memory_size : 0),
	     static_cast<unsigned long long>(memory_known ? memory->max_frame_buffer_size : 0),
	     memory_known ? memory->frame_buffer_alignment : 0, decoder);
}

static void TraceFrameResult(TraceOperation operation, int32_t result, void* decoder,
                            const Videodec2InputData* input, const Videodec2FrameBuffer* buffer,
                            const Videodec2OutputInfo* output, bool observed) {
	if (!TakeTraceRecord(operation, result)) { return; }
	// observed is set only after the real API initializes both output descriptors.
	// In particular, a backend buffer-size error is distinct from prevalidation.
	LOGF("Videodec2 result: operation=%s result=%08x decoder=%p output-observed=%u "
	     "au-bytes=%llu buffer-accepted=%d picture-valid=%d pictures=%u error-frame=%d "
	     "codec=%u width=%u height=%u pitch=%u buffer-bytes=%llu\n",
	     TraceOperationNames[static_cast<size_t>(operation)], unsigned(result), decoder, unsigned(observed),
	     static_cast<unsigned long long>(observed && input != nullptr ? input->au_size : 0),
	     observed ? int(buffer->is_accepted) : -1, observed ? int(output->is_valid) : -1,
	     observed ? unsigned(output->picture_count) : 0, observed ? int(output->is_error_frame) : -1,
	     observed ? output->codec_type : 0, observed ? output->frame_width : 0,
	     observed ? output->frame_height : 0, observed ? output->frame_pitch : 0,
	     static_cast<unsigned long long>(observed ? buffer->frame_buffer_size : 0));
}

static_assert(sizeof(Videodec2ComputeMemoryInfo) == 24);
static_assert(sizeof(Videodec2ComputeConfigInfo) == 16);
static_assert(sizeof(Videodec2DecoderConfigInfo) == 72);
static_assert(sizeof(Videodec2DecoderMemoryInfo) == 72);
static_assert(sizeof(Videodec2InputData) == 48);
static_assert(sizeof(Videodec2OutputInfo) == 56);
static_assert(sizeof(Videodec2FrameBuffer) == 32);
static_assert(sizeof(Videodec2AvcPictureInfo) == 120);

static std::mutex                g_decoder_mutex;
static std::unordered_set<void*> g_decoders;

static bool IsOutputInfoSizeValid(size_t this_size) {
	return this_size == sizeof(Videodec2OutputInfo) ||
	       (this_size | 8u) == sizeof(Videodec2OutputInfo);
}

static DecoderState* GetDecoder(Videodec2Decoder decoder) {
	std::scoped_lock lock(g_decoder_mutex);
	return g_decoders.contains(decoder) ? static_cast<DecoderState*>(decoder) : nullptr;
}

static void FillNoPictureOutput(const Videodec2FrameBuffer* frame_buffer,
                                Videodec2OutputInfo* output_info, uint32_t codec_type) {
	output_info->is_valid           = false;
	output_info->is_error_frame     = false;
	output_info->picture_count      = 0;
	output_info->is_discarded_frame = false;
	output_info->codec_type         = codec_type;
	output_info->frame_width        = 0;
	output_info->frame_pitch        = 0;
	output_info->frame_height       = 0;
	output_info->frame_buffer      = frame_buffer != nullptr ? frame_buffer->frame_buffer : nullptr;
	output_info->frame_buffer_size = frame_buffer != nullptr ? frame_buffer->frame_buffer_size : 0;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = 0;
	}
}

static int32_t MapDecoderResult(Decoder::Result result) {
	switch (result) {
		case Decoder::Result::Ok: return OK;
		case Decoder::Result::InputFull:
		case Decoder::Result::NoFrame:
		case Decoder::Result::ApiFail: return VIDEODEC2_ERROR_API_FAIL;
		case Decoder::Result::AccessUnit: return VIDEODEC2_ERROR_ACCESS_UNIT;
		case Decoder::Result::FrameBufferSize: return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
		case Decoder::Result::OversizeDecode: return VIDEODEC2_ERROR_OVERSIZE_DECODE;
	}
	return VIDEODEC2_ERROR_API_FAIL;
}

static void ApplyDecodedOutput(const Decoder::Output& decoded, Videodec2FrameBuffer* frame_buffer,
                               Videodec2OutputInfo* output_info) {
	frame_buffer->is_accepted = decoded.buffer_accepted;
	if (!decoded.valid) {
		return;
	}
	output_info->is_valid          = true;
	output_info->is_error_frame    = decoded.error_frame;
	output_info->picture_count     = 1;
	output_info->codec_type        = decoded.codec_type;
	output_info->frame_width       = decoded.width;
	output_info->frame_pitch       = decoded.pitch;
	output_info->frame_height      = decoded.height;
	output_info->frame_buffer      = decoded.buffer;
	output_info->frame_buffer_size = decoded.buffer_size;
	if (output_info->this_size == sizeof(Videodec2OutputInfo)) {
		output_info->frame_format         = VIDEODEC2_FRAME_FORMAT_DEFAULT;
		output_info->frame_pitch_in_bytes = decoded.pitch;
	}
}

static int32_t ValidateDecoderConfig(const Videodec2DecoderConfigInfo* config,
                                     bool                              require_compute_queue) {
	if (config->resource_type != VIDEODEC2_RESOURCE_TYPE_COMPUTE &&
	    config->resource_type != VIDEODEC2_RESOURCE_TYPE_VDEC) {
		return VIDEODEC2_ERROR_RESOURCE_TYPE;
	}

	if (!Decoder::IsCodecSupported(config->codec_type)) {
		return VIDEODEC2_ERROR_CODEC_TYPE;
	}

	if (config->reserved0 != 0 || config->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (config->decode_input_queue_depth == 0) {
		return VIDEODEC2_ERROR_INPUT_QUEUE_DEPTH;
	}

	if (config->max_dpb_frame_count < -1 || config->max_dpb_frame_count == 0) {
		return VIDEODEC2_ERROR_DPB_FRAME_COUNT;
	}

	if (config->max_frame_width < -1 || config->max_frame_height < -1 ||
	    config->max_frame_width == 0 || config->max_frame_height == 0) {
		return VIDEODEC2_ERROR_FRAME_WIDTH_HEIGHT;
	}

	// Query and creation share resource admission; only the COMPUTE resource
	// owns a compute queue (native creation dispatch at 0xf95 / 0x10f4).
	if (require_compute_queue && config->resource_type == VIDEODEC2_RESOURCE_TYPE_COMPUTE &&
	    config->compute_queue == nullptr) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE;
	}

	return OK;
}

static int32_t KYTY_SYSV_ABI
QueryComputeMemoryInfo(Videodec2ComputeMemoryInfo* compute_memory_info) {
	PRINT_NAME();

	if (compute_memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	compute_memory_info->cpu_gpu_memory_size = VIDEODEC2_MIN_MEMORY_SIZE;
	compute_memory_info->cpu_gpu_memory      = nullptr;

	return OK;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI AllocateComputeQueue(
    const Videodec2ComputeConfigInfo* compute_config_info,
    const Videodec2ComputeMemoryInfo* compute_memory_info, Videodec2ComputeQueue* compute_queue) {
	PRINT_NAME();

	if (compute_config_info == nullptr || compute_memory_info == nullptr ||
	    compute_queue == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (compute_config_info->this_size != sizeof(Videodec2ComputeConfigInfo) ||
	    compute_memory_info->this_size != sizeof(Videodec2ComputeMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (compute_config_info->reserved0 != 0 || compute_config_info->reserved1 != 0) {
		return VIDEODEC2_ERROR_CONFIG_INFO;
	}

	if (compute_config_info->compute_pipe_id > 4) {
		return VIDEODEC2_ERROR_COMPUTE_PIPE_ID;
	}

	if (compute_config_info->compute_queue_id > 7) {
		return VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
	}

	if (compute_memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (compute_memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	*compute_queue = compute_memory_info->cpu_gpu_memory;
	return OK;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI ReleaseComputeQueue(Videodec2ComputeQueue compute_queue) {
	PRINT_NAME();

	return compute_queue != nullptr ? OK : VIDEODEC2_ERROR_COMPUTE_QUEUE_ID;
}

static int32_t QueryDecoderMemoryInfoImpl(const Videodec2DecoderConfigInfo* config,
                                                    Videodec2DecoderMemoryInfo*       memory_info) {

	if (config == nullptr || memory_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, false);
	if (validation_result != OK) {
		return validation_result;
	}

	memory_info->cpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_memory             = nullptr;
	memory_info->gpu_memory_size        = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->gpu_memory             = nullptr;
	memory_info->cpu_gpu_memory_size    = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->cpu_gpu_memory         = nullptr;
	memory_info->max_frame_buffer_size  = VIDEODEC2_MIN_MEMORY_SIZE;
	memory_info->frame_buffer_alignment = 0x100;
	memory_info->reserved0              = 0;

	return OK;
}

static int32_t CreateDecoderImpl(const Videodec2DecoderConfigInfo* config,
                                           const Videodec2DecoderMemoryInfo* memory_info,
                                           Videodec2Decoder*                 decoder) {

	if (config == nullptr || memory_info == nullptr || decoder == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (config->this_size != sizeof(Videodec2DecoderConfigInfo) ||
	    memory_info->this_size != sizeof(Videodec2DecoderMemoryInfo)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	const auto validation_result = ValidateDecoderConfig(config, true);
	if (validation_result != OK) {
		return validation_result;
	}

	if (memory_info->cpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->cpu_gpu_memory_size < VIDEODEC2_MIN_MEMORY_SIZE ||
	    memory_info->max_frame_buffer_size < VIDEODEC2_MIN_MEMORY_SIZE) {
		return VIDEODEC2_ERROR_MEMORY_SIZE;
	}

	if (memory_info->cpu_memory == nullptr || memory_info->gpu_memory == nullptr ||
	    memory_info->cpu_gpu_memory == nullptr) {
		return VIDEODEC2_ERROR_MEMORY_POINTER;
	}

	Decoder::Config backend_config {config->codec_type, config->max_frame_width, config->max_frame_height};
	backend_config.retain_input_buffer = config->resource_type == VIDEODEC2_RESOURCE_TYPE_VDEC;
	auto* state = Decoder::Create(backend_config);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_API_FAIL;
	}

	{
		std::scoped_lock lock(g_decoder_mutex);
		g_decoders.insert(state);
	}

	*decoder = state;
	return OK;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI DeleteDecoder(Videodec2Decoder decoder) {
	PRINT_NAME();

	DecoderState* state = nullptr;
	{
		std::scoped_lock lock(g_decoder_mutex);
		auto             it = g_decoders.find(decoder);
		if (it == g_decoders.end()) {
			return VIDEODEC2_ERROR_DECODER_INSTANCE;
		}
		state = static_cast<DecoderState*>(*it);
		g_decoders.erase(it);
	}

	Decoder::Destroy(state);

	return OK;
}

static int32_t DecodeImpl(Videodec2Decoder decoder, const Videodec2InputData* input_data,
                                     Videodec2FrameBuffer* frame_buffer,
	                                    Videodec2OutputInfo*  output_info, bool* output_observed) {

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (input_data == nullptr || frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (input_data->this_size != sizeof(Videodec2InputData) ||
	    frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (input_data->au_size == 0) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_SIZE;
	}

	if (input_data->au_data == nullptr) {
		return VIDEODEC2_ERROR_ACCESS_UNIT_POINTER;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));
	*output_observed = true;

	Decoder::Output decoded {};
	const auto      result =
	    Decoder::Decode(state,
	                    {input_data->au_data, input_data->au_size, input_data->pts_data,
	                     input_data->dts_data, input_data->attached_data},
	                    {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t FlushImpl(Videodec2Decoder decoder, Videodec2FrameBuffer* frame_buffer,
                                    Videodec2OutputInfo* output_info, bool* output_observed) {

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}

	if (frame_buffer == nullptr || output_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (frame_buffer->this_size != sizeof(Videodec2FrameBuffer) ||
	    !IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (frame_buffer->frame_buffer_size == 0) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_SIZE;
	}

	if (frame_buffer->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_FRAME_BUFFER_POINTER;
	}

	frame_buffer->is_accepted = false;
	FillNoPictureOutput(frame_buffer, output_info, Decoder::GetCodecType(state));
	*output_observed = true;

	Decoder::Output decoded {};
	const auto      result = Decoder::Flush(
	    state, {frame_buffer->frame_buffer, frame_buffer->frame_buffer_size}, &decoded);
	ApplyDecodedOutput(decoded, frame_buffer, output_info);
	return MapDecoderResult(result);
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI QueryDecoderMemoryInfo(
    const Videodec2DecoderConfigInfo* config, Videodec2DecoderMemoryInfo* memory_info) {
	PRINT_NAME();
	const auto result = QueryDecoderMemoryInfoImpl(config, memory_info);
	TraceMemoryResult(TraceOperation::Query, result, config, memory_info, nullptr);
	return result;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI CreateDecoder(
    const Videodec2DecoderConfigInfo* config, const Videodec2DecoderMemoryInfo* memory_info,
    Videodec2Decoder* decoder) {
	PRINT_NAME();
	const auto result = CreateDecoderImpl(config, memory_info, decoder);
	TraceMemoryResult(TraceOperation::Create, result, config, memory_info, result == OK ? *decoder : nullptr);
	return result;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Decode(
    Videodec2Decoder decoder, const Videodec2InputData* input_data,
    Videodec2FrameBuffer* frame_buffer, Videodec2OutputInfo* output_info) {
	PRINT_NAME();
	bool observed = false;
	const auto result = DecodeImpl(decoder, input_data, frame_buffer, output_info, &observed);
	TraceFrameResult(TraceOperation::Decode, result, decoder, input_data, frame_buffer, output_info, observed);
	return result;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Flush(
    Videodec2Decoder decoder, Videodec2FrameBuffer* frame_buffer, Videodec2OutputInfo* output_info) {
	PRINT_NAME();
	bool observed = false;
	const auto result = FlushImpl(decoder, frame_buffer, output_info, &observed);
	TraceFrameResult(TraceOperation::Flush, result, decoder, nullptr, frame_buffer, output_info, observed);
	return result;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI Reset(Videodec2Decoder decoder) {
	PRINT_NAME();

	auto* state = GetDecoder(decoder);
	if (state == nullptr) {
		return VIDEODEC2_ERROR_DECODER_INSTANCE;
	}
	Decoder::Reset(state);
	return OK;
}

static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetPictureInfo(const Videodec2OutputInfo* output_info,
                                            void* first_picture_info, void* second_picture_info) {
	PRINT_NAME();

	if (output_info == nullptr || first_picture_info == nullptr) {
		return VIDEODEC2_ERROR_ARGUMENT_POINTER;
	}

	if (!IsOutputInfoSizeValid(output_info->this_size)) {
		return VIDEODEC2_ERROR_STRUCT_SIZE;
	}

	if (!output_info->is_valid || output_info->picture_count == 0 ||
	    output_info->frame_buffer == nullptr) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	Decoder::PictureInfo decoded {};
	if (!Decoder::GetPictureInfo(output_info->frame_buffer, &decoded) ||
	    decoded.codec_type != output_info->codec_type) {
		return VIDEODEC2_ERROR_OUTPUT_INFO;
	}

	auto fill_common = [&decoded](void* destination, bool valid) -> int32_t {
		auto*      bytes = static_cast<uint8_t*>(destination);
		const auto size  = *static_cast<const size_t*>(destination);
		if (size < 40 || size > 256) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}
		std::memset(bytes + sizeof(size_t), 0, size - sizeof(size_t));
		bytes[8] = valid ? 1 : 0;
		if (valid) {
			std::memcpy(bytes + 16, &decoded.pts, sizeof(decoded.pts));
			std::memcpy(bytes + 24, &decoded.dts, sizeof(decoded.dts));
			std::memcpy(bytes + 32, &decoded.attached_data, sizeof(decoded.attached_data));
		}
		return OK;
	};

	if (output_info->codec_type == 1) {
		const auto requested_size = *static_cast<const size_t*>(first_picture_info);
		if (requested_size != sizeof(Videodec2AvcPictureInfo) &&
		    (requested_size | 16u) != sizeof(Videodec2AvcPictureInfo)) {
			return VIDEODEC2_ERROR_STRUCT_SIZE;
		}

		const auto picture = Decoder::MakeAvcPictureInfo(decoded, requested_size);
		std::memcpy(first_picture_info, &picture, requested_size);
	} else {
		const auto result = fill_common(first_picture_info, true);
		if (result != OK) {
			return result;
		}
	}

	if (second_picture_info != nullptr) {
		const auto result = fill_common(second_picture_info, false);
		if (result != OK) {
			return result;
		}
	}
	return OK;
}

// ps5-vsh: kjrLbcyhEiw is routed to the AVC picture-info implementation.
// The export's symbolic naming provenance remains under review. The common
// implementation already takes the AVC path when codec_type == 1.
static int32_t __attribute__((force_align_arg_pointer)) KYTY_SYSV_ABI GetAvcPictureInfo(const Videodec2OutputInfo* output_info,
                                               void* first_picture_info,
                                               void* second_picture_info) {
	return GetPictureInfo(output_info, first_picture_info, second_picture_info);
}

LIB_DEFINE(InitVideoDec2_1) {
	PRINT_NAME_ENABLE(true);

	LIB_FUNC("RnDibcGCPKw", QueryComputeMemoryInfo);
	LIB_FUNC("eD+X2SmxUt4", AllocateComputeQueue);
	LIB_FUNC("UvtA3FAiF4Y", ReleaseComputeQueue);
	LIB_FUNC("qqMCwlULR+E", QueryDecoderMemoryInfo);
	LIB_FUNC("CNNRoRYd8XI", CreateDecoder);
	LIB_FUNC("jwImxXRGSKA", DeleteDecoder);
	LIB_FUNC("852F5+q6+iM", Decode);
	LIB_FUNC("l1hXwscLuCY", Flush);
	LIB_FUNC("wJXikG6QFN8", Reset);
	LIB_FUNC("NtXRa3dRzU0", GetPictureInfo);
	LIB_FUNC("kjrLbcyhEiw", GetAvcPictureInfo); // sceVideodec2GetAvcPictureInfo
}

} // namespace VideoDec2

} // namespace Libs
