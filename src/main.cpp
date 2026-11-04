#include "tests.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

size_t parse_size_argument(const char *option, const char *value) {
  std::string text(value);
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  size_t parsed_characters = 0;
  unsigned long long parsed = 0;
  try {
    parsed = std::stoull(text, &parsed_characters, 10);
  } catch (const std::exception &) {
    throw std::invalid_argument(std::string(option) +
                                " requires a non-negative integer");
  }
  if (parsed_characters != text.size() ||
      parsed > std::numeric_limits<size_t>::max()) {
    throw std::invalid_argument(std::string(option) +
                                " is outside the size_t range");
  }
  return static_cast<size_t>(parsed);
}

const char *require_value(int argc, char *argv[], int &index) {
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string(argv[index]) +
                                " requires a value");
  }
  return argv[++index];
}

struct CommandLineOptions {
  std::string test_name = "pir";
  size_t num_experiments = 10;
  size_t warmup = 3;
};

CommandLineOptions parse_command_line(int argc, char *argv[]) {
  CommandLineOptions cli;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--test") == 0) {
      cli.test_name = require_value(argc, argv, i);
    } else if (std::strcmp(argv[i], "--experiments") == 0) {
      cli.num_experiments = parse_size_argument(
          "--experiments", require_value(argc, argv, i));
    } else if (std::strcmp(argv[i], "--warmup") == 0) {
      cli.warmup = parse_size_argument("--warmup",
                                       require_value(argc, argv, i));
    } else {
      throw std::invalid_argument(std::string("Unknown option: ") + argv[i]);
    }
  }
  return cli;
}

int run_pir_test(const CommandLineOptions &cli) {
  if (cli.num_experiments >
      std::numeric_limits<size_t>::max() - cli.warmup) {
    throw std::overflow_error("experiment count plus warmup overflows");
  }
  TimerLogger::setWarmup(cli.warmup);
  PirTest test;
  test.num_experiments = cli.num_experiments + cli.warmup;
  test.run_test(cli.test_name);
  return 0;
}

}

int main(int argc, char *argv[]) {
  try {
    return run_pir_test(parse_command_line(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
