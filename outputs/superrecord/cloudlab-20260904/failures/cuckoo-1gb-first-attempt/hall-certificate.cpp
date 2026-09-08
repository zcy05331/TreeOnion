#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <vector>

struct Params { size_t item_count, batch_size, num_buckets, num_hashes = 3; uint64_t seed; };
uint64_t splitmix64_once(uint64_t value){ value += 0x9e3779b97f4a7c15ULL; value = (value^(value>>30))*0xbf58476d1ce4e5b9ULL; value=(value^(value>>27))*0x94d049bb133111ebULL; return value^(value>>31); }
size_t bucket_of(uint64_t ordinal, size_t hash_index, const Params &p){ return static_cast<size_t>(splitmix64_once(p.seed ^ (0x9e3779b97f4a7c15ULL*(hash_index+1)) ^ (ordinal*0xd6e8feb86659fd93ULL)) % p.num_buckets); }
std::vector<uint32_t> sibling_ordinals(size_t leaf, size_t h){ std::vector<uint32_t> v; for(size_t level=h; level>=1; --level){ size_t shift=h-level; size_t path_local=leaf>>shift; size_t sibling_local=path_local ^ 1ULL; v.push_back(static_cast<uint32_t>(((1ULL<<level)-2ULL)+sibling_local)); if(level==1) break;} return v; }
int main(){ size_t h=24, leaf=4180873; Params p{2*((1ULL<<h)-1), h, std::max((3*h+1)/2, h+1), 3, 0x63756b32ULL}; auto ords=sibling_ordinals(leaf,h); std::vector<std::vector<size_t>> cand(ords.size()); for(size_t i=0;i<ords.size();++i){ for(size_t j=0;j<p.num_hashes;++j){ size_t b=bucket_of(ords[i],j,p); if(std::find(cand[i].begin(), cand[i].end(), b)==cand[i].end()) cand[i].push_back(b);} }
 std::vector<int> bucket_to_item(p.num_buckets,-1), item_to_bucket(ords.size(),-1); auto dfs=[&](auto &self,size_t item,std::vector<char>&seen)->bool{ for(size_t b:cand[item]){ if(seen[b]) continue; seen[b]=1; if(bucket_to_item[b]<0 || self(self, bucket_to_item[b], seen)){ bucket_to_item[b]=item; item_to_bucket[item]=b; return true; } } return false;};
 size_t matched=0; for(size_t i=0;i<ords.size();++i){ std::vector<char> seen(p.num_buckets); if(dfs(dfs,i,seen)) ++matched; }
 std::cout << "H="<<h<<" leaf="<<leaf<<" k="<<h<<" buckets="<<p.num_buckets<<" hashes="<<p.num_hashes<<" matched="<<matched<<"\n";
 std::vector<char> reach_l(ords.size()), reach_r(p.num_buckets); std::vector<size_t> q; for(size_t i=0;i<ords.size();++i){ if(item_to_bucket[i]<0){ reach_l[i]=1; q.push_back(i); } }
 for(size_t qi=0; qi<q.size(); ++qi){ size_t i=q[qi]; for(size_t b:cand[i]){ if(!reach_r[b]){ reach_r[b]=1; int owner=bucket_to_item[b]; if(owner>=0 && !reach_l[owner]){ reach_l[owner]=1; q.push_back(owner); } } } }
 std::set<size_t> neigh; std::vector<size_t> subset; for(size_t i=0;i<ords.size();++i){ if(reach_l[i]){ subset.push_back(i); for(size_t b:cand[i]) neigh.insert(b); } }
 std::cout << "Hall subset size="<<subset.size()<<" neighbor buckets="<<neigh.size()<<" deficiency="<<(subset.size()-neigh.size())<<"\n";
 for(size_t i:subset){ size_t level=h-i; std::cout << "  path_index="<<i<<" level="<<level<<" ordinal="<<ords[i]<<" buckets="; for(size_t b:cand[i]) std::cout << b << ' '; std::cout << "\n"; }
 std::cout << "neighbors="; for(size_t b:neigh) std::cout << b << ' '; std::cout << "\n";
}
