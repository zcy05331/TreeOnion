#include "path_input.h"

#include <cstdlib>
#include <stdexcept>

#include "path_workload.h"

namespace {

std::uint64_t parse_u64(const std::string &value, const std::string &name) {
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::invalid_argument("invalid " + name + ": " + value);
    }
    return static_cast<std::uint64_t>(parsed);
}

void validate_leaf(size_t leaf, std::uint32_t height) {
    if (height > 30 || leaf >= (size_t{1} << height)) {
        throw std::invalid_argument("leaf is outside the requested tree height");
    }
}

bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

PathOptions parse_options(int argc, char **argv) {
    PathOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const std::string &flag) -> std::string {
            if (i + 1 >= argc) {
                throw std::invalid_argument("missing value for " + flag);
            }
            return argv[++i];
        };
        if (arg == "--height") {
            options.height = static_cast<std::uint32_t>(parse_u64(require_value(arg), arg));
        } else if (arg == "--leaf-file") {
            options.leaf_file = require_value(arg);
        } else if (arg == "--warmup") {
            options.warmups = static_cast<std::size_t>(parse_u64(require_value(arg), arg));
        } else if (arg == "--trials") {
            options.trials = static_cast<std::size_t>(parse_u64(require_value(arg), arg));
        } else if (arg == "--first-dim") {
            options.first_dim = static_cast<std::size_t>(parse_u64(require_value(arg), arg));
        } else if (arg == "--output") {
            options.output = require_value(arg);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }
    if (options.leaf_file.empty()) {
        throw std::invalid_argument("--leaf-file is required");
    }
    if (options.output.empty()) {
        throw std::invalid_argument("--output is required");
    }
    if (options.height < 1 || options.height > 30) {
        throw std::invalid_argument("--height must be in [1, 30]");
    }
    if (options.trials == 0) {
        throw std::invalid_argument("--trials must be positive");
    }
    if (!is_power_of_two(options.first_dim)) {
        throw std::invalid_argument("--first-dim must be a nonzero power of two");
    }
    return options;
}

PathSchedule load_schedule(const PathOptions &options) {
    PathSchedule schedule;
    schedule.all_leaves = path_workload::read_leaves(options.leaf_file);
    const std::size_t required = options.warmups + options.trials;
    if (schedule.all_leaves.size() < required) {
        throw std::invalid_argument("leaf file has fewer than warmup + trial leaves");
    }
    schedule.warmup_leaves.assign(schedule.all_leaves.begin(),
                                  schedule.all_leaves.begin() + options.warmups);
    schedule.measured_leaves.assign(schedule.all_leaves.begin() + options.warmups,
                                    schedule.all_leaves.begin() + required);
    for (size_t leaf : schedule.warmup_leaves) {
        validate_leaf(leaf, options.height);
    }
    for (size_t leaf : schedule.measured_leaves) {
        validate_leaf(leaf, options.height);
    }
    return schedule;
}

RawDB build_raw_db(std::uint32_t height) {
    const size_t entries = (size_t{1} << (height + 1)) - 1;
    RawDB db;
    db.reserve(entries);
    for (size_t index = 0; index < entries; ++index) {
        const auto record = path_workload::record_bytes(index);
        db.emplace_back(record.begin(), record.end());
    }
    return db;
}
