#pragma once
#include <cstddef>
#include <cstdint>
#include <array>

typedef unsigned __int128 uint128_t;

#define CONFIG_N2048_K1          0
#define CONFIG_N2048_K2_MP       1
#define CONFIG_N4096_K2_MP       2
#define CONFIG_N2048_K1_COMP     3
#define CONFIG_N2048_K1_COMP_T257 4
#define CONFIG_N2048_K1_COMP_S128 5
#ifndef ACTIVE_CONFIG
#define ACTIVE_CONFIG CONFIG_N2048_K1
#endif

#ifndef PIR_EP_LOW_BITS
#define PIR_EP_LOW_BITS 0
#endif
#ifndef PIR_KS_LOW_BITS
#define PIR_KS_LOW_BITS 0
#endif

namespace DBConsts {

  constexpr size_t DB_SIZE_MB = 128;
  constexpr double NoiseStdDev = 2.55;

  constexpr bool FST_DIM_POW2 = true;

#if ACTIVE_CONFIG == CONFIG_N2048_K1
  constexpr size_t PolyDegree   = 2048;
  constexpr size_t DefaultL_EP = 5;
#ifdef PIR_EP_ROWS
  constexpr size_t L_EP         = PIR_EP_ROWS;
#else
  constexpr size_t L_EP         = DefaultL_EP;
#endif
  constexpr size_t L_KEY        = 8;
  constexpr size_t DefaultL_KS  = 8;
#ifdef PIR_KS_ROWS
  constexpr size_t L_KS         = PIR_KS_ROWS;
#else
  constexpr size_t L_KS         = DefaultL_KS;
#endif
  constexpr size_t TREE_HEIGHT  = 10;
  constexpr size_t PlainMod     = 14;
  constexpr size_t SmallQWidth  = 22;
  constexpr std::array<size_t, 1> RnsMods = {60};
  constexpr bool CompositeFirstDim = false;
  constexpr std::array<size_t, 2> FirstDimRNSMods = {0, 0};

#elif ACTIVE_CONFIG == CONFIG_N2048_K1_COMP || ACTIVE_CONFIG == CONFIG_N2048_K1_COMP_T257 || \
    ACTIVE_CONFIG == CONFIG_N2048_K1_COMP_S128
  constexpr size_t PolyDegree   = 2048;
  constexpr size_t DefaultL_EP = 6;
#ifdef PIR_EP_ROWS
  constexpr size_t L_EP         = PIR_EP_ROWS;
#else
  constexpr size_t L_EP         = DefaultL_EP;
#endif
  constexpr size_t L_KEY        = 10;
  constexpr size_t DefaultL_KS  = 8;
#ifdef PIR_KS_ROWS
  constexpr size_t L_KS         = PIR_KS_ROWS;
#else
  constexpr size_t L_KS         = DefaultL_KS;
#endif
  constexpr size_t TREE_HEIGHT  = 10;
  constexpr size_t PlainMod     = ACTIVE_CONFIG == CONFIG_N2048_K1_COMP ? 13 : 9;
  constexpr size_t SmallQWidth  = 22;
  constexpr bool S128 = ACTIVE_CONFIG == CONFIG_N2048_K1_COMP_S128;
  constexpr std::array<size_t, 1> RnsMods = {S128 ? size_t{52} : size_t{58}};
  constexpr bool CompositeFirstDim = true;
  constexpr std::array<size_t, 2> FirstDimRNSMods = {S128 ? size_t{26} : size_t{29},
                                                     S128 ? size_t{26} : size_t{29}};

#elif ACTIVE_CONFIG == CONFIG_N2048_K2_MP
  constexpr size_t PolyDegree   = 2048;
  constexpr size_t DefaultL_EP = 5;
#ifdef PIR_EP_ROWS
  constexpr size_t L_EP         = PIR_EP_ROWS;
#else
  constexpr size_t L_EP         = DefaultL_EP;
#endif
  constexpr size_t L_KEY        = 8;
  constexpr size_t DefaultL_KS  = 8;
#ifdef PIR_KS_ROWS
  constexpr size_t L_KS         = PIR_KS_ROWS;
#else
  constexpr size_t L_KS         = DefaultL_KS;
#endif
  constexpr size_t TREE_HEIGHT  = 10;
  constexpr size_t PlainMod     = 10;
  constexpr size_t SmallQWidth  = 22;
  constexpr std::array<size_t, 2> RnsMods = {29, 29};
  constexpr bool CompositeFirstDim = false;
  constexpr std::array<size_t, 2> FirstDimRNSMods = {0, 0};

#elif ACTIVE_CONFIG == CONFIG_N4096_K2_MP
  constexpr size_t PolyDegree   = 4096;
  constexpr size_t DefaultL_EP = 5;
#ifdef PIR_EP_ROWS
  constexpr size_t L_EP         = PIR_EP_ROWS;
#else
  constexpr size_t L_EP         = DefaultL_EP;
#endif
  constexpr size_t L_KEY        = 8;
  constexpr size_t DefaultL_KS  = 8;
#ifdef PIR_KS_ROWS
  constexpr size_t L_KS         = PIR_KS_ROWS;
#else
  constexpr size_t L_KS         = DefaultL_KS;
#endif
  constexpr size_t TREE_HEIGHT  = 10;
  constexpr size_t PlainMod     = 40;
  constexpr size_t SmallQWidth  = 50;
  constexpr std::array<size_t, 2> RnsMods = {60, 60};
  constexpr bool CompositeFirstDim = false;
  constexpr std::array<size_t, 2> FirstDimRNSMods = {0, 0};

#else
  #error "Unknown ACTIVE_CONFIG"
#endif


  constexpr size_t max_ct_mod_width() {
    size_t w = 0;
    for (size_t i = 0; i < RnsMods.size(); i++)
      if (RnsMods[i] > w) w = RnsMods[i];
    return w;
  }

  constexpr size_t EP_LOW_BITS = PIR_EP_LOW_BITS;
  constexpr size_t KS_LOW_BITS = PIR_KS_LOW_BITS;

  static_assert(RnsMods.size() <= 2,
                "Only K ≤ 2 is supported by the MP gadget.");

  static_assert(!CompositeFirstDim || RnsMods.size() == 1,
                "CompositeFirstDim requires a single composite ct modulus");

}

using db_coeff_t = std::conditional_t<DBConsts::max_ct_mod_width() <= 32,
                                      uint32_t, uint64_t>;

using inter_coeff_t = std::conditional_t<DBConsts::max_ct_mod_width() <= 32,
                                         uint64_t, uint128_t>;
