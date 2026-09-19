#ifndef VBPIR_PATH_INPUT_H
#define VBPIR_PATH_INPUT_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "src/utils.h"

struct PathOptions {
    std::uint32_t height = 24;
    std::string leaf_file;
    std::string output;
    std::size_t warmups = 0;
    std::size_t trials = 100;
    std::size_t first_dim = 64;
};

struct PathSchedule {
    std::vector<size_t> all_leaves;
    std::vector<size_t> warmup_leaves;
    std::vector<size_t> measured_leaves;
};

PathOptions parse_options(int argc, char **argv);
PathSchedule load_schedule(const PathOptions &options);
RawDB build_raw_db(std::uint32_t height);

#endif
