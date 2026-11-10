// Exercises the production child presenter/readback, cross-process mailbox,
// parent upload and final alpha-blended scanout. No guest firmware or devices.
#include "common/emulatorConfig.h"
#include "graphics/presentation/presenter.h"
#include "graphics/presentation/window/windowInternal.h"
#include "graphics/presentation/window/runtimeWindowBridge.h"
#include "loader/runtimeChildProcess.h"
#include "loader/runtimeChildClient.h"
#include <windows.h>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace {
namespace Child = Loader::RuntimeChild;
namespace Graphics = Libs::Graphics;
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
uint64_t native_window(SDL_Window* window) {
	return reinterpret_cast<uint64_t>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}
void Pump() { SDL_Event event; while (SDL_PollEvent(&event)) {} }
uint64_t child_epoch = 0;
bool Focus(bool, bool, uint64_t epoch) { child_epoch = epoch; return true; }
template<class F> void Until(F&& predicate) {
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!predicate()) { Require(std::chrono::steady_clock::now() < end, "bounded compositor wait"); Pump(); Sleep(2); }
}
void Initialize(Graphics::WindowContext& window, bool child) {
	Require(SDL_InitSubSystem(SDL_INIT_VIDEO), "SDL video");
	window.graphic_ctx.screen_width = 64; window.graphic_ctx.screen_height = 64;
	window.window = SDL_CreateWindow("Composed frame test", 64, 64, SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
	Require(window.window != nullptr, "hidden window");
	if (child) Require(Graphics::AttachRuntimeChildWindow(native_window(window.window)), "authenticated hidden child");
	window.CreateVulkan();
}
void Pixel(const Child::FramePixels& frame, int r, int g, int b) {
	Require(frame.width && frame.height && frame.pixels.size() == size_t(frame.width) * frame.height * 4, "captured composed pixels");
	Require(frame.format == 37 || frame.format == 44, "SDR UNORM capture");
	const bool bgra = frame.format == 44;
	const auto offset = (size_t(frame.height / 2) * frame.width + frame.width / 2) * 4;
	const auto* pixel = frame.pixels.data() + offset;
	if (std::abs(int(pixel[bgra ? 2 : 0]) - r) > 2 || std::abs(int(pixel[1]) - g) > 2 || std::abs(int(pixel[bgra ? 0 : 2]) - b) > 2) {
		std::fprintf(stderr, "pixel %u,%u,%u expected %d,%d,%d\n", pixel[bgra ? 2 : 0], pixel[1], pixel[bgra ? 0 : 2], r,g,b);
		throw std::runtime_error("composed pixel mismatch");
	}
}
}

int RunComposedFramesTest(int argc, char** argv) try {
	Config::Load({.printf_direction = Config::LogDirection::Silent});
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	const bool child = std::string(argv[1]) == "--composed-frames-child";
	if (child) {
		Require(argc == 6, "child arguments");
		Require(Graphics::ConfigureRuntimeWindowRole(false, std::stoull(argv[2]), std::stoul(argv[3])), "child window role");
		Require(Loader::ConfigureRuntimeChildClient(argv[5], Child::EmbeddedWindow | Child::ComposedFrames), "child transport");
		Graphics::ConfigureRuntimeFocusServices(Focus, nullptr);
		Graphics::WindowContext window; Initialize(window, true);
		Loader::NotifyRuntimeChildEntry(100);
		uint64_t last_epoch = UINT64_MAX;
		while (Graphics::PumpRuntimeWindowBridge()) {
			Pump();
			// Model an app whose first frame requires foreground admission.
			if (child_epoch && last_epoch != child_epoch) {
				last_epoch = child_epoch;
				auto& frame = window.presenter->PrepareSolidForTesting(64, 64, child_epoch >= 3 ? std::array<float,4>{0,1,0,1} : std::array<float,4>{0,0,1,1});
				const Graphics::Presenter::Layer layer {&frame, 0, false}; window.presenter->Present(std::span(&layer, 1));
			} else window.presenter->PresentIdleFrame();
			Sleep(5);
		}
		return 0;
	}
	Graphics::WindowContext window; Initialize(window, false);
	wchar_t executable[32768] {}; Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "test executable");
	const auto directory = std::filesystem::current_path() / "_TempData" / ("composed-test-" + std::to_string(GetCurrentProcessId()));
	std::filesystem::create_directories(directory);
	Child::Spawn spawn; spawn.key = {104,0,1,GetTickCount64(),GetCurrentProcessId()}; spawn.executable = executable;
	spawn.directory = directory; spawn.log = directory / "child.log"; spawn.parent_window = native_window(window.window); spawn.composed_frames = true;
	spawn.arguments = {L"--composed-frames-child",std::to_wstring(spawn.parent_window),std::to_wstring(GetCurrentProcessId())};
	uint32_t error = 0; auto process = Child::Process::Create(spawn, &error); Require(process != nullptr, "spawn composed child");
	Until([&] { process->Poll(); Require(!process->Retired(), "child startup"); return process->FocusReady(); });
	Require(!process->PresentationReady(), "focus admission does not require a first frame");
	Require(process->Focus(true,true,1), "foreground before first frame");
	Until([&] { process->Poll(); return process->FocusAcknowledged() == 1 && process->PresentationReady(); });
	Child::SetFrameSource(process->Frames(), spawn.key, true);
	Require(window.presenter->NeedsSystemOverlayRefresh() && window.presenter->PresentLastFrame(), "remote frames refresh even without local guest planes");
	Pixel(window.presenter->CaptureComposedForTesting(), 0,0,255);
	HWND child_window = FindWindowExW(reinterpret_cast<HWND>(spawn.parent_window), nullptr, nullptr, nullptr);
	Require(child_window && !(GetWindowLongPtrW(child_window,GWL_STYLE) & WS_VISIBLE), "composed child never covers parent");
	auto& overlay = window.presenter->PrepareSolidForTesting(64,64,{1,0,0,0.5f});
	const Graphics::Presenter::Layer layer {&overlay,256,false}; window.presenter->Present(std::span(&layer,1));
	Pixel(window.presenter->CaptureComposedForTesting(),128,0,127);
	// Controller-only shell ownership leaves the moving application visible.
	Require(process->Focus(true,false,2), "overlay focus");
	Until([&] { process->Poll(); return process->FocusAcknowledged() == 2; });
	Pixel(window.presenter->CaptureComposedForTesting(),128,0,127);
	window.presenter->ClearLayer(256);
	Pixel(window.presenter->CaptureComposedForTesting(),0,0,255);
	Child::SetShellUiFrameStatus(0,6); Pixel(window.presenter->CaptureComposedForTesting(),0,0,0);
	Child::SetShellUiFrameStatus(2,6); Pixel(window.presenter->CaptureComposedForTesting(),0,0,255);
	const auto before = process->Frames()->Revision();
	Require(process->Focus(true,true,3), "resume app");
	Until([&] { process->Poll(); return process->FocusAcknowledged() == 3 && process->Frames()->Revision() > before + 1; });
	Pixel(window.presenter->CaptureComposedForTesting(),0,255,0);
	Require(SDL_SetWindowSize(window.window,96,48), "resize parent"); Pump(); window.Resize(96,48);
	Until([&] { process->Poll(); Pump(); auto pixels = window.presenter->CaptureComposedForTesting(); return pixels.width == 96 && pixels.height == 48; });
	Pixel(window.presenter->CaptureComposedForTesting(),0,255,0);
	process->Close(true); Until([&] { return process->Poll().retired; });
	Child::RetireFrameSource(spawn.key); Pixel(window.presenter->CaptureComposedForTesting(),0,0,0);
	// A new generation cannot resurrect the old application's retained image.
	auto old = spawn.key; ++spawn.key.sequence; spawn.log = directory / "relaunch.log";
	process = Child::Process::Create(spawn,&error); Require(process != nullptr,"relaunch");
	Child::SetFrameSource(process->Frames(),spawn.key,true);
	Pixel(window.presenter->CaptureComposedForTesting(),0,0,0);
	Child::RetireFrameSource(old); Require(Child::GetFrameSource().key == spawn.key,"stale retirement rejected");
	Until([&] { process->Poll(); Require(!process->Retired(),"relaunch startup"); return process->FocusReady(); });
	Require(!process->PresentationReady() && process->Focus(true,true,1), "fresh generation focus before first frame");
	Until([&] { process->Poll(); return process->FocusAcknowledged() == 1 && process->PresentationReady(); });
	Pixel(window.presenter->CaptureComposedForTesting(),0,0,255);
	process->Close(false); Until([&] { return process->Poll().retired; });
	Require(process->ExitCode() == 0,"clean child exit");
	Child::RetireFrameSource(spawn.key); Pixel(window.presenter->CaptureComposedForTesting(),0,0,0);
	std::printf("two-process production Vulkan composition/alpha/resize/retirement/relaunch passed\n"); return 0;
} catch (const std::exception& error) { std::fprintf(stderr,"composed frame test: %s\n",error.what()); return 1; }
