#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_RNPSBUNDLE_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_RNPSBUNDLE_H_

#include <cstdint>
#include <filesystem>

namespace Loader {

struct RnpsHeader {
	char     magic[8];
	uint32_t version;
	uint32_t headerSize;
	uint32_t fieldA;
	uint32_t fieldB;
	uint32_t variant0;
	uint32_t variant1;
	uint32_t size0;
	uint32_t size1;
	uint32_t size2;
	uint32_t flags;
};

bool IsRnpsBundle(const std::filesystem::path& file_name);
bool IsRawHbcBundle(const std::filesystem::path& file_name);
bool GetHbcVersion(const std::filesystem::path& file_name, uint32_t* out);
bool ParseRnpsHeader(const std::filesystem::path& file_name, RnpsHeader* header);
bool HasHermesBytecode(const std::filesystem::path& file_name);

} // namespace Loader

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_RNPSBUNDLE_H_ */
