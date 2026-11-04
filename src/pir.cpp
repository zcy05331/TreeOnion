#include "pir.h"
#include "bv_keyswitch.h"
#include "database_constants.h"
#include "utils.h"
#include "hexl/hexl.hpp"

#include <cassert>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

void PirParams::init_composite_rns() {
  std::vector<size_t> rns_bits(DBConsts::FirstDimRNSMods.begin(),
                               DBConsts::FirstDimRNSMods.end());
  auto rns_primes = utils::generate_ntt_friendly_primes(rns_bits,
                                                        DBConsts::PolyDegree);
  const uint64_t q1 = rns_primes[0];
  const uint64_t q2 = rns_primes[1];
  const uint64_t crt_mod = q1 * q2;
  const uint64_t w1 = intel::hexl::MinimalPrimitiveRoot(2 * DBConsts::PolyDegree, q1);
  const uint64_t w2 = intel::hexl::MinimalPrimitiveRoot(2 * DBConsts::PolyDegree, q2);
  const uint64_t w_crt = utils::crt_combine(w1, q1, w2, q2);
  utils::register_ntt_root(DBConsts::PolyDegree, crt_mod, w_crt);
  rns_mods_ = {crt_mod};
  composite_rns_.enabled = true;
  composite_rns_.q1 = q1;
  composite_rns_.q2 = q2;
  composite_rns_.w1 = w1;
  composite_rns_.w2 = w2;
  composite_rns_.w_crt = w_crt;
  uint64_t q1_inv;
  if (!utils::try_invert_uint_mod(q1 % q2, q2, q1_inv))
    throw std::runtime_error("PirParams: q1 and q2 must be coprime");
  composite_rns_.q1_inv_mod_q2 = q1_inv;
}

PirParams::PirParams()
    : rns_mod_bits_(DBConsts::RnsMods.begin(), DBConsts::RnsMods.end()) {
  if constexpr (DBConsts::CompositeFirstDim) {
    init_composite_rns();
  } else {
    rns_mods_ = utils::generate_ntt_friendly_primes(rns_mod_bits_,
                                                    DBConsts::PolyDegree);
  }

  plain_mod_ = utils::generate_prime(DBConsts::PlainMod);

  small_q_ = utils::generate_ntt_friendly_primes(
                 {static_cast<int>(DBConsts::SmallQWidth)}, DBConsts::PolyDegree)[0];

  const size_t rns_cnt = rns_mods_.size();
  rns_tables_.r64_mod_q.resize(rns_cnt);
  for (size_t i = 0; i < rns_cnt; i++) {
    rns_tables_.r64_mod_q[i] = static_cast<uint64_t>(
        (static_cast<uint128_t>(1) << 64) % rns_mods_[i]);
  }
  if (rns_cnt == 2) {
    if (!utils::try_invert_uint_mod(rns_mods_[0] % rns_mods_[1],
                                    rns_mods_[1],
                                    rns_tables_.q0_inv_mod_q1)) {
      throw std::runtime_error("PirParams: coeff moduli not coprime");
    }
  }

  size_t ct_mod_width = get_ct_mod_width();
  if (l_ep_ == 0 || l_key_ == 0) {
    throw std::invalid_argument("PirParams: GSW gadget rows must be positive");
  }
  if (ep_low_bits_ >= ct_mod_width) {
    throw std::invalid_argument("PirParams: PIR_EP_LOW_BITS must be smaller than log2(q)");
  }
  if (ep_low_bits_ > 0 && K() != 1) {
    throw std::invalid_argument(
        "PirParams: approximate data GSW decomposition is only implemented for K=1");
  }
  if (ep_low_bits_ > 0 || l_ep_ != DBConsts::DefaultL_EP) {
    base_log2_ = (ct_mod_width - ep_low_bits_) / l_ep_ + 1;
  } else {
    base_log2_ = (ct_mod_width + l_ep_ - 1) / l_ep_;
  }
  if (base_log2_ >= 63) {
    throw std::invalid_argument(
        "PirParams: data GSW base_log2 exceeds the signed decomposition range");
  }
  base_log2_key_ = (ct_mod_width + l_key_ - 1) / l_key_;

  const size_t target_num_pt =
      DBConsts::DB_SIZE_MB * 1024 * 1024 / get_pt_size();
  apply_layout(
      {target_num_pt, DBConsts::TREE_HEIGHT, DBConsts::FST_DIM_POW2});
  DEBUG_PRINT("target_num_pt: " << target_num_pt_);
  DEBUG_PRINT("fst_dim_sz: " << fst_dim_sz_ << ", num_dims: " << num_dims_);
}

void PirParams::apply_layout(const PirLayoutConfig &layout) {
  if (layout.target_num_pt == 0) {
    throw std::invalid_argument("PirParams: target_num_pt must be positive");
  }

  constexpr size_t max_expansion_height =
      std::bit_width(DBConsts::PolyDegree) - 1;
  if (layout.expansion_height > max_expansion_height ||
      layout.expansion_height >= std::numeric_limits<size_t>::digits) {
    throw std::invalid_argument(
        "PirParams: expansion_height exceeds the ring automorphism capacity");
  }

  const auto [fst_dim_sz, num_dims] = utils::calculate_db_shape(
      layout.target_num_pt, l_ep_, layout.expansion_height,
      layout.fst_dim_pow2);
  const size_t other_dim_sz =
      utils::roundup_div(layout.target_num_pt, fst_dim_sz);
  if (other_dim_sz > std::numeric_limits<size_t>::max() / fst_dim_sz) {
    throw std::overflow_error("PirParams: rounded database shape overflows");
  }

  target_num_pt_ = layout.target_num_pt;
  expansion_height_ = layout.expansion_height;
  fst_dim_pow2_ = layout.fst_dim_pow2;
  fst_dim_sz_ = fst_dim_sz;
  num_dims_ = num_dims;
  num_pt_ = fst_dim_sz * other_dim_sz;
}

PirParams PirParams::with_layout(const PirLayoutConfig &layout) const {
  PirParams result(*this);
  result.apply_layout(layout);
  return result;
}

PirParams PirParams::with_query_shape(const PirQueryShapeConfig &shape) const {
  constexpr size_t max_expansion_height =
      std::bit_width(DBConsts::PolyDegree) - 1;
  if (shape.expansion_height > max_expansion_height) {
    throw std::invalid_argument(
        "PirParams: query shape exceeds the ring automorphism capacity");
  }
  if (shape.fst_dim_sz == 0) {
    throw std::invalid_argument(
        "PirParams: query shape needs a positive first dimension");
  }
  const size_t capacity = size_t{1} << shape.expansion_height;
  if (shape.fst_dim_sz > capacity ||
      shape.num_selector_bits >
          (capacity - shape.fst_dim_sz) / l_ep_) {
    throw std::invalid_argument(
        "PirParams: query shape does not fit the expansion capacity");
  }

  PirParams result(*this);
  result.fst_dim_sz_ = shape.fst_dim_sz;
  result.num_dims_ = 1 + shape.num_selector_bits;
  result.expansion_height_ = shape.expansion_height;
  result.num_pt_ = shape.fst_dim_sz;
  result.target_num_pt_ = shape.fst_dim_sz;
  return result;
}

bool PirParams::scheme_compatible(const PirParams &other) const {
  return scheme_identity() == other.scheme_identity();
}

PirSchemeIdentity PirParams::scheme_identity() const {
  static_assert(sizeof(double) == sizeof(uint64_t));
  static_assert(std::numeric_limits<double>::is_iec559,
                "scheme identity requires IEEE-754 double");
  PirSchemeIdentity id;
  id.ring_degree = DBConsts::PolyDegree;
  id.plaintext_modulus = plain_mod_;
  id.rns_modulus_bits = rns_mod_bits_;
  id.rns_moduli = rns_mods_;
  id.response_modulus = small_q_;
  id.response_modulus_bits = DBConsts::SmallQWidth;
  id.selector_gadget_rows = l_ep_;
  id.selector_base_log2 = base_log2_;
  id.selector_low_bits = ep_low_bits_;
  id.completion_key_gadget_rows = l_key_;
  id.completion_key_base_log2 = base_log2_key_;
  id.bv_ks_rows = bvks::gadget_rows();
  id.bv_ks_base_log2 = bvks::base_log2_for_scheme(*this);
  id.bv_ks_low_bits = bvks::low_bits_for_scheme(*this);
  id.r64_mod_q = rns_tables_.r64_mod_q;
  id.q0_inv_mod_q1 = rns_tables_.q0_inv_mod_q1;
  id.composite_first_dim = composite_rns_.enabled;
  id.composite_q1 = composite_rns_.q1;
  id.composite_q2 = composite_rns_.q2;
  id.composite_w1 = composite_rns_.w1;
  id.composite_w2 = composite_rns_.w2;
  id.composite_w_crt = composite_rns_.w_crt;
  id.composite_q1_inv_mod_q2 = composite_rns_.q1_inv_mod_q2;
  id.noise_std_dev_ieee754 = std::bit_cast<uint64_t>(get_noise_std_dev());
  return id;
}

const size_t PirParams::get_ct_mod_width() const {
  size_t ct_mod_width = 0;
  for (size_t i = 0; i < K(); ++i) {
    ct_mod_width += rns_mod_bits_[i];
  }
  return ct_mod_width;
}

void PirParams::print_params() const {
  PRINT_BAR;
  std::cout << "                       PIR PARAMETERS                         " << std::endl;
  PRINT_BAR;

  auto print_field = [](const std::string& label, const std::string& value, int label_width = 35) {
    std::string padded_label = label;
    padded_label.resize(label_width, ' ');
    std::cout << "  " << padded_label << "= " << value << std::endl;
  };

  auto print_field_num = [&print_field](const std::string& label, auto value) {
    print_field(label, std::to_string(value));
  };

  print_field_num("Database size (MB)", get_DBSize_MB());
  print_field_num("Physical storage (MB)", get_physical_storage_MB());
  print_field_num("Plaintext size (KB)", get_pt_size() / 1024);
  print_field_num("num_pt", num_pt_);
  print_field_num("fst_dim_sz", fst_dim_sz_);
  print_field_num("num_dims", num_dims_);
  print_field_num("expansion tree height", get_expan_height());

  print_field_num("l_ep (data RGSW)",  l_ep_);
  print_field_num("l_key (key RGSW)",  l_key_);
  print_field_num("l_ks (BV keyswitch)", DBConsts::L_KS);
  print_field_num("ep low bits", ep_low_bits_);
  print_field_num("ks low bits", DBConsts::KS_LOW_BITS);

  print_field_num("poly_modulus_degree", DBConsts::PolyDegree);

  std::string bits_str = "[";
  std::string mods_str = "[";
  for (size_t i = 0; i < rns_mods_.size(); ++i) {
    bits_str += std::to_string(rns_mod_bits_[i]);
    mods_str += std::to_string(rns_mods_[i]);
    if (i + 1 < rns_mods_.size()) { bits_str += " + "; mods_str += " + "; }
  }
  bits_str += "] = " + std::to_string(get_ct_mod_width()) + " bits";
  mods_str += "]";
  print_field("rns_mods (bits)", bits_str, 40);
  print_field("rns_mods", mods_str, 40);

  print_field("plain_modulus",
              std::to_string(plain_mod_) + " (log_2 ≈ " +
                  std::to_string(static_cast<int>(std::ceil(std::log2(plain_mod_)))) +
                  ")", 40);

  if (K() == 1) {
    print_field("small_q (mod-switch)",
                std::to_string(small_q_) + " (log_2 ≈ " +
                    std::to_string(static_cast<int>(std::ceil(std::log2(small_q_)))) +
                    ")", 40);
  }

  if (composite_rns_.enabled) {
    print_field("composite split q1*q2",
                std::to_string(composite_rns_.q1) + " * " +
                    std::to_string(composite_rns_.q2),
                40);
  }

  std::cout << "==============================================================" << std::endl;
}
