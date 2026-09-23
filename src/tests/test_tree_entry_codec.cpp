#include "tests.h"
#include "tree_entry_codec.h"
#include "tree_entry_test_util.h"

#include <array>
#include <bit>
#include <random>
#include <span>
#include <vector>

using namespace tree_entry_codec;

namespace {

template <typename Fn>
bool throws_invalid(Fn &&fn) {
  try {
    fn();
  } catch (const std::invalid_argument &) {
    return true;
  }
  return false;
}

template <typename Fn>
bool throws_range(Fn &&fn) {
  try {
    fn();
  } catch (const std::out_of_range &) {
    return true;
  }
  return false;
}

EntryBits random_entry(size_t bits, ChaChaRng &rng) {
  EntryBits entry = zero_entry(bits);
  for (size_t bit = 0; bit < bits; ++bit) {
    if (rng() & 1U) set_bit(entry, bit, 1);
  }
  return entry;
}

EncodedEntry per_bit_oracle(const EntryBits &entry, const CodecSpec &spec) {
  validate_codec_spec(spec);
  validate_entry(entry);
  require_test(entry.bit_length == spec.entry_bits,
               "oracle fixture entry width");
  const size_t c = spec.payload_bits_per_coeff;
  const size_t plane_bits = c * spec.record_coeffs;
  EncodedEntry planes(spec.plane_count, PlaneDigits(spec.record_coeffs, 0));
  for (size_t bit = 0; bit < spec.entry_bits; ++bit) {
    if (get_bit(entry, bit) == 0) continue;
    const size_t plane = bit / plane_bits;
    const size_t within = bit % plane_bits;
    planes[plane][within / c] |= uint64_t{1} << (within % c);
  }
  return planes;
}

EntryBits entry_with_positions(size_t bits,
                               std::span<const size_t> positions) {
  EntryBits entry = zero_entry(bits);
  for (size_t bit : positions) set_bit(entry, bit, 1);
  return entry;
}

}  // namespace

void PirTest::test_tree_entry_codec() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  const uint64_t t = scheme.get_plain_mod();
  const size_t c = tree_entry_test::payload_bits(scheme);

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
    high.bytes[1] = 0x20;
    require_test(throws_invalid([&] { validate_entry(high); }),
                 "nonzero padding bits must be rejected");
    require_test(throws_range([&] { (void)get_bit(e, 13); }),
                 "get_bit past the length must throw");
    require_test(throws_range([&] { set_bit(e, 13, 1); }),
                 "set_bit past the length must throw");
    set_bit(e, 5, 1);
    require_test(get_bit(e, 5) == 1, "set_bit sets");
    set_bit(e, 5, 0);
    require_test(get_bit(e, 5) == 0 && e == zero_entry(13),
                 "set_bit(...,0) clears");
    const std::vector<uint8_t> source = {0xA5, 0xE3};
    const EntryBits copied = entry_from_bytes(source, 13);
    require_test(copied.bytes[0] == 0xA5 && copied.bytes[1] == 0x03,
                 "entry_from_bytes masks high bits in the final partial byte");
  }

  {
    CodecSpec spec{256, c, 8, minimal_plane_count(256, c, 8), t};
    validate_codec_spec(spec);
    if (c == 12) {
      require_test(spec.plane_count == 3, "32B g8 uses P=3");
    }
    CodecSpec wide = spec;
    wide.plane_count += 1;
    require_test(throws_invalid([&] { validate_codec_spec(wide); }),
                 "nonminimal plane count must be rejected");
    CodecSpec too_wide = spec;
    too_wide.payload_bits_per_coeff = std::bit_width(t);
    too_wide.plane_count =
        minimal_plane_count(256, too_wide.payload_bits_per_coeff, 8);
    require_test(throws_invalid([&] { validate_codec_spec(too_wide); }),
                 "payload width above the modulus must be rejected");
  }

  if (c == 12 && scheme.get_poly_degree() == 2048) {
    const std::array<std::pair<size_t, size_t>, 5> expected = {
        std::pair<size_t, size_t>{128, 4},
        std::pair<size_t, size_t>{256, 8},
        std::pair<size_t, size_t>{384, 32},
        std::pair<size_t, size_t>{512, 16},
        std::pair<size_t, size_t>{1024, 32},
    };
    const std::array<size_t, 5> expected_p = {3, 3, 1, 3, 3};
    for (size_t i = 0; i < expected.size(); ++i) {
      const auto choice =
          choose_layout(expected[i].first, c, scheme.get_poly_degree());
      require_test(choice.g == expected[i].second &&
                       choice.plane_count == expected_p[i] &&
                       choice.rho == scheme.get_poly_degree() / choice.g,
                   "final h1 layout policy");
    }
  }

  ChaChaRng rng(0x636f646563ULL);
  const std::vector<size_t> entry_sizes = {1, 7, 13, 160, 256, 512, 8192,
                                           30000};
  const std::vector<size_t> record_widths = {1, 2, 7, 16, 64, 2048};
  size_t round_trips = 0;
  for (size_t E : entry_sizes) {
    for (size_t G : record_widths) {
      CodecSpec spec{E, c, G, minimal_plane_count(E, c, G), t};
      EntryBits entry = random_entry(E, rng);
      const EncodedEntry encoded = encode_entry(entry, spec);
      require_test(encoded == per_bit_oracle(entry, spec),
                   "encoder must match the independent per-bit oracle");
      require_test(encoded.size() == spec.plane_count,
                   "encoder must emit exactly P planes");
      for (const PlaneDigits &plane : encoded) {
        require_test(plane.size() == G, "each plane has G digits");
        for (uint64_t digit : plane) {
          require_test(digit < (uint64_t{1} << c) && digit < t,
                       "every digit is below 2^c and t");
        }
      }
      require_test(decode_entry(encoded, spec) == entry,
                   "decode must invert encode");
      ++round_trips;
    }
  }
  BENCH_PRINT("h1 entry codec round trips: " << round_trips);

  const std::vector<size_t> payload_widths = {1, 7, 12, 31, 63};
  const std::vector<size_t> boundary_entries = {13, 256, 30000};
  size_t oracle_cases = 0;
  for (size_t payload_bits : payload_widths) {
    const uint64_t modulus = uint64_t{1} << payload_bits;
    for (size_t E : boundary_entries) {
      const size_t G = payload_bits == 1 ? 63 : 7;
      CodecSpec spec{E, payload_bits, G,
                     minimal_plane_count(E, payload_bits, G), modulus};
      const size_t last = E - 1;
      const size_t near_byte = E > 9 ? 9 : last;
      const size_t near_digit = std::min(payload_bits, last);
      const size_t near_plane = std::min(payload_bits * G, last);
      const size_t positions[] = {size_t{0}, near_byte, near_digit,
                                  near_plane, last};
      const EntryBits entry = entry_with_positions(E, positions);
      const EncodedEntry encoded = encode_entry(entry, spec);
      require_test(encoded == per_bit_oracle(entry, spec),
                   "byte-fragment encoder matches per-bit oracle");
      require_test(decode_entry(encoded, spec) == entry,
                   "byte-fragment encoded planes decode exactly");
      ++oracle_cases;
    }
  }
  BENCH_PRINT("h1 entry codec boundary cases: " << oracle_cases);

  require_test(throws_invalid([&] {
                 CodecSpec spec{13, c, 4, minimal_plane_count(13, c, 4), t};
                 (void)encode_entry(zero_entry(12), spec);
               }),
               "entry of the wrong bit length must be rejected");
}
