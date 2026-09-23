#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

// ChaCha20 keystream generator (the RFC 8439 block function, 20 rounds, with
// a 64-bit block counter) behind the UniformRandomBitGenerator interface, so
// the standard distributions and the samplers in utils work unchanged. It
// replaces std::mt19937_64 for every secret-dependent draw: the client secret
// key, encryption masks and errors, and the key-switching keys.
//
// Default construction draws the 256-bit key from the operating system
// (std::random_device); the seeded constructor gives reproducible streams and
// is meant for tests.
class ChaChaRng {
 public:
  using result_type = uint64_t;

  ChaChaRng() {
    std::random_device device;
    for (uint32_t &word : key_) word = device();
  }

  explicit ChaChaRng(const std::array<uint32_t, 8> &key) : key_(key) {}

  explicit ChaChaRng(uint64_t seed) {
    for (size_t i = 0; i < key_.size(); i += 2) {
      seed += 0x9e3779b97f4a7c15ULL;
      uint64_t z = seed;
      z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
      z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
      z ^= z >> 31;
      key_[i] = static_cast<uint32_t>(z);
      key_[i + 1] = static_cast<uint32_t>(z >> 32);
    }
  }

  static constexpr result_type min() { return 0; }
  static constexpr result_type max() { return ~result_type{0}; }

  result_type operator()() {
    if (next_ + 2 > block_.size()) refill();
    const uint64_t lo = block_[next_], hi = block_[next_ + 1];
    next_ += 2;
    return lo | (hi << 32);
  }

 private:
  static uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
  static void quarter(uint32_t &a, uint32_t &b, uint32_t &c, uint32_t &d) {
    a += b; d ^= a; d = rotl(d, 16);
    c += d; b ^= c; b = rotl(b, 12);
    a += b; d ^= a; d = rotl(d, 8);
    c += d; b ^= c; b = rotl(b, 7);
  }

  void refill() {
    std::array<uint32_t, 16> in = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574,
                                   key_[0], key_[1], key_[2], key_[3],
                                   key_[4], key_[5], key_[6], key_[7],
                                   static_cast<uint32_t>(counter_), static_cast<uint32_t>(counter_ >> 32),
                                   0, 0};
    std::array<uint32_t, 16> x = in;
    for (int round = 0; round < 10; ++round) {
      quarter(x[0], x[4], x[8], x[12]);
      quarter(x[1], x[5], x[9], x[13]);
      quarter(x[2], x[6], x[10], x[14]);
      quarter(x[3], x[7], x[11], x[15]);
      quarter(x[0], x[5], x[10], x[15]);
      quarter(x[1], x[6], x[11], x[12]);
      quarter(x[2], x[7], x[8], x[13]);
      quarter(x[3], x[4], x[9], x[14]);
    }
    for (size_t i = 0; i < block_.size(); ++i) block_[i] = x[i] + in[i];
    ++counter_;
    next_ = 0;
  }

  std::array<uint32_t, 8> key_{};
  uint64_t counter_ = 0;
  std::array<uint32_t, 16> block_{};
  size_t next_ = 16;  // next unused word of block_; 16 forces the first refill
};
