#include "utils.h"

#include <algorithm>
#include <iostream>
#include <sstream>

std::string utils::uint128_to_string(uint128_t value) {
    // Split the 128-bit value into two 64-bit parts
    uint64_t high = value >> 64;
    uint64_t low = static_cast<uint64_t>(value);

    std::ostringstream oss;

    // Print the high part, if it's non-zero, to avoid leading zeros
    if (high != 0) {
        oss << high << " * 2^64 + " << low;
    } else {
        oss << low;
    }
    return oss.str();
}



// Plaintext checks and console output.
void utils::print_plaintext(const RlwePt &plaintext, const size_t count) {
  const size_t coeff_count = plaintext.coeff_count();
  for (size_t i = 0; i < std::min(count, coeff_count); ++i) {
    std::cout << plaintext.data[i] << ", ";
  }
  std::cout << std::endl;
}

bool utils::plaintext_is_equal(const RlwePt &plaintext1, const RlwePt &plaintext2) {
  const size_t coeff_count1 = plaintext1.coeff_count();
  const size_t coeff_count2 = plaintext2.coeff_count();

  if (coeff_count1 != coeff_count2) {
    std::cerr << "Plaintexts have different coefficient counts" << std::endl;
    return false;
  }

  for (size_t i = 0; i < coeff_count1; i++) {
    if (plaintext1.data[i] != plaintext2.data[i]) {
      std::cerr << "Plaintexts are not equal at coefficient " << i << std::endl;
      return false;
    }
  }
  return true;
}

void utils::print_progress(size_t current, size_t total) {
    float progress = static_cast<float>(current) / total;
    size_t bar_width = 70;

    // Move the cursor to the beginning and clear the line.
    std::cout << "\r\033[K[";

    size_t pos = static_cast<size_t>(bar_width * progress);
    for (size_t i = 0; i < bar_width; ++i) {
        if (i < pos)
            std::cout << "=";
        else if (i == pos)
            std::cout << ">";
        else
            std::cout << " ";
    }
    std::cout << "] " << size_t(progress * 100.0) << " %";
    std::cout.flush();
}


