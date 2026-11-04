#include "tests.h"
#include "tree_query.h"

#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

void test_pir_row_sweep();
void test_query_shape_exact_fit();

namespace {

PlaintextSource record_source(const PirParams &params) {
  const uint64_t t = params.get_plain_mod();
  const size_t n = params.get_poly_degree();
  return [t, n](size_t index, RlwePt &out) {
    out.data.resize(n);
    for (size_t c = 0; c < n; ++c) out.data[c] = (31 * index + 7 * c + 5) % t;
  };
}

void sweep_rows(PirClient &client, const SharedPirSessionKeys &keys,
                const PirParams &params, const std::string &layout_name) {
  PirServer server(params);
  server.set_client_session_keys(client.get_client_id(), keys);
  const PlaintextSource source = record_source(params);
  server.load_data(params.get_num_pt(), source);

  const size_t n0 = params.get_fst_dim_sz();
  const size_t rows = params.get_other_dim_sz();
  const size_t h = params.get_num_other_dims();
  const size_t offset = (size_t{1} << h) - rows;

  int min_budget = std::numeric_limits<int>::max();
  uint64_t worst_noise = 0;
  for (size_t row = 0; row < rows; ++row) {
    for (size_t col : {size_t{0}, n0 - 1}) {
      const size_t idx = row * n0 + col;
      RlweCt query = client.fast_generate_query(params, idx);
      RlweCt response = server.make_query(client.get_client_id(), query);
      std::stringstream wire;
      server.save_resp_to_stream(response, wire);
      RlweCt loaded = client.load_resp_from_stream(wire);
      uint64_t max_noise = 0;
      int budget = 0;
      const RlwePt decoded =
          client.decrypt_mod_q_measured(loaded, max_noise, budget);
      RlwePt expected;
      source(idx, expected);
      require_test(utils::plaintext_is_equal(decoded, expected),
                   layout_name + " layout: row " + std::to_string(row) + " (" +
                       (row < offset ? "unpaired" : "paired") + "), column " +
                       std::to_string(col) + ", plaintext " +
                       std::to_string(idx) + " decoded to the wrong record");
      min_budget = std::min(min_budget, budget);
      worst_noise = std::max(worst_noise, max_noise);
    }
  }
  BENCH_PRINT(layout_name << " layout: N0=" << n0 << ", " << rows
              << " rows (h=" << h << ", " << offset << " unpaired), "
              << 2 * rows << " queries decoded; min budget " << min_budget
              << " bits (max noise " << worst_noise << ")");
}

bool plaintext_is_zero(const RlwePt &pt) {
  for (uint64_t value : pt.data) {
    if (value != 0) return false;
  }
  return true;
}

}

void test_pir_row_sweep() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);
  SharedPirSessionKeys keys = client.create_session_keys();

  const PirParams ragged = scheme.with_layout({88, 5, true});
  const size_t ragged_rows = ragged.get_other_dim_sz();
  require_test((ragged_rows & (ragged_rows - 1)) != 0 &&
                   ragged.get_num_other_dims() >= 3,
               "ragged fixture needs a non-power-of-two row count under at "
               "least three selector levels");
  sweep_rows(client, keys, ragged, "ragged");

  const PirParams perfect = scheme.with_layout({64, 5, true});
  const size_t perfect_rows = perfect.get_other_dim_sz();
  require_test(perfect_rows == (size_t{1} << perfect.get_num_other_dims()),
               "perfect fixture needs a power-of-two row count");
  sweep_rows(client, keys, perfect, "perfect");
}

void test_query_shape_exact_fit() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);
  SharedPirSessionKeys keys = client.create_session_keys();
  const size_t l = scheme.get_l();
  const uint64_t t = scheme.get_plain_mod();

  auto rejects = [&](const PirQueryShapeConfig &shape) {
    try {
      (void)scheme.with_query_shape(shape);
    } catch (const std::invalid_argument &) {
      return true;
    }
    return false;
  };

  for (size_t h : {size_t{5}, size_t{7}}) {
    const size_t capacity = size_t{1} << h;
    const size_t selectors = (capacity - 1) / l;
    const size_t fst = capacity - l * selectors;
    const PirParams view = scheme.with_query_shape({fst, selectors, h});
    require_test(view.get_fst_dim_sz() == fst &&
                     view.get_num_dims() == selectors + 1 &&
                     view.get_expan_height() == h &&
                     view.get_other_dim_sz() == 1,
                 "exact-fit view does not report the requested shape");
    require_test(rejects({fst + 1, selectors, h}),
                 "accepted one first-dimension slot beyond an exact fit");
    require_test(rejects({fst, selectors + 1, h}),
                 "accepted one selector group beyond an exact fit");
    require_test(rejects({0, 0, h}),
                 "accepted a query shape with an empty first dimension");

    PirServer server(view);
    server.set_client_session_keys(client.get_client_id(), keys);
    uint64_t capacity_inv = 0;
    require_test(utils::try_invert_uint_mod(capacity, t, capacity_inv),
                 "expansion capacity must be invertible mod t");
    RlweCt query = client.fresh_zero_ct();
    add_bfv_query_constant(query, scheme, capacity - 1, h, capacity_inv);
    const std::vector<RlweCt> leaves =
        server.expand_query(client.get_client_id(), query);
    require_test(leaves.size() == capacity,
                 "exact-fit expansion must return every leaf");
    for (size_t j = 0; j < capacity; ++j) {
      RlwePt pt = client.decrypt_ct(leaves[j]);
      const uint64_t want = (j + 1 == capacity) ? 1 : 0;
      require_test(pt.data[0] == want,
                   "exact-fit leaf " + std::to_string(j) +
                       " decrypts to the wrong constant");
      pt.data[0] = 0;
      require_test(plaintext_is_zero(pt),
                   "exact-fit leaf " + std::to_string(j) +
                       " carries a stray message");
    }
    BENCH_PRINT("exact fit h=" << h << ": " << fst << " + " << l << "*"
                << selectors << " = " << capacity
                << " slots accepted, all " << capacity << " leaves expanded");
  }
}
