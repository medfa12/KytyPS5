#include "common/archive.h"
#include "common/common.h"
#include "common/dateTime.h"
#include "common/debug.h"
#include "common/file.h"
#include "common/stringUtils.h"
#include "common/threads.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "kytyGitVersion.h"
#include "loader/childLaunch.h"
#include "loader/rnpsBundle.h"
#include "loader/systemContent.h"
#include "loader/runtimeChildProtocol.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <string_view>
#include <vector>
#include <fmt/format.h>
#include <magic_enum.hpp>

using namespace Common;
using namespace Emulator;

static std::string GetBuildString() {
	Date date = Date::FromMacros(std::string(__DATE__));

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	std::string type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	std::string type = "Release";
#else
	std::string type = "????";
#endif

	std::string compiler = Debug::GetCompiler() + "-" + Debug::GetLinker();

	std::string str =
	    fmt::format("{}, {}, ver = {}, git = {}, date = {}", type.c_str(), compiler.c_str(),
	                KYTY_VERSION, KYTY_GIT_VERSION, date.ToString().c_str());

	return str;
}

static void PrintUsage() {
	::printf("%s\n", GetBuildString().c_str());
	::printf("kyty_emulator --game <dir|elf|zar> [options]\n\n");
	::printf("Options:\n");
	::printf("  --game <dir|elf|zar>                 Game directory, ELF, or ZArchive to load.\n");
	::printf("  --game-patch <json>                  ETAHen cheat file.\n");
	::printf("  --fw-root <dir>                      Extracted firmware root (ps5-vsh system mounts).\n");
	::printf("  --vsh-session                        Enable same-session VSH application launching.\n");
  ::printf("  --guest-arg <value>                  Extra guest argv entry (repeatable).\n");
	::printf("  --launch-title <TITLE_ID>            Re-target --game fw dir to a system title.\n");
	::printf("  --rnps-resident <secs>               Keep Hermes runtime alive pumping live ticks.\n");
	::printf("  --rnps-param <k=v>                   RNPS startup param (repeatable).\n");
	::printf("  --co-elf <path>                      Co-host an extra ELF in-process (repeatable).\n");
	::printf("  --co-elf-arg <value>                 Argument for the preceding --co-elf (max 2 each).\n");
	::printf("  --mount <guest>=<host>               Expose host path at guest mount point (repeatable).\n");
	::printf("  --param-json <file>                  Application metadata override.\n");
	::printf("  --guest-env <name>=<value>           Set guest process environment (repeatable).\n");
	::printf("  --addcont-dir <dir>                  Add-ons (DLC) folder (default: none).\n");
	::printf(
	    "  --addcont <label>[=<dir>]            Add-on (DLC) by entitlement label with its data in\n"
	    "                                       <dir>, or license only without <dir>. Repeatable.\n");
	::printf("  --screen-width <num>                 Window width. Default: 1280.\n");
	::printf("  --screen-height <num>                Window height. Default: 720.\n");
	::printf("  --window-width <num>                 Host client width, independent of guest video.\n");
	::printf("  --window-height <num>                Host client height, independent of guest video.\n");
	::printf("  --vsh-app-backend subprocess|shared  VSH native app backend (Windows: subprocess).\n");
	::printf(
	    "  --user-name <name>                   Local user name (1-16 bytes). Default: Kyty.\n");
	::printf("  --user-id <num>                      Local user ID. Default: %d.\n",
	         Config::DEFAULT_USER_ID);
	::printf("  --devkit-activation-days <num>       Devkit activation horizon in days. Default: 30.\n");
	::printf("  --pup-expiration-days <num>          PUP trial days remaining (negative = eternal). Default: 3.\n");
	::printf("  --mic <name>                        Capture from this microphone; omit for silence.\n");
	::printf("  --controller-color <#RRGGBB>        Override the controller lightbar color.\n");
	::printf("  --controller-volume <0-100>         DualSense speaker volume. Default: 50.\n");
	::printf("  --controller-vibration <0-100>      DualSense vibration intensity. Default: 100.\n");
	::printf("  --keyboard-player <0-4>             Keyboard player; 0 shares player 1 with the gamepad. Default: 0.\n");
	::printf(
	    "  --present-mode <value>               Fifo, Mailbox, or Immediate. Default: Mailbox.\n");
	::printf(
	    "  --gpu <index>                        Vulkan physical device index. Default: auto.\n");
	::printf("  --fullscreen                         Run in borderless desktop fullscreen.\n");
	::printf("  --hdr                                Enable HDR10 output when supported.\n");
	::printf(
	    "  --hide-cursor                        Hide the cursor after 2 s idle. Default: off.\n");
	::printf(
	    "  --stretch                            Stretch the image to the window. Default: off.\n");
	::printf("  --no-splash                          Disable the startup splash screen.\n");
	::printf("  --vr                                 Enable the virtual VR headset.\n");
	::printf("  --trinity                            Enable experimental PS5 Pro mode for supported titles\n"
	         "                                       (PSSR uses linear scaling). Default: off.\n");
	::printf("  --amd-cpu                            Apply AMD CPU instruction patches.\n");
	::printf("  --guest-tsc                          Run the guest TSC at the PS5's 1.6 GHz.\n");
	::printf("  --vblank-frequency <num>             Virtual vblank frequency. Default: 60.\n");
	::printf("  --console-language <0-29>            Console language. Default: 1 (English US).\n");
	::printf("  --vulkan-validation                  Enable Vulkan validation.\n");
	::printf("  --network-connected                  Use the host local network.\n");
	::printf("  --emulate-psn                        Simulate PSN sign-in for compatibility.\n");
	::printf("  --gpu-assisted-validation            Bounds-check shader accesses on the GPU.\n"
	         "                                       Implies --vulkan-validation; very slow.\n");
	::printf("  --shader-validation                  Enable shader validation.\n");
	::printf("  --shader-cache                       Keep compiled shaders and pipelines between boots.\n");
	::printf("  --tessellation                      Draw tessellation patches; skipped by default.\n");
	::printf("  --occlusion-queries                  Count occlusion query samples on the GPU.\n");
	::printf("  --shader-optimization-type <value>   None, Size, or Performance.\n");
	::printf("  --shader-log-direction <value>       Silent, Console, or File.\n");
	::printf("  --shader-log-folder <path>           Shader log output folder.\n");
	::printf("  --command-buffer-dump                Enable command buffer dumps.\n");
	::printf("  --command-buffer-dump-folder <path>  Command buffer dump folder.\n");
	::printf("  --graphics-debug-dump                Enable graphics debug dumps.\n");
	::printf("  --printf-direction <value>           Silent, Console, or File.\n");
	::printf("  --printf-output-file <path>          Guest printf output file.\n");
	::printf("  --profile                            Enable the Tracy profiler.\n");
	::printf("  --spirv-debug-printf                 Enable SPIR-V debug printf.\n");
	::printf("  --readback-linear-images             Read back writable linear images on submit.\n");
	::printf("  --readback-tiled-images              Read back small writable tiled images on submit.\n");
	::printf("  --sync-raw-image-buffers             Synchronize raw reads of GPU images.\n");
	::printf("  --no-trophy-notifications            Disable trophy unlock toasts and their sound.\n");
	::printf(
	    "  --trophy-duration <seconds>          Trophy toast duration, 5-10. Default: 6.\n");
	::printf("  --perf-overlay                       Show the performance panel (F2 or Cmd+P).\n");
	::printf("  --playgo-hack                       Use the supplied PlayGo stub fallback.\n");
	::printf("  --skip-notice-screen                 Skip startup logos and notices in supported games.\n");
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	::printf("  --redzone                            Protect the guest SysV red zone.\n");
#endif
	::printf("  --keymap <Control=Input>             DualSense mapping; may be repeated.\n");
	::printf("  --rd                                 Enable RenderDoc capture.\n");
}

static bool NextArg(int argc, char* argv[], int& index, std::string& out) {
	if (index + 1 >= argc) {
		return false;
	}

	index++;
	out = argv[index];
	return true;
}

template <typename E>
static bool ParseEnum(const std::string& value, E& out) {
	auto enum_value = magic_enum::enum_cast<E>(value.c_str());
	if (!enum_value.has_value()) {
		return false;
	}

	out = enum_value.value();
	return true;
}

static bool ParseConsoleLanguage(const std::string& value, uint32_t& out) {
	uint32_t language = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), language);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    language > Config::MAX_CONSOLE_LANGUAGE) {
		return false;
	}
	out = language;
	return true;
}

static bool ParseUint32(const std::string& value, uint32_t& out) {
	uint32_t number   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseControllerColor(const std::string& value, Config::ControllerColor& out) {
	if (value.size() != 7 || value[0] != '#') {
		return false;
	}
	uint32_t rgb = 0;
	auto [end, error] = std::from_chars(value.data() + 1, value.data() + value.size(), rgb, 16);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = {static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8),
	       static_cast<uint8_t>(rgb)};
	return true;
}

static bool ParseInt32(const std::string& value, int32_t& out) {
	int32_t number    = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
	if (error != std::errc {} || end != value.data() + value.size()) {
		return false;
	}
	out = number;
	return true;
}

static bool ParseUserId(const std::string& value, int32_t& out) {
	int32_t user_id   = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), user_id);
	if (error != std::errc {} || end != value.data() + value.size() ||
	    !Config::IsConfiguredUserIdValid(user_id)) {
		return false;
	}
	out = user_id;
	return true;
}

using Config::ConfigOptions;

struct Flag {
	std::string_view name;
	bool ConfigOptions::* field;
	bool                  value = true;
};

static constexpr Flag FLAGS[] = {
    {"--rd", &ConfigOptions::renderdoc_enabled},
    {"--fullscreen", &ConfigOptions::fullscreen_enabled},
    {"--hdr", &ConfigOptions::hdr_enabled},
    {"--hide-cursor", &ConfigOptions::hide_cursor_enabled},
    {"--stretch", &ConfigOptions::stretch_enabled},
    {"--no-splash", &ConfigOptions::splash_enabled, false},
    {"--vr", &ConfigOptions::vr_enabled},
    {"--trinity", &ConfigOptions::pro_enabled},
    {"--amd-cpu", &ConfigOptions::amd_cpu_enabled},
    {"--guest-tsc", &ConfigOptions::guest_tsc_enabled},
    {"--network-connected", &ConfigOptions::network_connected},
    {"--emulate-psn", &ConfigOptions::psn_emulation_enabled},
    {"--vulkan-validation", &ConfigOptions::vulkan_validation_enabled},
    {"--gpu-assisted-validation", &ConfigOptions::gpu_assisted_validation_enabled},
    {"--shader-validation", &ConfigOptions::shader_validation_enabled},
    {"--shader-cache", &ConfigOptions::shader_cache_enabled},
    {"--tessellation", &ConfigOptions::tessellation_enabled},
    {"--occlusion-queries", &ConfigOptions::occlusion_queries_enabled},
    {"--command-buffer-dump", &ConfigOptions::command_buffer_dump_enabled},
    {"--graphics-debug-dump", &ConfigOptions::graphics_debug_dump_enabled},
    {"--profile", &ConfigOptions::profiler_enabled},
    {"--spirv-debug-printf", &ConfigOptions::spirv_debug_printf_enabled},
    {"--readback-linear-images", &ConfigOptions::readback_linear_images},
    {"--readback-tiled-images", &ConfigOptions::readback_tiled_images},
    {"--sync-raw-image-buffers", &ConfigOptions::sync_raw_image_buffers},
    {"--no-trophy-notifications", &ConfigOptions::trophy_enabled, false},
    {"--perf-overlay", &ConfigOptions::perf_overlay_enabled},
    {"--playgo-hack", &ConfigOptions::playgo_hack_enabled},
    {"--skip-notice-screen", &ConfigOptions::skip_notice_screen},
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
    {"--redzone", &ConfigOptions::red_zone_protection_enabled},
#endif
};

static bool ParseArgs(int argc, char* argv[], RunOptions& options, bool& show_help) {
	show_help = false;
	bool screen_width_supplied = false, screen_height_supplied = false;

	for (int i = 1; i < argc; i++) {
		std::string arg = std::string(argv[i]);
		std::string value;

		if (arg == "--help" || arg == "-h") {
			show_help = true;
			continue;
		}
		if (arg == "--vsh-session") {
			options.vsh_session = true;
			continue;
		}

		if (const auto* flag = std::ranges::find(FLAGS, arg, &Flag::name); flag != std::end(FLAGS)) {
			options.config.*flag->field = flag->value;
			continue;
		}

		if (!arg.starts_with("--")) {
			::printf("game input must be provided with --game\n");
			return false;
		}

		if (!NextArg(argc, argv, i, value)) {
			::printf("missing value for %s\n", arg.c_str());
			return false;
		}

		if (arg == "--game") {
			if (!options.app0_dir.empty()) {
				::printf("--game can only be specified once\n");
				return false;
			}

			value           = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

		if (Common::File::IsDirectoryExisting(path)) {
			options.app0_dir = path;
			options.elf      = "/app0/eboot.bin";
			// ps5-vsh: RNPS Hermes apps (NPXS40024 etc.) have no eboot.bin,
			// only manifest.json + application.ps.bundle.
			if (!Common::File::IsFileExisting(path / "eboot.bin") &&
			    Loader::SystemContentIsRnpsApp(path)) {
				options.elf = "/app0/application.ps.bundle";
				if (Loader::SystemContentLoadRnpsManifest(path)) {
					Loader::RnpsManifest manifest {};
					if (Loader::SystemContentGetRnpsManifest(&manifest)) {
						::printf("RNPS app: %s (%s) title %s RN %s backend %s\n",
						         manifest.applicationName.c_str(),
						         manifest.applicationVersion.c_str(),
						         manifest.titleId.c_str(), manifest.rnVersion.c_str(),
						         manifest.jsBackend.c_str());
					}
				}
				Loader::RnpsHeader header {};
				if (Loader::ParseRnpsHeader(path / "application.ps.bundle", &header)) {
					::printf("RNPS bundle: ver %u header %u flags %u hermes %s\n",
					         header.version, header.headerSize, header.flags,
					         Loader::HasHermesBytecode(path / "application.ps.bundle")
					             ? "plain"
					             : "encrypted");
				}
			}
		} else if (Common::IsSupportedArchive(path) && Common::File::IsFileExisting(path)) {
				const auto root = Common::MakeArchivePath(path);
				if (!Common::File::IsFileExisting(root / "eboot.bin")) {
					::printf("Archive does not contain eboot.bin: %s\n", value.c_str());
					return false;
				}
				options.app0_dir = root;
				options.elf      = "/app0/eboot.bin";
			} else if (Common::File::IsFileExisting(path)) {
				options.app0_dir = path.parent_path();

				if (options.app0_dir.empty()) {
					options.app0_dir = ".";
				}

				options.elf = std::filesystem::path("/app0") / path.filename();
			} else {
				::printf("--game must point to an existing directory, ELF, or archive: %s\n",
				         value.c_str());
				return false;
			}
		} else if (arg == "--guest-exec") {
			options.elf = Common::PathFromUtf8(Common::FixFilenameSlash(value));
			options.config.guest_arguments.assign(argv + i + 1, argv + argc);
			break;
		} else if (arg == "--game-patch") {
			if (!options.game_patch.empty()) {
				::printf("--game-patch can only be specified once\n");
				return false;
			}
			value = Common::FixFilenameSlash(value);
			const auto path = Common::PathFromUtf8(value);

			if (!Common::File::IsFileExisting(path)) {
				::printf("--game-patch must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.game_patch = path;
		} else if (arg == "--addcont-dir") {
			if (!options.addcont_dir.empty()) {
				::printf("--addcont-dir can only be specified once\n");
				return false;
			}
			options.addcont_dir = Common::PathFromUtf8(Common::FixFilenameSlash(value));
		} else if (arg == "--addcont") {
			auto entry = Common::ParseAddContent(value);
			if (!entry) {
				::printf("--addcont needs <16-character A-Z/0-9 label>[=<existing folder>]: %s\n",
				         value.c_str());
				return false;
			}
			options.addcont.push_back(std::move(*entry));
		} else if (arg == "--screen-width") {
			screen_width_supplied = true;
			if (!ParseUint32(value, options.config.screen_width) ||
			    options.config.screen_width == 0) {
				::printf("invalid screen width: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--screen-height") {
			screen_height_supplied = true;
			if (!ParseUint32(value, options.config.screen_height) ||
			    options.config.screen_height == 0) {
				::printf("invalid screen height: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--window-width" || arg == "--window-height") {
			uint32_t size = 0;
			if (!ParseUint32(value, size) || !size || size > 16384) { return false; }
			(arg == "--window-width" ? options.config.window_width : options.config.window_height) = size;
		} else if (arg == "--user-name") {
			if (value.empty() || value.size() > Config::MAX_USER_NAME_LENGTH) {
				::printf("invalid user name: must contain 1-%zu bytes\n",
				         Config::MAX_USER_NAME_LENGTH);
				return false;
			}
			options.config.user_name = value;
		} else if (arg == "--user-id") {
			if (!ParseUserId(value, options.config.user_id)) {
				::printf("invalid user ID: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--devkit-activation-days") {
			if (!ParseInt32(value, options.config.devkit_activation_days)) {
				::printf("invalid devkit activation days: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--pup-expiration-days") {
			if (!ParseInt32(value, options.config.pup_expiration_days)) {
				::printf("invalid PUP expiration days: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--mic") {
			options.config.audio_input_device = value;
		} else if (arg == "--controller-color") {
			Config::ControllerColor color {};
			if (!ParseControllerColor(value, color)) {
				::printf("invalid controller color (expected #RRGGBB): %s\n", value.c_str());
				return false;
			}
			options.config.controller_color = color;
		} else if (arg == "--controller-volume") {
			if (!ParseUint32(value, options.config.controller_speaker_volume) ||
			    options.config.controller_speaker_volume > 100) {
				::printf("invalid controller volume: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--controller-vibration") {
			if (!ParseUint32(value, options.config.controller_vibration_intensity) ||
			    options.config.controller_vibration_intensity > 100) {
				::printf("invalid controller vibration intensity: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--keyboard-player") {
			if (!ParseUint32(value, options.config.keyboard_player) ||
			    options.config.keyboard_player > static_cast<uint32_t>(Config::MAX_PLAYERS)) {
				::printf("invalid keyboard player: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--present-mode") {
			if (!ParseEnum(value, options.config.present_mode)) {
				::printf("invalid present mode: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--gpu") {
			if (!ParseInt32(value, options.config.gpu_index)) {
				::printf("invalid gpu index: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--vblank-frequency") {
			if (!ParseUint32(value, options.config.vblank_frequency)) {
				::printf("invalid vblank frequency: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--trophy-duration") {
			if (!ParseUint32(value, options.config.trophy_duration_seconds)) {
				::printf("invalid trophy duration: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--console-language") {
			if (!ParseConsoleLanguage(value, options.config.console_language)) {
				::printf("invalid console language: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-optimization-type") {
			if (!ParseEnum(value, options.config.shader_optimization_type)) {
				::printf("invalid shader optimization type: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-direction") {
			if (!ParseEnum(value, options.config.shader_log_direction)) {
				::printf("invalid shader log direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--shader-log-folder") {
			options.config.shader_log_folder = Common::PathFromUtf8(value);
		} else if (arg == "--command-buffer-dump-folder") {
			options.config.command_buffer_dump_folder = Common::PathFromUtf8(value);
		} else if (arg == "--printf-direction") {
			if (!ParseEnum(value, options.config.printf_direction)) {
				::printf("invalid printf direction: %s\n", value.c_str());
				return false;
			}
		} else if (arg == "--printf-output-file") {
			options.config.printf_output_file = Common::PathFromUtf8(value);
		} else if (arg == "--keymap") {
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid keymap: %s\n", value.c_str());
				return false;
			}
			options.config.keymap.push_back(value);
		} else if (arg == "--fw-root") {
			// ps5-vsh: extracted firmware root for system mounts.
			value = Common::FixFilenameSlash(value);
			if (!Common::File::IsDirectoryExisting(value)) {
				::printf("--fw-root must point to an existing directory: %s\n", value.c_str());
				return false;
			}
			options.fw_root = value;
		} else if (arg == "--vsh-app-backend") {
			if (value != "subprocess" && value != "shared") { ::printf("invalid VSH app backend\n"); return false; }
			options.vsh_subprocess = value == "subprocess";
		} else if (arg == "--vsh-child") {
			if (!options.runtime_child.empty() || value.empty()) { return false; }
			options.runtime_child = value;
		} else if (arg == "--vsh-parent-window") {
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.runtime_parent_window);
			if (parsed.ec != std::errc() || parsed.ptr != value.data() + value.size() || !options.runtime_parent_window) { return false; }
		} else if (arg == "--vsh-parent-pid") {
			if (!ParseUint32(value, options.runtime_parent_pid) || !options.runtime_parent_pid) { return false; }
		} else if (arg == "--guest-arg") {
			// ps5-vsh: extra guest argv (repeatable). Guest sees
			// argv[0]=KytyEmu then these in order.
			options.guest_args.push_back(value);
		} else if (arg == "--launch-title") {
			// ps5-vsh: re-target --game fw dir to a system title (single use).
			if (!options.launch_title.empty()) {
				::printf("--launch-title can only be specified once\n");
				return false;
			}
			if (value.empty()) {
				::printf("--launch-title requires a non-empty TITLE_ID\n");
				return false;
			}
			options.launch_title = value;
		} else if (arg == "--co-elf") {
			// ps5-vsh: co-host an extra ELF in-process (repeatable).
			if (value.empty()) {
				::printf("--co-elf requires a path\n");
				return false;
			}
			options.co_elfs.push_back({std::filesystem::path(value), {}});
		} else if (arg == "--co-elf-arg") {
			// ps5-vsh: up to two args attach to the preceding --co-elf.
			if (options.co_elfs.empty()) {
				::printf("--co-elf-arg needs a preceding --co-elf\n");
				return false;
			}
			auto& back = options.co_elfs.back();
			if (back.args.size() >= 2) {
				::printf("--co-elf-arg accepts at most two args per --co-elf\n");
				return false;
			}
			back.args.push_back(value);
		} else if (arg == "--rnps-resident") {
			// ps5-vsh: keep Hermes runtime alive pumping live ticks (single use).
			uint32_t secs = 0;
			if (!ParseUint32(value, secs) || secs == 0 || secs > 3600) {
				::printf("--rnps-resident requires 1-3600 seconds: %s\n", value.c_str());
				return false;
			}
			options.rnps_resident_secs = secs;
		} else if (arg == "--rnps-param") {
			// ps5-vsh: RNPS startup param k=v (repeatable, later wins).
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("invalid rnps-param (want k=v): %s\n", value.c_str());
				return false;
			}
			options.rnps_params.emplace_back(value.substr(0, split),
			                                 value.substr(split + 1));
		} else if (arg == "--mount") {
			// ps5-vsh: expose a host directory or file at a guest mount
			// point (voldemort parity, e.g. --mount
			// /app0/psm/Application=<stage>/Application). Repeatable.
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0 || split + 1 == value.size()) {
				::printf("--mount expects <guest_point>=<host_path>: %s\n", value.c_str());
				return false;
			}
			const std::string guest_point = value.substr(0, split);
			value                         = Common::FixFilenameSlash(value.substr(split + 1));
			const auto host_path          = Common::PathFromUtf8(value);
			if (!Common::File::IsDirectoryExisting(host_path) &&
			    !Common::File::IsFileExisting(host_path)) {
				::printf("--mount host path does not exist: %s\n", value.c_str());
				return false;
			}
			options.extra_mounts.emplace_back(guest_point, host_path);
		} else if (arg == "--param-json") {
			// ps5-vsh: application metadata override (voldemort parity).
			if (!options.param_json.empty()) {
				::printf("--param-json can only be specified once\n");
				return false;
			}
			value               = Common::FixFilenameSlash(value);
			const auto path     = Common::PathFromUtf8(value);
			if (!Common::File::IsFileExisting(path)) {
				::printf("--param-json must point to an existing file: %s\n", value.c_str());
				return false;
			}
			options.param_json = path;
		} else if (arg == "--guest-env") {
			// ps5-vsh: guest process environment k=v (voldemort parity,
			// e.g. MONO_THREADS_SUSPEND=preemptive). Repeatable.
			const auto split = value.find('=');
			if (split == std::string::npos || split == 0) {
				::printf("--guest-env expects <name>=<value>\n");
				return false;
			}
			options.config.guest_environment[value.substr(0, split)] = value.substr(split + 1);
		} else {
			::printf("unknown option: %s\n", arg.c_str());
			return false;
		}
	}

	if (options.vsh_session && (options.fw_root.empty() || options.co_elfs.empty())) {
		::printf("--vsh-session requires --fw-root and the co-hosted VSH boot chain\n");
		return false;
	}
	if (!options.runtime_child.empty() && (options.vsh_session || !options.co_elfs.empty())) {
		::printf("managed standalone child cannot co-host the VSH boot chain\n"); return false;
	}
	if ((!options.runtime_child.empty() && (!options.runtime_parent_window || !options.runtime_parent_pid)) ||
	    (options.runtime_child.empty() && (options.runtime_parent_window || options.runtime_parent_pid))) { return false; }
	if (options.vsh_session) {
		if (!screen_width_supplied) { options.config.screen_width = 1920; }
		if (!screen_height_supplied) { options.config.screen_height = 1080; }
		if (!options.config.window_width) { options.config.window_width = 1280; }
		if (!options.config.window_height) { options.config.window_height = 720; }
	}
	if (!options.launch_title.empty()) {
		// ps5-vsh: re-target an already-specified --game fw dir to a system
		// title without ShellCore. Normal form (--game <title dir> + --fw-root)
		// is untouched when --launch-title is absent.
		if (options.app0_dir.empty()) {
			::printf("--launch-title requires --game <fw dump root>\n");
			return false;
		}
		const std::string title = options.launch_title;
		// Treat --game as the fw dump base ONLY if --fw-root was not given.
		const std::filesystem::path base =
		    options.fw_root.empty() ? options.app0_dir : options.fw_root;
		const std::filesystem::path cand_b =
		    base / "system_b" / "vsh" / "app" / title;
		const std::filesystem::path cand_ex = base / "system_ex_b" / "app" / title;
		// ps5-vsh: RNPS system titles live under system_ex_b/rnps/apps/<title>
		// (manifest.json + application.ps.bundle, no eboot.bin).
		const std::filesystem::path cand_rnps = base / "system_ex_b" / "rnps" / "apps" / title;
		std::filesystem::path child;
		bool                  child_is_rnps = false;
		if (Common::File::IsDirectoryExisting(cand_b)) {
			child = cand_b;
		} else if (Common::File::IsDirectoryExisting(cand_ex)) {
			child = cand_ex;
		} else if (Common::File::IsDirectoryExisting(cand_rnps)) {
			child          = cand_rnps;
			child_is_rnps  = true;
		} else {
			::printf("--launch-title %s not found under %s/system_b/vsh/app, "
			         "%s/system_ex_b/app nor %s/system_ex_b/rnps/apps\n",
			         title.c_str(), Common::PathToString(base).c_str(),
			         Common::PathToString(base).c_str(),
			         Common::PathToString(base).c_str());
			return false;
		}
		Loader::ChildLaunchSpec spec {};
		if (!Loader::ProbeChildTitle(child.parent_path(), title, &spec) ||
		    !Loader::IsLaunchableTitle(spec.app0_dir)) {
			::printf("--launch-title %s is not launchable in %s\n", title.c_str(),
			         Common::PathToString(child).c_str());
			return false;
		}
		options.app0_dir = spec.app0_dir;
		// ps5-vsh: RNPS titles boot application.ps.bundle (no eboot.bin);
		// ELF titles keep /app0/eboot.bin.
		if (child_is_rnps || Loader::SystemContentIsRnpsApp(spec.app0_dir)) {
			options.elf = std::filesystem::path("/app0") / "application.ps.bundle";
			::printf("ps5-vsh: RNPS title %s -> /app0/application.ps.bundle\n",
			         title.c_str());
			// ps5-vsh: mirror the --game RNPS gate (manifest must be loaded
			// or emulator.cpp skips the Hermes path for the ELF loader).
			if (Loader::SystemContentLoadRnpsManifest(spec.app0_dir)) {
				Loader::RnpsManifest manifest {};
				if (Loader::SystemContentGetRnpsManifest(&manifest)) {
					::printf("RNPS app: %s (%s) title %s RN %s backend %s\n",
					         manifest.applicationName.c_str(),
					         manifest.applicationVersion.c_str(),
					         manifest.titleId.c_str(), manifest.rnVersion.c_str(),
					         manifest.jsBackend.c_str());
				}
			}
		} else {
			options.elf = std::filesystem::path("/app0") / "eboot.bin";
		}
		if (options.fw_root.empty()) {
			options.fw_root = base;
		}
		if (options.guest_args.empty() && title == "NPXS40038") {
			options.guest_args = Loader::BuildChildArgv("SceVideoCore4KMain", "release");
			::printf("ps5-vsh: default guest args for %s: SceVideoCore4KMain release\n",
			         title.c_str());
		}
	}

	if (options.config.gpu_assisted_validation_enabled) {
		options.config.vulkan_validation_enabled = true;
	}

	return show_help || (!options.app0_dir.empty() && !options.elf.empty());
}

static int Main(int argc, char* argv[]) {
	VirtualMemory::Init();
	InitializeThreads();

	RunOptions options;
	bool       show_help = false;

	if (argc < 2) {
		PrintUsage();
		return 0;
	}

	if (!ParseArgs(argc, argv, options, show_help)) {
		PrintUsage();
		return 1;
	}

	if (show_help) {
		PrintUsage();
		return 0;
	}

	Run(options);

	return 0;
}

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS

int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::string> utf8_args;
    utf8_args.reserve(static_cast<size_t>(argc));

    for (int index = 0; index < argc; index++) {
        const std::wstring_view wide(argv[index]);
        const std::u16string utf16(wide.begin(), wide.end());

        utf8_args.push_back(Common::Utf16ToUtf8(utf16));
    }

    std::vector<char*> utf8_argv;
    utf8_argv.reserve(utf8_args.size());

    for (auto& argument: utf8_args) {
        utf8_argv.push_back(argument.data());
    }

    return Main(argc, utf8_argv.data());
}

#else

int main(int argc, char* argv[]) {
    return Main(argc, argv);
}

#endif
