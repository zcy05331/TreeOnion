#include "adapter_types.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

using namespace std;

namespace treepir_adapter {

string read_file(const string &path)
{
    ifstream in(path, ios::binary);
    if (!in.is_open()) {
        throw runtime_error("unable to open input: " + path);
    }
    return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
}

namespace {
constexpr char kColors[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

size_t skip_ws(const string &s, size_t pos)
{
    while (pos < s.size() && isspace(static_cast<unsigned char>(s[pos]))) {
        ++pos;
    }
    return pos;
}

size_t find_key(const string &s, const string &key, size_t pos = 0)
{
    string needle = "\"" + key + "\"";
    size_t found = s.find(needle, pos);
    if (found == string::npos) {
        return found;
    }
    size_t colon = s.find(':', found + needle.size());
    if (colon == string::npos) {
        throw runtime_error("malformed JSON near key " + key);
    }
    return colon + 1;
}

uint64_t parse_uint_at(const string &s, size_t &pos)
{
    pos = skip_ws(s, pos);
    if (pos >= s.size() || !isdigit(static_cast<unsigned char>(s[pos]))) {
        throw runtime_error("expected unsigned integer in JSON");
    }
    uint64_t value = 0;
    while (pos < s.size() && isdigit(static_cast<unsigned char>(s[pos]))) {
        unsigned digit = static_cast<unsigned>(s[pos] - '0');
        if (value > (numeric_limits<uint64_t>::max() - digit) / 10) {
            throw runtime_error("integer overflow while parsing JSON");
        }
        value = value * 10 + digit;
        ++pos;
    }
    return value;
}

uint64_t parse_required_uint(const string &s, const string &key)
{
    size_t pos = find_key(s, key);
    if (pos == string::npos) {
        throw runtime_error("missing required key: " + key);
    }
    return parse_uint_at(s, pos);
}

uint64_t parse_optional_uint(const string &s, const string &key, uint64_t fallback)
{
    size_t pos = find_key(s, key);
    if (pos == string::npos) {
        return fallback;
    }
    return parse_uint_at(s, pos);
}

string parse_optional_string_in_object(const string &object, const string &key, const string &fallback)
{
    size_t pos = find_key(object, key);
    if (pos == string::npos) {
        return fallback;
    }
    pos = skip_ws(object, pos);
    if (pos >= object.size() || object[pos] != '"') {
        return fallback;
    }
    ++pos;
    string value;
    while (pos < object.size()) {
        char ch = object[pos++];
        if (ch == '"') {
            return value;
        }
        if (ch == '\\' && pos < object.size()) {
            value.push_back(object[pos++]);
        } else {
            value.push_back(ch);
        }
    }
    throw runtime_error("unterminated JSON string for key " + key);
}

vector<uint64_t> parse_uint_array_in_object(const string &object, const string &key)
{
    size_t pos = find_key(object, key);
    if (pos == string::npos) {
        throw runtime_error("missing required array key: " + key);
    }
    pos = skip_ws(object, pos);
    if (pos >= object.size() || object[pos] != '[') {
        throw runtime_error("expected array for key " + key);
    }
    ++pos;
    vector<uint64_t> values;
    while (true) {
        pos = skip_ws(object, pos);
        if (pos >= object.size()) {
            throw runtime_error("unterminated array for key " + key);
        }
        if (object[pos] == ']') {
            ++pos;
            break;
        }
        values.push_back(parse_uint_at(object, pos));
        pos = skip_ws(object, pos);
        if (pos < object.size() && object[pos] == ',') {
            ++pos;
        }
    }
    return values;
}

vector<uint64_t> parse_optional_uint_array(const string &s, const string &key)
{
    size_t pos = find_key(s, key);
    if (pos == string::npos) {
        return {};
    }
    pos = skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '[') {
        throw runtime_error("expected array for key " + key);
    }
    size_t start = pos;
    int depth = 0;
    do {
        if (pos >= s.size()) {
            throw runtime_error("unterminated array for key " + key);
        }
        if (s[pos] == '[') ++depth;
        if (s[pos] == ']') --depth;
        ++pos;
    } while (depth != 0);
    string object = "{\"" + key + "\":" + s.substr(start, pos - start) + "}";
    return parse_uint_array_in_object(object, key);
}

vector<string> result_objects(const string &s)
{
    size_t pos = find_key(s, "results");
    if (pos == string::npos) {
        throw runtime_error("missing required key: results");
    }
    pos = skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '[') {
        throw runtime_error("results must be an array");
    }
    ++pos;
    vector<string> objects;
    while (true) {
        pos = skip_ws(s, pos);
        if (pos >= s.size()) {
            throw runtime_error("unterminated results array");
        }
        if (s[pos] == ']') {
            break;
        }
        if (s[pos] != '{') {
            throw runtime_error("expected object in results array");
        }
        size_t start = pos;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        do {
            char ch = s[pos++];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
            } else if (ch == '"') {
                in_string = true;
            } else if (ch == '{') {
                ++depth;
            } else if (ch == '}') {
                --depth;
            }
            if (pos > s.size()) {
                throw runtime_error("unterminated result object");
            }
        } while (depth != 0);
        objects.push_back(s.substr(start, pos - start));
        pos = skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') {
            ++pos;
        }
    }
    return objects;
}

uint64_t pow2(unsigned int exponent)
{
    if (exponent >= 63) {
        throw runtime_error("height too large for uint64_t leaf count");
    }
    return 1ULL << exponent;
}

uint64_t node_count_for_height(unsigned int height)
{
    return pow2(height + 1) - 2;
}

vector<uint64_t> balanced_color_sizes(unsigned int height)
{
    uint64_t total = node_count_for_height(height);
    uint64_t base = total / height;
    uint64_t larger = total % height;
    vector<uint64_t> sizes(height);
    for (unsigned int i = 0; i < height; ++i) {
        sizes[i] = base + (i >= height - larger ? 1 : 0);
    }
    return sizes;
}

} // namespace

InputData parse_input(const string &json)
{
    InputData input;
    input.height = static_cast<unsigned int>(parse_required_uint(json, "height"));
    input.leaf_count = parse_optional_uint(json, "leaf_count", input.height < 63 ? pow2(input.height) : 0);
    input.entry_size = static_cast<size_t>(parse_optional_uint(json, "entry_size_bytes", kEntrySize));
    input.warmups = static_cast<size_t>(parse_optional_uint(json, "warmups", 0));
    input.trials = static_cast<size_t>(parse_optional_uint(json, "trials", 0));
    input.max_bucket_entries = parse_optional_uint(json, "max_bucket_entries", 0);
    input.color_bucket_sizes = parse_optional_uint_array(json, "color_bucket_sizes");
    if (input.color_bucket_sizes.empty()) {
        input.color_bucket_sizes = balanced_color_sizes(input.height);
    }
    if (input.max_bucket_entries == 0) {
        input.max_bucket_entries = *max_element(input.color_bucket_sizes.begin(), input.color_bucket_sizes.end());
    }
    if (input.height == 0 || input.height > sizeof(kColors) - 1) {
        throw runtime_error("height must be in 1..36 for the bundled TreePIR colors");
    }
    if (input.entry_size != kEntrySize) {
        throw runtime_error("only 32-byte TreePIR entries are supported by this adapter");
    }
    if (input.color_bucket_sizes.size() != input.height) {
        throw runtime_error("color_bucket_sizes length must match height");
    }

    auto objects = result_objects(json);
    input.results.reserve(objects.size());
    for (size_t i = 0; i < objects.size(); ++i) {
        const string &object = objects[i];
        InputTrial trial;
        trial.iteration = static_cast<size_t>(parse_optional_uint(object, "iteration", i + 1));
        trial.leaf_id = parse_required_uint(object, "leaf_id");
        string phase = parse_optional_string_in_object(object, "phase", i < input.warmups ? "warmup" : "measured");
        trial.warmup = phase == "warmup";
        trial.path_nodes = parse_uint_array_in_object(object, "path_nodes");
        trial.sub_indices = parse_uint_array_in_object(object, "sub_indices");
        if (trial.path_nodes.size() != input.height || trial.sub_indices.size() != input.height) {
            throw runtime_error("path_nodes and sub_indices length must match height");
        }
        input.results.push_back(std::move(trial));
    }
    if (input.results.empty()) {
        throw runtime_error("input contains no result rows");
    }
    return input;
}

Options parse_options(int argc, char **argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        auto require_value = [&](const string &name) -> string {
            if (i + 1 >= argc) {
                throw runtime_error(name + " requires a value");
            }
            return argv[++i];
        };
        if (arg == "--input") {
            options.input = require_value(arg);
        } else if (arg == "--output") {
            options.output = require_value(arg);
        } else if (arg == "--warmup") {
            options.has_warmups_override = true;
            options.warmups_override = stoull(require_value(arg));
        } else if (arg == "--trials") {
            options.has_trials_override = true;
            options.trials_override = stoull(require_value(arg));
        } else if (arg == "--poly-degree") {
            options.poly_degree = static_cast<uint32_t>(stoul(require_value(arg)));
        } else if (arg == "--plain-logt") {
            options.plain_logt = static_cast<uint32_t>(stoul(require_value(arg)));
        } else if (arg == "--pir-d") {
            options.pir_d = static_cast<uint32_t>(stoul(require_value(arg)));
        } else if (arg == "--max-color-entries") {
            options.max_color_entries = stoull(require_value(arg));
        } else if (arg == "--validate-input-only") {
            options.validate_input_only = true;
        } else {
            throw runtime_error("usage: treepir_sealpir_adapter --input FILE [--output FILE|-] [--warmup N] [--trials N] [--poly-degree 8192] [--plain-logt 20] [--pir-d 1|2] [--max-color-entries N] [--validate-input-only]");
        }
    }
    if (options.input.empty()) {
        throw runtime_error("--input is required");
    }
    if (options.pir_d != 1 && options.pir_d != 2) {
        throw runtime_error("--pir-d must be 1 or 2 for this pilot adapter");
    }
    return options;
}

vector<InputTrial> select_trials(const InputData &input, const Options &options)
{
    if (options.has_warmups_override && options.warmups_override == 0) {
        throw runtime_error("--warmup must be greater than zero when specified");
    }
    if (options.has_trials_override && options.trials_override == 0) {
        throw runtime_error("--trials must be greater than zero when specified");
    }

    size_t warmups = options.has_warmups_override ? options.warmups_override : input.warmups;
    size_t trials = options.has_trials_override ? options.trials_override : input.trials;
    size_t desired = warmups + trials;
    if (desired == 0) {
        desired = input.results.size();
    }
    if (desired == 0) {
        throw runtime_error("schedule has no rows to run");
    }
    if (desired > input.results.size()) {
        throw runtime_error("requested warmup+trials exceeds input result rows");
    }
    vector<InputTrial> selected(input.results.begin(), input.results.begin() + desired);
    for (size_t i = 0; i < selected.size(); ++i) {
        selected[i].warmup = i < warmups;
    }
    return selected;
}

} // namespace treepir_adapter
