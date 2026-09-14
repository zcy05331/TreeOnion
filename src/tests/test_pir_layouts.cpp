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

// Record `index` has coefficient c = (31*index + 7*c + 5) mod t: neighbouring
// rows differ in every coefficient, so a response that decodes an adjacent
// row (or a zero padding row) is never mistaken for the requested record.
PlaintextSource record_source(const PirParams &params) {
  const uint64_t t = params.get_plain_mod();
  const size_t n = params.get_poly_degree();
  return [t, n](size_t index, RlwePt &out) {
    out.data.resize(n);
    for (size_t c = 0; c < n; ++c) out.data[c] = (31 * index + 7 * c + 5) % t;
  };
}

// Query every row of the remaining-dimension tree of `params` at both ends of
// the first dimension through the production pipeline (packed query,
// ExpandBFV, RGSW completion, first dimension, ragged CMux tree, mod switch,
// wire codec) and require each response to decode to the requested record.
void sweep_rows(PirClient &client, const SharedPirSessionKeys &keys,
                const PirParams &params, const std::string &layout_name) {
  PirServer server(params);
  server.set_client_session_keys(client.get_client_id(), keys);
  const PlaintextSource source = record_source(params);
  // The sweep visits every cell of the layout, and rows * N0 == get_num_pt()
  // is the padded count, which exceeds the requested target whenever the
  // layout rounds up (11 x 8 == 88 at L_EP = 6, but 6 x 16 == 96 at L_EP = 5).
  // Loading only the target would leave the padding cells as server zeros and
  // the sweep would compare them against a record that was never stored.
  server.load_data(params.get_num_pt(), source);

  const size_t n0 = params.get_fst_dim_sz();
  const size_t rows = params.get_other_dim_sz();
  const size_t h = params.get_num_other_dims();
  // evaluate_other_dim's deepest level: rows below `offset` have no sibling
  // and stay in place, rows from `offset` on are folded in pairs by the first
  // selector before the perfect levels take over.
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

}  // namespace

// Every row of a ragged and of a perfect remaining-dimension tree round-trips
// through make_query. test_pir samples four rows of an 86-row tree and
// test_pir_profile one paired row of a 3-row tree, so an off-by-one in
// evaluate_other_dim's `offset`, `corrected_idx` or `i/2 + offset` write-back,
// or in get_query_indices' folded-pair bit and MSB-first emission, could keep
// decoding most rows; enumerating rows offset-1 (last unpaired), offset (first
// paired) and every path through the perfect levels does not let it.
void test_pir_row_sweep() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);
  SharedPirSessionKeys keys = client.create_session_keys();

  // 88 plaintexts under expansion height 5: at L_EP=6 this is N0=8 with 11
  // rows (h=4: rows 0..4 unpaired, rows 5..10 folded into parents 5..7, then
  // three perfect levels); at L_EP=5 it is N0=16 with 6 rows (h=3, 2
  // unpaired). With N0=8 every row is one load_data tile, so the two swept
  // columns are also both tile edges.
  const PirParams ragged = scheme.with_layout({88, 5, true});
  const size_t ragged_rows = ragged.get_other_dim_sz();
  require_test((ragged_rows & (ragged_rows - 1)) != 0 &&
                   ragged.get_num_other_dims() >= 3,
               "ragged fixture needs a non-power-of-two row count under at "
               "least three selector levels");
  sweep_rows(client, keys, ragged, "ragged");

  // 64 plaintexts under height 5: N0=16 with 4 rows, a complete deepest level
  // (offset 0) followed by one perfect level.
  const PirParams perfect = scheme.with_layout({64, 5, true});
  const size_t perfect_rows = perfect.get_other_dim_sz();
  require_test(perfect_rows == (size_t{1} << perfect.get_num_other_dims()),
               "perfect fixture needs a power-of-two row count");
  sweep_rows(client, keys, perfect, "perfect");
}

// with_query_shape's capacity check and fast_expand_qry's useful-leaf pruning
// at the accept-side boundary fst_dim_sz + L_EP*selectors == 2^h. Tree
// queries produce this shape whenever w is a power of two (L=21, a=5 at
// L_EP=6: 32 + 6*16 = 128 = 2^7). test_tree_query only rejects over-capacity
// shapes and expands shapes strictly below capacity, so there the pruning
// branch is always live and the exact-fit accept path never runs.
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
    // Fill the capacity exactly: as many selector groups as fit, the
    // remaining slots (at least one) as the first dimension.
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

    // Expand a query whose only constant sits in the last logical slot. With
    // useful_cnt == capacity no subtree may be pruned, so the leaf that every
    // smaller shape skips must come back as a clean encryption of 1 and all
    // other leaves as encryptions of 0.
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
