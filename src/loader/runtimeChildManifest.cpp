#include "loader/runtimeChildManifest.h"

namespace Loader::RuntimeChild {
namespace {
bool Within(const std::filesystem::path& path, const std::filesystem::path& base) {
	auto p = path.begin();
	for (auto b = base.begin(); b != base.end(); ++b, ++p) { if (p == path.end() || *p != *b) { return false; } }
	return true;
}
}
bool BuildManifest(const ManifestInput& in, const Key& key, Spawn* out, std::string* error) {
	const auto fail = [&](const char* why) { if (error) { *error = why; } return false; };
	if (!out || !key.Valid() || in.title.size() != 9 || in.title.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos ||
	    in.guest_arguments.size() > 15 || in.user < 0 || in.user_name.size() > 64 || in.output.empty()) { return fail("invalid child manifest identity"); }
	std::error_code ec;
	const auto app = std::filesystem::canonical(in.app, ec);
	if (ec || !std::filesystem::is_regular_file(app / "eboot.bin", ec)) { return fail("missing child executable"); }
	const auto metadata = std::filesystem::canonical(in.metadata, ec);
	if (ec || !Within(metadata, app) || !std::filesystem::is_regular_file(metadata, ec)) { return fail("missing or unconfined child metadata"); }
	auto firmware = in.firmware;
	if (!firmware.empty()) { firmware = std::filesystem::canonical(firmware, ec); if (ec) { return fail("missing firmware"); } }
	auto root = std::filesystem::weakly_canonical(std::filesystem::absolute(in.output, ec), ec);
	if (ec) { return fail("invalid output root"); }
	if (Within(root, app) || (!firmware.empty() && Within(root, firmware))) { return fail("writable child output cannot be inside content/firmware"); }
	std::filesystem::create_directories(root, ec);
	if (ec) { return fail("cannot prepare output root"); }
	const auto directory = root / (in.title + "-" + std::to_string(key.sequence) + "-" + std::to_string(key.nonce_lo) + "-" + std::to_string(key.nonce_hi));
	if (!std::filesystem::create_directory(directory, ec) || ec) { return fail("run directory already exists or cannot be created"); }
	// Dedicated title data, not ShellUI's live profile. No deletion/overwriting.
	const auto data = root / "title-data" / in.title;
	const auto prospective_data = std::filesystem::weakly_canonical(data, ec);
	if (ec || !Within(prospective_data, root)) { return fail("title data escapes dedicated child store"); }
	std::filesystem::create_directories(data, ec);
	if (ec) { return fail("cannot prepare title data"); }
	const auto actual_data = std::filesystem::canonical(data, ec);
	if (ec || !Within(actual_data, root)) { return fail("title data escapes dedicated child store"); }
	Spawn result; result.directory = directory; result.log = directory / "host.log"; result.key = key; result.exclusive_data = data;
	const auto wide = [](const std::string& value) { return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()), value.size())).wstring(); };
	const auto arg = [&](const wchar_t* name, const std::wstring& value) { result.arguments.emplace_back(name); result.arguments.push_back(value); };
	arg(L"--game", app.wstring()); arg(L"--param-json", metadata.wstring());
	if (!firmware.empty()) { arg(L"--fw-root", firmware.wstring()); }
	if (in.network_connected) { result.arguments.emplace_back(L"--network-connected"); }
	arg(L"--user-id", std::to_wstring(in.user)); arg(L"--user-name", wide(in.user_name));
	arg(L"--console-language", std::to_wstring(in.language));
	arg(L"--screen-width", L"1920"); arg(L"--screen-height", L"1080");
	arg(L"--window-width", L"1280"); arg(L"--window-height", L"720");
	arg(L"--printf-direction", L"File"); arg(L"--printf-output-file", (directory / "guest.log").wstring());
	arg(L"--mount", L"/data=" + data.wstring());
	for (const auto& value: in.guest_arguments) { if (value.size() > 4096 || value.find('\0') != std::string::npos) { return fail("invalid guest argument"); } arg(L"--guest-arg", wide(value)); }
	for (const auto& value: in.keymap) { arg(L"--keymap", wide(value)); }
	*out = std::move(result); return true;
}
}
