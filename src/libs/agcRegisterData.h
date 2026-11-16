#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace AgcRegisterData {

// Own all arrays for the lifetime of the emulator. No payload is embedded here.
// Register/Compact are the existing AGC ABI types; this loader is also usable
// by a standalone fixture test without creating a Vulkan context.
template <typename Register, typename Compact> class Data {
public:
  explicit Data(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() != 72258) {
      throw std::runtime_error("Missing or incorrectly sized AGC data; run retail firmware setup");
    }
    std::string text(72258, '\0');
    file.seekg(0);
    if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) {
      throw std::runtime_error("Cannot read AGC register data");
    }
    // Accidental-change checksum, not an authentication boundary. The setup
    // extractor validates the complete source module with SHA-256.
    uint64_t checksum = UINT64_C(0xcbf29ce484222325);
    for (unsigned char byte : text) { checksum = (checksum ^ byte) * UINT64_C(0x100000001b3); }
    if (checksum != UINT64_C(0x0530bb966aa00b8a)) {
      throw std::runtime_error("AGC data checksum mismatch; regenerate from matching firmware");
    }
    const auto document = nlohmann::json::parse(text);
    if (document.at("schema") != "kyty-agc-base-arrays-v1" ||
        document.at("source_sha256") != "a12b0f396a6464800a68006d6d64e00349a7b37fc3c1c08b6236e8ca564cbd6d" ||
        document.at("arrays").size() != 67) {
      throw std::runtime_error("Unsupported AGC data schema or source");
    }
    for (const auto& [name, array] : document.at("arrays").items()) {
      auto words = array.template get<std::vector<uint32_t>>();
      if (name.ends_with("_types")) {
        if (words.size() % 3 != 0) { throw std::runtime_error("Invalid AGC type count"); }
        types.emplace(name, std::move(words));
      } else {
        if (words.size() % 2 != 0) { throw std::runtime_error("Invalid AGC register count"); }
        auto& regs = registers[name];
        for (size_t i = 0; i < words.size(); i += 2) { regs.push_back({words[i], words[i + 1]}); }
      }
    }
    for (bool internal : {false, true}) {
      for (unsigned version : {0u, 4u, 5u, 7u, 8u, 9u, 10u, 11u}) {
        Build(internal, version);
      }
    }
    // Preserve the existing derived banks without modifying their shared bases.
    auto& public11 = tables.at(Key(false, 11));
    auto& effective11 = registers["public11_effective"];
    effective11 = registers.at(Key(false, 11) + "_tbl0_regs");
    PatchPublic(effective11);
    public11.tbl0_regs = effective11.data();
    auto public12 = tables.at(Key(false, 10));
    auto& effective12 = registers["public12_effective"];
    effective12 = registers.at(Key(false, 10) + "_tbl0_regs");
    PatchPublic(effective12);
    public12.tbl0_regs = effective12.data();
    tables.emplace(Key(false, 12), public12);
    auto internal13 = tables.at(Key(true, 11));
    auto& bank13 = registers["internal13_effective"];
    bank13 = registers.at(Key(true, 11) + "_tbl1_regs");
    for (auto& reg : bank13) {
      if (reg.offset == 0x81) { reg.value = 1; }
      else if (reg.offset == 0x101) { reg.value = 3; }
    }
    internal13.tbl1_regs = bank13.data();
    tables.emplace(Key(true, 13), internal13);
  }

  Data(const Data&) = delete;
  Data& operator=(const Data&) = delete;

  Compact* Get(bool internal, uint32_t version) {
    static constexpr unsigned public_versions[] = {0, 0, 0, 0, 4, 5, 5, 7, 8, 9, 10, 11, 12, 11};
    static constexpr unsigned internal_versions[] = {0, 0, 0, 0, 4, 5, 5, 7, 8, 9, 10, 11, 10, 13};
    version = version <= 13 ? version : 13;
    return &tables.at(Key(internal, internal ? internal_versions[version] : public_versions[version]));
  }

private:
  std::map<std::string, std::vector<Register>> registers;
  std::map<std::string, std::vector<uint32_t>> types;
  std::map<std::string, std::vector<uint16_t>> offsets;
  std::map<std::string, Compact> tables;

  static std::string Key(bool internal, unsigned version) {
    return std::string("g_agc_") + (internal ? "internal" : "public") +
           "_reg_defaults_v" + std::to_string(version);
  }

  static void PatchPublic(std::vector<Register>& regs) {
    for (auto& reg : regs) { if (reg.offset == 0x201) { reg.value |= 0x10000; } }
  }

  // Pointer indices describe the compact host layout, not register payloads.
  static std::vector<uint16_t> Offsets(bool internal, unsigned version, unsigned table, size_t count) {
    if (!count) { return {}; }
    if (internal) {
      if (table == 3) { return version <= 4 ? std::vector<uint16_t>{0, 1} : std::vector<uint16_t>{0, 1, 3}; }
      std::vector<uint16_t> result;
      for (size_t i = 0; i < count; ++i) { result.push_back(static_cast<uint16_t>(i)); }
      return result;
    }
    unsigned prefix = 0;
    std::vector<uint16_t> tail;
    if (table == 0) {
      switch (version) {
      case 0: prefix = 59; tail = {60,76,78,80,84,92,94,102,104,121,123,125,129,131,133,135,295,297,329,361,521}; break;
      case 4: prefix = 59; tail = {60,76,78,80,84,92,94,102,104,118,120,122,126,128,130,132,260,262,294,326,486}; break;
      case 5: prefix = 57; tail = {58,74,76,78,82,90,92,100,101,103,119,121,123,125,129,131,133,135,263,265,297,329,489}; break;
      case 10: prefix = 56; tail = {57,73,75,77,81,89,91,99,100,102,118,120,122,124,128,130,132,134,262,264,296,320,480}; break;
      case 11: prefix = 61; tail = {62,78,80,82,86,94,96,104,105,107,124,126,128,130,134,136,138,140,268,270,302,326,486}; break;
      default: prefix = 55; tail = {56,72,74,76,80,88,90,98,99,101,117,119,121,123,127,129,131,133,261,263,295,327,487}; break;
      }
    } else if (table == 1) {
      prefix = version == 11 ? 15 : 12;
      switch (version) {
      case 0: tail = {15,21,23,25,27,29,31,33,37,41,45,47,49,65,97,129}; break;
      case 4: tail = {15,18,20,22,24,26,28,30,34,38,42,44,46,62,94,126}; break;
      case 9: tail = {15,19,21,23,25,27,29,31,32,36,40,44,46,48,64,96,128}; break;
      case 11: tail = {18,21,23,25,27,29,31,33,34,38,42,46,48,50,66,98,130}; break;
      default: tail = {15,18,20,22,24,26,28,30,31,35,39,43,45,47,63,95,127}; break;
      }
    } else if (table == 2) {
      if (version <= 4) { prefix = 42; }
      else if (version == 5) { prefix = 42; tail = {43,51,67,71,72}; }
      else if (version == 11) { prefix = 15; tail = {16,24,40,44,52,53}; }
      else { prefix = 14; tail = {15,23,39,43,51,52}; }
    }
    std::vector<uint16_t> result;
    for (unsigned i = 0; i < prefix; ++i) { result.push_back(static_cast<uint16_t>(i)); }
    result.insert(result.end(), tail.begin(), tail.end());
    for (auto index : result) { if (index >= count) { throw std::runtime_error("AGC pointer offset out of range"); } }
    return result;
  }

  void Build(bool internal, unsigned version) {
    const auto key = Key(internal, version);
    std::array<Register*, 4> data{};
    std::array<uint32_t, 4> counts{}, pointer_counts{};
    std::array<const uint16_t*, 4> pointers{};
    for (unsigned i = 0; i < 4; ++i) {
      const auto name = key + "_tbl" + std::to_string(i) + "_regs";
      const auto found = registers.find(name);
      if (found == registers.end()) { continue; }
      auto& regs = found->second;
      data[i] = regs.data();
      counts[i] = static_cast<uint32_t>(regs.size());
      auto& indices = offsets[name];
      indices = Offsets(internal, version, i, regs.size());
      pointers[i] = indices.data();
      pointer_counts[i] = static_cast<uint32_t>(indices.size());
    }
    auto& type = types.at(key + "_types");
    tables.emplace(key, Compact{data[0], counts[0], pointers[0], pointer_counts[0],
                               data[1], counts[1], pointers[1], pointer_counts[1],
                               data[2], counts[2], pointers[2], pointer_counts[2],
                               data[3], counts[3], pointers[3], pointer_counts[3],
                               type.data(), static_cast<uint32_t>(type.size() / 3)});
  }
};
} // namespace AgcRegisterData
