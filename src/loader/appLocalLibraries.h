#ifndef KYTY_LOADER_APP_LOCAL_LIBRARIES_H
#define KYTY_LOADER_APP_LOCAL_LIBRARIES_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace Loader {

// App-local only: no firmware mounts, prefix matching or asset recursion. A
// missing dependency is left to symbol admission (it may be provided by HLE).
class AppLocalLibraries {
public:
	explicit AppLocalLibraries(const std::filesystem::path& root);
	static bool IsModuleFile(std::string_view name);
	static bool IsDependencyBasename(std::string_view name);
	bool Adjacent(std::vector<std::filesystem::path>* paths, std::string* error) const;
	bool Resolve(std::string_view basename, std::filesystem::path* path, std::string* error) const;

private:
	bool Confined(const std::filesystem::path& path, std::string* error) const;
	std::filesystem::path m_root;
};

} // namespace Loader
#endif
