// Child role with production controller logic and deterministic hardware fakes.
#define main ExistingControllerSettingsMain
#include "../ControllerSettingsTests.cpp"
#undef main
#include "loader/runtimeChildClient.h"
#include <windows.h>

int main() {
	using namespace Loader::RuntimeChild;
	Key key {108,0,1,0x556677,0x8899}; InputMailbox parent; Check(parent.Create(key), "child fixture mapping");
	HANDLE read = nullptr, write = nullptr, observed = nullptr, notify = nullptr, mapping = nullptr;
	Check(CreatePipe(&read,&write,nullptr,4096) && CreatePipe(&observed,&notify,nullptr,4096), "child fixture owned pipes");
	Check(DuplicateHandle(GetCurrentProcess(),reinterpret_cast<HANDLE>(parent.Handle()),GetCurrentProcess(),&mapping,0,FALSE,DUPLICATE_SAME_ACCESS), "independent mapping ownership");
	Check(Loader::ConfigureRuntimeChildClient(EncodeArguments({key,reinterpret_cast<uint64_t>(read),reinterpret_cast<uint64_t>(notify),reinterpret_cast<uint64_t>(mapping)}),BrokeredInput), "authenticated managed child role");
	Config::Initialize(); Config::Load(Config::ConfigOptions {}); ConfigureRuntimeControllerRole(false);
	{
		Controller fixture; PadData data {};
		Check(PadReadState(1,&data) == 0 && !data.connected, "child has no keyboard/physical fallback pad");
		SetRuntimeControllerFocus(true,1);
		PadReport report; report.epoch = 1; report.connected = 1; report.connections = 1; report.buttons = PAD_BUTTON_CROSS | PAD_BUTTON_PS;
		report.axes[0] = 200; report.touch[0] = {900,400,4,1,0}; report.gyro = {1,2,3};
		Check(parent.Push(report), "parent broker sample"); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && data.connected && data.buttons == PAD_BUTTON_CROSS && data.left_stick_x == 200 && data.touch_data.touch_num == 1 && data.angular_velocity_z == 3, "child guest consumes one full normalized pad without PS");
		SetAxis(HOST_INPUT_CONTROLLER_ID,Axis::LeftX,0); SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,true);
		Check(PadReadState(1,&data) == 0 && data.left_stick_x == 0 && data.buttons == (PAD_BUTTON_CROSS | PAD_BUTTON_CIRCLE), "embedded keyboard merges with brokered input");
		parent.Push(report); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && data.left_stick_x == 0 && (data.buttons & PAD_BUTTON_CIRCLE), "broker heartbeat cannot overwrite a held local key");
		SetButton(42,PAD_BUTTON_TRIANGLE,true);
		Check(PadReadState(1,&data) == 0 && !(data.buttons & PAD_BUTTON_TRIANGLE), "child still rejects locally sourced physical gamepad input");
		SetRuntimeControllerFocus(false,2); Check(PadReadState(1,&data) == 0 && !data.buttons && data.left_stick_x == 128, "child Home immediately neutralizes report/history");
		SetRuntimeControllerFocus(true,3); report.epoch = 3; Check(parent.Push(report), "held report in resume epoch"); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && !data.buttons && data.left_stick_x == 128 && !data.touch_data.touch_num, "held button/stick/touch not replayed on child resume");
		report.buttons = 0; report.axes[0] = 128; report.touch = {}; parent.Push(report); PumpRuntimeChildController();
		SetAxis(HOST_INPUT_CONTROLLER_ID,Axis::LeftX,128); SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,false);
		report.buttons = PAD_BUTTON_CROSS; report.axes[0] = 200; parent.Push(report); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && data.buttons == PAD_BUTTON_CROSS && data.left_stick_x == 200, "release/repress re-arms resumed child");
		report.connected = 0; report.buttons = 0; report.axes[0] = 128; parent.Push(report); PumpRuntimeChildController();
		SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,true);
		SetRightStick(HOST_INPUT_CONTROLLER_ID,230,40);
		SetTouchPad(HOST_INPUT_CONTROLLER_ID,0,true,0.25f,0.5f);
		Check(PadReadState(1,&data) == 0 && data.connected && (data.buttons & PAD_BUTTON_CIRCLE) && data.right_stick_x == 230 && data.touch_data.touch_num == 1, "keyboard and mouse work without a physical pad");
		report.eligible = 0; parent.Push(report); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && !data.buttons && data.right_stick_x == 128 && !data.touch_data.touch_num, "OS focus loss neutralizes local controls");
		report.eligible = 1; parent.Push(report); PumpRuntimeChildController();
		Check(PadReadState(1,&data) == 0 && !data.buttons && data.right_stick_x == 128, "OS focus regain cannot replay held local input");
		SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,false); SetRightStick(HOST_INPUT_CONTROLLER_ID,128,128); SetTouchPad(HOST_INPUT_CONTROLLER_ID,0,false,0.25f,0.5f);
		SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,true); SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,false);
		PadData history[64]; const int history_count = PadRead(1,history,64); bool saw_press = false;
		for (int i = 0; i < history_count; ++i) { saw_press |= (history[i].buttons & PAD_BUTTON_CIRCLE) != 0; }
		Check(saw_press && PadReadState(1,&data) == 0 && !data.buttons, "short local key taps retain press/release history between broker ticks");
		rumble.clear(); effects.clear(); haptics.clear(); SetRumble(100,50);
		PadOutput output; Check(parent.ReadOutput(&output) && output.epoch == 3 && output.large_motor == 100 && output.small_motor == 50, "child vibration routed through epoch-tagged mailbox");
		Check(rumble.empty() && effects.empty() && haptics.empty(), "child never touches even fake physical output");
		now += 300; PumpRuntimeChildController(); Check(PadReadState(1,&data) == 0 && !data.connected && !data.buttons, "missing parent heartbeat becomes neutral disconnect");
	}
	Config::Shutdown(); CloseHandle(write); CloseHandle(observed);
	std::puts("subprocess child normalized pad/history/focus/ownership checks passed"); return 0;
}
