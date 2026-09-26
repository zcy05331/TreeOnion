#include "tests.h"
#include "query_wire.h"

#include <sstream>

void test_query_wire();

// The seeded query codec under the active configuration: the byte count is
// 32 + ceil(N ceil(log2 q) / 8), the reader returns c0 bit for bit through the
// CRT composition, and it regenerates c1 from the seed alone. Two queries
// draw different seeds.
void test_query_wire() {
  print_func_name(__FUNCTION__);
  PirParams scheme;
  PirClient client(scheme);

  QuerySeed seed{}, other_seed{};
  const RlweCt query = client.fresh_zero_ct_seeded(seed);
  const RlweCt other = client.fresh_zero_ct_seeded(other_seed);
  require_test(seed != other_seed && query.c1 != other.c1,
               "every seeded query draws a fresh seed and mask");

  std::stringstream wire;
  const size_t bytes = save_seeded_query(query, seed, scheme, wire);
  require_test(bytes == seeded_query_bytes(scheme) &&
                   wire.str().size() == bytes,
               "the codec writes exactly the documented number of bytes");
  if (paper_config_active()) {
    require_test(bytes == 14880,
                 "the paper configuration sends a 14,880-byte query");
  }

  const RlweCt received = load_seeded_query(wire, scheme);
  require_test(received.c0 == query.c0,
               "c0 survives the round trip through the composite modulus");
  require_test(received.c1 == query.c1,
               "the reader regenerates c1 from the seed");
  require_test(!received.ntt_form, "the received query is in coefficient form");

  BENCH_PRINT("seeded query: " << bytes << " bytes (" << scheme.get_rns_mods().size()
              << " limb(s), c1 regenerated from the seed)");
}
