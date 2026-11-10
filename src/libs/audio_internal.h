#ifndef EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
#define EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_

#include "common/common.h"

#include <memory>
#include <span>

namespace Libs::Audio { class Audio; }

namespace Libs::Audio::AudioInternal {

enum class Format {
	Unknown,
	Signed16bitMono,
	Signed16bitStereo,
	Signed16bit8Ch,
	FloatMono,
	FloatStereo,
	Float8Ch,
	Signed16bit8ChStd,
	Float8ChStd,
	Float12Ch,
};

struct OutputParam {
	int          handle = 0;
	const void*  data   = nullptr;
	const float* gains  = nullptr;
};

static constexpr int OUT_PORTS_MAX = 32;

int      AudioOutOpen(int type, uint32_t samples_num, uint32_t freq, Format format);
void     AudioOutClose(int handle);
bool     AudioOutHasDevice(int handle);
uint32_t AudioOutOutputs(const OutputParam* params, uint32_t num, bool blocking = true);

// Host-only media lease. Never exposes/reuses a guest AudioOut integer handle.
struct NativeOutputIdentity {
	int owner = 0, context = 0, decoder = 0;
	uint64_t epoch = 0;
};
enum class NativeQueueResult { Accepted, Backpressure, Unavailable, Invalid, Retired, Failed };
struct NativeOutputStatus {
	NativeQueueResult result = NativeQueueResult::Unavailable;
	uint64_t generation = 0;
	uint32_t queued_frames = 0;
	bool paused = true;
	bool input_ended = false;
};
class NativeOutput final {
public:
	~NativeOutput();
	NativeOutput(const NativeOutput&) = delete;
	NativeOutput& operator=(const NativeOutput&) = delete;
	NativeQueueResult TryQueue(std::span<const uint8_t> pcm, uint32_t frames, uint64_t generation);
	NativeOutputStatus Status() const;
	bool SetPaused(bool paused);
	bool SetGain(float gain);
	bool Flush(uint64_t new_generation);
	// Finish finite input, releasing SDL resampler tail without discarding PCM.
	// No further queue admission until Flush establishes a new generation.
	bool EndInput(uint64_t generation);
	void Retire(); // Nonblocking admission closure; safe under source lifecycle locks.
	void Close();  // Destroy SDL resources outside source lifecycle locks.
	bool IsClosed() const;
private:
	friend class ::Libs::Audio::Audio;
	struct Impl;
	explicit NativeOutput(std::shared_ptr<Impl> impl);
	std::shared_ptr<Impl> m_impl;
};
// Only actual mono/stereo S16/F32 PCM is supported. Starts paused. A null result
// means no real output stream was acquired; there is no silent-device success.
// Capacity must be >=16 frames. Each submission must be <=capacity-8 so that
// the bundled SDL resampler's at-most-seven retained input frames cannot block
// the next complete submission forever. Larger PCM blocks must be split.
std::shared_ptr<NativeOutput> OpenNativeOutput(NativeOutputIdentity identity, uint32_t rate,
                                             Format format, uint64_t generation,
                                             uint32_t max_queued_frames);

} // namespace Libs::Audio::AudioInternal

#endif // EMULATOR_INCLUDE_EMULATOR_AUDIO_INTERNAL_H_
