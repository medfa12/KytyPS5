// Production audio.cpp, with existing deterministic consuming SDL stream fakes.
#define main ExistingAudioTimingMain
#include "../AudioOutTimingTests.cpp"
#undef main

int main() {
	Libs::Audio::SetRuntimeAudioFocus(false);
	Libs::Audio::Initialize();
	using namespace Libs::Audio::AudioInternal;
	const auto guest = AudioOutOpen(0,256,48000,Format::FloatStereo);
	Check(guest > 0 && streams.size() == 1 && streams[0]->gain == 0 && !streams[0]->paused, "future ordinary stream muted but consuming");
	auto native = OpenNativeOutput({100,1,2,1},48000,Format::FloatStereo,1,960);
	Check(native && streams.size() == 2 && streams[1]->gain == 0, "future native stream inherits focus gain");
	Check(native->SetGain(0.4f) && streams[1]->gain == 0, "guest volume cannot undo focus mute");
	Check(native->SetPaused(false), "guest can start native playback");
	std::array<uint8_t,480*8> pcm {};
	Check(native->TryQueue(pcm,480,1) == NativeQueueResult::Accepted, "background PCM admission");
	now += 20000;
	Check(native->Status().queued_frames == 0 && !streams[1]->paused, "muted native queue drains instead of deadlocking backpressure");
	Check(Libs::Audio::SetRuntimeAudioFocus(true) && streams[0]->gain == 1 && streams[1]->gain == 0.4f, "existing gains restored, including guest native volume");
	Check(Libs::Audio::SetRuntimeAudioFocus(false) && streams[0]->gain == 0 && streams[1]->gain == 0, "Home mutes all existing streams");
	auto future = OpenNativeOutput({100,1,3,1},48000,Format::FloatMono,1,960);
	Check(future && streams.back()->gain == 0, "later native stream also inherits mute");
	const auto ordinary2 = AudioOutOpen(0,256,48000,Format::FloatStereo);
	Check(ordinary2 > 0 && streams.back()->gain == 0 && !streams.back()->paused, "later ordinary stream inherits mute without pause");
	native->Close(); future->Close(); AudioOutClose(guest); AudioOutClose(ordinary2);
	Libs::Audio::Shutdown(); Libs::Audio::SetRuntimeAudioFocus(true);
	Check(streams.empty() && audio_refs == 0, "focus test resources retired");
	std::puts("subprocess audio gain/inheritance/consumption checks passed"); return 0;
}
