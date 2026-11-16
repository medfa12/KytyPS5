#include "loader/appLocalLibraries.h"

#include <algorithm>
#include <array>

namespace Loader {
namespace {
constexpr std::array<const char*, 4> Directories {"", "sce_module", "sce_modules", "lib"};

std::string Lower(std::string_view name) {
	std::string result(name);
	for (auto& c: result) { if (c >= 'A' && c <= 'Z') { c += 'a' - 'A'; } }
	return result;
}

bool Fail(std::string* error, const std::string& message) {
	if (error != nullptr) { *error = message; }
	return false;
}
} // namespace

AppLocalLibraries::AppLocalLibraries(const std::filesystem::path& root)
    : m_root(std::filesystem::weakly_canonical(root).lexically_normal()) {
	if (m_root.filename().empty() && m_root.has_relative_path()) { m_root = m_root.parent_path(); }
}

bool AppLocalLibraries::IsModuleFile(std::string_view name) {
	const auto lower = Lower(name);
	return lower.ends_with(".prx") || lower.ends_with(".sprx");
}

bool AppLocalLibraries::IsDependencyBasename(std::string_view name) {
	if (name.empty() || name.size() > 255 || name.front() == '.' || !IsModuleFile(name)) { return false; }
	return std::all_of(name.begin(), name.end(), [](unsigned char c) {
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		       c == '_' || c == '-' || c == '+' || c == '.';
	});
}

bool AppLocalLibraries::Confined(const std::filesystem::path& path, std::string* error) const {
	std::error_code ec;
	const auto real = std::filesystem::canonical(path, ec);
	if (!ec) {
		for (const auto* dir: Directories) {
			if (real.parent_path() == (dir[0] == '\0' ? m_root : m_root / dir)) { return true; }
		}
	}
	return Fail(error, "app-local module escapes allowed directories: " + path.generic_string());
}

bool AppLocalLibraries::Adjacent(std::vector<std::filesystem::path>* paths, std::string* error) const {
	paths->clear();
	// Preserve VSH's existing eager adjacent-module semantics. lib/ is deliberately
	// NOT enumerated: only a concrete DT_NEEDED basename can select a file there.
	for (size_t i = 0; i < 3; ++i) {
		const auto directory = m_root / Directories[i];
		if (!std::filesystem::is_directory(directory)) { continue; }
		// Check the directory before listing it: a redirected sce_module must
		// not turn eager adjacency into an asset/lib/foreign-directory scan.
		if (std::filesystem::canonical(directory) != (i == 0 ? m_root : m_root / Directories[i])) {
			return Fail(error, "redirected adjacent app-local directory: " + directory.generic_string());
		}
		for (const auto& item: std::filesystem::directory_iterator(directory)) {
			const auto name = item.path().filename().string();
			if (!item.is_regular_file() || !IsModuleFile(name) || Lower(name).starts_with("libkernel")) { continue; }
			if (!IsDependencyBasename(name) || !Confined(item.path(), error)) {
				return Fail(error, "invalid adjacent app-local module: " + item.path().generic_string());
			}
			paths->push_back(item.path());
		}
	}
	std::sort(paths->begin(), paths->end());
	return true;
}

bool AppLocalLibraries::Resolve(std::string_view basename, std::filesystem::path* path, std::string* error) const {
	path->clear();
	if (!IsDependencyBasename(basename)) { return Fail(error, "invalid app-local DT_NEEDED basename: " + std::string(basename)); }
	// Native kernel files must not replace the emulator's host-backed boundary.
	if (Lower(basename).starts_with("libkernel")) { return true; }
	for (const auto* dir: Directories) {
		const auto candidate = m_root / dir / std::string(basename);
		if (!std::filesystem::exists(candidate)) { continue; }
		if (!std::filesystem::is_regular_file(candidate)) { return Fail(error, "app-local dependency is not a file: " + candidate.generic_string()); }
		if (!Confined(candidate, error)) { return false; }
		*path = candidate;
		return true;
	}
	return true;
}

} // namespace Loader
