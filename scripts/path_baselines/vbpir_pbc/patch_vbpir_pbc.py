"""Apply the VBPIR path-adapter hooks to a staged upstream VBPIR_PBC tree."""

from pathlib import Path
import sys

if len(sys.argv) != 2:
    raise SystemExit("usage: patch_vbpir_pbc.py /path/to/staged/vbpir-pbc")

root = Path(sys.argv[1])

def replace_once(path, old, new):
    file = root / path
    text = file.read_text()
    if old not in text:
        raise SystemExit(f"pattern not found in {path}")
    file.write_text(text.replace(old, new, 1))

replace_once("header/batchpirclient.h",
"""    vector<uint64_t> get_cuckoo_table();
    size_t get_serialized_commm_size();
""",
"""    vector<uint64_t> get_cuckoo_table();
    vector<uint64_t> get_cuckoo_keys();
    size_t get_serialized_commm_size();
""")
replace_once("header/batchpirclient.h",
"""    vector<uint64_t> cuckoo_table_;
    bool is_cuckoo_generated_;
""",
"""    vector<uint64_t> cuckoo_table_;
    vector<uint64_t> cuckoo_keys_;
    bool is_cuckoo_generated_;
""")
replace_once("src/batchpirclient.cpp",
"""    cuckoo_table_.resize(std::ceil(batchpir_params_.get_batch_size() * batchpir_params_.get_cuckoo_factor()), batchpir_params_.get_default_value());

    std::unordered_map<uint64_t, std::vector<size_t>> key_to_buckets;
""",
"""    cuckoo_table_.resize(std::ceil(batchpir_params_.get_batch_size() * batchpir_params_.get_cuckoo_factor()), batchpir_params_.get_default_value());
    cuckoo_keys_.assign(cuckoo_table_.size(), batchpir_params_.get_default_value());

    std::unordered_map<uint64_t, std::vector<size_t>> key_to_buckets;
""")
replace_once("src/batchpirclient.cpp",
"""        cuckoo_table_[key] = value;
    }
""",
"""        cuckoo_table_[key] = value;
        cuckoo_keys_[key] = value;
    }
""")
replace_once("src/batchpirclient.cpp",
"""vector<uint64_t> BatchPIRClient::get_cuckoo_table()
{
    return cuckoo_table_;
}

void BatchPIRClient::translate_cuckoo()
""",
"""vector<uint64_t> BatchPIRClient::get_cuckoo_table()
{
    return cuckoo_table_;
}

vector<uint64_t> BatchPIRClient::get_cuckoo_keys()
{
    return cuckoo_keys_;
}

void BatchPIRClient::translate_cuckoo()
""")

replace_once("header/batchpirparams.h",
"""    void set_max_bucket_size(size_t max_bucket_size);

    void print_params() const;
""",
"""    void set_max_bucket_size(size_t max_bucket_size);
    void override_first_dimension_size(size_t dim_size);

    void print_params() const;
""")
replace_once("src/batchpirparams.cpp",
"""void BatchPirParams::set_max_bucket_size(size_t max_bucket_size){
    max_bucket_size_ = max_bucket_size;
    set_first_dimension_size(max_bucket_size_);
}


void BatchPirParams::print_params() const {
""",
"""void BatchPirParams::set_max_bucket_size(size_t max_bucket_size){
    max_bucket_size_ = max_bucket_size;
    set_first_dimension_size(max_bucket_size_);
}

void BatchPirParams::override_first_dimension_size(size_t dim_size){
    dim_size_ = dim_size;
}


void BatchPirParams::print_params() const {
""")

replace_once("header/batchpirserver.h", "#include <fstream>\n", "#include <fstream>\n#include <map>\n")
replace_once("header/batchpirserver.h",
"""    BatchPIRServer(unsigned int tree_height, unsigned int children, BatchPirParams &batchpir_params);

    std::unordered_map<std::string, uint64_t> get_hash_map() const;
""",
"""    BatchPIRServer(unsigned int tree_height, unsigned int children, BatchPirParams &batchpir_params);
    BatchPIRServer(BatchPirParams &batchpir_params, const RawDB &rawdb,
                   size_t first_dimension_override = 0);

    std::unordered_map<std::string, uint64_t> get_hash_map() const;
""")
replace_once("header/batchpirserver.h",
"""    PIRResponseList generate_response(uint32_t client_id, vector<PIRQuery> queries);
    bool check_decoded_entries""",
"""    PIRResponseList generate_response(uint32_t client_id, vector<PIRQuery> queries);
    std::map<std::string, double> get_last_stage_ms() const;
    bool check_decoded_entries""")
replace_once("header/batchpirserver.h",
"""    std::unordered_map<std::string, uint64_t> map_; // map from key to bucket index


    void simeple_hash();
""",
"""    std::unordered_map<std::string, uint64_t> map_; // map from key to bucket index
    std::map<std::string, double> last_stage_ms_;


    void simeple_hash();
""")

replace_once("src/batchpirserver.cpp",
"""#include "header/batchpirserver.h"

BatchPIRServer::BatchPIRServer""",
"""#include "header/batchpirserver.h"

namespace {
double elapsed_ms(std::chrono::high_resolution_clock::time_point start,
                  std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
}

BatchPIRServer::BatchPIRServer""")
replace_once("src/batchpirserver.cpp",
"""    std::cout << "BatchPIRServer: PIR servers preparation complete." << std::endl;
}


void BatchPIRServer::populate_raw_db()
""",
"""    std::cout << "BatchPIRServer: PIR servers preparation complete." << std::endl;
}

BatchPIRServer::BatchPIRServer(BatchPirParams &batchpir_params, const RawDB &rawdb,
                               size_t first_dimension_override)
{
    tree_height_ = 0;
    children_ = 2;
    batchpir_params_ = &batchpir_params;
    is_client_keys_set_ = false;
    is_simple_hash_ = false;
    rawdb_ = rawdb;

    if (rawdb_.size() != batchpir_params_->get_num_entries())
    {
        throw std::invalid_argument("raw database size does not match BatchPirParams");
    }
    for (const auto &entry : rawdb_)
    {
        if (entry.size() != batchpir_params_->get_entry_size())
        {
            throw std::invalid_argument("raw database entry size does not match BatchPirParams");
        }
    }

    simeple_hash();
    if (first_dimension_override != 0)
    {
        batchpir_params_->override_first_dimension_size(first_dimension_override);
    }
    prepare_pir_server();
}


void BatchPIRServer::populate_raw_db()
""")
replace_once("src/batchpirserver.cpp",
"""    vector<PIRResponseList> responses;

    for (int i = 0; i < server_list_.size(); i++)
    {
        responses.push_back(server_list_[i].generate_response(client_id, queries[i]));
    }

    
    return merge_responses(responses, client_id);
}

PIRResponseList BatchPIRServer::merge_responses""",
"""    vector<PIRResponseList> responses;
    last_stage_ms_.clear();

    for (int i = 0; i < server_list_.size(); i++)
    {
        responses.push_back(server_list_[i].generate_response(client_id, queries[i]));
        for (const auto &entry : server_list_[i].get_last_stage_ms())
        {
            last_stage_ms_[entry.first] += entry.second;
        }
    }

    
    auto compress_start = std::chrono::high_resolution_clock::now();
    PIRResponseList merged = merge_responses(responses, client_id);
    last_stage_ms_["response_compression"] +=
        elapsed_ms(compress_start, std::chrono::high_resolution_clock::now());
    return merged;
}

std::map<std::string, double> BatchPIRServer::get_last_stage_ms() const
{
    return last_stage_ms_;
}

PIRResponseList BatchPIRServer::merge_responses""")

replace_once("header/server.h", "#include <chrono>\n", "#include <chrono>\n#include <map>\n")
replace_once("header/server.h",
"""    PIRResponseList generate_response(uint32_t client_id, PIRQuery query);

    bool check_decoded_entry""",
"""    PIRResponseList generate_response(uint32_t client_id, PIRQuery query);
    std::map<std::string, double> get_last_stage_ms() const;

    bool check_decoded_entry""")
replace_once("header/server.h",
"""    PIRQuery query_; 
    size_t num_databases_;
""",
"""    PIRQuery query_; 
    size_t num_databases_;
    std::map<std::string, double> last_stage_ms_;
""")

replace_once("src/server.cpp",
"""#include "server.h"
#include <bitset>

// Constructor
""",
"""#include "server.h"
#include <bitset>

namespace {
double elapsed_ms(std::chrono::high_resolution_clock::time_point start,
                  std::chrono::high_resolution_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}
}

// Constructor
""")
replace_once("src/server.cpp",
"""vector<seal::Ciphertext> Server::rotate_copy_query(uint32_t client_id)
{
    vector<seal::Ciphertext> rotated_query;
""",
"""vector<seal::Ciphertext> Server::rotate_copy_query(uint32_t client_id)
{
    auto stage_start = std::chrono::high_resolution_clock::now();
    vector<seal::Ciphertext> rotated_query;
""")
replace_once("src/server.cpp",
"""    return rotated_query;
}
""",
"""    last_stage_ms_["query_expansion"] +=
        elapsed_ms(stage_start, std::chrono::high_resolution_clock::now());
    return rotated_query;
}
""")
replace_once("src/server.cpp",
"""    auto rotated_query = rotate_copy_query(client_id);
    vector<Ciphertext> first_intermediate_data;
""",
"""    auto rotated_query = rotate_copy_query(client_id);
    auto stage_start = std::chrono::high_resolution_clock::now();
    vector<Ciphertext> first_intermediate_data;
""")
replace_once("src/server.cpp",
"""    return first_intermediate_data;
}

vector<Ciphertext> Server::process_first_dimension_delayed_mod""",
"""    last_stage_ms_["first_dimension_multiply"] +=
        elapsed_ms(stage_start, std::chrono::high_resolution_clock::now());
    return first_intermediate_data;
}

vector<Ciphertext> Server::process_first_dimension_delayed_mod""")
replace_once("src/server.cpp",
"""vector<Ciphertext> Server::process_first_dimension_delayed_mod(uint32_t client_id)
{
    auto rotated_query = rotate_copy_query(client_id);
    vector<Ciphertext> first_intermediate_data;
""",
"""vector<Ciphertext> Server::process_first_dimension_delayed_mod(uint32_t client_id)
{
    auto rotated_query = rotate_copy_query(client_id);
    auto stage_start = std::chrono::high_resolution_clock::now();
    vector<Ciphertext> first_intermediate_data;
""")
replace_once("src/server.cpp",
"""    return first_intermediate_data;
}

vector<Ciphertext> Server::old_process_first_dimension_delayed_mod""",
"""    last_stage_ms_["first_dimension_multiply"] +=
        elapsed_ms(stage_start, std::chrono::high_resolution_clock::now());
    return first_intermediate_data;
}

vector<Ciphertext> Server::old_process_first_dimension_delayed_mod""")
replace_once("src/server.cpp",
"""vector<Ciphertext> Server::process_second_dimension(uint32_t client_id, vector<Ciphertext> first_intermediate_data)
{

    vector<Ciphertext> second_intermediate_data;
""",
"""vector<Ciphertext> Server::process_second_dimension(uint32_t client_id, vector<Ciphertext> first_intermediate_data)
{
    auto stage_start = std::chrono::high_resolution_clock::now();

    vector<Ciphertext> second_intermediate_data;
""")
replace_once("src/server.cpp",
"""    return second_intermediate_data;
}

PIRResponseList Server::process_last_dimension""",
"""    last_stage_ms_["dimension_reduction"] +=
        elapsed_ms(stage_start, std::chrono::high_resolution_clock::now());
    return second_intermediate_data;
}

PIRResponseList Server::process_last_dimension""")
replace_once("src/server.cpp",
"""PIRResponseList Server::process_last_dimension(uint32_t client_id, vector<Ciphertext> second_intermediate_data, bool is_2d_pir_)
{
    PIRResponseList ct_acc;
""",
"""PIRResponseList Server::process_last_dimension(uint32_t client_id, vector<Ciphertext> second_intermediate_data, bool is_2d_pir_)
{
    auto stage_start = std::chrono::high_resolution_clock::now();
    PIRResponseList ct_acc;
""")
replace_once("src/server.cpp",
"""    return ct_acc;
}
""",
"""    last_stage_ms_["dimension_reduction"] +=
        elapsed_ms(stage_start, std::chrono::high_resolution_clock::now());
    return ct_acc;
}
""")
replace_once("src/server.cpp",
"""    query_ = query;

    // Time process_first_dimension function
""",
"""    query_ = query;
    last_stage_ms_.clear();

    // Time process_first_dimension function
""")
replace_once("src/server.cpp",
"""    return response;
}

bool Server::check_decoded_entry""",
"""    return response;
}

std::map<std::string, double> Server::get_last_stage_ms() const
{
    return last_stage_ms_;
}

bool Server::check_decoded_entry""")

for rel in ("src/batchpirclient.cpp", "src/batchpirparams.cpp", "src/batchpirserver.cpp"):
    file = root / rel
    file.write_text(file.read_text().rstrip() + "\n")
