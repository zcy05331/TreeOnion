#pragma once

#include "bv_keyswitch.h"
#include "pir.h"
#include "rlwe.h"

#include <cstddef>
#include <functional>

RlweCt project_keep_stride(RlweCt ct, size_t depth,
                           const bvks::BvGaloisKeys &keys,
                           const PirParams &scheme,
                           const std::function<void(size_t, const RlweCt &)> *
                               checkpoint = nullptr);
