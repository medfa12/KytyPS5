#include "retainedShaderFixture.h"

#include <array>
#include <string>

static void SetFixtureRoot(const char* path) {
#ifdef _WIN32
  _putenv_s("KYTY_TEST_SHADER_FIXTURES", path);
#else
  setenv("KYTY_TEST_SHADER_FIXTURES", path, 1);
#endif
}

int main(int argc, char** argv) {
  if (argc != 3) { return 2; }
  // This harness receives external retained input and scratch directories.
  const auto scratch = std::filesystem::path(argv[2]);
  std::filesystem::create_directories(scratch);
  SetFixtureRoot("");
  if (RetainedShaderFixture::Load("runtime-buffer-records.bin", 308,
                                 UINT64_C(0x77fab954771e46e2))) { return 3; }
  SetFixtureRoot(argv[1]);
  const auto retained = RetainedShaderFixture::Load("runtime-buffer-records.bin", 308,
                                                   UINT64_C(0x77fab954771e46e2));
  if (!retained || retained->size() != 77) { return 4; }
  std::vector<uint8_t> raw;
  for (auto word : *retained) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      raw.push_back(static_cast<uint8_t>(word >> shift));
    }
  }
  const auto testfile = scratch / "runtime-buffer-records.bin";
  SetFixtureRoot(argv[2]);
  const auto rejects = [&] {
    try {
      (void)RetainedShaderFixture::Load("runtime-buffer-records.bin", 308,
                                       UINT64_C(0x77fab954771e46e2));
      return false;
    } catch (const std::runtime_error&) { return true; }
  };
  if (!rejects()) { return 5; }
  for (size_t size = 0; size < raw.size(); ++size) {
    { std::ofstream out(testfile, std::ios::binary); out.write(reinterpret_cast<const char*>(raw.data()), size); }
    if (!rejects()) { return 6; }
  }
  for (size_t index = 0; index < raw.size(); ++index) {
    raw[index] ^= 1;
    { std::ofstream out(testfile, std::ios::binary); out.write(reinterpret_cast<const char*>(raw.data()), raw.size()); }
    if (!rejects()) { return 7; }
    raw[index] ^= 1;
  }
  std::filesystem::remove(testfile);
  std::puts("PASS: absent opt-in skips; retained input decodes; missing file, 308 truncations and 308 mutations reject");
  return 0;
}
