// Include inside VirtualMemoryAllocationTests' fixture namespace. Top-level add
// libs/kernelNormalizePath.h; link kernelNormalizePath.cpp and dispatch
// TestKernelNormalizePath(). No filesystem, native firmware, DRM state or guest
// process is required. Parent owns CMake/main integration and execution.
void TestKernelNormalizePath() {
 namespace Path = Libs::KernelNormalizePath;
 constexpr const char* test = "KernelNormalizePath";
 constexpr int invalid = static_cast<int>(0x80020016u);
 constexpr int too_long = static_cast<int>(0x8002003fu);
 Loader::SymbolDatabase symbols;
 Path::InitKernelNormalizePath_1(&symbols);
 const auto* exact = symbols.FindRegistration({"KB7Vc2+5S54", "libkernel", 1, "libkernel", 1, 1,
                                              Loader::SymbolType::Func}, "Normalize");
 const auto* fallback = symbols.FindByNid("KB7Vc2+5S54", Loader::SymbolType::Func);
 Check(test, exact && fallback && exact->vaddr == fallback->vaddr &&
             exact->vaddr == reinterpret_cast<uint64_t>(&Path::Normalize), "registered real normalizer, not zero-output stub");
 auto normalize = reinterpret_cast<decltype(&Path::Normalize)>(exact->vaddr);
 std::array<char, 1100> output {};
 const auto reset = [&] { output.fill(static_cast<char>(0x5a)); };
 const auto unchanged = [&] {
  return std::all_of(output.begin(), output.end(), [](char value) { return value == 0x5a; });
 };
 struct Case { const char* input; const char* expected; };
 constexpr Case cases[] = {
  {"/download0/drm/widevine", "/download0/drm/widevine"},
  {"/download0//drm/./cache/../widevine", "/download0/drm/widevine"},
  {"/", "/"}, {"////", "/"}, {"/.", "/"}, {"/..", "/"},
  {"/../../x", "/x"}, {"/a/..", "/"}, {"/a/../../b", "/b"}, {"/a/b/..", "/a"},
  {"/a/b/../", "/a/"}, {"/a/.", "/a"}, {"/a/./", "/a/"},
  {"/a//b///", "/a/b/"}, {"/a/.../b", "/a/.../b"},
  // POSIX '/' is the only separator. No Windows rewriting or host resolution.
  {"/a\\b/c:de", "/a\\b/c:de"}, {"/not-created/child", "/not-created/child"}
 };
 for (const auto& item: cases) {
  reset();
  const size_t expected_bytes = std::strlen(item.expected) + 1;
  Check(test, normalize(item.input, std::strlen(item.input) + 1, output.data(), expected_bytes) == 0 &&
              std::strcmp(output.data(), item.expected) == 0 && output[expected_bytes] == 0x5a,
        "firmware lexical normalization, exact output capacity, no writes past NUL");
 }
 reset();
 const char input[] = "/download0/drm/widevine";
 Check(test, normalize(input, sizeof(input) - 1, output.data(), output.size()) == invalid && unchanged(),
       "source byte bound must include terminator");
 Check(test, normalize(input, sizeof(input), output.data(), sizeof(input) - 1) == invalid && unchanged(),
       "insufficient destination rejects without publishing partial path");
 for (const char* bad: {"", "relative/path", "C:/download0/drm/widevine"}) {
  Check(test, normalize(bad, std::strlen(bad) + 1, output.data(), output.size()) == invalid && unchanged(),
        "empty/relative/host paths are not absolute guest paths");
 }
 Check(test, normalize(nullptr, 1, output.data(), output.size()) == invalid && unchanged() &&
             normalize(input, 0, output.data(), output.size()) == invalid && unchanged() &&
             normalize(input, sizeof(input), nullptr, 2) == invalid &&
             normalize(input, sizeof(input), output.data(), 1) == invalid && unchanged(), "native pointer/capacity validation");
 Check(test, normalize(reinterpret_cast<const char*>(1), 20, output.data(), output.size()) == invalid && unchanged(),
       "unreadable source fails closed, no blind dereference");
 Check(test, normalize(input, UINT64_MAX, output.data(), output.size()) == invalid && unchanged() &&
             normalize(input, sizeof(input), output.data(), UINT64_MAX) == invalid && unchanged(),
       "declared input/output address overflow rejects");
 std::array<char, 64> overlapping {};
 std::memcpy(overlapping.data(), "/a", 3);
 const auto before = overlapping;
 Check(test, normalize(overlapping.data(), 3, overlapping.data(), 64) == invalid && overlapping == before &&
             normalize(overlapping.data(), 32, overlapping.data() + 16, 32) == invalid && overlapping == before,
       "native disjoint-declared-range rule includes same pointer and overlap beyond actual NUL");
 Check(test, normalize(overlapping.data(), 3, overlapping.data() + 3, 3) == 0 &&
             std::strcmp(overlapping.data() + 3, "/a") == 0, "adjacent declared ranges allowed");
 std::string longest = "/" + std::string(1022, 'a');
 reset();
 Check(test, normalize(longest.c_str(), longest.size() + 1, output.data(), output.size()) == 0 &&
             std::strcmp(output.data(), longest.c_str()) == 0 && output[1024] == 0x5a, "native 1023-byte normalized path fits");
 reset();
 longest += '/';
 Check(test, normalize(longest.c_str(), longest.size() + 1, output.data(), output.size()) == too_long && unchanged(),
       "native scratch bound includes final trailing slash");
 std::string intermediate = "/" + std::string(1023, 'a') + "/..";
 Check(test, normalize(intermediate.c_str(), intermediate.size() + 1, output.data(), output.size()) == too_long && unchanged(),
       "oversized intermediate path fails even when later dotdot would reduce it");
 std::string collapsible(4096, '/'); collapsible += "download0/drm/widevine";
 Check(test, normalize(collapsible.c_str(), collapsible.size() + 1, output.data(), output.size()) == 0 &&
             std::strcmp(output.data(), input) == 0, "1024 scratch limit is not guessed source-record size");
 // Reproduce Esvm 1dc50's immediate predicate without fake CDM state: the
 // actual normalized storage path must be populated, not merely return zero.
 reset();
 Check(test, normalize(input, 0x18, output.data(), 0x400) == 0 &&
             std::strlen(output.data()) == 0x17 && std::memcmp(output.data(), "/download0/", 11) == 0,
       "native Widevine storage-path validation now receives genuine lexical output");
}
