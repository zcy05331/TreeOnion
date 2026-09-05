#include "pir.hpp"
#include "pir_client.hpp"
#include "pir_server.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;
using namespace seal;
using Clock = chrono::high_resolution_clock;

namespace {

constexpr size_t kEntrySize = 32;
constexpr char kColors[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

struct Options {
    string input;
    string output = "-";
    size_t warmups_override = numeric_limits<size_t>::max();
    size_t trials_override = numeric_limits<size_t>::max();
    bool has_warmups_override = false;
    bool has_trials_override = false;
    uint32_t poly_degree = 8192;
    uint32_t plain_logt = 20;
    uint32_t pir_d = 2;
    uint64_t max_color_entries = 0;
    bool validate_input_only = false;
};

struct InputTrial {
    size_t iteration = 0;
    bool warmup = false;
    uint64_t leaf_id = 0;
    vector<uint64_t> path_nodes;
    vector<uint64_t> sub_indices;
};

struct InputData {
    unsigned int height = 0;
    uint64_t leaf_count = 0;
    size_t entry_size = kEntrySize;
    size_t warmups = 0;
    size_t trials = 0;
    uint64_t max_bucket_entries = 0;
    vector<uint64_t> color_bucket_sizes;
    vector<uint64_t> effective_color_bucket_sizes;
    uint64_t effective_max_bucket_entries = 0;
    bool smoke_cap_active = false;
    vector<InputTrial> results;
};

struct TrialAccum {
    size_t iteration = 0;
    bool warmup = false;
    uint64_t leaf_id = 0;
    vector<uint64_t> path_nodes;
    vector<uint64_t> sub_indices;
    double query_ms = 0.0;
    double query_serialize_ms = 0.0;
    double server_generate_reply_ms = 0.0;
    double server_expansion_ms = 0.0;
    double server_query_ntt_ms = 0.0;
    double server_database_ntt_ms = 0.0;
    double server_multiply_accumulate_ms = 0.0;
    double server_inverse_ntt_ms = 0.0;
    double server_recursive_decomposition_modswitch_ms = 0.0;
    double server_modswitch_ms = 0.0;
    double server_ms = 0.0;
    double response_serialize_ms = 0.0;
    double decode_ms = 0.0;
    uint64_t query_bytes = 0;
    uint64_t response_bytes = 0;
    uint64_t query_ciphertexts = 0;
    uint64_t response_ciphertexts = 0;
    uint64_t decoded_entries = 0;
    bool correct = true;
    vector<double> server_generate_reply_color_ms;
    vector<double> server_expansion_color_ms;
    vector<double> server_query_ntt_color_ms;
    vector<double> server_database_ntt_color_ms;
    vector<double> server_multiply_accumulate_color_ms;
    vector<double> server_inverse_ntt_color_ms;
    vector<double> server_recursive_decomposition_modswitch_color_ms;
    vector<double> server_modswitch_color_ms;
    vector<double> server_color_ms;
    vector<uint64_t> query_color_bytes;
    vector<uint64_t> response_color_bytes;
};

string read_file(const string &path)
{
    ifstream in(path, ios::binary);
    if (!in.is_open()) {
        throw runtime_error("unable to open input: " + path);
    }
    return string((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
}

void write_output(const string &path, const string &contents)
{
    if (path == "-") {
        cout << contents << '\n';
        return;
    }
    ofstream out(path);
    if (!out.is_open()) {
        throw runtime_error("unable to open output: " + path);
    }
    out << contents << '\n';
}

string json_escape(const string &value)
{
    ostringstream out;
    for (char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default: out << ch; break;
        }
    }
    return out.str();
}

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

uint64_t mix(uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

vector<uint8_t> deterministic_entry(unsigned int height, unsigned int color, uint64_t index)
{
    uint64_t state = mix((static_cast<uint64_t>(height) << 48) ^ (static_cast<uint64_t>(color) << 40) ^ index);
    vector<uint8_t> entry(kEntrySize);
    for (size_t i = 0; i < entry.size(); ++i) {
        if ((i % 8) == 0) {
            state = mix(state + i);
        }
        entry[i] = static_cast<uint8_t>((state >> (8 * (i % 8))) & 0xff);
    }
    return entry;
}

unique_ptr<const uint8_t[]> make_color_database(unsigned int height, unsigned int color, uint64_t entries)
{
    auto db = unique_ptr<uint8_t[]>(new uint8_t[entries * kEntrySize]);
    for (uint64_t index = 0; index < entries; ++index) {
        vector<uint8_t> entry = deterministic_entry(height, color, index);
        copy(entry.begin(), entry.end(), db.get() + index * kEntrySize);
    }
    return unique_ptr<const uint8_t[]>(db.release());
}

double elapsed_ms(Clock::time_point start, Clock::time_point end)
{
    return chrono::duration_cast<chrono::duration<double, milli>>(end - start).count();
}

size_t count_query_ciphertexts(const PirQuery &query)
{
    size_t total = 0;
    for (const auto &part : query) {
        total += part.size();
    }
    return total;
}

uint64_t serialize_query_payload_none(const PirQuery &query)
{
    uint64_t total = 0;
    for (const auto &part : query) {
        for (const auto &ct : part) {
            ostringstream stream(ios::out | ios::binary);
            total += static_cast<uint64_t>(ct.save(stream, compr_mode_type::none));
        }
    }
    return total;
}


uint64_t serialize_reply_payload_none(const PirReply &reply)
{
    uint64_t total = 0;
    for (const auto &ct : reply) {
        ostringstream stream(ios::out | ios::binary);
        total += static_cast<uint64_t>(ct.save(stream, compr_mode_type::none));
    }
    return total;
}

void write_uint_array(ostream &out, const vector<uint64_t> &values)
{
    out << '[';
    for (size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << values[i];
    }
    out << ']';
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

string render_json(
    const InputData &input,
    const Options &options,
    const vector<InputTrial> &selected,
    const vector<TrialAccum> &results,
    double offline_key_setup_ms,
    double database_setup_ms,
    uint64_t helper_key_bytes)
{
    size_t measured_count = 0;
    double query_ms = 0.0;
    double query_serialize_ms = 0.0;
    double server_generate_reply_ms = 0.0;
    double server_expansion_ms = 0.0;
    double server_query_ntt_ms = 0.0;
    double server_database_ntt_ms = 0.0;
    double server_multiply_accumulate_ms = 0.0;
    double server_inverse_ntt_ms = 0.0;
    double server_recursive_decomposition_modswitch_ms = 0.0;
    double server_modswitch_ms = 0.0;
    double server_ms = 0.0;
    double response_serialize_ms = 0.0;
    double decode_ms = 0.0;
    double query_bytes = 0.0;
    double response_bytes = 0.0;
    bool all_correct = true;
    for (const auto &r : results) {
        all_correct = all_correct && r.correct;
        if (!r.warmup) {
            ++measured_count;
            query_ms += r.query_ms;
            query_serialize_ms += r.query_serialize_ms;
            server_generate_reply_ms += r.server_generate_reply_ms;
            server_expansion_ms += r.server_expansion_ms;
            server_query_ntt_ms += r.server_query_ntt_ms;
            server_database_ntt_ms += r.server_database_ntt_ms;
            server_multiply_accumulate_ms += r.server_multiply_accumulate_ms;
            server_inverse_ntt_ms += r.server_inverse_ntt_ms;
            server_recursive_decomposition_modswitch_ms += r.server_recursive_decomposition_modswitch_ms;
            server_modswitch_ms += r.server_modswitch_ms;
            server_ms += r.server_ms;
            response_serialize_ms += r.response_serialize_ms;
            decode_ms += r.decode_ms;
            query_bytes += static_cast<double>(r.query_bytes);
            response_bytes += static_cast<double>(r.response_bytes);
        }
    }
    if (measured_count) {
        query_ms /= measured_count;
        query_serialize_ms /= measured_count;
        server_generate_reply_ms /= measured_count;
        server_expansion_ms /= measured_count;
        server_query_ntt_ms /= measured_count;
        server_database_ntt_ms /= measured_count;
        server_multiply_accumulate_ms /= measured_count;
        server_inverse_ntt_ms /= measured_count;
        server_recursive_decomposition_modswitch_ms /= measured_count;
        server_modswitch_ms /= measured_count;
        server_ms /= measured_count;
        response_serialize_ms /= measured_count;
        decode_ms /= measured_count;
        query_bytes /= measured_count;
        response_bytes /= measured_count;
    }

    ostringstream out;
    out << fixed << setprecision(3);
    out << '{';
    out << "\"model\":\"TreePIR+SealPIRplusCore\",";
    out << "\"input\":\"" << json_escape(options.input) << "\",";
    out << "\"run_mode\":\"" << (options.validate_input_only ? "validate_input_only" : "in_process_core") << "\",";
    out << "\"transport_mode\":\"manual_uncompressed_ciphertext_save\",";
    out << "\"query_payload_label\":\"actual_existing_query_ciphertexts_save_none\",";
    out << "\"response_payload_label\":\"server_modswitch_then_manual_ciphertext_save_none\",";
    out << "\"height\":" << input.height << ',';
    out << "\"leaf_count\":" << input.leaf_count << ',';
    out << "\"entry_size_bytes\":" << input.entry_size << ',';
    out << "\"warmups\":" << count_if(selected.begin(), selected.end(), [](const InputTrial &t) { return t.warmup; }) << ',';
    out << "\"trials\":" << count_if(selected.begin(), selected.end(), [](const InputTrial &t) { return !t.warmup; }) << ',';
    out << "\"layout\":{\"color_buckets\":" << input.height;
    out << ",\"full_max_bucket_entries\":" << input.max_bucket_entries;
    out << ",\"effective_max_bucket_entries\":" << input.effective_max_bucket_entries;
    out << ",\"smoke_cap_active\":" << (input.smoke_cap_active ? "true" : "false");
    out << ",\"full_color_bucket_sizes\":";
    write_uint_array(out, input.color_bucket_sizes);
    out << ",\"effective_color_bucket_sizes\":";
    write_uint_array(out, input.effective_color_bucket_sizes);
    out << "},";
    out << "\"seal_parameters\":{\"scheme\":\"BFV\",\"poly_modulus_degree\":" << options.poly_degree << ",\"plain_logt\":" << options.plain_logt << ",\"pir_d\":" << options.pir_d << ",\"coeff_modulus\":\"BFVDefault\"},";
    out << "\"regime\":\"sequential_color_processing; per-path server_ms sums one SealPIR generate_reply per color and is not an all-colors-hot-cache single wall time\",";
    out << "\"stage_definitions\":{\"offline_key_setup_ms\":\"sum of per-color PIRClient construction and Galois key generation\",\"database_setup_ms\":\"sum of per-color deterministic 32B database generation, SealPIR plaintext packing, and NTT preprocess\",\"query_color_bytes\":\"per-color bytes from saving the same generated PirQuery ciphertexts with compr_mode_type::none\",\"server_generate_reply_color_ms\":\"per-color PIRServer::generate_reply wall time in color order\",\"server_expansion_ms\":\"sum inside generate_reply expanding encrypted selection vectors\",\"server_query_ntt_ms\":\"sum inside generate_reply transforming expanded query ciphertexts to NTT\",\"server_database_ntt_ms\":\"sum inside generate_reply transforming current plaintext database/intermediate plaintexts to NTT when needed\",\"server_multiply_accumulate_ms\":\"sum inside generate_reply multiply_plain/add accumulation\",\"server_inverse_ntt_ms\":\"sum inside generate_reply transform_from_ntt on intermediate ciphertexts\",\"server_recursive_decomposition_modswitch_ms\":\"sum inside generate_reply recursive mod-switch and ciphertext decomposition to plaintexts\",\"server_modswitch_color_ms\":\"per-color time to apply the upstream response mod-switch step; included in server_ms\",\"server_color_ms\":\"per-color generate_reply plus server_modswitch time; sums to server_ms\",\"response_color_bytes\":\"per-color bytes after server mod-switch and Ciphertext::save(compr_mode_type::none)\"},";
    out << "\"offline_key_setup_ms\":" << offline_key_setup_ms << ',';
    out << "\"database_setup_ms\":" << database_setup_ms << ',';
    out << "\"actual_helper_key_bytes_uncompressed\":" << helper_key_bytes << ',';
    out << "\"measured_summary\":{\"count\":" << measured_count;
    out << ",\"query_ms_mean\":" << query_ms;
    out << ",\"query_serialize_ms_mean\":" << query_serialize_ms;
    out << ",\"server_generate_reply_ms_mean\":" << server_generate_reply_ms;
    out << ",\"server_expansion_ms_mean\":" << server_expansion_ms;
    out << ",\"server_query_ntt_ms_mean\":" << server_query_ntt_ms;
    out << ",\"server_database_ntt_ms_mean\":" << server_database_ntt_ms;
    out << ",\"server_multiply_accumulate_ms_mean\":" << server_multiply_accumulate_ms;
    out << ",\"server_inverse_ntt_ms_mean\":" << server_inverse_ntt_ms;
    out << ",\"server_recursive_decomposition_modswitch_ms_mean\":" << server_recursive_decomposition_modswitch_ms;
    out << ",\"server_modswitch_ms_mean\":" << server_modswitch_ms;
    out << ",\"server_ms_mean\":" << server_ms;
    out << ",\"response_serialize_ms_mean\":" << response_serialize_ms;
    out << ",\"decode_ms_mean\":" << decode_ms;
    out << ",\"actual_query_payload_bytes_mean\":" << query_bytes;
    out << ",\"actual_response_payload_bytes_mean\":" << response_bytes;
    out << ",\"all_correct\":" << (all_correct ? "true" : "false") << "},";
    auto write_double_array = [&out](const vector<double> &values) {
        out << '[';
        for (size_t i = 0; i < values.size(); ++i) {
            if (i) out << ',';
            out << values[i];
        }
        out << ']';
    };
    out << "\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        if (i) out << ',';
        out << '{';
        out << "\"iteration\":" << r.iteration;
        out << ",\"phase\":\"" << (r.warmup ? "warmup" : "measured") << "\"";
        out << ",\"leaf_id\":" << r.leaf_id;
        out << ",\"path_nodes\":";
        write_uint_array(out, r.path_nodes);
        out << ",\"sub_indices\":";
        write_uint_array(out, r.sub_indices);
        out << ",\"query_ms\":" << r.query_ms;
        out << ",\"query_serialize_ms\":" << r.query_serialize_ms;
        out << ",\"server_generate_reply_ms\":" << r.server_generate_reply_ms;
        out << ",\"server_generate_reply_color_ms\":[";
        for (size_t color = 0; color < r.server_generate_reply_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_generate_reply_color_ms[color];
        }
        out << "]";
        out << ",\"server_expansion_ms\":" << r.server_expansion_ms;
        out << ",\"server_expansion_color_ms\":";
        write_double_array(r.server_expansion_color_ms);
        out << ",\"server_query_ntt_ms\":" << r.server_query_ntt_ms;
        out << ",\"server_query_ntt_color_ms\":";
        write_double_array(r.server_query_ntt_color_ms);
        out << ",\"server_database_ntt_ms\":" << r.server_database_ntt_ms;
        out << ",\"server_database_ntt_color_ms\":";
        write_double_array(r.server_database_ntt_color_ms);
        out << ",\"server_multiply_accumulate_ms\":" << r.server_multiply_accumulate_ms;
        out << ",\"server_multiply_accumulate_color_ms\":";
        write_double_array(r.server_multiply_accumulate_color_ms);
        out << ",\"server_inverse_ntt_ms\":" << r.server_inverse_ntt_ms;
        out << ",\"server_inverse_ntt_color_ms\":";
        write_double_array(r.server_inverse_ntt_color_ms);
        out << ",\"server_recursive_decomposition_modswitch_ms\":" << r.server_recursive_decomposition_modswitch_ms;
        out << ",\"server_recursive_decomposition_modswitch_color_ms\":";
        write_double_array(r.server_recursive_decomposition_modswitch_color_ms);
        out << ",\"server_modswitch_ms\":" << r.server_modswitch_ms;
        out << ",\"server_modswitch_color_ms\":[";
        for (size_t color = 0; color < r.server_modswitch_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_modswitch_color_ms[color];
        }
        out << "]";
        out << ",\"server_ms\":" << r.server_ms;
        out << ",\"server_color_ms\":[";
        for (size_t color = 0; color < r.server_color_ms.size(); ++color) {
            if (color) out << ',';
            out << r.server_color_ms[color];
        }
        out << "]";
        out << ",\"response_serialize_ms\":" << r.response_serialize_ms;
        out << ",\"decode_ms\":" << r.decode_ms;
        out << ",\"actual_query_payload_bytes_unseeded\":" << r.query_bytes;
        out << ",\"query_color_bytes\":";
        write_uint_array(out, r.query_color_bytes);
        out << ",\"actual_response_payload_bytes\":" << r.response_bytes;
        out << ",\"response_color_bytes\":";
        write_uint_array(out, r.response_color_bytes);
        out << ",\"query_ciphertexts\":" << r.query_ciphertexts;
        out << ",\"response_ciphertexts\":" << r.response_ciphertexts;
        out << ",\"decoded_entries\":" << r.decoded_entries;
        out << ",\"correct\":" << (r.correct ? "true" : "false") << '}';
    }
    out << "]}";
    return out.str();
}

int run(int argc, char **argv)
{
    Options options = parse_options(argc, argv);
    InputData input = parse_input(read_file(options.input));
    vector<InputTrial> selected = select_trials(input, options);

    input.effective_color_bucket_sizes = input.color_bucket_sizes;
    input.smoke_cap_active = options.max_color_entries != 0;
    if (input.smoke_cap_active) {
        for (auto &size : input.effective_color_bucket_sizes) {
            size = min<uint64_t>(size, options.max_color_entries);
        }
    }
    input.effective_max_bucket_entries = *max_element(input.effective_color_bucket_sizes.begin(), input.effective_color_bucket_sizes.end());
    for (const auto &trial : selected) {
        for (size_t color = 0; color < trial.sub_indices.size(); ++color) {
            uint64_t available = input.effective_color_bucket_sizes[color];
            if (trial.sub_indices[color] >= available) {
                throw runtime_error("sub_index outside effective color bucket; raise --max-color-entries or use full run");
            }
        }
    }

    if (options.validate_input_only) {
        vector<TrialAccum> empty;
        for (const auto &trial : selected) {
            TrialAccum acc;
            acc.iteration = trial.iteration;
            acc.warmup = trial.warmup;
            acc.leaf_id = trial.leaf_id;
            acc.path_nodes = trial.path_nodes;
            acc.sub_indices = trial.sub_indices;
            acc.server_generate_reply_color_ms.assign(input.height, 0.0);
            acc.server_expansion_color_ms.assign(input.height, 0.0);
            acc.server_query_ntt_color_ms.assign(input.height, 0.0);
            acc.server_database_ntt_color_ms.assign(input.height, 0.0);
            acc.server_multiply_accumulate_color_ms.assign(input.height, 0.0);
            acc.server_inverse_ntt_color_ms.assign(input.height, 0.0);
            acc.server_recursive_decomposition_modswitch_color_ms.assign(input.height, 0.0);
            acc.server_modswitch_color_ms.assign(input.height, 0.0);
            acc.server_color_ms.assign(input.height, 0.0);
            acc.query_color_bytes.assign(input.height, 0);
            acc.response_color_bytes.assign(input.height, 0);
            empty.push_back(std::move(acc));
        }
        write_output(options.output, render_json(input, options, selected, empty, 0.0, 0.0, 0));
        return 0;
    }

    EncryptionParameters enc_params(scheme_type::bfv);
    gen_encryption_params(options.poly_degree, options.plain_logt, enc_params);
    verify_encryption_params(enc_params);

    vector<TrialAccum> accum;
    accum.reserve(selected.size());
    for (const auto &trial : selected) {
        TrialAccum acc;
        acc.iteration = trial.iteration;
        acc.warmup = trial.warmup;
        acc.leaf_id = trial.leaf_id;
        acc.path_nodes = trial.path_nodes;
        acc.sub_indices = trial.sub_indices;
        acc.server_generate_reply_color_ms.assign(input.height, 0.0);
        acc.server_expansion_color_ms.assign(input.height, 0.0);
        acc.server_query_ntt_color_ms.assign(input.height, 0.0);
        acc.server_database_ntt_color_ms.assign(input.height, 0.0);
        acc.server_multiply_accumulate_color_ms.assign(input.height, 0.0);
        acc.server_inverse_ntt_color_ms.assign(input.height, 0.0);
        acc.server_recursive_decomposition_modswitch_color_ms.assign(input.height, 0.0);
        acc.server_modswitch_color_ms.assign(input.height, 0.0);
        acc.server_color_ms.assign(input.height, 0.0);
        acc.query_color_bytes.assign(input.height, 0);
        acc.response_color_bytes.assign(input.height, 0);
        accum.push_back(std::move(acc));
    }

    double offline_key_setup_ms = 0.0;
    double database_setup_ms = 0.0;
    uint64_t helper_key_bytes = 0;
    const uint32_t client_id = 0;
    for (unsigned int color = 0; color < input.height; ++color) {
        uint64_t entries = input.effective_color_bucket_sizes[color];
        PirParams pir_params;
        gen_pir_params(entries, kEntrySize, options.pir_d, enc_params, pir_params, true, true, true);
        uint64_t largest_dimension = *max_element(pir_params.nvec.begin(), pir_params.nvec.end());
        if (largest_dimension > options.poly_degree) {
            throw runtime_error("SealPIR expand_query cannot handle nvec dimension > poly degree; use --pir-d 2 or larger --poly-degree");
        }

        auto key_start = Clock::now();
        PIRClient client(enc_params, pir_params);
        GaloisKeys galois_keys = client.generate_galois_keys();
        auto key_end = Clock::now();
        offline_key_setup_ms += elapsed_ms(key_start, key_end);
        ostringstream key_stream(ios::out | ios::binary);
        helper_key_bytes += static_cast<uint64_t>(galois_keys.save(key_stream, compr_mode_type::none));

        auto db_start = Clock::now();
        unique_ptr<const uint8_t[]> db = make_color_database(input.height, color, entries);
        PIRServer server(enc_params, pir_params);
        server.set_galois_key(client_id, galois_keys);
        server.set_database(db, entries, kEntrySize);
        server.preprocess_database();
        auto db_end = Clock::now();
        database_setup_ms += elapsed_ms(db_start, db_end);

        for (size_t i = 0; i < selected.size(); ++i) {
            const uint64_t element_index = selected[i].sub_indices[color];
            const uint64_t fv_index = client.get_fv_index(element_index);
            const uint64_t fv_offset = client.get_fv_offset(element_index);

            auto query_start = Clock::now();
            PirQuery query = client.generate_query(fv_index);
            auto query_end = Clock::now();
            accum[i].query_ms += elapsed_ms(query_start, query_end);
            accum[i].query_ciphertexts += count_query_ciphertexts(query);

            auto query_ser_start = Clock::now();
            uint64_t query_bytes = serialize_query_payload_none(query);
            auto query_ser_end = Clock::now();
            accum[i].query_serialize_ms += elapsed_ms(query_ser_start, query_ser_end);
            accum[i].query_bytes += query_bytes;
            accum[i].query_color_bytes[color] = query_bytes;

            auto server_start = Clock::now();
            PirReply reply = server.generate_reply(query, client_id);
            auto server_end = Clock::now();
            double color_generate_reply_ms = elapsed_ms(server_start, server_end);
            const SealPirServerProfile &profile = server.last_profile();
            accum[i].server_generate_reply_ms += color_generate_reply_ms;
            accum[i].server_generate_reply_color_ms[color] = color_generate_reply_ms;
            accum[i].server_expansion_ms += profile.expansion_ms;
            accum[i].server_expansion_color_ms[color] = profile.expansion_ms;
            accum[i].server_query_ntt_ms += profile.query_ntt_ms;
            accum[i].server_query_ntt_color_ms[color] = profile.query_ntt_ms;
            accum[i].server_database_ntt_ms += profile.database_ntt_ms;
            accum[i].server_database_ntt_color_ms[color] = profile.database_ntt_ms;
            accum[i].server_multiply_accumulate_ms += profile.multiply_accumulate_ms;
            accum[i].server_multiply_accumulate_color_ms[color] = profile.multiply_accumulate_ms;
            accum[i].server_inverse_ntt_ms += profile.inverse_ntt_ms;
            accum[i].server_inverse_ntt_color_ms[color] = profile.inverse_ntt_ms;
            accum[i].server_recursive_decomposition_modswitch_ms += profile.recursive_decomposition_modswitch_ms;
            accum[i].server_recursive_decomposition_modswitch_color_ms[color] = profile.recursive_decomposition_modswitch_ms;
            accum[i].response_ciphertexts += reply.size();

            auto response_modswitch_start = Clock::now();
            server.mod_switch_reply(reply);
            auto response_modswitch_end = Clock::now();
            double modswitch_ms = elapsed_ms(response_modswitch_start, response_modswitch_end);
            accum[i].server_modswitch_ms += modswitch_ms;
            accum[i].server_modswitch_color_ms[color] = modswitch_ms;
            accum[i].server_ms += color_generate_reply_ms + modswitch_ms;
            accum[i].server_color_ms[color] = color_generate_reply_ms + modswitch_ms;

            auto response_ser_start = Clock::now();
            uint64_t reply_bytes = serialize_reply_payload_none(reply);
            auto response_ser_end = Clock::now();
            accum[i].response_serialize_ms += elapsed_ms(response_ser_start, response_ser_end);
            accum[i].response_bytes += reply_bytes;
            accum[i].response_color_bytes[color] = reply_bytes;

            auto decode_start = Clock::now();
            vector<uint8_t> decoded = client.decode_reply(reply, fv_offset);
            auto decode_end = Clock::now();
            accum[i].decode_ms += elapsed_ms(decode_start, decode_end);
            ++accum[i].decoded_entries;
            accum[i].correct = accum[i].correct && (decoded == deterministic_entry(input.height, color, element_index));
        }
    }

    write_output(options.output, render_json(input, options, selected, accum, offline_key_setup_ms, database_setup_ms, helper_key_bytes));
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    try {
        return run(argc, argv);
    } catch (const exception &e) {
        cerr << "treepir_sealpir_adapter error: " << e.what() << '\n';
        return 1;
    }
}
