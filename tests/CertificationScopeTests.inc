void TestCertificationScope() {
  Loader::SymbolDatabase symbols;
  Libs::InitSystemService_1(&symbols);
  const auto* symbol = symbols.Find({"KgiwSrROsIQ", "SystemServiceYouTubeDeviceAuth", 1,
                                     "SystemService", 1, 1, Loader::SymbolType::Func});
  Check(symbol != nullptr, "resolve firmware certification-scope export");
  const auto get = reinterpret_cast<int(KYTY_SYSV_ABI *)(char*, size_t)>(symbol->vaddr);
  constexpr int invalid = static_cast<int>(0x80a10003u);
  constexpr char expected[] = "1p-cobalt-ps5";
  std::array<char, 32> output;
  for (size_t size = 0; size < sizeof(expected); ++size) {
    output.fill('!');
    Check(get(output.data() + 1, size) == invalid &&
              std::all_of(output.begin(), output.end(), [](char b) { return b == '!'; }),
          "undersized scope output preserves bytes");
  }
  Check(get(nullptr, sizeof(expected)) == invalid, "null scope buffer rejected");
  for (size_t size : {sizeof(expected), size_t{30}}) {
    output.fill('!');
    Check(get(output.data() + 1, size) == 0 &&
              std::memcmp(output.data() + 1, expected, sizeof(expected)) == 0 &&
              output.front() == '!' &&
              std::all_of(output.begin() + 1 + sizeof(expected), output.end(), [](char b) { return b == '!'; }),
          "scope getter writes exact firmware constant and terminator");
  }

  const auto* signing = symbols.Find({"Oxh0CSsTZ78", "SystemServiceYouTubeDeviceAuth", 1,
                                      "SystemService", 1, 1, Loader::SymbolType::Func});
  Check(signing != nullptr, "resolve separate certification-signing export");
  const auto sign = reinterpret_cast<int(KYTY_SYSV_ABI *)(const char*, size_t, const void*, size_t, void*, size_t)>(signing->vaddr);
  const std::array<char, 12> algorithm {'H','M','A','C','-','S','H','A','2','5','6','!'};
  std::array<uint8_t, 128> message {};
  std::array<uint8_t, 64> digest;
  digest.fill(0xa5);
  const auto before = digest;
  for (size_t algorithm_size : {size_t{10}, size_t{11}, size_t{12}}) {
    for (size_t message_size : {size_t{0}, size_t{1}, size_t{128}, size_t{129}, SIZE_MAX}) {
      for (size_t capacity : {size_t{31}, size_t{32}, size_t{62}}) {
        const bool valid = algorithm_size >= 11 && message_size >= 1 && message_size <= 128 && capacity >= 32;
        Check(sign(algorithm.data(), algorithm_size, message.data(), message_size, digest.data() + 1, capacity) ==
                  (valid ? static_cast<int>(0x80a10001u) : invalid) && digest == before,
              "signing validates full-width spans and preserves output when service unavailable");
      }
    }
  }
  Check(sign(nullptr, 11, message.data(), 1, digest.data(), 32) == invalid &&
            sign(algorithm.data(), 11, nullptr, 1, digest.data(), 32) == invalid &&
            sign(algorithm.data(), 11, message.data(), 1, nullptr, 32) == invalid &&
            sign("HMAC-SHA257", 11, message.data(), 1, digest.data(), 32) == invalid && digest == before,
        "signing rejects null inputs and wrong algorithms without fabricated digest");
}
