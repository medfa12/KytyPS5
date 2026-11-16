// Reuse existing deterministic hardware/time fakes, never real SDL pads/HID.
#define main ExistingControllerSettingsMain
#include "../ControllerSettingsTests.cpp"
#undef main

int main() {
	Config::Initialize(); Config::Load(Config::ConfigOptions {});
	ConfigureRuntimeControllerRole(true);
	{
		Controller fixture;
		SetButton(1, PAD_BUTTON_CROSS, true); SetAxis(1, Axis::LeftX, 255);
		SetRuntimeControllerFocus(false, 1);
		SetButton(1, PAD_BUTTON_PS, true);
		PadData ordinary {}, system {};
		Check(PadReadState(1,&ordinary) == 0 && !ordinary.buttons && ordinary.left_stick_x == 128, "shell ordinary input suppressed while child focused");
		Check(PadReadSystemState(1,&system) == 0 && system.buttons == PAD_BUTTON_PS, "system PS survives without leaking navigation");
		SetButton(1,PAD_BUTTON_PS,false);
		PulseRuntimeSystemPs();
		Check(PadReadSystemState(1,&system) == 0 && system.buttons == PAD_BUTTON_PS,
		      "child PS pulse reaches privileged shell input while app owns controller focus");
		Check(PadReadState(1,&ordinary) == 0 && !ordinary.buttons && !(RuntimePadSnapshot().buttons & PAD_BUTTON_PS),
		      "forwarded PS never leaks into ordinary or brokered app controls");
		now += 151;
		Check(PadReadSystemState(1,&system) == 0 && !system.buttons, "forwarded PS releases without another child message");
		const auto raw = RuntimePadSnapshot(); Check((raw.buttons & PAD_BUTTON_CROSS) && raw.axes[0] == 255, "broker sees raw hardware while shell ordinary reader is neutral");
		SetRuntimeControllerFocus(true, 2);
		Check(PadReadState(1,&ordinary) == 0 && !ordinary.buttons && ordinary.left_stick_x == 128, "Home drops history and held controls");
		PadData history[64]; const auto count = PadRead(1,history,64);
		for (int i = 0; i < count; ++i) { Check(!history[i].buttons && history[i].left_stick_x == 128, "old history not replayed to shell"); }
		SetButton(1, PAD_BUTTON_CROSS, false); SetAxis(1, Axis::LeftX, 128);
		SetButton(1, PAD_BUTTON_CROSS, true); SetAxis(1, Axis::LeftX, 255);
		Check(PadReadState(1,&ordinary) == 0 && ordinary.buttons == PAD_BUTTON_CROSS && ordinary.left_stick_x == 255, "fresh shell navigation after release");
		SetRuntimeControllerFocus(false,3); rumble.clear(); haptics.clear(); effects.clear();
		SetRumble(200,100); Check(rumble.empty() && haptics.empty(), "background parent cannot output hardware rumble");
		Loader::RuntimeChild::PadOutput output; output.epoch = 3; output.flags = 7; output.large_motor = 90; output.small_motor = 40;
		output.red = 7; output.green = 8; output.blue = 9;
		ApplyRuntimePadOutput(output,4,true); Check(rumble.empty(), "stale output epoch rejected");
		ApplyRuntimePadOutput(output,3,false); Check(rumble.empty(), "background child output rejected");
		ApplyRuntimePadOutput(output,3,true); Check(!rumble.empty() && rumble.back().large == 90 * 257 && pad_colors[1][0] == 7 && !effects.empty(), "focused child rumble/LED/trigger routed only through parent");
		StopRuntimePadOutput(); Check(rumble.back().large == 0 && rumble.back().small == 0 && pad_colors[1][0] == 0, "loss/exit zeros physical output");
		const auto connections = RuntimePadSnapshot().connections;
		Disconnect(1);
		const auto fallback = RuntimePadSnapshot();
		Check(fallback.connected && !fallback.buttons && fallback.axes[0] == 128 && fallback.connections == connections + 1,
		      "physical unplug transitions to a neutral virtual keyboard pad");
		SetButton(HOST_INPUT_CONTROLLER_ID,PAD_BUTTON_CIRCLE,true); SetRightStick(HOST_INPUT_CONTROLLER_ID,210,50);
		const auto keyboard = RuntimePadSnapshot();
		Check(keyboard.connected && keyboard.buttons == PAD_BUTTON_CIRCLE && keyboard.axes[2] == 210,
		      "parent-window keyboard/mouse remains connected for child forwarding");
		Check(PadReadState(1,&ordinary) == 0 && !ordinary.buttons && ordinary.right_stick_x == 128,
		      "parent keyboard broker does not leak controls into background Home");
		Connect(1); Check(RuntimePadSnapshot().connected && RuntimePadSnapshot().connections == connections + 2, "broker reconnect increments connection generation after keyboard fallback");
	}
	ConfigureRuntimeControllerRole(false); Config::Shutdown();
	std::puts("subprocess controller focus/system PS/output checks passed"); return 0;
}
