// Infomap tracer main entry. Wraps InfomapWrapper to run partition,
// then dumps stage-by-stage trace JSON to stdout + per-stage L +
// final flat partition CSV.
//
// The trace state lives in infomap_base_traced.cpp (forked
// InfomapBase.cpp + per-stage hooks at every fineTune/coarseTune /
// findTopModulesRepeatedly boundary inside InfomapBase::partition).
//
// Build: ./build.sh -> /tmp/infomap_kernel_check
// Run:   /tmp/infomap_kernel_check <edge.csv> <seed> <out.csv>

#include "Infomap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace infomap {
struct InfomapTraceStage {
    std::string label;
    double L = 0.0;
    double L_index = 0.0;
    double L_module = 0.0;
    unsigned int num_top_modules = 0;
    std::vector<unsigned int> leaf_to_top;
};
struct InfomapTrace {
    double L_one_level = 0.0;
    double L_init_singleton = 0.0;
    double L_final = 0.0;
    unsigned int num_leaf_nodes = 0;
    std::vector<InfomapTraceStage> stages;
};
extern InfomapTrace& getInfomapTrace();
extern void resetInfomapTrace();
}

using namespace infomap;

// CSV reader: returns (orig_id_list-in-encounter-order, edge_pairs).
struct TracerEdgeData {
    std::vector<unsigned int> orig_ids;          // unique node IDs in input order
    std::map<unsigned int, unsigned int> id2idx; // orig -> compact
    std::vector<std::pair<unsigned int, unsigned int>> edges;
};

static TracerEdgeData read_edges(const std::string& path) {
    // Mirror canonical_run.py:
    //   nodes = pd.unique(df[[src_col, tgt_col]].values.ravel('K'))
    // pandas DataFrame .values is column-major (Fortran) for 2 cols, so
    // ravel('K') yields [src0, src1, ..., srcM, tgt0, ..., tgtM]. After
    // unique() this is "all unique srcs in row order, then all NEW
    // unique tgts in row order".
    TracerEdgeData d;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "cannot open edge file: %s\n", path.c_str());
        std::exit(2);
    }
    std::vector<unsigned int> srcs, tgts;
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) { first = false; continue; } // header
        if (line.empty()) continue;
        std::vector<std::string> tok;
        std::string cur;
        for (char c : line) {
            if (c == ',' || c == '\t' || c == ' ') {
                if (!cur.empty()) { tok.push_back(cur); cur.clear(); }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) tok.push_back(cur);
        if (tok.size() < 2) continue;
        unsigned int u = static_cast<unsigned int>(std::stoul(tok[0]));
        unsigned int v = static_cast<unsigned int>(std::stoul(tok[1]));
        if (u == v) continue;
        srcs.push_back(u);
        tgts.push_back(v);
    }
    // Phase 1: assign indices by src-column order.
    for (unsigned int u : srcs) {
        if (d.id2idx.find(u) == d.id2idx.end()) {
            d.id2idx[u] = d.orig_ids.size();
            d.orig_ids.push_back(u);
        }
    }
    // Phase 2: assign new indices by tgt-column order.
    for (unsigned int v : tgts) {
        if (d.id2idx.find(v) == d.id2idx.end()) {
            d.id2idx[v] = d.orig_ids.size();
            d.orig_ids.push_back(v);
        }
    }
    d.edges.reserve(srcs.size());
    for (size_t i = 0; i < srcs.size(); i++) {
        d.edges.emplace_back(srcs[i], tgts[i]);
    }
    return d;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr,
                     "usage: %s <edge.csv> <seed> <out.csv>\n",
                     argv[0]);
        return 2;
    }
    std::string edge_csv = argv[1];
    int seed = std::atoi(argv[2]);
    std::string out_csv = argv[3];

    TracerEdgeData ed = read_edges(edge_csv);
    std::fprintf(stderr, "[TRACE-IM] loaded n=%zu m=%zu seed=%d\n",
                 ed.orig_ids.size(), ed.edges.size(), seed);

    // Mirror canonical_run.py + pypi: --seed N --silent --two-level.
    std::ostringstream args;
    args << "--seed " << seed << " --silent --two-level";

    resetInfomapTrace();

    InfomapWrapper im(args.str());
    // Add edges using the *compact* node ids (0..n-1) so that the
    // output module ids are deterministic vs canonical_run.py which
    // also uses compact ids via its node_map.
    for (const auto& e : ed.edges) {
        unsigned int u = ed.id2idx.at(e.first);
        unsigned int v = ed.id2idx.at(e.second);
        im.addLink(u, v, 1.0);
    }
    im.run();

    auto& trace = getInfomapTrace();
    double L_canon = im.codelength();

    // Build final partition as orig_id -> module_id by walking the leaf
    // tree (post-partition). Mirrors canonical_run.py's iteration order.
    std::vector<std::pair<unsigned int, unsigned int>> rows; // (orig_id, module_id)
    auto modules_map = im.getModules(1, false);
    {
        for (const auto& kv : modules_map) {
            unsigned int compact_id = kv.first;
            unsigned int module_id = kv.second;
            unsigned int orig = ed.orig_ids[compact_id];
            rows.emplace_back(orig, module_id);
        }
    }
    // Also store the canonical module_id per compact-leaf-index in the
    // last (final) trace stage so JS replay can use the SAME numbering
    // canonical_run.py emits (= getModules(1, false).second pre-ASC-
    // renumber). Replaces the walk-up assignment from
    // traceCaptureStage("final"), which uses root child-iter order
    // (different ordering on multi-level partitions when fineTune /
    // coarseTune reorder children mid-iteration).
    if (!trace.stages.empty() && trace.stages.back().label == "final") {
        auto& last = trace.stages.back();
        if (last.leaf_to_top.size() == ed.orig_ids.size()) {
            for (const auto& kv : modules_map) {
                unsigned int compact_id = kv.first;
                unsigned int module_id = kv.second;
                if (compact_id < last.leaf_to_top.size())
                    last.leaf_to_top[compact_id] = module_id;
            }
        }
    }

    // Drop singletons + renumber 0..K-1 + sort by node_id (matches
    // canonical_run.py's post-processing).
    std::map<unsigned int, unsigned int> mod_count;
    for (const auto& r : rows) mod_count[r.second]++;
    std::vector<std::pair<unsigned int, unsigned int>> kept;
    for (const auto& r : rows) if (mod_count[r.second] > 1) kept.push_back(r);
    std::set<unsigned int> uniq_mods;
    for (const auto& r : kept) uniq_mods.insert(r.second);
    std::map<unsigned int, unsigned int> remap;
    {
        unsigned int next = 0;
        for (unsigned int m : uniq_mods) remap[m] = next++;
    }
    std::vector<std::pair<unsigned int, unsigned int>> renumbered;
    for (const auto& r : kept) renumbered.emplace_back(r.first, remap[r.second]);
    std::sort(renumbered.begin(), renumbered.end());

    {
        std::ofstream out(out_csv);
        out << "node_id,cluster_id\n";
        for (const auto& r : renumbered) {
            out << r.first << "," << r.second << "\n";
        }
    }

    // Emit trace JSON to stdout.
    std::cout << std::setprecision(17) << "{\n";
    std::cout << "  \"L_canon\": " << L_canon << ",\n";
    std::cout << "  \"L_one_level\": " << trace.L_one_level << ",\n";
    std::cout << "  \"L_init_singleton\": " << trace.L_init_singleton << ",\n";
    std::cout << "  \"L_final\": " << trace.L_final << ",\n";
    std::cout << "  \"num_leaf_nodes\": " << trace.num_leaf_nodes << ",\n";
    std::cout << "  \"num_top_modules\": " << im.numTopModules() << ",\n";
    std::cout << "  \"renum_to_orig\": [";
    for (size_t i = 0; i < ed.orig_ids.size(); i++) {
        if (i) std::cout << ",";
        std::cout << ed.orig_ids[i];
    }
    std::cout << "],\n";
    std::cout << "  \"edges\": [";
    for (size_t i = 0; i < ed.edges.size(); i++) {
        if (i) std::cout << ",";
        std::cout << "[" << ed.id2idx.at(ed.edges[i].first)
                  << "," << ed.id2idx.at(ed.edges[i].second) << "]";
    }
    std::cout << "],\n";
    std::cout << "  \"stages\": [\n";
    for (size_t si = 0; si < trace.stages.size(); si++) {
        const auto& s = trace.stages[si];
        if (si) std::cout << ",\n";
        std::cout << "    {\"label\":\"" << s.label
                  << "\",\"L\":" << s.L
                  << ",\"L_index\":" << s.L_index
                  << ",\"L_module\":" << s.L_module
                  << ",\"num_top_modules\":" << s.num_top_modules
                  << ",\"leaf_to_top\":[";
        for (size_t k = 0; k < s.leaf_to_top.size(); k++) {
            if (k) std::cout << ",";
            std::cout << s.leaf_to_top[k];
        }
        std::cout << "]}";
    }
    std::cout << "\n  ]\n}\n";
    std::cout.flush();
    std::fflush(stdout);

    std::fprintf(stderr, "[TRACE-IM] L_canon=%.17g num_modules=%u stages=%zu\n",
                 L_canon, im.numTopModules(), trace.stages.size());
    return 0;
}
