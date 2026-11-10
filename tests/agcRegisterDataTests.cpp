// Standalone ABI snapshot oracle. Compile with AGC_REFERENCE_INCLUDE pointing
// to an externally retained old table to generate a baseline; never bundle it.
#include "libs/agcRegisterData.h"
#include <algorithm>
#include <iostream>
#include <memory>

struct ShaderRegister { uint32_t offset; uint32_t value; };
struct CompactRegisterDefaults {
  ShaderRegister* tbl0_regs; uint32_t tbl0_register_count; const uint16_t* tbl0_pointer_offsets; uint32_t tbl0_pointer_count;
  ShaderRegister* tbl1_regs; uint32_t tbl1_register_count; const uint16_t* tbl1_pointer_offsets; uint32_t tbl1_pointer_count;
  ShaderRegister* tbl2_regs; uint32_t tbl2_register_count; const uint16_t* tbl2_pointer_offsets; uint32_t tbl2_pointer_count;
  ShaderRegister* tbl3_regs; uint32_t tbl3_register_count; const uint16_t* tbl3_pointer_offsets; uint32_t tbl3_pointer_count;
  uint32_t* types; uint32_t count;
};

#ifdef AGC_REFERENCE_INCLUDE
#include AGC_REFERENCE_INCLUDE
#endif

int main(int argc, char** argv) {
  if (argc != 3) { return 2; }
  try {
#ifndef AGC_REFERENCE_INCLUDE
    AgcRegisterData::Data<ShaderRegister, CompactRegisterDefaults> data(argv[1]);
#endif
    std::ofstream output(argv[2], std::ios::binary);
    if (!output) { return 3; }
    const auto word = [&](uint32_t value) {
      for (unsigned i = 0; i < 4; ++i) { output.put(static_cast<char>(value >> (8 * i))); }
    };
    std::map<const void*, uint32_t> identities;
    const auto identity = [&](const void* pointer) {
      if (!pointer) { word(0); return; }
      const auto [it, inserted] = identities.emplace(pointer, static_cast<uint32_t>(identities.size() + 1));
      word(it->second);
    };
    for (bool internal : {false, true}) {
      for (uint32_t version : {0u,1u,2u,3u,4u,5u,6u,7u,8u,9u,10u,11u,12u,13u,14u,UINT32_MAX}) {
        const unsigned index = version <= 13 ? version : 13;
#ifdef AGC_REFERENCE_INCLUDE
        auto* table = internal ? g_agc_internal_reg_defaults_by_version[index] : g_agc_public_reg_defaults_by_version[index];
#else
        auto* table = data.Get(internal, version);
        if (table != data.Get(internal, version)) { throw std::runtime_error("Unstable table lifetime"); }
#endif
        identity(table);
        const std::array<ShaderRegister*, 4> regs{table->tbl0_regs,table->tbl1_regs,table->tbl2_regs,table->tbl3_regs};
        const std::array<uint32_t, 4> counts{table->tbl0_register_count,table->tbl1_register_count,table->tbl2_register_count,table->tbl3_register_count};
        const std::array<const uint16_t*, 4> offsets{table->tbl0_pointer_offsets,table->tbl1_pointer_offsets,table->tbl2_pointer_offsets,table->tbl3_pointer_offsets};
        const std::array<uint32_t, 4> pointers{table->tbl0_pointer_count,table->tbl1_pointer_count,table->tbl2_pointer_count,table->tbl3_pointer_count};
        for (unsigned t = 0; t < 4; ++t) {
          identity(regs[t]); word(counts[t]); word(pointers[t]);
          for (unsigned i = 0; i < counts[t]; ++i) { word(regs[t][i].offset); word(regs[t][i].value); }
          for (unsigned i = 0; i < pointers[t]; ++i) {
            if (offsets[t][i] >= counts[t]) { throw std::runtime_error("Out-of-range pointer"); }
            word(offsets[t][i]);
          }
        }
        identity(table->types); word(table->count);
        for (unsigned i = 0; i < table->count * 3; ++i) { word(table->types[i]); }
      }
    }
    if (!output) { return 4; }
    std::cout << "PASS: complete public/internal ABI snapshot, versions 0..13 and fallback, stable identities\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
