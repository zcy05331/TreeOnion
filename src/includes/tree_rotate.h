#pragma once

#include "server.h"
#include "tree_index.h"

#include <cstddef>
#include <span>

void tree_ct_add_inplace(RlweCt &acc, const RlweCt &x, const PirParams &scheme);

RlweCt mul_x_pow(const RlweCt &ct, size_t exponent_mod_2n,
                 const PirParams &scheme);

RlweCt rot_select(const RlweCt &ct, GSWCt &encrypted_bit, size_t shift,
                  PirServer &mux, const PirParams &scheme);

RlweCt private_rotate_level(RlweCt ct, const LevelPlan &plan,
                            std::span<GSWCt> gamma_selectors, PirServer &mux,
                            const PirParams &scheme);
