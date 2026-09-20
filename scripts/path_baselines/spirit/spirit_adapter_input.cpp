#include "spirit_adapter_types.h"

#include "../common/path_workload.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace spirit_path_adapter {
namespace {

std::uint64_t checked_pow2(unsigned exponent)
{
    if (exponent < 1 || exponent > 30) {
        throw std::invalid_argument("height outside supported range [1, 30]");
    }
    if (exponent >= 63) {
        throw std::invalid_argument("height is too large for uint64_t indexing");
    }
    return std::uint64_t{1} << exponent;
}

std::uint64_t checked_record_count(unsigned height)
{
    if (height >= 62) {
        throw std::invalid_argument("height is too large for full-tree record count");
    }
    return (std::uint64_t{1} << (height + 1)) - 1;
}

std::size_t next_power_of_two(std::size_t value)
{
    if (value == 0) {
        return 1;
    }
    std::size_t power = 1;
    while (power < value) {
        if (power > (std::numeric_limits<std::size_t>::max() >> 1)) {
            throw std::overflow_error("batch size overflow");
        }
        power <<= 1;
    }
    return power;
}

std::uint64_t parse_u64(const std::string &text, const std::string &label)
{
    std::uint64_t value = 0;
    const char *begin = text.data();
    const char *end = text.data() + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc() || ptr != end) {
        throw std::invalid_argument("invalid unsigned integer for " + label + ": " + text);
    }
    return value;
}

void require_value(int argc, char **argv, int &i, const std::string &flag)
{
    if (i + 1 >= argc) {
        throw std::invalid_argument(flag + " requires a value");
    }
    ++i;
}

std::vector<std::uint64_t> with_public_dummy_padding(
    std::uint64_t record_count,
    const std::vector<std::uint64_t> &path,
    std::size_t batch_size)
{
    std::vector<std::uint64_t> query = path;
    std::unordered_set<std::uint64_t> used(path.begin(), path.end());
    std::uint64_t candidate = 0;
    while (query.size() < batch_size) {
        while (candidate < record_count && used.count(candidate) != 0) {
            ++candidate;
        }
        if (candidate >= record_count) {
            throw std::invalid_argument("not enough public dummy records to pad the path batch");
        }
        query.push_back(candidate);
        used.insert(candidate);
        ++candidate;
    }
    return query;
}

} // namespace

Options parse_options(int argc, char **argv)
{
    Options options;
    bool saw_height = false;
    bool saw_leaf_file = false;
    bool saw_warmup = false;
    bool saw_trials = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--scheme") {
            require_value(argc, argv, i, arg);
            options.scheme = argv[i];
        } else if (arg == "--height") {
            require_value(argc, argv, i, arg);
            options.height = static_cast<unsigned>(parse_u64(argv[i], arg));
            saw_height = true;
        } else if (arg == "--leaf-file") {
            require_value(argc, argv, i, arg);
            options.leaf_file = argv[i];
            saw_leaf_file = true;
        } else if (arg == "--warmup") {
            require_value(argc, argv, i, arg);
            options.warmup = static_cast<std::size_t>(parse_u64(argv[i], arg));
            saw_warmup = true;
        } else if (arg == "--trials") {
            require_value(argc, argv, i, arg);
            options.trials = static_cast<std::size_t>(parse_u64(argv[i], arg));
            saw_trials = true;
        } else if (arg == "--output") {
            require_value(argc, argv, i, arg);
            options.output = argv[i];
        } else if (arg == "--lwe-dimension") {
            require_value(argc, argv, i, arg);
            options.lwe_dimension = static_cast<std::size_t>(parse_u64(argv[i], arg));
        } else if (arg == "--poly-degree") {
            require_value(argc, argv, i, arg);
            options.poly_modulus_degree = static_cast<std::size_t>(parse_u64(argv[i], arg));
        } else if (arg == "--record-plain-modulus") {
            require_value(argc, argv, i, arg);
            options.record_plain_modulus = parse_u64(argv[i], arg);
        } else if (arg == "--num-hash-funcs") {
            require_value(argc, argv, i, arg);
            options.num_hash_funcs = static_cast<std::size_t>(parse_u64(argv[i], arg));
        } else if (arg == "--cuckoo-factor") {
            require_value(argc, argv, i, arg);
            options.cuckoo_factor = std::stod(argv[i]);
        } else if (arg == "--max-attempts") {
            require_value(argc, argv, i, arg);
            options.max_attempts = static_cast<std::size_t>(parse_u64(argv[i], arg));
        } else if (arg == "--no-pad-batch-to-power2") {
            options.pad_batch_to_power2 = false;
        } else if (arg == "--validate-input-only") {
            options.validate_input_only = true;
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }
    if (!saw_height) {
        throw std::invalid_argument("missing required --height");
    }
    if (!saw_leaf_file) {
        throw std::invalid_argument("missing required --leaf-file");
    }
    if (!saw_warmup) {
        throw std::invalid_argument("missing required --warmup");
    }
    if (!saw_trials) {
        throw std::invalid_argument("missing required --trials");
    }
    if (options.trials == 0) {
        throw std::invalid_argument("--trials must be greater than zero");
    }
    if (options.scheme != "spirit_rc") {
        throw std::invalid_argument("SPIRIT standalone baseline has been removed; use --scheme spirit_rc");
    }
    if (options.record_plain_modulus != 256) {
        throw std::invalid_argument("SPIRIT batch byte records require --record-plain-modulus 256");
    }
    return options;
}

Workload build_workload(const Options &options)
{
    Workload workload;
    workload.height = options.height;
    workload.leaf_count = checked_pow2(options.height);
    workload.record_count = checked_record_count(options.height);
    workload.path_record_count = static_cast<std::size_t>(options.height) + 1;
    workload.batch_size = options.pad_batch_to_power2
        ? next_power_of_two(workload.path_record_count)
        : workload.path_record_count;
    workload.warmup = options.warmup;
    workload.trials = options.trials;
    if (workload.batch_size > workload.record_count) {
        throw std::invalid_argument("batch size exceeds full database record count");
    }

    const std::vector<std::size_t> leaves = path_workload::read_leaves(options.leaf_file);
    const std::size_t needed = options.warmup + options.trials;
    if (leaves.size() < needed) {
        throw std::invalid_argument("leaf file does not contain enough leaves for warmup+trials");
    }
    workload.schedule.reserve(needed);
    for (std::size_t i = 0; i < needed; ++i) {
        WorkloadTrial trial;
        trial.iteration = i;
        trial.warmup = i < options.warmup;
        trial.leaf = leaves[i];
        const std::vector<std::size_t> path = path_workload::path_indices(options.height, leaves[i]);
        trial.path_indices.assign(path.begin(), path.end());
        trial.query_indices = with_public_dummy_padding(
            workload.record_count, trial.path_indices, workload.batch_size);
        workload.schedule.push_back(std::move(trial));
    }
    return workload;
}

} // namespace spirit_path_adapter
