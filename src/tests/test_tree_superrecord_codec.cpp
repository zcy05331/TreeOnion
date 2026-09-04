#include "tree_superrecord_test_util.h"

#include "tree_superrecord_common.h"

#include <limits>
#include <random>

using namespace tree_superrecord;
using tree_superrecord_test::throws_invalid;

namespace {

template <typename Fn>
bool throws_range(Fn &&fn) {
  try {
    fn();
  } catch (const std::out_of_range &) {
    return true;
  }
  return false;
}

EntryBits random_entry(size_t bits, std::mt19937_64 &rng) {
  EntryBits entry = zero_entry(bits);
  for (size_t bit = 0; bit < bits; ++bit) {
    if (rng() & 1U) set_bit(entry, bit, 1);
  }
  return entry;
}

}  // namespace

// Checked arithmetic rejections, entry validation, and exact round trips of
// the multi-plane codec across entry, digit, plane, and partial-byte
// boundaries (design specification, section 7).
void PirTest::test_tree_superrecord_codec() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  const uint64_t t = scheme.get_plain_mod();
  const size_t c = tree_superrecord_test::payload_bits(scheme);

  // ---- checked helpers ----
  const size_t max = std::numeric_limits<size_t>::max();
  require_test(throws_invalid([&] { (void)checked_add(max, 1, "x"); }),
               "addition overflow must be rejected");
  require_test(throws_invalid([&] { (void)checked_mul(max / 2 + 1, 2, "x"); }),
               "multiplication overflow must be rejected");
  require_test(throws_invalid([&] {
                 (void)checked_mul_u128(~static_cast<uint128>(0), 2, "x");
               }),
               "128-bit multiplication overflow must be rejected");
  require_test(throws_invalid([&] { (void)checked_pow2(64, "x"); }),
               "2^64 must be rejected");
  require_test(throws_invalid([&] {
                 (void)checked_bit_ceil((size_t{1} << 63) + 1, "x");
               }),
               "bit_ceil overflow must be rejected");
  require_test(throws_invalid([&] { (void)ceil_div(1, 0); }),
               "zero divisor must be rejected");
  require_test(throws_invalid([&] { (void)round_up(1, 0); }),
               "zero alignment must be rejected");
  require_test(throws_invalid([&] { (void)round_up(max, 16); }),
               "round-up overflow must be rejected");
  require_test(checked_add(2, 3, "x") == 5 && checked_mul(6, 7, "x") == 42 &&
                   checked_pow2(10, "x") == 1024 && ceil_div(7, 2) == 4 &&
                   ceil_div(8, 2) == 4 && round_up(342, 16) == 352 &&
                   round_up(352, 16) == 352 && round_up(0, 16) == 0 &&
                   exact_log2(32, "x") == 5 && bit_width(0) == 0 &&
                   bit_width(5) == 3,
               "checked helper values");
  require_test(throws_invalid([&] { (void)exact_log2(12, "x"); }),
               "non-power-of-two log2 must be rejected");

  // ---- entry validation ----
  {
    EntryBits e = zero_entry(13);
    require_test(e.bytes.size() == 2, "13 bits need two bytes");
    validate_entry(e);
    EntryBits extra = e;
    extra.bytes.push_back(0);
    require_test(throws_invalid([&] { validate_entry(extra); }),
                 "long byte array must be rejected");
    EntryBits shorter = e;
    shorter.bytes.pop_back();
    require_test(throws_invalid([&] { validate_entry(shorter); }),
                 "short byte array must be rejected");
    EntryBits high = e;
    high.bytes[1] = 0x20;  // bit 13, above the length
    require_test(throws_invalid([&] { validate_entry(high); }),
                 "nonzero padding bits must be rejected");
    EntryBits zero_len;
    require_test(throws_invalid([&] { validate_entry(zero_len); }),
                 "zero bit length must be rejected");
    require_test(throws_range([&] { (void)get_bit(e, 13); }),
                 "get_bit past the length must throw");
    require_test(throws_range([&] { set_bit(e, 13, 1); }),
                 "set_bit past the length must throw");
    set_bit(e, 5, 1);
    require_test(get_bit(e, 5) == 1, "set_bit sets");
    set_bit(e, 5, 0);
    require_test(get_bit(e, 5) == 0 && e == zero_entry(13),
                 "set_bit(...,0) clears");
  }

  // ---- codec spec ----
  {
    CodecSpec spec{256, c, 3, 64, minimal_plane_count(256, c, 3, 64), t};
    validate_codec_spec(spec);
    if (c == 12) {
      require_test(spec.plane_count == 1, "Bundle3 packs exactly one plane");
      require_test(spec.plane_count * c * 64 - 3 * 256 == 0,
                   "Bundle3 has no slot padding");
    }
    CodecSpec wide = spec;
    wide.plane_count += 1;
    require_test(throws_invalid([&] { validate_codec_spec(wide); }),
                 "nonminimal plane count must be rejected");
    CodecSpec too_wide = spec;
    too_wide.payload_bits_per_coeff = std::bit_width(t);  // 2^c > t
    too_wide.plane_count = minimal_plane_count(
        256, too_wide.payload_bits_per_coeff, 3, 64);
    require_test(throws_invalid([&] { validate_codec_spec(too_wide); }),
                 "payload width above the modulus must be rejected");
  }

  // ---- round trips ----
  std::mt19937_64 rng(0x636f646563ULL);
  const std::vector<size_t> entry_sizes = {1, 7, 13, 160, 256, 512, 8192, 30000};
  const std::vector<std::pair<size_t, size_t>> shapes = {
      {1, 1}, {3, 2}, {5, 64}, {2, 2048}, {7, 16}, {3, 64}};
  size_t round_trips = 0;
  for (size_t E : entry_sizes) {
    for (const auto &[h, G] : shapes) {
      CodecSpec spec{E, c, h, G, minimal_plane_count(E, c, h, G), t};
      validate_codec_spec(spec);
      std::vector<EntryBits> entries;
      for (size_t lane = 0; lane < h; ++lane) {
        entries.push_back(random_entry(E, rng));
      }
      const EncodedSuperRecord encoded = encode_super_record(entries, spec);
      require_test(encoded.size() == spec.plane_count,
                   "encoder must emit exactly P planes");
      for (const PlaneDigits &plane : encoded) {
        require_test(plane.size() == G, "each plane has G digits");
        for (uint64_t digit : plane) {
          require_test(digit < (uint64_t{1} << c) && digit < t,
                       "every digit is below 2^c and t");
        }
      }
      require_test(decode_super_record(encoded, spec) == entries,
                   "decode must invert encode");
      // Tail lanes: fewer entries than h decode to zero entries.
      if (h > 1) {
        std::vector<EntryBits> partial(entries.begin(), entries.end() - 1);
        const std::vector<EntryBits> decoded =
            decode_super_record(encode_super_record(partial, spec), spec);
        require_test(decoded.back() == zero_entry(E) &&
                         std::equal(partial.begin(), partial.end(),
                                    decoded.begin()),
                     "missing tail lane decodes to zero");
      }
      ++round_trips;
    }
  }
  BENCH_PRINT("codec round trips: " << round_trips << " (E x (h, G) shapes)");
  require_test(throws_invalid([&] {
                 CodecSpec spec{13, c, 2, 4, minimal_plane_count(13, c, 2, 4), t};
                 std::vector<EntryBits> three(3, zero_entry(13));
                 (void)encode_super_record(three, spec);
               }),
               "more entries than h must be rejected");
  require_test(throws_invalid([&] {
                 CodecSpec spec{13, c, 2, 4, minimal_plane_count(13, c, 2, 4), t};
                 std::vector<EntryBits> wrong(1, zero_entry(12));
                 (void)encode_super_record(wrong, spec);
               }),
               "entry of the wrong bit length must be rejected");
}
