// Included by KernelFileSystemTests.cpp. Uses real LNC registration/focus exports,
// real pad routing and a connected keyboard-backed controller. Only guest process
// execution is replaced by an accepting runtime launch backend.
void TestVshInputFocus(const std::filesystem::path& root) {
  namespace Lnc = Libs::LncUtil::Lnc;
  namespace Pad = Libs::Controller;
  namespace Signin = Libs::Dialog::SigninDialog;
  Loader::SymbolDatabase symbols;
  Libs::LncUtil::InitLncUtil_1(&symbols);
  Libs::InitPad_1(&symbols);
  Libs::InitSystemService_1(&symbols);
  const auto resolve = [&](const char* nid) {
    const auto* symbol = symbols.FindByNid(nid, Loader::SymbolType::Func);
    Check(symbol != nullptr, "VSH input/status export resolves");
    return symbol->vaddr;
  };
  const auto launch = reinterpret_cast<int(KYTY_SYSV_ABI *)(
      const char*, const char* const*, const Lnc::LaunchAppParam*)>(resolve("+nRJUD-7qCk"));
  const auto set_focus = reinterpret_cast<int(KYTY_SYSV_ABI *)(
      int32_t, uint64_t, uint64_t)>(resolve("v7DYuX0G5TQ"));
  const auto set_controller = reinterpret_cast<int(KYTY_SYSV_ABI *)(int32_t)>(resolve("X8gYbyLG1wk"));
  const auto device_open = reinterpret_cast<int(KYTY_SYSV_ABI *)(uint64_t, int, int)>(resolve("d7bXuEBycDI"));
  const auto capability = reinterpret_cast<int(KYTY_SYSV_ABI *)(uint64_t, uint32_t*)>(resolve("qtasqbvwgV4"));
  const auto ui_read = reinterpret_cast<int(KYTY_SYSV_ABI *)(int, void*)>(resolve("FYqcaosYiIo"));
  struct Status {
    int32_t event_num;
    bool overlay, background, vr;
    uint8_t reserved[127];
  };
  static_assert(sizeof(Status) == 136);
  const auto get_status = reinterpret_cast<int(KYTY_SYSV_ABI *)(Status*)>(resolve("rPo6tV8D9bM"));

  Loader::Program shell, app, other, module, direct, unregistered, accepted;
  shell.unique_id = 710001;
  app.unique_id = 710002;
  other.unique_id = 710003;
  module.unique_id = 710004;
  direct.unique_id = 710005;
  unregistered.unique_id = 710006;
  accepted.unique_id = 710007;
  shell.file_name = root / "NPXS40087" / "eboot.bin";
  app.file_name = root / "PPSA71002" / "eboot.bin";
  other.file_name = root / "PPSA71003" / "eboot.bin";
  app.application_owner = &app;
  other.application_owner = &other;
  module.application_owner = &app;
  unregistered.application_owner = &unregistered;
  accepted.application_owner = &accepted;
  Lnc::ConfigureVshRuntime(true);
  Lnc::RegisterProgramLaunch(&shell, root / "missing.json", true);
  Loader::RuntimeLinker::SetCurrentProgram(&shell);
  int32_t shell_id = -1;
  Check(Lnc::TryGetCurrentAppId(&shell_id), "ShellUI has a registered LNC identity");
  Lnc::SetRuntimeLaunchHandler([](const Lnc::PendingLaunch&) { return 0; });
  Lnc::LaunchAppParam param {};
  param.size = sizeof(param);
  param.user_id = Config::GetUserId();
  const auto start = [&](Loader::Program* program, const char* title) {
    const int id = launch(title, nullptr, &param);
    Check(id > 0, "fake runtime backend accepts real LNC launch request");
    const std::string guest_root = std::string("/user/app/") + title;
    Check(Lnc::RegisterRuntimeLaunchPath(id, guest_root + "/eboot.bin", guest_root + "/sce_sys") &&
              Lnc::RuntimeLaunchAccepted(id), "runtime launch accepted with registered path");
    Lnc::RegisterProgramLaunch(program, root / "missing.json", false, id);
    Check(Lnc::RuntimeLaunchStarted(id), "runtime launch reaches running state");
    return id;
  };
  const int app_id = start(&app, "PPSA71002");
  const int other_id = start(&other, "PPSA71003");
  Check(set_focus(app_id, 0, 0) == 0 && set_controller(app_id) == 0,
        "running application obtains app and controller focus");

  Pad::Initialize();
  Check(Pad::PadInit() == 0 && Pad::IsPlayerConnected(0), "keyboard fixture is genuinely connected");
  const int handle = Pad::PadOpen(Config::GetUserId(), 0, 0, nullptr);
  Check(handle > 0, "ordinary controller port opens");
  const auto identity = Libs::Mbus::VirtualPadDeviceInfo(Config::GetUserId());
  uint64_t device_id = 0;
  int32_t device_type = 0, owner = 0;
  std::memcpy(&device_id, identity.data(), sizeof(device_id));
  std::memcpy(&device_type, identity.data() + 12, sizeof(device_type));
  std::memcpy(&owner, identity.data() + 200, sizeof(owner));
  Check(device_id == 2 && device_type == 24 && owner == Config::GetUserId(),
        "keyboard/mouse enumerate as owned PS5 DualSense (MBUS24/PSM Bond9)");
  Check(device_open(device_id, 5, 0) == Pad::PAD_ERROR_INVALID_ARG &&
            device_open(device_id, 0, 0) == Pad::PAD_ERROR_INVALID_ARG &&
            device_open(device_id, 4, 1) == Pad::PAD_ERROR_INVALID_ARG &&
            device_open(device_id + 1, 4, 0) == Pad::PAD_ERROR_INVALID_HANDLE,
        "virtual DualSense rejects unrelated device-class probes and IDs");
  uint32_t features = 0xdeadbeef;
  Check(capability(device_id + 1, &features) == Pad::PAD_ERROR_INVALID_HANDLE && features == 0xdeadbeef &&
            capability(device_id, &features) == 0 && features == 0x1f,
        "capability lookup refers to the same DualSense and preserves failed outputs");
  const int system_handle = device_open(device_id, 4, 0);
  Check(system_handle == handle, "ShellUI opens the genuine DualSense port type4 on the shared input device");
  Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS | Pad::PAD_BUTTON_PS, true);
  Pad::SetAxis(Pad::HOST_INPUT_CONTROLLER_ID, Pad::Axis::LeftX, 17);
  Pad::SetRightStick(Pad::HOST_INPUT_CONTROLLER_ID, 43, 211);
  Pad::SetAxis(Pad::HOST_INPUT_CONTROLLER_ID, Pad::Axis::TriggerRight, 200);
  Pad::SetTouchPad(Pad::HOST_INPUT_CONTROLLER_ID, 0, true, 0.25f, 0.75f);
  const float gyro[] {1.0f, 2.0f, 3.0f};
  Pad::SetSensor(Pad::HOST_INPUT_CONTROLLER_ID, Pad::Sensor::Gyro, gyro, 1000);
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  Pad::PadData live {};
  Check(Pad::PadReadState(handle, &live) == 0 && live.connected && live.left_stick_x == 17 &&
            live.right_stick_x == 43 && live.right_stick_y == 211 &&
            live.analog_buttons_r2 == 200 && live.touch_data.touch_num == 1 &&
            live.angular_velocity_z == 3.0f && (live.buttons & Pad::PAD_BUTTON_CROSS) != 0,
        "module resolves focused application owner and receives live input");
  Status status {};
  Check(get_status(&status) == 0 && !status.overlay && !status.background,
        "focused app starts without system overlay");

  Check(set_controller(other_id) == 0, "controller focus transfers independently");
  const auto check_neutral = [&](const Pad::PadData& data) {
    Check(data.connected && data.connected_count == live.connected_count && data.buttons == 0 &&
              data.left_stick_x == 128 && data.left_stick_y == 128 &&
              data.right_stick_x == 128 && data.right_stick_y == 128 &&
              data.analog_buttons_l2 == 0 && data.analog_buttons_r2 == 0 &&
              data.touch_data.touch_num == 0 && data.angular_velocity_x == 0 &&
              data.angular_velocity_y == 0 && data.angular_velocity_z == 0 &&
              data.orientation_x == 0 && data.orientation_y == 0 && data.orientation_z == 0 &&
              data.orientation_w == 1 && data.acceleration_x == 0 &&
              data.acceleration_y == 1 && data.acceleration_z == 0,
          "background reader gets connected neutral controls, touch and motion");
  };
  const auto check_inactive = [&](Loader::Program* program) {
    Loader::RuntimeLinker::SetCurrentProgram(program);
    Pad::PadData data {};
    Check(Pad::PadReadState(handle, &data) == 0, "inactive runtime snapshot succeeds");
    check_neutral(data);
    std::array<Pad::PadData, 64> history {};
    Check(Pad::PadRead(handle, history.data(), 64) == 1, "inactive runtime receives one neutral report");
    check_neutral(history[0]);
    Check(history[0].timestamp >= data.timestamp && get_status(&status) == 0 &&
              status.overlay && status.background, "inactive runtime is timestamped and overlaid");
  };
  Check(set_focus(-1, 0, 0) == 0 && set_controller(-1) == 0, "clear both focus targets");
  check_inactive(&unregistered);
  check_inactive(&module);
  check_inactive(&other);
  Loader::RuntimeLinker::SetCurrentProgram(&shell);
  Check(Pad::PadReadState(system_handle, &live) == 0 && (live.buttons & Pad::PAD_BUTTON_PS) != 0,
        "no-target sentinel keeps the system observer alive");
  const int accepted_id = launch("PPSA71007", nullptr, &param);
  Check(accepted_id > 0 &&
            Lnc::RegisterRuntimeLaunchPath(accepted_id, "/user/app/PPSA71007/eboot.bin", "/user/app/PPSA71007/sce_sys") &&
            Lnc::RuntimeLaunchAccepted(accepted_id), "third runtime fixture reaches accepted state only");
  Lnc::RegisterProgramLaunch(&accepted, root / "missing.json", false, accepted_id);
  Check(set_focus(accepted_id, 0, 0) < 0 && set_controller(accepted_id) < 0,
        "accepted runtime cannot acquire display or controller focus");
  check_inactive(&accepted);
  Check(Lnc::RuntimeLaunchFailed(accepted_id, static_cast<int>(0x80940001)), "accepted fixture fails before entry");
  check_inactive(&accepted);
  Check(set_focus(app_id, 0, 0) == 0 && set_controller(other_id) == 0,
        "restore independent display/controller focus for read tests");
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  Pad::PadData neutral {};
  const auto before = Libs::LibKernel::KernelGetProcessTime();
  Check(Pad::PadReadState(handle, &neutral) == 0, "background snapshot succeeds");
  const auto after = Libs::LibKernel::KernelGetProcessTime();
  check_neutral(neutral);
  Check(neutral.timestamp >= before && neutral.timestamp <= after,
        "background snapshot has a current timestamp");
  Check(get_status(&status) == 0 && !status.overlay && !status.background,
        "controller focus alone does not change application visibility");
  Check(set_focus(shell_id, 0, 0) == 0 && get_status(&status) == 0 &&
            status.overlay && status.background, "VSH overlay backgrounds the runtime application");

  // ShellUI's privileged ABI uses a 112-byte buffer and a distinct count offset.
  Loader::RuntimeLinker::SetCurrentProgram(&shell);
  struct UiBuffer { std::array<uint8_t, 112> data {}; uint64_t guard = 0x123456789abcdef0; } ui;
  Pad::PadData ui_state {};
  Check(ui_read(system_handle, ui.data.data()) == 0, "ShellUI system-device read succeeds");
  std::memcpy(&ui_state, ui.data.data(), 88);
  Check(ui_state.connected && (ui_state.buttons & Pad::PAD_BUTTON_PS) != 0 &&
            ui.data[77] == live.connected_count && ui.guard == 0x123456789abcdef0,
        "system PS-button observation remains alive with the privileged ABI intact");
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  Check(ui_read(system_handle, ui.data.data()) == 0, "runtime UI-ABI snapshot succeeds");
  std::memcpy(&ui_state, ui.data.data(), 88);
  ui_state.connected_count = ui.data[77];
  check_neutral(ui_state);

  // Queue a press/release/PS sequence after draining the setup reports.
  Loader::RuntimeLinker::SetCurrentProgram(&other);
  Pad::ResetInputState();
  std::array<Pad::PadData, 64> reports {};
  Check(Pad::PadRead(handle, reports.data(), 64) == 1, "foreground drains reset report");
  Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS, true);
  Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, Pad::PAD_BUTTON_CROSS, false);
  Pad::SetButton(Pad::HOST_INPUT_CONTROLLER_ID, Pad::PAD_BUTTON_PS, true);
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  for (int read = 0; read < 2; ++read) {
    Check(Pad::PadRead(handle, reports.data(), 64) == 1, "background history read is one snapshot");
    check_neutral(reports[0]);
    Check(reports[0].timestamp >= neutral.timestamp, "neutral history timestamp is monotonic");
  }
  Loader::RuntimeLinker::SetCurrentProgram(&other);
  Check(Pad::PadRead(handle, reports.data(), 64) == 3 &&
            reports[0].buttons == Pad::PAD_BUTTON_CROSS && reports[1].buttons == 0 &&
            reports[2].buttons == Pad::PAD_BUTTON_PS,
        "background reads did not drain the foreground press/release history");
  Check(set_controller(app_id) == 0 && set_focus(app_id, 0, 0) == 0, "application regains both focuses");
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  Check(Pad::PadReadState(handle, &live) == 0 && live.buttons == Pad::PAD_BUTTON_PS &&
            get_status(&status) == 0 && !status.overlay && !status.background,
        "restoring focus restores live controls and clears VSH status");

  struct SigninParam { int32_t size = 16; int32_t user = Config::GetUserId(); int32_t reserved[2] {}; } signin;
  Check(Signin::SigninDialogInitialize() == 0 && Signin::SigninDialogOpen(&signin) == 0 &&
            get_status(&status) == 0 && status.overlay && status.background,
        "host dialog overlay/background survives a focused VSH application");
  Check(set_focus(shell_id, 0, 0) == 0 && Signin::SigninDialogClose() == 0 &&
            Signin::SigninDialogTerminate() == 0 && get_status(&status) == 0 &&
            status.overlay && status.background,
        "closing host overlay does not clear the independent VSH overlay");

  Loader::RuntimeLinker::SetCurrentProgram(&direct);
  Check(Pad::PadReadState(handle, &live) == 0 && live.buttons == Pad::PAD_BUTTON_PS &&
            get_status(&status) == 0 && !status.overlay && !status.background,
        "ordinary direct-game owner retains input/status behavior");
  Check(set_controller(other_id) == 0, "other app owns controller before disabling VSH");
  Check(set_focus(other_id, 0, 0) == 0 && Lnc::RuntimeLaunchQuiescentExit(other_id),
        "focused runtime exits and clears both focus targets");
  Check(Lnc::GetFocusedAppId() == -1 && Lnc::GetControllerFocusedAppId() == -1,
        "exit restores the system-only no-target sentinel");
  check_inactive(&other);
  check_inactive(&module);
  check_inactive(&unregistered);
  Check(set_focus(other_id, 0, 0) < 0 && set_controller(other_id) < 0,
        "exited runtime cannot reacquire focus");
  Loader::RuntimeLinker::SetCurrentProgram(&shell);
  Check(ui_read(system_handle, ui.data.data()) == 0, "ShellUI UI read survives focused runtime exit");
  std::memcpy(&ui_state, ui.data.data(), 88);
  Check(ui_state.connected && ui_state.buttons == Pad::PAD_BUTTON_PS,
        "exit does not suppress system PS-button observation");
  Check(Lnc::RuntimeLaunchQuiescentExit(app_id), "remaining runtime fixture exits quiescently");
  check_inactive(&module);
  Lnc::ConfigureVshRuntime(false);
  Loader::RuntimeLinker::SetCurrentProgram(&module);
  Check(Pad::PadReadState(handle, &live) == 0 && live.buttons == Pad::PAD_BUTTON_PS &&
            get_status(&status) == 0 && !status.overlay && !status.background,
        "ConfigureVshRuntime gates both controller and SystemService policy");
  Lnc::SetRuntimeLaunchHandler({});
  Loader::RuntimeLinker::SetCurrentProgram(nullptr);
  Check(Pad::PadClose(system_handle) == 0 && Pad::PadClose(handle) == 0, "release both pad owners");
  Pad::Shutdown();
}
