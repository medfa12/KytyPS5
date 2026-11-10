// Actual production haptics/Bluetooth/audio units with existing fake endpoints.
#define main ExistingPadHapticsMain
#include "../PadHapticsTests.cpp"
#undef main
#include "loader/runtimeChildClient.h"
#include <windows.h>

int main() {
	HANDLE read = nullptr, write = nullptr, observed = nullptr, notify = nullptr;
	Check(CreatePipe(&read,&write,nullptr,4096) && CreatePipe(&observed,&notify,nullptr,4096), "child hardware guard pipes");
	Loader::RuntimeChild::Key key {109,0,1,0x1234,0x5678};
	Check(Loader::ConfigureRuntimeChildClient(Loader::RuntimeChild::EncodeArguments({key,reinterpret_cast<uint64_t>(read),reinterpret_cast<uint64_t>(notify),0})), "child hardware guard role");
	using namespace Libs::Controller;
	Check(!DualSenseHaptics::Open(48000,false) && !DualSenseHaptics::Open(48000,true), "child cannot open controller audio endpoints");
	Check(!DualSenseBluetooth::Open(48000,true,1,nullptr), "child cannot open direct controller HID");
	Check(!DualSenseHaptics::SetVibration(1,100,50,100), "child cannot bypass parent hardware output broker");
	{
		Libs::Audio::Audio audio;
		const auto port = audio.AudioOutOpen(Libs::Audio::AUDIO_OUT_PORT_TYPE_VIBRATION,256,48000,Libs::Audio::Audio::Format::FloatStereo);
		Check(port.IsValid(), "child vibration audio port retains guest timing contract");
	}
	Check(opens == 0 && hid_opens == 0 && audio_refs == 0 && rumble_calls == 0, "zero controller audio/HID/rumble hardware ownership in child");
	CloseHandle(write); CloseHandle(observed);
	std::puts("subprocess child controller audio/HID exclusion checks passed"); return 0;
}
