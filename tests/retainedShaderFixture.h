#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <vector>

namespace RetainedShaderFixture {
inline bool Configured() {
  const char* root = std::getenv("KYTY_TEST_SHADER_FIXTURES");
  return root && *root;
}
// Test-input checksum only, not an authentication or runtime admission policy.
inline std::optional<std::vector<uint32_t>> Load(const char* name, size_t bytes, uint64_t checksum) {
  const char* root = std::getenv("KYTY_TEST_SHADER_FIXTURES");
  if (!root || !*root) {
    std::printf("SKIP: retained shader %s (set KYTY_TEST_SHADER_FIXTURES)\n", name);
    return std::nullopt;
  }
  const auto path = std::filesystem::u8path(root) / name;
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() != static_cast<std::streamoff>(bytes) || bytes % 4 != 0) {
    throw std::runtime_error("Missing or incorrectly sized retained shader: " + path.string());
  }
  std::vector<uint8_t> raw(bytes);
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes))) {
    throw std::runtime_error("Cannot read retained shader: " + path.string());
  }
  uint64_t digest = UINT64_C(0xcbf29ce484222325);
  for (const auto byte : raw) { digest = (digest ^ byte) * UINT64_C(0x100000001b3); }
  if (digest != checksum) { throw std::runtime_error("Retained shader checksum mismatch: " + path.string()); }
  std::vector<uint32_t> words(bytes / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    words[i] = uint32_t(raw[4 * i]) | (uint32_t(raw[4 * i + 1]) << 8) |
               (uint32_t(raw[4 * i + 2]) << 16) | (uint32_t(raw[4 * i + 3]) << 24);
  }
  std::printf("LOADED: retained shader %s (%zu bytes, checksum verified)\n", name, bytes);
  return words;
}
} // namespace RetainedShaderFixture
