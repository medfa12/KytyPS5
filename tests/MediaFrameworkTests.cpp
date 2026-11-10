#define KYTY_MEDIA_FRAMEWORK_TEST
#include "graphics/shader/shaderBindings.h"
#include "libs/libMediaFramework.cpp"

#include <cstdio>
#include <filesystem>
#include <thread>

namespace {
std::filesystem::path   root;
std::mutex              thread_mutex;
std::condition_variable thread_wake;
int                     thread_count = 0;
void                    Check(bool ok, const char* message) {
	if (!ok) {
		std::fprintf(stderr, "MediaFrameworkTests: %s\n", message);
		std::abort();
	}
}
} // namespace
namespace Libs::LibKernel {
int KYTY_SYSV_ABI PthreadCreate(Pthread* out, const PthreadAttr*, pthread_entry_func_t entry,
                                void* arg, const char*) {
	{
		std::lock_guard lock(thread_mutex);
		++thread_count;
	}
	*out = reinterpret_cast<Pthread>(new std::thread([=] {
		entry(arg);
		{
			std::lock_guard lock(thread_mutex);
			--thread_count;
		}
		thread_wake.notify_all();
	}));
	return 0;
}
int KYTY_SYSV_ABI PthreadJoin(Pthread thread, void**) {
	auto* t = reinterpret_cast<std::thread*>(thread);
	t->join();
	delete t;
	return 0;
}
int KYTY_SYSV_ABI PthreadDetach(Pthread thread) {
	auto* t = reinterpret_cast<std::thread*>(thread);
	t->detach();
	delete t;
	return 0;
}
namespace FileSystem {
std::filesystem::path GetRealFilename(const std::string& path) {
	return path.starts_with("/app0/") ? root / path.substr(6) : std::filesystem::path {};
}
} // namespace FileSystem
} // namespace Libs::LibKernel
namespace {
using namespace Libs::MediaFramework;
struct Events {
	std::mutex                mutex;
	std::condition_variable   wake;
	std::vector<int>          events;
	int                       id              = 0;
	bool                      delete_on_error = false;
	static KYTY_SYSV_ABI void Call(int event, const void*, void* user) {
		auto& e = *static_cast<Events*>(user);
		// Exercise callback reentry: this deadlocks if the source lock is held.
		int state = -1;
		Check(Ready(e.id, &state) == 0 || event == 0xf, "callback query");
		if (e.delete_on_error && event == 0x13) Check(Delete(e.id) == 0, "delete from callback");
		{
			std::lock_guard lock(e.mutex);
			e.events.push_back(event);
		}
		e.wake.notify_all();
	}
	void Wait(int event) {
		std::unique_lock lock(mutex);
		Check(wake.wait_for(
		          lock, std::chrono::seconds(10),
		          [&] { return std::find(events.begin(), events.end(), event) != events.end(); }),
		      "event timeout");
	}
};
void Fixture(const std::filesystem::path& path) {
	// Generated grayscale ramp: 12 frames, 32x16, 12fps, MPEG4 all-intra.
	// ffmpeg -f lavfi -i "nullsrc=s=32x16:r=12:d=1,geq=lum='16+N*17':cb=128:cr=128"
	//   -an -c:v mpeg4 -q:v 1 -g 1 -bf 0 -map_metadata -1 -fflags +bitexact -flags:v +bitexact
	//   media-gray.mp4
	std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path() / "media-gray.mp4",
	                           path);
}
} // namespace
int main(int argc, char** argv) {
	using namespace Libs::MediaFramework;
	root = std::filesystem::temp_directory_path() /
	       ("kyty-media-" + std::to_string(Clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(root);
	Fixture(root / "frames.mp4");
	{
		Decoder fixture;
		Check(fixture.Open((root / "frames.mp4").string()), "fixture decode");
	}
	Events    events;
	Callbacks callbacks {Events::Call, &events, 0xff};
	int no_callback = 0;
	Check(Add(nullptr, nullptr, &no_callback) == static_cast<int>(0x81790001u) &&
	          Add("/app0/frames.mp4", nullptr, nullptr) == static_cast<int>(0x81790001u),
	      "native MediaFramework invalid-argument error");
	Check(Add("/app0/frames.mp4", nullptr, &no_callback) == 0 && no_callback > 0,
	      "native Add accepts an omitted callback block");
	Check(Delete(no_callback) == 0, "release callback-free source");
	Check(Add("https://example.invalid/video.mp4", &callbacks, &events.id) < 0,
	      "reject network source");
	Check(Add("/app0/frames.mp4", &callbacks, &events.id) == 0 && events.id > 0, "create source");
	Check(Load(events.id) == 0, "load");
	events.Wait(8);
	{
		std::lock_guard lock(events.mutex);
		Check(std::find(events.events.begin(), events.events.end(), 9) == events.events.end(),
		      "successful load must not report waiting");
	}
	int ready = 0, canvas = 0, type = -1;
	Check(Ready(events.id, &ready) == 0 && ready == 4, "ready after real decode");
	Check(Canvas(events.id, &canvas, &type) == 0 && canvas > 0 && type == 0, "canvas");
	Libs::Graphics::ShaderTextureResource texture;
	Check(BuildCanvasTexture(canvas, texture.fields), "build media texture descriptor");
	Check(IsCanvasLive(canvas), "decoded canvas is live");
	Check(texture.Width5() == 31 && texture.Height5() == 15 &&
	          texture.Format() == Libs::Graphics::Prospero::BufferFormat::k8_8_8_8UNorm &&
	          texture.Type() == Libs::Graphics::Prospero::ImageType::kColor2D &&
	          texture.DstSelXYZW() == 0xfac && CanvasFromTextureAddress(texture.Base40()) == canvas,
	      "media descriptor decodes using renderer layout");
	Check(CanvasFromTextureAddress(texture.Base40() + 1) == 0 &&
	          CanvasFromTextureAddress(0x1000000000) == 0 &&
	          !BuildCanvasTexture(canvas ^ 1, texture.fields),
	      "reject ordinary and malformed media addresses");
	std::vector<uint8_t> first, later, paused;
	Check(CopyCanvasFrame(canvas, 32, 16, first) && first[0] == 0 && first[3] == 255,
	      "first decoded frame");
	Check(Seek(events.id, 0.5) == 0 && CopyCanvasFrame(canvas, 32, 16, later), "seek decode");
	Check(later != first && later[0] >= 100, "seek changed pixels");
	Check(Play(events.id) == 0, "play");
	events.Wait(0x10);
	Check(Pause(events.id) == 0, "pause");
	events.Wait(0x11);
	double before = 0, after = 0;
	CurrentTime(events.id, &before);
	std::this_thread::sleep_for(std::chrono::milliseconds(80));
	CurrentTime(events.id, &after);
	Check(before == after && CopyCanvasFrame(canvas, 32, 16, paused) && paused == later,
	      "paused time and pixels");
	Check(SetLoop(events.id, true) == 0 && Seek(events.id, 0.99) == 0 && Play(events.id) == 0,
	      "start loop");
	std::this_thread::sleep_for(std::chrono::milliseconds(160));
	CurrentTime(events.id, &after);
	Check(after < 0.5 && CopyCanvasFrame(canvas, 32, 16, later) && later[0] < 100,
	      "loop clock and decoder rewind");
	Check(SetLoop(events.id, false) == 0 && Seek(events.id, 0.99) == 0, "end playback");
	events.Wait(0xd);
	Check(CopyCanvasFrame(canvas, 32, 16, later) && later[0] >= 214 && later[0] <= 223,
	      "retain final frame after decoder drain");
	Check(Delete(events.id) == 0 && Ready(events.id, &ready) < 0 &&
	          !CopyCanvasFrame(canvas, 32, 16, later),
	      "delete invalidates source and canvas");
	Check(OwnsCanvas(canvas) && !OwnsCanvas(0) && !OwnsCanvas(canvas ^ 1),
	      "retired media ID remains owned");
	Check(!IsCanvasLive(canvas), "deleted canvas can retire its GPU image");
	Check(BuildCanvasTexture(canvas, texture.fields) && texture.Width5() == 0 &&
	          texture.Height5() == 0 && CanvasFromTextureAddress(texture.Base40()) == canvas,
	      "retired descriptor remains a host-owned transparent fallback");
	Events    failed;
	Callbacks bad {Events::Call, &failed, 0xff};
	Check(Add("/app0/missing.mp4", &bad, &failed.id) < 0 && failed.id == 0,
	      "missing local file rejected before publishing a source");
	{ auto* file = std::fopen((root / "invalid.mp4").string().c_str(), "wb");
	  Check(file != nullptr, "create invalid fixture"); std::fputs("not a video", file); std::fclose(file); }
	Check(Add("/app0/invalid.mp4", &bad, &failed.id) == 0 && Load(failed.id) == 0,
	      "invalid source lifecycle");
	failed.Wait(0x13);
	Check(Ready(failed.id, &ready) == 0 && ready == 0, "failed decode is not ready");
	Check(Delete(failed.id) == 0, "delete failed source");
	Events reentrant;
	reentrant.delete_on_error = true;
	Callbacks removed {Events::Call, &reentrant, 0xff};
	Check(Add("/app0/invalid.mp4", &removed, &reentrant.id) == 0 && Load(reentrant.id) == 0,
	      "callback-delete source");
	reentrant.Wait(0x13);
	{
		std::unique_lock lock(thread_mutex);
		Check(
		    thread_wake.wait_for(lock, std::chrono::seconds(10), [] { return thread_count == 0; }),
		    "all workers exit after callback deletion");
	}
	Check(Ready(reentrant.id, &ready) < 0, "callback deletion invalidates source");
	for (int i = 1; i < argc; ++i) {
		Decoder decoder;
		Check(decoder.Open(argv[i]), "external clip opens");
		int count = 1;
		while (decoder.Next())
			++count;
		Check(decoder.Copy(96, 54, later), "external EOF retains frame");
		Check(decoder.At(0) && decoder.Copy(96, 54, first), "external decoder rewinds");
		std::printf("Decoded %d frames: %s\n", count, argv[i]);
	}
	std::filesystem::remove_all(root);
	std::puts("MediaFrameworkTests: all cases passed");
}
