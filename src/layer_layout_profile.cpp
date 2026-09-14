#include "layer_layout_planner.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace {

// ---------------------------------------------------------------------------
// Minimal JSON (writer helpers + strict reader), enough for the profile file.
// ---------------------------------------------------------------------------

struct JsonValue {
  enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  std::string raw_number;  // exact integer text for uint64 fields
  std::string str;
  std::vector<JsonValue> array;
  std::vector<std::pair<std::string, JsonValue>> object;

  const JsonValue &at(const std::string &key) const {
    for (const auto &[k, v] : object) {
      if (k == key) return v;
    }
    throw std::invalid_argument("layer layout profile: missing key \"" + key +
                                "\"");
  }
  bool has(const std::string &key) const {
    for (const auto &[k, v] : object) {
      if (k == key) return true;
    }
    return false;
  }
  uint64_t as_u64() const {
    if (type != Type::Number) {
      throw std::invalid_argument("layer layout profile: expected a number");
    }
    if (raw_number.find_first_of(".eE") == std::string::npos) {
      return std::stoull(raw_number);
    }
    if (number < 0 || number != std::floor(number)) {
      throw std::invalid_argument(
          "layer layout profile: expected a non-negative integer");
    }
    return static_cast<uint64_t>(number);
  }
  double as_double() const {
    if (type != Type::Number) {
      throw std::invalid_argument("layer layout profile: expected a number");
    }
    return number;
  }
  const std::string &as_string() const {
    if (type != Type::String) {
      throw std::invalid_argument("layer layout profile: expected a string");
    }
    return str;
  }
  bool as_bool() const {
    if (type != Type::Bool) {
      throw std::invalid_argument("layer layout profile: expected a boolean");
    }
    return boolean;
  }
  const std::vector<JsonValue> &as_array() const {
    if (type != Type::Array) {
      throw std::invalid_argument("layer layout profile: expected an array");
    }
    return array;
  }
};

class JsonParser {
 public:
  explicit JsonParser(const std::string &text) : text_(text) {}

  JsonValue parse_document() {
    JsonValue value = parse_value();
    skip_ws();
    if (pos_ != text_.size()) fail("trailing characters");
    return value;
  }

 private:
  const std::string &text_;
  size_t pos_ = 0;

  [[noreturn]] void fail(const char *what) const {
    throw std::invalid_argument(std::string("layer layout profile JSON: ") +
                                what + " at offset " + std::to_string(pos_));
  }
  void skip_ws() {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
            text_[pos_] == '\t')) {
      ++pos_;
    }
  }
  bool consume(char c) {
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }
  void expect(char c) {
    if (!consume(c)) fail("unexpected character");
  }
  JsonValue parse_value() {
    skip_ws();
    if (pos_ >= text_.size()) fail("unexpected end of input");
    const char c = text_[pos_];
    JsonValue v;
    if (c == '{') {
      v.type = JsonValue::Type::Object;
      ++pos_;
      if (consume('}')) return v;
      do {
        skip_ws();
        std::string key = parse_string();
        expect(':');
        JsonValue member = parse_value();
        v.object.emplace_back(std::move(key), std::move(member));
      } while (consume(','));
      expect('}');
      return v;
    }
    if (c == '[') {
      v.type = JsonValue::Type::Array;
      ++pos_;
      if (consume(']')) return v;
      do {
        v.array.push_back(parse_value());
      } while (consume(','));
      expect(']');
      return v;
    }
    if (c == '"') {
      v.type = JsonValue::Type::String;
      v.str = parse_string();
      return v;
    }
    if (text_.compare(pos_, 4, "true") == 0) {
      v.type = JsonValue::Type::Bool;
      v.boolean = true;
      pos_ += 4;
      return v;
    }
    if (text_.compare(pos_, 5, "false") == 0) {
      v.type = JsonValue::Type::Bool;
      pos_ += 5;
      return v;
    }
    if (text_.compare(pos_, 4, "null") == 0) {
      pos_ += 4;
      return v;
    }
    // number
    const size_t start = pos_;
    while (pos_ < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
            text_[pos_] == '-' || text_[pos_] == '+' || text_[pos_] == '.' ||
            text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
    }
    if (start == pos_) fail("unexpected character");
    v.type = JsonValue::Type::Number;
    v.raw_number = text_.substr(start, pos_ - start);
    v.number = std::stod(v.raw_number);
    return v;
  }
  std::string parse_string() {
    expect('"');
    std::string out;
    while (true) {
      if (pos_ >= text_.size()) fail("unterminated string");
      const char c = text_[pos_++];
      if (c == '"') break;
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) fail("bad escape");
      const char e = text_[pos_++];
      switch (e) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          if (pos_ + 4 > text_.size()) fail("bad unicode escape");
          const unsigned code =
              static_cast<unsigned>(std::stoul(text_.substr(pos_, 4), nullptr, 16));
          pos_ += 4;
          if (code < 0x80) {
            out.push_back(static_cast<char>(code));
          } else {
            fail("non-ASCII unicode escapes are not supported");
          }
          break;
        }
        default: fail("bad escape");
      }
    }
    return out;
  }
};

std::string json_quote(const std::string &value) {
  std::ostringstream out;
  out << '"';
  for (unsigned char c : value) {
    switch (c) {
      case '"': out << "\\\""; break;
      case '\\': out << "\\\\"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (c < 0x20) {
          out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
              << static_cast<unsigned>(c) << std::dec;
        } else {
          out << c;
        }
    }
  }
  out << '"';
  return out.str();
}

void write_features(std::ostringstream &out, const LayerLayoutFeatures &f,
                    const char *indent) {
  out << indent << "\"expansion_height\": " << f.expansion_height << ",\n"
      << indent << "\"first_dim_size\": " << f.first_dim_size << ",\n"
      << indent << "\"other_dim_size\": " << f.other_dim_size << ",\n"
      << indent << "\"other_dim_count\": " << f.other_dim_count << ",\n"
      << indent << "\"padded_plaintexts\": " << f.padded_plaintexts << ",\n"
      << indent << "\"padding_plaintexts\": " << f.padding_plaintexts << ",\n"
      << indent << "\"useful_expanded_ciphertexts\": "
      << f.useful_expanded_ciphertexts << ",\n"
      << indent << "\"expansion_substitutions\": " << f.expansion_substitutions
      << ",\n"
      << indent << "\"first_dim_query_ntts\": " << f.first_dim_query_ntts
      << ",\n"
      << indent << "\"selector_rows_to_complete\": "
      << f.selector_rows_to_complete << ",\n"
      << indent << "\"crt_coefficients_to_compose\": "
      << f.crt_coefficients_to_compose << ",\n"
      << indent << "\"inverse_ntts\": " << f.inverse_ntts << ",\n"
      << indent << "\"cmux_count\": " << f.cmux_count << ",\n"
      << indent << "\"logical_padded_bytes\": " << f.logical_padded_bytes
      << ",\n"
      << indent << "\"physical_scan_bytes\": " << f.physical_scan_bytes;
}

LayerLayoutFeatures read_features(const JsonValue &v) {
  LayerLayoutFeatures f;
  f.expansion_height = v.at("expansion_height").as_u64();
  f.first_dim_size = v.at("first_dim_size").as_u64();
  f.other_dim_size = v.at("other_dim_size").as_u64();
  f.other_dim_count = v.at("other_dim_count").as_u64();
  f.padded_plaintexts = v.at("padded_plaintexts").as_u64();
  f.padding_plaintexts = v.at("padding_plaintexts").as_u64();
  f.useful_expanded_ciphertexts = v.at("useful_expanded_ciphertexts").as_u64();
  f.expansion_substitutions = v.at("expansion_substitutions").as_u64();
  f.first_dim_query_ntts = v.at("first_dim_query_ntts").as_u64();
  f.selector_rows_to_complete = v.at("selector_rows_to_complete").as_u64();
  f.crt_coefficients_to_compose = v.at("crt_coefficients_to_compose").as_u64();
  f.inverse_ntts = v.at("inverse_ntts").as_u64();
  f.cmux_count = v.at("cmux_count").as_u64();
  f.logical_padded_bytes = v.at("logical_padded_bytes").as_u64();
  f.physical_scan_bytes = v.at("physical_scan_bytes").as_u64();
  return f;
}

std::string detect_cpu_model() {
#if defined(__APPLE__)
  char buffer[256];
  size_t length = sizeof(buffer);
  if (sysctlbyname("machdep.cpu.brand_string", buffer, &length, nullptr, 0) ==
      0) {
    return std::string(buffer, length > 0 ? length - 1 : 0);
  }
  return "unknown";
#else
  std::ifstream cpuinfo("/proc/cpuinfo");
  std::string line;
  while (std::getline(cpuinfo, line)) {
    if (line.rfind("model name", 0) == 0) {
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        size_t start = colon + 1;
        while (start < line.size() && line[start] == ' ') ++start;
        return line.substr(start);
      }
    }
  }
  return "unknown";
#endif
}

double median_of(std::vector<double> values) {
  if (values.empty()) {
    throw std::invalid_argument("layer layout profile: empty sample set");
  }
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  return n % 2 == 1 ? values[n / 2]
                    : 0.5 * (values[n / 2 - 1] + values[n / 2]);
}


}  // namespace

// ---------------------------------------------------------------------------
// Profile environment, JSON I/O, validation
// ---------------------------------------------------------------------------

LayerLayoutProfileEnvironment detect_layer_layout_environment(
    const PirParams &reference, const std::string &commit,
    const std::string &build_type, const std::string &config) {
  LayerLayoutProfileEnvironment env;
  env.commit = commit;
  env.build_type = build_type;
  env.config = config;
  env.cpu = detect_cpu_model();
#if defined(__x86_64__)
  env.architecture = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__)
  env.architecture = "arm64";
#else
  env.architecture = "unknown";
#endif
  env.compiler = __VERSION__;
  env.hexl_version = "1.2.6";
  env.poly_degree = reference.get_poly_degree();
  env.log_q = reference.get_ct_mod_width();
  env.log_t = std::bit_width(reference.get_plain_mod());
  env.log_q_prime = std::bit_width(reference.get_small_q() - 1);
  env.l_ep = reference.get_l();
  env.l_key = reference.get_l_key();
  env.l_ks = DBConsts::L_KS;
  env.composite_first_dim = reference.get_composite_rns().enabled;
  return env;
}

void save_layer_layout_profile(const LayerLayoutProfile &profile,
                               const std::string &path) {
  std::ostringstream out;
  out << std::setprecision(17);
  const auto &e = profile.environment;
  out << "{\n  \"schema_version\": " << json_quote(profile.schema_version)
      << ",\n  \"environment\": {\n"
      << "    \"commit\": " << json_quote(e.commit) << ",\n"
      << "    \"build_type\": " << json_quote(e.build_type) << ",\n"
      << "    \"config\": " << json_quote(e.config) << ",\n"
      << "    \"cpu\": " << json_quote(e.cpu) << ",\n"
      << "    \"architecture\": " << json_quote(e.architecture) << ",\n"
      << "    \"compiler\": " << json_quote(e.compiler) << ",\n"
      << "    \"hexl_version\": " << json_quote(e.hexl_version) << ",\n"
      << "    \"poly_degree\": " << e.poly_degree << ",\n"
      << "    \"log_q\": " << e.log_q << ",\n"
      << "    \"log_t\": " << e.log_t << ",\n"
      << "    \"log_q_prime\": " << e.log_q_prime << ",\n"
      << "    \"L_EP\": " << e.l_ep << ",\n"
      << "    \"L_KEY\": " << e.l_key << ",\n"
      << "    \"L_KS\": " << e.l_ks << ",\n"
      << "    \"composite_first_dim\": "
      << (e.composite_first_dim ? "true" : "false") << "\n  },\n"
      << "  \"tree_height\": " << profile.tree_height << ",\n"
      << "  \"nodes_per_plaintext\": " << profile.nodes_per_plaintext << ",\n"
      << "  \"warmups\": " << profile.warmups << ",\n"
      << "  \"measured_trials\": " << profile.measured_trials << ",\n"
      << "  \"trial_seed\": " << profile.trial_seed << ",\n"
      << "  \"padding_budget\": " << profile.padding_budget << ",\n"
      << "  \"legacy_total_padded_plaintexts\": "
      << profile.legacy_total_padded_plaintexts << ",\n"
      << "  \"selected_total_padded_plaintexts\": "
      << profile.selected_total_padded_plaintexts << ",\n"
      << "  \"selected_expansion_heights\": [";
  for (size_t i = 0; i < profile.selected_expansion_heights.size(); ++i) {
    out << (i ? ", " : "") << profile.selected_expansion_heights[i];
  }
  out << "],\n  \"measurements\": [";
  for (size_t i = 0; i < profile.measurements.size(); ++i) {
    const LayerLayoutMeasurement &m = profile.measurements[i];
    out << (i ? ",\n    {\n" : "\n    {\n")
        << "      \"level\": " << m.level << ",\n";
    write_features(out, m.features, "      ");
    out << ",\n      \"dominated\": " << (m.dominated ? "true" : "false")
        << ",\n      \"median_server_ms\": " << m.median_server_ms
        << ",\n      \"server_samples_ms\": [";
    for (size_t j = 0; j < m.server_samples_ms.size(); ++j) {
      out << (j ? ", " : "") << m.server_samples_ms[j];
    }
    out << "]\n    }";
  }
  out << (profile.measurements.empty() ? "]\n}\n" : "\n  ]\n}\n");

  const std::string tmp = path + ".tmp";
  {
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    if (!file) {
      throw std::runtime_error("cannot write layer layout profile: " + tmp);
    }
    file << out.str();
    file.flush();
    if (!file) {
      throw std::runtime_error("cannot flush layer layout profile: " + tmp);
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    throw std::runtime_error("cannot rename layer layout profile into place: " +
                             path);
  }
}

LayerLayoutProfile load_layer_layout_profile(const std::string &path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("cannot read layer layout profile: " + path);
  }
  std::stringstream buffer;
  buffer << file.rdbuf();
  const std::string text = buffer.str();
  JsonValue root = JsonParser(text).parse_document();

  LayerLayoutProfile profile;
  profile.schema_version = root.at("schema_version").as_string();
  if (profile.schema_version != "layer-layout-profile-v1") {
    throw std::invalid_argument("unsupported layer layout profile schema: " +
                                profile.schema_version);
  }
  const JsonValue &e = root.at("environment");
  auto &env = profile.environment;
  env.commit = e.at("commit").as_string();
  env.build_type = e.at("build_type").as_string();
  env.config = e.at("config").as_string();
  env.cpu = e.at("cpu").as_string();
  env.architecture = e.at("architecture").as_string();
  env.compiler = e.at("compiler").as_string();
  env.hexl_version = e.at("hexl_version").as_string();
  env.poly_degree = e.at("poly_degree").as_u64();
  env.log_q = e.at("log_q").as_u64();
  env.log_t = e.at("log_t").as_u64();
  env.log_q_prime = e.at("log_q_prime").as_u64();
  env.l_ep = e.at("L_EP").as_u64();
  env.l_key = e.at("L_KEY").as_u64();
  env.l_ks = e.at("L_KS").as_u64();
  env.composite_first_dim = e.at("composite_first_dim").as_bool();
  profile.tree_height = root.at("tree_height").as_u64();
  profile.nodes_per_plaintext = root.at("nodes_per_plaintext").as_u64();
  profile.warmups = root.at("warmups").as_u64();
  profile.measured_trials = root.at("measured_trials").as_u64();
  profile.trial_seed = root.at("trial_seed").as_u64();
  profile.padding_budget = root.at("padding_budget").as_double();
  profile.legacy_total_padded_plaintexts =
      root.at("legacy_total_padded_plaintexts").as_u64();
  profile.selected_total_padded_plaintexts =
      root.at("selected_total_padded_plaintexts").as_u64();
  for (const JsonValue &h : root.at("selected_expansion_heights").as_array()) {
    profile.selected_expansion_heights.push_back(h.as_u64());
  }
  for (const JsonValue &m : root.at("measurements").as_array()) {
    LayerLayoutMeasurement measurement;
    measurement.level = m.at("level").as_u64();
    measurement.features = read_features(m);
    measurement.dominated = m.at("dominated").as_bool();
    measurement.median_server_ms = m.at("median_server_ms").as_double();
    for (const JsonValue &s : m.at("server_samples_ms").as_array()) {
      measurement.server_samples_ms.push_back(s.as_double());
    }
    if (measurement.server_samples_ms.empty()) {
      throw std::invalid_argument(
          "layer layout profile: a measurement has no samples");
    }
    // The selector minimises these numbers, so a file cannot carry a
    // negative or non-finite sample, and its stored median must be the
    // median of its samples (1e-4 relative covers the writer's 6-digit
    // formatting; a hand edit of either field is refused).
    for (double sample : measurement.server_samples_ms) {
      if (!std::isfinite(sample) || sample < 0.0) {
        throw std::invalid_argument(
            "layer layout profile: server_samples_ms must be finite and "
            "non-negative");
      }
    }
    const double median = median_server_ms_of(measurement.server_samples_ms);
    if (!std::isfinite(measurement.median_server_ms) ||
        std::abs(measurement.median_server_ms - median) >
            1e-4 * std::max(1.0, median)) {
      throw std::invalid_argument(
          "layer layout profile: median_server_ms does not match "
          "server_samples_ms");
    }
    measurement.median_server_ms = median;
    profile.measurements.push_back(std::move(measurement));
  }
  return profile;
}

double median_server_ms_of(const std::vector<double> &samples_ms) {
  if (samples_ms.empty()) {
    throw std::invalid_argument("median of an empty sample set");
  }
  std::vector<double> sorted = samples_ms;
  std::sort(sorted.begin(), sorted.end());
  const size_t n = sorted.size();
  return n % 2 == 1 ? sorted[n / 2]
                    : 0.5 * (sorted[n / 2 - 1] + sorted[n / 2]);
}

std::string describe_layer_layout_profile_mismatch(
    const LayerLayoutProfile &profile,
    const LayerLayoutProfileEnvironment &runtime, size_t tree_height,
    size_t nodes_per_pt) {
  const auto &e = profile.environment;
  const auto mismatch = [](const std::string &field, const std::string &have,
                           const std::string &want) {
    return "layer layout profile " + field + " mismatch: profile has \"" +
           have + "\", runtime has \"" + want + "\"";
  };
  const auto num = [](size_t v) { return std::to_string(v); };
  if (e.poly_degree != runtime.poly_degree)
    return mismatch("poly_degree", num(e.poly_degree), num(runtime.poly_degree));
  if (e.log_q != runtime.log_q)
    return mismatch("log_q", num(e.log_q), num(runtime.log_q));
  if (e.log_t != runtime.log_t)
    return mismatch("log_t", num(e.log_t), num(runtime.log_t));
  if (e.log_q_prime != runtime.log_q_prime)
    return mismatch("log_q_prime", num(e.log_q_prime), num(runtime.log_q_prime));
  if (e.l_ep != runtime.l_ep) return mismatch("L_EP", num(e.l_ep), num(runtime.l_ep));
  if (e.l_key != runtime.l_key)
    return mismatch("L_KEY", num(e.l_key), num(runtime.l_key));
  if (e.l_ks != runtime.l_ks) return mismatch("L_KS", num(e.l_ks), num(runtime.l_ks));
  if (e.composite_first_dim != runtime.composite_first_dim)
    return mismatch("composite_first_dim", e.composite_first_dim ? "true" : "false",
                    runtime.composite_first_dim ? "true" : "false");
  if (profile.nodes_per_plaintext != nodes_per_pt)
    return mismatch("nodes_per_plaintext", num(profile.nodes_per_plaintext),
                    num(nodes_per_pt));
  if (profile.tree_height != tree_height)
    return mismatch("tree_height", num(profile.tree_height), num(tree_height));
  if (e.architecture != runtime.architecture)
    return mismatch("architecture", e.architecture, runtime.architecture);
  if (e.cpu != runtime.cpu) return mismatch("cpu", e.cpu, runtime.cpu);
  if (e.compiler != runtime.compiler)
    return mismatch("compiler", e.compiler, runtime.compiler);
  if (e.hexl_version != runtime.hexl_version)
    return mismatch("hexl_version", e.hexl_version, runtime.hexl_version);
  return "";
}

