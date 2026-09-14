#!/usr/bin/env python3
import shutil
import sys
from pathlib import Path


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old not in text:
        raise RuntimeError(f"patch anchor not found: {label}")
    return text.replace(old, new, 1)


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch_sealpir_core.py SEALPIRPLUS_DIR OUTPUT_DIR")
    src = Path(sys.argv[1]).resolve()
    out = Path(sys.argv[2]).resolve()
    out.mkdir(parents=True, exist_ok=True)

    for name in ["pir.hpp", "pir.cpp", "pir_client.hpp", "pir_client.cpp", "pir_server.hpp", "pir_server.cpp"]:
        shutil.copy2(src / name, out / name)

    header_path = out / "pir_server.hpp"
    header = header_path.read_text()
    header = replace_once(header, '#include <vector>\n', '#include <vector>\n\nstruct SealPirServerProfile {\n  double expansion_ms = 0.0;\n  double query_ntt_ms = 0.0;\n  double database_ntt_ms = 0.0;\n  double multiply_accumulate_ms = 0.0;\n  double inverse_ntt_ms = 0.0;\n  double recursive_decomposition_modswitch_ms = 0.0;\n};\n', 'profile struct')
    header = replace_once(header, '  int serialize_reply(PirReply &reply, std::stringstream &stream);\n\n  void set_galois_key', '  int serialize_reply(PirReply &reply, std::stringstream &stream);\n  void mod_switch_reply(PirReply &reply);\n  const SealPirServerProfile &last_profile() const { return last_profile_; }\n\n  void set_galois_key', 'profile methods')
    header = replace_once(header, '  std::unique_ptr<seal::BatchEncoder> encoder_;\n  std::shared_ptr<seal::SEALContext> context_;', '  std::unique_ptr<seal::BatchEncoder> encoder_;\n  std::shared_ptr<seal::SEALContext> context_;\n  SealPirServerProfile last_profile_;', 'profile member')
    header_path.write_text(header)

    cpp_path = out / "pir_server.cpp"
    cpp = cpp_path.read_text()
    cpp = replace_once(cpp, '#include "pir_client.hpp"\n', '#include "pir_client.hpp"\n#include <chrono>\n', 'chrono include')
    cpp = replace_once(cpp, 'using namespace seal::util;\n', 'using namespace seal::util;\n\nnamespace {\nusing SealPirProfileClock = std::chrono::high_resolution_clock;\n\ndouble sealpir_profile_ms(SealPirProfileClock::time_point start, SealPirProfileClock::time_point end) {\n  return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();\n}\n}\n', 'profile helpers')
    cpp = replace_once(cpp, 'int PIRServer::serialize_reply(PirReply &reply, stringstream &stream) {\n  int output_size = 0;\n  for (int i = 0; i < reply.size(); i++) {\n    evaluator_->mod_switch_to_inplace(reply[i], context_->last_parms_id());\n    output_size += reply[i].save(stream);\n  }\n  return output_size;\n}\n\nPirReply PIRServer::generate_reply', 'int PIRServer::serialize_reply(PirReply &reply, stringstream &stream) {\n  int output_size = 0;\n  for (int i = 0; i < reply.size(); i++) {\n    evaluator_->mod_switch_to_inplace(reply[i], context_->last_parms_id());\n    output_size += reply[i].save(stream);\n  }\n  return output_size;\n}\n\nvoid PIRServer::mod_switch_reply(PirReply &reply) {\n  for (auto &ct : reply) {\n    evaluator_->mod_switch_to_inplace(ct, context_->last_parms_id());\n  }\n}\n\nPirReply PIRServer::generate_reply', 'mod switch method')
    cpp = replace_once(cpp, 'PirReply PIRServer::generate_reply(PirQuery &query, uint32_t client_id) {\n\n  vector<uint64_t> nvec = pir_params_.nvec;', 'PirReply PIRServer::generate_reply(PirQuery &query, uint32_t client_id) {\n\n  last_profile_ = SealPirServerProfile{};\n  vector<uint64_t> nvec = pir_params_.nvec;', 'profile reset')
    cpp = replace_once(cpp, '    for (uint32_t j = 0; j < query[i].size(); j++) {\n      uint64_t total = N;', '    auto expansion_start = SealPirProfileClock::now();\n    for (uint32_t j = 0; j < query[i].size(); j++) {\n      uint64_t total = N;', 'expansion start')
    cpp = replace_once(cpp, '      expanded_query_part.clear();\n    }\n    if (expanded_query.size() != n_i) {', '      expanded_query_part.clear();\n    }\n    last_profile_.expansion_ms += sealpir_profile_ms(expansion_start, SealPirProfileClock::now());\n    if (expanded_query.size() != n_i) {', 'expansion end')
    cpp = replace_once(cpp, '    // Transform expanded query to NTT, and ...\n    for (uint32_t jj = 0; jj < expanded_query.size(); jj++) {\n      evaluator_->transform_to_ntt_inplace(expanded_query[jj]);\n    }', '    // Transform expanded query to NTT, and ...\n    auto query_ntt_start = SealPirProfileClock::now();\n    for (uint32_t jj = 0; jj < expanded_query.size(); jj++) {\n      evaluator_->transform_to_ntt_inplace(expanded_query[jj]);\n    }\n    last_profile_.query_ntt_ms += sealpir_profile_ms(query_ntt_start, SealPirProfileClock::now());', 'query ntt')
    cpp = replace_once(cpp, '    if ((!is_db_preprocessed_) || i > 0) {\n      for (uint32_t jj = 0; jj < cur->size(); jj++) {\n        evaluator_->transform_to_ntt_inplace((*cur)[jj],\n                                             context_->first_parms_id());\n      }\n    }', '    auto database_ntt_start = SealPirProfileClock::now();\n    if ((!is_db_preprocessed_) || i > 0) {\n      for (uint32_t jj = 0; jj < cur->size(); jj++) {\n        evaluator_->transform_to_ntt_inplace((*cur)[jj],\n                                             context_->first_parms_id());\n      }\n    }\n    last_profile_.database_ntt_ms += sealpir_profile_ms(database_ntt_start, SealPirProfileClock::now());', 'database ntt')
    cpp = replace_once(cpp, '    for (uint64_t k = 0; k < product; k++) {\n\n      evaluator_->multiply_plain(expanded_query[0], (*cur)[k],', '    auto multiply_start = SealPirProfileClock::now();\n    for (uint64_t k = 0; k < product; k++) {\n\n      evaluator_->multiply_plain(expanded_query[0], (*cur)[k],', 'multiply start')
    cpp = replace_once(cpp, '        evaluator_->add_inplace(intermediateCtxts[k],\n                                temp); // Adds to first component.\n      }\n    }\n\n    for (uint32_t jj = 0; jj < intermediateCtxts.size(); jj++) {', '        evaluator_->add_inplace(intermediateCtxts[k],\n                                temp); // Adds to first component.\n      }\n    }\n    last_profile_.multiply_accumulate_ms += sealpir_profile_ms(multiply_start, SealPirProfileClock::now());\n\n    auto inverse_ntt_start = SealPirProfileClock::now();\n    for (uint32_t jj = 0; jj < intermediateCtxts.size(); jj++) {', 'multiply end inverse start')
    cpp = replace_once(cpp, '      // cout << "const term of ctxt " << jj << " = " <<\n      // intermediateCtxts[jj][0] << endl;\n    }\n\n    if (i == nvec.size() - 1) {', '      // cout << "const term of ctxt " << jj << " = " <<\n      // intermediateCtxts[jj][0] << endl;\n    }\n    last_profile_.inverse_ntt_ms += sealpir_profile_ms(inverse_ntt_start, SealPirProfileClock::now());\n\n    if (i == nvec.size() - 1) {', 'inverse end')
    cpp = replace_once(cpp, '    } else {\n      intermediate_plain.clear();', '    } else {\n      auto recursive_start = SealPirProfileClock::now();\n      intermediate_plain.clear();', 'recursive start')
    cpp = replace_once(cpp, '      }\n      product = intermediate_plain.size(); // multiply by expansion rate.\n    }', '      }\n      product = intermediate_plain.size(); // multiply by expansion rate.\n      last_profile_.recursive_decomposition_modswitch_ms += sealpir_profile_ms(recursive_start, SealPirProfileClock::now());\n    }', 'recursive end')
    cpp_path.write_text(cpp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
