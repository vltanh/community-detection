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

// Match infomap_base_traced.cpp's plogp override so any inline
// instantiation in this compilation unit also uses Math.LOG2E form.
#include "infomath_traced.h"

#include "Infomap.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
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
struct InfomapTraceLevel {
    bool is_main = true;
    std::vector<unsigned int> predefined_modules;
    unsigned int active_n = 0;
    std::vector<unsigned int> leaf_to_active;
    std::vector<double> init_flow;
    std::vector<double> init_enter;
    std::vector<double> init_exit;
    double init_L = 0.0;
    double init_L_index = 0.0;
    double init_L_module = 0.0;
};
struct InfomapTraceMove {
    int level_idx = -1;
    unsigned int v = 0;
    unsigned int oldM = 0;
    unsigned int newM = 0;
    double oldDeltaEnter = 0.0;
    double oldDeltaExit = 0.0;
    double newDeltaEnter = 0.0;
    double newDeltaExit = 0.0;
    double L_after = 0.0;
};
struct InfomapTraceVisit {
    unsigned int v = 0;
    std::vector<unsigned int> link_order;
    bool moved = false;
    unsigned int newM = 0;
    double L_after = 0.0;
    double L_index = 0.0;
    double L_module = 0.0;
    double enterFlow = 0.0;
    double enter_log_enter = 0.0;
    double exit_log_exit = 0.0;
    double flow_log_flow = 0.0;
    double nodeFlow_log_nodeFlow = 0.0;
    double enterFlow_log_enterFlow = 0.0;
    double exitNetworkFlow = 0.0;
    double exitNetworkFlow_log_exitNetworkFlow = 0.0;
    unsigned int oldM = 0;
    double oldM_enter_pre = 0.0, oldM_exit_pre = 0.0, oldM_flow_pre = 0.0;
    double newM_enter_pre = 0.0, newM_exit_pre = 0.0, newM_flow_pre = 0.0;
    double oldM_enter_post = 0.0, oldM_exit_post = 0.0, oldM_flow_post = 0.0;
    double newM_enter_post = 0.0, newM_exit_post = 0.0, newM_flow_post = 0.0;
    double deltaEEOld = 0.0, deltaEENew = 0.0;
    double deltaEnterOld = 0.0, deltaExitOld = 0.0;
    double deltaEnterNew = 0.0, deltaExitNew = 0.0;
    double node_enter = 0.0, node_exit = 0.0, node_flow = 0.0;
};
struct InfomapTraceCall {
    int level_idx = -1;
    bool is_first_loop = true;
    std::vector<unsigned int> visit_order;
    std::vector<InfomapTraceVisit> visits;
    std::vector<unsigned int> rng_peek;
};
struct InfomapTrace {
    double L_final = 0.0;
    unsigned int num_leaf_nodes = 0;
    std::vector<InfomapTraceStage> stages;
    std::vector<InfomapTraceLevel> levels;
    std::vector<InfomapTraceMove> moves;
    std::vector<InfomapTraceCall> calls;
};
extern InfomapTrace& getInfomapTrace();
extern void resetInfomapTrace();
}

using namespace infomap;

struct TracerEdgeData {
    std::vector<unsigned int> orig_ids;          // unique orig IDs in pandas pd.unique('K') order
    std::vector<std::pair<unsigned int, unsigned int>> edges; // (compact_u, compact_v)
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
    std::map<unsigned int, unsigned int> id2idx;
    auto intern = [&](unsigned int orig) {
        auto ins = id2idx.emplace(orig, static_cast<unsigned int>(d.orig_ids.size()));
        if (ins.second) d.orig_ids.push_back(orig);
        return ins.first->second;
    };
    for (unsigned int u : srcs) intern(u);
    for (unsigned int v : tgts) intern(v);
    d.edges.reserve(srcs.size());
    for (size_t i = 0; i < srcs.size(); i++) {
        d.edges.emplace_back(id2idx.at(srcs[i]), id2idx.at(tgts[i]));
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
    // Compact ids 0..n-1 so output module ids match canonical_run.py
    // (same node_map ordering).
    for (const auto& e : ed.edges) {
        im.addLink(e.first, e.second, 1.0);
    }
    im.run();

    auto& trace = getInfomapTrace();
    double L_canon = im.codelength();

    // Walk getModules(1, false) once: build orig_id->module_id rows for
    // CSV emit + overwrite the final stage's leaf_to_top with canonical's
    // numbering (= post-consolidatedClusterIndex module index, in
    // contrast to traceCaptureStage's root-child-iter order which can
    // differ after fineTune/coarseTune reshuffles).
    auto modules_map = im.getModules(1, false);
    std::vector<std::pair<unsigned int, unsigned int>> rows;
    rows.reserve(modules_map.size());
    InfomapTraceStage* final_stage = nullptr;
    if (!trace.stages.empty() && trace.stages.back().label == "final"
            && trace.stages.back().leaf_to_top.size() == ed.orig_ids.size()) {
        final_stage = &trace.stages.back();
    }
    std::map<unsigned int, unsigned int> mod_count;
    for (const auto& kv : modules_map) {
        unsigned int compact_id = kv.first;
        unsigned int module_id = kv.second;
        rows.emplace_back(ed.orig_ids[compact_id], module_id);
        ++mod_count[module_id];
        if (final_stage != nullptr && compact_id < final_stage->leaf_to_top.size()) {
            final_stage->leaf_to_top[compact_id] = module_id;
        }
    }

    // Post-process for the CSV: drop singletons + ASC-renumber +
    // sort-by-node_id to match canonical_run.py.
    std::map<unsigned int, unsigned int> remap;
    {
        unsigned int next = 0;
        for (const auto& mc : mod_count) {
            if (mc.second > 1) remap[mc.first] = next++;
        }
    }
    std::vector<std::pair<unsigned int, unsigned int>> renumbered;
    renumbered.reserve(rows.size());
    for (const auto& r : rows) {
        auto it = remap.find(r.second);
        if (it != remap.end()) renumbered.emplace_back(r.first, it->second);
    }
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
        std::cout << "[" << ed.edges[i].first
                  << "," << ed.edges[i].second << "]";
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
    std::cout << "\n  ],\n";

    auto emit_double_array = [](const std::vector<double>& v) {
        for (size_t i = 0; i < v.size(); i++) {
            if (i) std::cout << ",";
            std::cout << v[i];
        }
    };
    auto emit_uint_array = [](const std::vector<unsigned int>& v) {
        for (size_t i = 0; i < v.size(); i++) {
            if (i) std::cout << ",";
            std::cout << v[i];
        }
    };

    std::cout << "  \"levels\": [\n";
    for (size_t li = 0; li < trace.levels.size(); li++) {
        const auto& l = trace.levels[li];
        if (li) std::cout << ",\n";
        std::cout << "    {\"is_main\":" << (l.is_main ? "true" : "false")
                  << ",\"active_n\":" << l.active_n
                  << ",\"init_L\":" << l.init_L
                  << ",\"init_L_index\":" << l.init_L_index
                  << ",\"init_L_module\":" << l.init_L_module
                  << ",\"leaf_to_active\":[";
        emit_uint_array(l.leaf_to_active);
        std::cout << "],\"init_flow\":[";
        emit_double_array(l.init_flow);
        std::cout << "],\"init_enter\":[";
        emit_double_array(l.init_enter);
        std::cout << "],\"init_exit\":[";
        emit_double_array(l.init_exit);
        std::cout << "],\"predef\":[";
        emit_uint_array(l.predefined_modules);
        std::cout << "]}";
    }
    std::cout << "\n  ],\n";

    std::cout << "  \"moves\": [\n";
    for (size_t mi = 0; mi < trace.moves.size(); mi++) {
        const auto& m = trace.moves[mi];
        if (mi) std::cout << ",\n";
        std::cout << "    {\"l\":" << m.level_idx
                  << ",\"v\":" << m.v
                  << ",\"o\":" << m.oldM
                  << ",\"n\":" << m.newM
                  << ",\"oDE\":" << m.oldDeltaEnter
                  << ",\"oDX\":" << m.oldDeltaExit
                  << ",\"nDE\":" << m.newDeltaEnter
                  << ",\"nDX\":" << m.newDeltaExit
                  << ",\"L\":" << m.L_after << "}";
    }
    std::cout << "\n  ],\n";

    std::cout << "  \"calls\": [\n";
    for (size_t ci = 0; ci < trace.calls.size(); ci++) {
        const auto& c = trace.calls[ci];
        if (ci) std::cout << ",\n";
        std::cout << "    {\"l\":" << c.level_idx
                  << ",\"fl\":" << (c.is_first_loop ? 1 : 0)
                  << ",\"rng\":[";
        emit_uint_array(c.rng_peek);
        std::cout << "],\"vo\":[";
        emit_uint_array(c.visit_order);
        std::cout << "],\"visits\":[";
        for (size_t vi = 0; vi < c.visits.size(); vi++) {
            const auto& v = c.visits[vi];
            if (vi) std::cout << ",";
            std::cout << "{\"v\":" << v.v
                      << ",\"lo\":[";
            emit_uint_array(v.link_order);
            std::cout << "],\"m\":" << (v.moved ? 1 : 0)
                      << ",\"n\":" << v.newM
                      << ",\"L\":" << v.L_after
                      << ",\"Li\":" << v.L_index
                      << ",\"Lm\":" << v.L_module
                      << ",\"ef\":" << v.enterFlow
                      << ",\"ele\":" << v.enter_log_enter
                      << ",\"xle\":" << v.exit_log_exit
                      << ",\"fle\":" << v.flow_log_flow
                      << ",\"nfle\":" << v.nodeFlow_log_nodeFlow
                      << ",\"eflnef\":" << v.enterFlow_log_enterFlow
                      << ",\"exnf\":" << v.exitNetworkFlow
                      << ",\"exfle\":" << v.exitNetworkFlow_log_exitNetworkFlow
                      << ",\"oM\":" << v.oldM
                      << ",\"oMe0\":" << v.oldM_enter_pre
                      << ",\"oMx0\":" << v.oldM_exit_pre
                      << ",\"oMf0\":" << v.oldM_flow_pre
                      << ",\"nMe0\":" << v.newM_enter_pre
                      << ",\"nMx0\":" << v.newM_exit_pre
                      << ",\"nMf0\":" << v.newM_flow_pre
                      << ",\"oMe1\":" << v.oldM_enter_post
                      << ",\"oMx1\":" << v.oldM_exit_post
                      << ",\"oMf1\":" << v.oldM_flow_post
                      << ",\"nMe1\":" << v.newM_enter_post
                      << ",\"nMx1\":" << v.newM_exit_post
                      << ",\"nMf1\":" << v.newM_flow_post
                      << ",\"dEEo\":" << v.deltaEEOld
                      << ",\"dEEn\":" << v.deltaEENew
                      << ",\"dEo\":" << v.deltaEnterOld
                      << ",\"dXo\":" << v.deltaExitOld
                      << ",\"dEn\":" << v.deltaEnterNew
                      << ",\"dXn\":" << v.deltaExitNew
                      << ",\"vne\":" << v.node_enter
                      << ",\"vnx\":" << v.node_exit
                      << ",\"vnf\":" << v.node_flow << "}";
        }
        std::cout << "]}";
    }
    std::cout << "\n  ]\n}\n";
    std::cout.flush();
    std::fflush(stdout);

    std::fprintf(stderr,
                 "[TRACE-IM] L_canon=%.17g num_modules=%u stages=%zu levels=%zu moves=%zu calls=%zu\n",
                 L_canon, im.numTopModules(), trace.stages.size(),
                 trace.levels.size(), trace.moves.size(),
                 trace.calls.size());
    return 0;
}
