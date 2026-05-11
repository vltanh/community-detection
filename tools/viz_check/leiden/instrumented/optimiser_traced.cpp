// [TRACER FORK] Verbatim copy of libleidenalg/src/Optimiser.cpp +
// trace injection at every shuffle site + every per-visit move in
// move_nodes. Other functions (move_nodes_constrained, merge_nodes,
// merge_nodes_constrained) are unchanged from canonical.
#include "Optimiser.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// [TRACE-LD-LG] LEVEL_GRAPH dump — emit per-level-transition snapshot of
// (collapsed graph + partition admin) for byte-equal cross-check vs JS.
// Gated by env LEIDEN_DUMP_LG=1; silent otherwise so stress matrix unaffected.
static bool _ld_lg_enabled() {
  static int v = -1;
  if (v < 0) {
    const char* s = std::getenv("LEIDEN_DUMP_LG");
    v = (s && s[0] == '1') ? 1 : 0;
  }
  return v != 0;
}

// [TRACE-LD-CAND] / [TRACE-LD-MV] / [TRACE-LD-CACHE] probe gate.
// Set LEIDEN_DUMP_PROBES=1 to enable per-candidate, per-move_node-admin,
// and per-cache_neigh_communities accumulator emissions. Silent by default
// so the 3-tier stress harness is unaffected. Probes mirror canonical
// `#ifdef DEBUG` blocks in libleidenalg/src/{Optimiser,MutableVertexPartition,
// CPMVertexPartition,ModularityVertexPartition}.cpp, routed to stderr with
// `[TRACE-LD-*]` prefixes for grep-able diff vs JS-side mirror prints.
bool _ld_probes_enabled() {
  static int v = -1;
  if (v < 0) {
    const char* s = std::getenv("LEIDEN_DUMP_PROBES");
    v = (s && s[0] == '1') ? 1 : 0;
  }
  return v != 0;
}
// Bit-exact double emission (hex) for accumulator probes.
static void _ld_emit_hex(double x) {
  unsigned long long u;
  std::memcpy(&u, &x, 8);
  fprintf(stderr, "%016llx", u);
}

// [TRACE-LD-SHUFFLE] per-step Fisher-Yates probe wrapper. Closes P0 #10
// (GraphHelper.cpp:37 per-shuffle step `get_random_int(0, idx, rng)`
// return) + lemire-mapping per-call (P0 #11) via the `range` field
// (range = idx + 1, return value is the bounded draw — the only two
// scalars cross-side reproducible without editing vendored igraph).
// Calls get_random_int directly (same path as canonical shuffle) so
// the RNG stream is byte-identical to the unwrapped shuffle. The cpp
// `shuffle()` from GraphHelper.cpp is bypassed only when probes are
// enabled; production stress passes still call the canonical shuffle.
static void _ld_traced_shuffle(std::vector<size_t>& v, igraph_rng_t* rng,
                               const char* site_tag) {
  size_t n = v.size();
  if (n > 0) {
    for (size_t idx = n - 1; idx > 0; idx--) {
      size_t rand_idx = get_random_int(0, idx, rng);
      if (_ld_probes_enabled()) {
        // range = idx + 1 (igraph_rng_get_integer maps to inclusive bound).
        fprintf(stderr, "[TRACE-LD-SHUFFLE] site=%s idx=%zu range=%zu rand_idx=%zu\n",
                site_tag, idx, idx + 1, rand_idx);
      }
      size_t tmp = v[idx];
      v[idx] = v[rand_idx];
      v[rand_idx] = tmp;
    }
  }
}
static void _ld_lg_emit_hex(double x) {
  unsigned long long u;
  std::memcpy(&u, &x, 8);
  fprintf(stderr, "%016llx", u);
}
static void _ld_lg_dump(size_t level_idx, Graph* g, MutableVertexPartition* p) {
  if (!_ld_lg_enabled()) return;
  size_t K = g->vcount();
  size_t E = g->ecount();
  double tw = g->total_weight();
  fprintf(stderr, "[TRACE-LD-LG] LEVEL_GRAPH level=%zu K=%zu E=%zu directed=%d csl=%d tw=",
          level_idx, K, E, g->is_directed(), g->correct_self_loops());
  _ld_lg_emit_hex(tw);
  fprintf(stderr, "\n");
  for (size_t v = 0; v < K; v++) {
    double ns = g->node_size(v);
    double sw = g->node_self_weight(v);
    double so = g->strength(v, IGRAPH_OUT);
    double si = g->strength(v, IGRAPH_IN);
    fprintf(stderr, "[TRACE-LD-LG] VERT level=%zu v=%zu nsize=", level_idx, v);
    _ld_lg_emit_hex(ns);
    fprintf(stderr, " nsw=");
    _ld_lg_emit_hex(sw);
    fprintf(stderr, " sout=");
    _ld_lg_emit_hex(so);
    fprintf(stderr, " sin=");
    _ld_lg_emit_hex(si);
    fprintf(stderr, "\n");
  }
  for (size_t e = 0; e < E; e++) {
    size_t from, to;
    g->edge(e, from, to);
    double w = g->edge_weight(e);
    fprintf(stderr, "[TRACE-LD-LG] EDGE level=%zu e=%zu from=%zu to=%zu w=",
            level_idx, e, from, to);
    _ld_lg_emit_hex(w);
    fprintf(stderr, "\n");
  }
  size_t nc = p->n_communities();
  for (size_t c = 0; c < nc; c++) {
    double tin = p->total_weight_in_comm(c);
    double tfrom = p->total_weight_from_comm(c);
    double tto = p->total_weight_to_comm(c);
    double cs = p->csize(c);
    size_t cn = p->cnodes(c);
    fprintf(stderr, "[TRACE-LD-LG] COMM level=%zu c=%zu cnodes=%zu csize=",
            level_idx, c, cn);
    _ld_lg_emit_hex(cs);
    fprintf(stderr, " tin=");
    _ld_lg_emit_hex(tin);
    fprintf(stderr, " tfrom=");
    _ld_lg_emit_hex(tfrom);
    fprintf(stderr, " tto=");
    _ld_lg_emit_hex(tto);
    fprintf(stderr, "\n");
  }
}

// [TRACE-LD-LG] Dump partition membership only (no graph state). Used at
// the point right before collapse_graph(sub_partition) so we can compare
// post-refine sub-partition membership cpp vs JS.
static void _ld_lg_dump_membership(const char* tag, size_t level_idx,
                                   MutableVertexPartition* p) {
  if (!_ld_lg_enabled()) return;
  size_t n = p->get_graph()->vcount();
  fprintf(stderr, "[TRACE-LD-LG] %s level=%zu n=%zu ncomm=%zu\n",
          tag, level_idx, n, p->n_communities());
  for (size_t v = 0; v < n; v++) {
    fprintf(stderr, "[TRACE-LD-LG] %s_MEM level=%zu v=%zu c=%zu\n",
            tag, level_idx, v, p->membership(v));
  }
}

struct LeidenTraceMove {
    size_t pass;
    size_t visit_idx;
    size_t v;
    size_t from_comm;
    size_t to_comm;
    double dQ;
    bool moved;
};
struct LeidenTracePass {
    size_t pass;
    size_t n_nodes_in_queue;
    int phase;                                 // 0 = move_nodes, 1 = merge_nodes_constrained
    size_t level;                              // collapse depth (vcount of operating graph)
    std::vector<size_t> shuffled_nodes;        // post-shuffle order
    std::vector<size_t> pre_membership;        // membership at pass start (before any move)
    std::vector<LeidenTraceMove> moves;
    double total_improv;
    size_t nb_moves;
    std::vector<size_t> post_membership;       // POST-renumber membership
};
struct LeidenTrace {
    std::vector<LeidenTracePass> passes;
    double Q_init;
    double Q_final;
    size_t n_communities_final;
};
static LeidenTrace gTrace;
const LeidenTrace& leiden_trace_get() { return gTrace; }
void leiden_trace_reset() { gTrace = LeidenTrace{}; }

/****************************************************************************
  Create a new Optimiser object

  Parameters:
    consider_comms
                 -- Consider communities in a specific manner:
        ALL_COMMS       -- Consider all communities for improvement.
        ALL_NEIGH_COMMS -- Consider all neighbour communities for
                           improvement.
        RAND_COMM       -- Consider a random commmunity for improvement.
        RAND_NEIGH_COMM -- Consider a random community among the neighbours
                           for improvement.
****************************************************************************/
Optimiser::Optimiser()
{
  this->consider_comms = Optimiser::ALL_NEIGH_COMMS;
  this->optimise_routine = Optimiser::MOVE_NODES;
  this->refine_consider_comms = Optimiser::ALL_NEIGH_COMMS;
  this->refine_routine = Optimiser::MERGE_NODES;
  this->refine_partition = true;
  this->consider_empty_community = true;
  this->max_comm_size = 0;

  igraph_rng_init(&rng, &igraph_rngtype_mt19937);
  igraph_rng_seed(&rng, time(NULL));
}

Optimiser::~Optimiser()
{
  igraph_rng_destroy(&rng);
}

void Optimiser::print_settings()
{
  cerr << "Consider communities method:\t" << this->consider_comms << endl;
  cerr << "Refine partition:\t" << this->refine_partition << endl;
}

/*****************************************************************************
  optimise the provided partition.
*****************************************************************************/
double Optimiser::optimise_partition(MutableVertexPartition* partition)
{
 size_t n = partition->get_graph()->vcount();
 vector<bool> is_membership_fixed(n, false);
 return this->optimise_partition(partition, is_membership_fixed);
}

double Optimiser::optimise_partition(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed)
{
  return this->optimise_partition(partition, is_membership_fixed, this->max_comm_size);
}

double Optimiser::optimise_partition(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed, size_t max_comm_size)
{
  vector<MutableVertexPartition*> partitions(1);
  partitions[0] = partition;
  vector<double> layer_weights(1, 1.0);
  return this->optimise_partition(partitions, layer_weights, is_membership_fixed, max_comm_size);
}

double Optimiser::optimise_partition(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed)
{
  return this->optimise_partition(partitions, layer_weights, is_membership_fixed, this->max_comm_size);
}

/*****************************************************************************
  optimise the providede partitions simultaneously. We here use the sum
  of the difference of the moves as the overall quality function, each partition
  weighted by the layer weight.
*****************************************************************************/
/*****************************************************************************
  optimise the provided partition.
*****************************************************************************/
double Optimiser::optimise_partition(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, size_t max_comm_size)
{
  #ifdef DEBUG
    cerr << "void Optimiser::optimise_partition(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, size_t max_comm_size)" << endl;
  #endif

  double q = 0.0;

  // Number of multiplex layers
  size_t nb_layers = partitions.size();
  if (nb_layers == 0)
    throw Exception("No partitions provided.");

  // Get graphs for all layers
  vector<Graph*> graphs(nb_layers);
  for (size_t layer = 0; layer < nb_layers; layer++)
    graphs[layer] = partitions[layer]->get_graph();

  // Number of nodes in the graphs. Should be the same across
  // all graphs, so we only take the first one.
  size_t n = graphs[0]->vcount();

  // Make sure that all graphs contain the exact same number of nodes.
  // We assume the index of each vertex in the graph points to the
  // same node (but then in a different layer).
  for (Graph* graph : graphs)
    if (graph->vcount() != n)
      throw Exception("Number of nodes are not equal for all graphs.");

  // Get the fixed membership for fixed nodes
  vector<size_t> fixed_nodes;
  vector<size_t> fixed_membership(n);
  for (size_t v = 0; v < n; v++) {
    if (is_membership_fixed[v]) {
      fixed_nodes.push_back(v);
      fixed_membership[v] = partitions[0]->membership(v);
    }
  }

  // Initialize the vector of the collapsed graphs for all layers
  vector<Graph*> collapsed_graphs(nb_layers);
  vector<MutableVertexPartition*> collapsed_partitions(nb_layers);

  // Declare the collapsed_graph variable which will contain the graph
  // collapsed by its communities. We will use this variables at each
  // further iteration, so we don't keep a collapsed graph at each pass.
  for (size_t layer = 0; layer < nb_layers; layer++)
  {
    collapsed_graphs[layer] = graphs[layer];
    collapsed_partitions[layer] = partitions[layer];
  }

  // Declare which nodes in the collapsed graph are fixed, which to start is
  // simply equal to is_membership_fixed
  vector<bool> is_collapsed_membership_fixed(is_membership_fixed);

  // This reflects the aggregate node, which to start with is simply equal to the graph.
  vector<size_t> aggregate_node_per_individual_node = range(n);
  bool aggregate_further = true;
  // As long as there remains improvement iterate
  double improv = 0.0;
  size_t _ld_lg_level_idx = 0;     // 0 = original; +1 per level transition
  // Level-0 dump: original graph + initial partition (singletons).
  _ld_lg_dump(_ld_lg_level_idx, collapsed_graphs[0], collapsed_partitions[0]);
  do
  {

    // Optimise partition for collapsed graph
    #ifdef DEBUG
      q = 0.0;
      for (size_t layer = 0; layer < nb_layers; layer++)
        q += partitions[layer]->quality()*layer_weights[layer];
      cerr << "Quality before moving " <<  q << endl;
    #endif
    if (this->optimise_routine == Optimiser::MOVE_NODES)
      improv += this->move_nodes(collapsed_partitions, layer_weights, is_collapsed_membership_fixed, this->consider_comms, this->consider_empty_community, false, max_comm_size);

    else if (this->optimise_routine == Optimiser::MERGE_NODES)
      improv += this->merge_nodes(collapsed_partitions, layer_weights, is_collapsed_membership_fixed, this->consider_comms, false, max_comm_size);

    #ifdef DEBUG
      cerr << "Found " << collapsed_partitions[0]->n_communities() << " communities, improved " << improv << endl;
      q = 0.0;
      for (size_t layer = 0; layer < nb_layers; layer++)
        q += partitions[layer]->quality()*layer_weights[layer];
      cerr << "Quality after moving " <<  q << endl;
    #endif // DEBUG

    // Make sure improvement on coarser scale is reflected on the
    // scale of the graph as a whole.
    for (size_t layer = 0; layer < nb_layers; layer++)
    {
      if (collapsed_partitions[layer] != partitions[layer])
      {
        if (this->refine_partition)
          partitions[layer]->from_coarse_partition(collapsed_partitions[layer], aggregate_node_per_individual_node);
        else
          partitions[layer]->from_coarse_partition(collapsed_partitions[layer]);
      }
    }

    #ifdef DEBUG
      q = 0.0;
      for (size_t layer = 0; layer < nb_layers; layer++)
        q += partitions[layer]->quality()*layer_weights[layer];
      cerr << "Quality on finer partition " << q << endl;
    #endif // DEBUG

    #ifdef DEBUG
        cerr << "Number of communities: " << partitions[0]->n_communities() << endl;
    #endif

    // Collapse graph (i.e. community graph)
    // If we do refine the partition, we separate communities in slightly more
    // fine-grained parts for which we collapse the graph.
    vector<MutableVertexPartition*> sub_collapsed_partitions(nb_layers);

    vector<Graph*> new_collapsed_graphs(nb_layers);
    vector<MutableVertexPartition*> new_collapsed_partitions(nb_layers);

    if (this->refine_partition)
    {
      // First create a new partition, which should be a sub partition
      // of the collapsed partition, i.e. such that all clusters of
      // the partition are strictly partitioned in the subpartition.

      #ifdef DEBUG
        cerr << "\tBefore SLM " << collapsed_partitions[0]->n_communities() << " communities." << endl;
      #endif
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        sub_collapsed_partitions[layer] = collapsed_partitions[layer]->create(collapsed_graphs[layer]);
      }

      // Then move around nodes but restrict movement to within original communities.
      #ifdef DEBUG
        cerr << "\tStarting refinement with " << sub_collapsed_partitions[0]->n_communities() << " communities." << endl;
      #endif
      if (this->refine_routine == Optimiser::MOVE_NODES)
        this->move_nodes_constrained(sub_collapsed_partitions, layer_weights, refine_consider_comms, collapsed_partitions[0], max_comm_size);
      else if (this->refine_routine == Optimiser::MERGE_NODES)
        this->merge_nodes_constrained(sub_collapsed_partitions, layer_weights, refine_consider_comms, collapsed_partitions[0], max_comm_size);
      #ifdef DEBUG
        cerr << "\tAfter applying refinement found " << sub_collapsed_partitions[0]->n_communities() << " communities." << endl;
      #endif

      // Determine new aggregate node per individual node
      for (size_t v = 0; v < n; v++)
      {
        size_t aggregate_node = aggregate_node_per_individual_node[v];
        aggregate_node_per_individual_node[v] = sub_collapsed_partitions[0]->membership(aggregate_node);
      }

      // Collapse graph based on sub collapsed partition
      // [TRACE-LD-LG] Dump sub-partition membership + main partition
      // membership at the moment collapse_graph reads from them, so a
      // JS-side mirror dump can be diffed pre-collapse.
      _ld_lg_dump_membership("SUB", _ld_lg_level_idx, sub_collapsed_partitions[0]);
      _ld_lg_dump_membership("MAIN", _ld_lg_level_idx, collapsed_partitions[0]);
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        new_collapsed_graphs[layer] = collapsed_graphs[layer]->collapse_graph(sub_collapsed_partitions[layer]);
      }

      // Determine the membership for the collapsed graph
      vector<size_t> new_collapsed_membership(new_collapsed_graphs[0]->vcount());

      // Every node within the collapsed graph should be assigned
      // to the community of the original partition before the refinement.
      // We thus check for each node what the community is in the refined partition
      // and set the membership equal to the original partition (i.e.
      // even though the aggregation may be slightly different, the
      // membership of the aggregated nodes is as indicated by the original partition.)
      #ifdef DEBUG
        //cerr << "Refinement\tOrig" << endl;
      #endif // DEBUG
      for (size_t v = 0; v < collapsed_graphs[0]->vcount(); v++)
      {
        size_t new_aggregate_node = sub_collapsed_partitions[0]->membership(v);
        new_collapsed_membership[new_aggregate_node] = collapsed_partitions[0]->membership(v);
        #ifdef DEBUG
          //cerr << sub_collapsed_partition->membership(v) << "\t" << sub_collapsed_partition->membership(v) << endl;
        #endif // DEBUG
      }

      // Determine which collapsed nodes are fixed
      is_collapsed_membership_fixed.clear();
      is_collapsed_membership_fixed.resize(new_collapsed_graphs[0]->vcount(), false);
      for (size_t v = 0; v < n; v++)
        if (is_membership_fixed[v])
          is_collapsed_membership_fixed[aggregate_node_per_individual_node[v]] = true;

      // Create new collapsed partition
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        delete sub_collapsed_partitions[layer];
        new_collapsed_partitions[layer] = collapsed_partitions[layer]->create(new_collapsed_graphs[layer], new_collapsed_membership);
      }
    }
    else
    {
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        new_collapsed_graphs[layer] = collapsed_graphs[layer]->collapse_graph(collapsed_partitions[layer]);
        // Create collapsed partition (i.e. default partition of each node in its own community).
        new_collapsed_partitions[layer] = collapsed_partitions[layer]->create(new_collapsed_graphs[layer]);
        #ifdef DEBUG
          cerr << "Layer " << layer << endl;
          cerr << "Old collapsed graph " << collapsed_graphs[layer] << ", vcount is " << collapsed_graphs[layer]->vcount() << endl;
          cerr << "New collapsed graph " << new_collapsed_graphs[layer] << ", vcount is " << new_collapsed_graphs[layer]->vcount() << endl;
        #endif
      }
    }

    // Determine whether to aggregate further
    // If all is fixed, no need to aggregate
    aggregate_further = false;
    for (const bool& membership_fixed : is_collapsed_membership_fixed)
    {
      if(!membership_fixed) {
        aggregate_further = true;
        break;
      }
    }
    // [TRACE-LD-AGG] Closes P0 #18. Decomposed aggregate_further short-
    // circuit: emit the three input scalars + final value BEFORE the
    // composite `&&` is applied. cpp:425-426 / Optimiser.cpp ~283-295.
    bool _ld_any_unfixed = aggregate_further;
    bool _ld_new_lt_old = (new_collapsed_graphs[0]->vcount() < collapsed_graphs[0]->vcount());
    bool _ld_old_gt_ncomm = (collapsed_graphs[0]->vcount() > collapsed_partitions[0]->n_communities());
    if (_ld_probes_enabled()) {
      fprintf(stderr, "[TRACE-LD-AGG] level=%zu any_unfixed=%d new_vcount=%zu old_vcount=%zu n_comms=%zu new_lt_old=%d old_gt_ncomm=%d\n",
              _ld_lg_level_idx,
              _ld_any_unfixed ? 1 : 0,
              new_collapsed_graphs[0]->vcount(),
              collapsed_graphs[0]->vcount(),
              collapsed_partitions[0]->n_communities(),
              _ld_new_lt_old ? 1 : 0,
              _ld_old_gt_ncomm ? 1 : 0);
    }
    // else, check whether anything has stirred since last time
    aggregate_further &= (new_collapsed_graphs[0]->vcount() < collapsed_graphs[0]->vcount()) &&
                         (collapsed_graphs[0]->vcount() > collapsed_partitions[0]->n_communities());
    if (_ld_probes_enabled()) {
      fprintf(stderr, "[TRACE-LD-AGG] level=%zu final=%d\n",
              _ld_lg_level_idx, aggregate_further ? 1 : 0);
    }

    #ifdef DEBUG
      cerr << "Aggregate further " << aggregate_further << endl;
    #endif

    // Delete the previous collapsed partition and graph
    for (size_t layer = 0; layer < nb_layers; layer++)
    {
      if (collapsed_partitions[layer] != partitions[layer])
        delete collapsed_partitions[layer];
      if (collapsed_graphs[layer] != graphs[layer])
        delete collapsed_graphs[layer];
    }

    // and set them to the new one.
    collapsed_partitions = new_collapsed_partitions;
    collapsed_graphs = new_collapsed_graphs;

    // [TRACE-LD-LG] Per-level snapshot for byte-equal cross-check vs JS.
    _ld_lg_level_idx++;
    _ld_lg_dump(_ld_lg_level_idx, collapsed_graphs[0], collapsed_partitions[0]);

    #ifdef DEBUG
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        cerr <<   "Calculate partition " << layer  << " quality." << endl;
        q = partitions[layer]->quality()*layer_weights[layer];
        cerr <<   "Calculate collapsed partition quality." << endl;
        double q_collapsed = 0.0;
        q_collapsed += collapsed_partitions[layer]->quality()*layer_weights[layer];
        if (fabs(q - q_collapsed) > 1e-6)
        {
          cerr << "ERROR: Quality of original partition and collapsed partition are not equal." << endl;
        }
        cerr <<   "partition->quality()=" << q
             << ", collapsed_partition->quality()=" << q_collapsed << endl;
        cerr <<   "graph->total_weight()=" << graphs[layer]->total_weight()
             << ", collapsed_graph->total_weight()=" << collapsed_graphs[layer]->total_weight() << endl;
        cerr <<   "graph->vcount()=" << graphs[layer]->vcount()
             << ", collapsed_graph->vcount()="  << collapsed_graphs[layer]->vcount() << endl;
        cerr <<   "graph->ecount()=" << graphs[layer]->ecount()
             << ", collapsed_graph->ecount()="  << collapsed_graphs[layer]->ecount() << endl;
        cerr <<   "graph->is_directed()=" << graphs[layer]->is_directed()
             << ", collapsed_graph->is_directed()="  << collapsed_graphs[layer]->is_directed() << endl;
        cerr <<   "graph->correct_self_loops()=" << graphs[layer]->correct_self_loops()
             << ", collapsed_graph->correct_self_loops()="  << collapsed_graphs[layer]->correct_self_loops() << endl << endl;
      }
    #endif // DEBUG

  } while (aggregate_further);

  // Clean up memory after use.
  for (size_t layer = 0; layer < nb_layers; layer++)
  {
    if (collapsed_partitions[layer] != partitions[layer])
      delete collapsed_partitions[layer];

    if (collapsed_graphs[layer] != graphs[layer])
      delete collapsed_graphs[layer];
  }

  // Make sure the resulting communities are called 0,...,r-1
  // where r is the number of communities. The exception is fixed
  // nodes which should keep the numbers of the original communities
  q = 0.0;
  partitions[0]->renumber_communities();
  partitions[0]->renumber_communities(fixed_nodes, fixed_membership);
  vector<size_t> const& membership = partitions[0]->membership();
  // We only renumber the communities for the first graph,
  // since the communities for the other graphs should just be equal
  // to the membership of the first graph.
  for (size_t layer = 1; layer < nb_layers; layer++)
  {
    partitions[layer]->set_membership(membership);
    q += partitions[layer]->quality()*layer_weights[layer];
  }
  return improv;
}

/*****************************************************************************
    Move nodes to other communities depending on how other communities are
    considered, see consider_comms parameter of the class.

    Parameters:
      partition -- The partition to optimise.
******************************************************************************/
double Optimiser::move_nodes(MutableVertexPartition* partition)
{
  return this->move_nodes(partition, this->consider_comms);
}

double Optimiser::move_nodes(MutableVertexPartition* partition, int consider_comms)
{
  vector<bool> is_membership_fixed(partition->get_graph()->vcount());
  return this->move_nodes(partition, is_membership_fixed, consider_comms, false);
}

double Optimiser::move_nodes(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes)
{
  return this->move_nodes(partition, is_membership_fixed, consider_comms, renumber_fixed_nodes, this->max_comm_size);
}

double Optimiser::move_nodes(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes, size_t max_comm_size)
{
  vector<MutableVertexPartition*> partitions(1);
  partitions[0] = partition;
  vector<double> layer_weights(1, 1.0);
  return this->move_nodes(partitions, layer_weights, is_membership_fixed, consider_comms, this->consider_empty_community, renumber_fixed_nodes, max_comm_size);
}

double Optimiser::merge_nodes(MutableVertexPartition* partition)
{
  return this->merge_nodes(partition, this->consider_comms);
}

double Optimiser::merge_nodes(MutableVertexPartition* partition, int consider_comms)
{
  vector<bool> is_membership_fixed(partition->get_graph()->vcount());
  return this->merge_nodes(partition, is_membership_fixed, consider_comms, false);
}

double Optimiser::merge_nodes(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes)
{
  return this->merge_nodes(partition, is_membership_fixed, consider_comms, renumber_fixed_nodes, this->max_comm_size);
}

double Optimiser::merge_nodes(MutableVertexPartition* partition, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes, size_t max_comm_size)
{
  vector<MutableVertexPartition*> partitions(1);
  partitions[0] = partition;
  vector<double> layer_weights(1, 1.0);
  return this->merge_nodes(partitions, layer_weights, is_membership_fixed, consider_comms, renumber_fixed_nodes, max_comm_size);
}

double Optimiser::move_nodes_constrained(MutableVertexPartition* partition, MutableVertexPartition* constrained_partition)
{
  return this->move_nodes_constrained(partition, this->refine_consider_comms, constrained_partition);
}

double Optimiser::move_nodes_constrained(MutableVertexPartition* partition, int consider_comms, MutableVertexPartition* constrained_partition)
{
  return this->move_nodes_constrained(partition, consider_comms, constrained_partition, this->max_comm_size);
}

double Optimiser::move_nodes_constrained(MutableVertexPartition* partition, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)
{
  vector<MutableVertexPartition*> partitions(1);
  partitions[0] = partition;
  vector<double> layer_weights(1, 1.0);
  return this->move_nodes_constrained(partitions, layer_weights, consider_comms, constrained_partition, max_comm_size);
}

double Optimiser::merge_nodes_constrained(MutableVertexPartition* partition, MutableVertexPartition* constrained_partition)
{
  return this->merge_nodes_constrained(partition, this->refine_consider_comms, constrained_partition);
}

double Optimiser::merge_nodes_constrained(MutableVertexPartition* partition, int consider_comms, MutableVertexPartition* constrained_partition)
{
  return this->merge_nodes_constrained(partition, consider_comms, constrained_partition, this->max_comm_size);
}

double Optimiser::merge_nodes_constrained(MutableVertexPartition* partition, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)
{
  vector<MutableVertexPartition*> partitions(1);
  partitions[0] = partition;
  vector<double> layer_weights(1, 1.0);
  return this->merge_nodes_constrained(partitions, layer_weights, consider_comms, constrained_partition, max_comm_size);
}

/*****************************************************************************
  Move nodes to neighbouring communities such that each move improves the
  given quality function maximally (i.e. greedily) for multiple layers,
  i.e. for multiplex networks. Each node will be in the same community in
  each layer, but the method may be different, or the weighting may be
  different for different layers. Notably, this can be used in the case of
  negative links, where you would like to weigh the negative links with a
  negative weight.

  Parameters:
    partitions -- The partitions to optimise.
    layer_weights -- The weights used for the different layers.
******************************************************************************/
double Optimiser::move_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, bool renumber_fixed_nodes)
{
  return this->move_nodes(partitions, layer_weights, is_membership_fixed, this->consider_comms, this->consider_empty_community, renumber_fixed_nodes);
}

double Optimiser::move_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, int consider_empty_community)
{
  return this->move_nodes(partitions, layer_weights, is_membership_fixed, consider_comms, consider_empty_community, true);
}

double Optimiser::move_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, int consider_empty_community, bool renumber_fixed_nodes)
{
    return this->move_nodes(partitions, layer_weights, is_membership_fixed, consider_comms, consider_empty_community, renumber_fixed_nodes, this->max_comm_size);
}

double Optimiser::move_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, int consider_empty_community, bool renumber_fixed_nodes, size_t max_comm_size)
{
  #ifdef DEBUG
    cerr << "double Optimiser::move_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, int consider_empty_community, bool renumber_fixed_nodes, size_t max_comm_size)" << endl;
  #endif
  // Number of multiplex layers
  size_t nb_layers = partitions.size();
  if (nb_layers == 0)
    return -1.0;
  // Get graphs
  vector<Graph*> graphs(nb_layers);
  for (size_t layer = 0; layer < nb_layers; layer++)
    graphs[layer] = partitions[layer]->get_graph();
  // Number of nodes in the graph
  size_t n = graphs[0]->vcount();

  // Get the fixed membership for fixed nodes
  vector<size_t> fixed_nodes;
  vector<size_t> fixed_membership(n);
  if (renumber_fixed_nodes) {
    for (size_t v = 0; v < n; v++) {
      if (is_membership_fixed[v]) {
        fixed_nodes.push_back(v);
        fixed_membership[v] = partitions[0]->membership(v);
      }
    }
  }

  // Total improvement while moving nodes
  double total_improv = 0.0;

  for (Graph* graph : graphs)
    if (graph->vcount() != n)
      throw Exception("Number of nodes are not equal for all graphs.");
  // Number of moved nodes during one loop
  size_t nb_moves = 0;

  // Fixed nodes are also stable nodes
  vector<bool> is_node_stable(is_membership_fixed);

  // Establish vertex order
  // We normally initialize the normal vertex order
  // of considering node 0,1,...
  // But if we use a random order, we shuffle this order.
  // Also, we skip fixed nodes from the queue for efficiency reasons
  vector<size_t> nodes;
  for (size_t v = 0; v != is_membership_fixed.size(); v++) {
    if (!is_membership_fixed[v])
      nodes.push_back(v);
  }
  if (_ld_probes_enabled()) _ld_traced_shuffle(nodes, &rng, "move");
  else                      shuffle(nodes, &rng);
  // [TRACE-LD] Capture per-pass init: phase, level, queue, pre_membership.
  gTrace.passes.push_back({});
  size_t pass_idx = gTrace.passes.size() - 1;
  gTrace.passes[pass_idx].pass = pass_idx;
  gTrace.passes[pass_idx].phase = 0;       // move_nodes
  gTrace.passes[pass_idx].level = n;       // current graph vcount = "level" proxy
  gTrace.passes[pass_idx].n_nodes_in_queue = nodes.size();
  gTrace.passes[pass_idx].shuffled_nodes = nodes;
  {
    auto const& mem = partitions[0]->membership();
    gTrace.passes[pass_idx].pre_membership.assign(mem.begin(), mem.end());
  }
  fprintf(stderr, "[TRACE-LD] PASS_BEGIN pass=%zu phase=move level=%zu queue=%zu\n",
          pass_idx, n, nodes.size());
  deque<size_t> vertex_order(nodes.begin(), nodes.end());

  // Initialize the degree vector
  // If we want to debug the function, we will calculate some additional values.
  // In particular, the following consistencies could be checked:
  // (1) - The difference in the quality function after a move should match
  //       the reported difference when calling diff_move.
  // (2) - The quality function should be exactly the same value after
  //       aggregating/collapsing the graph.

  vector<bool> comm_added(partitions[0]->n_communities(), false);
  vector<size_t> comms;

  // As long as the queue is not empty
  while(!vertex_order.empty())
  {
    size_t v = vertex_order.front(); vertex_order.pop_front();

    // What is the current community of the node (this should be the same for all layers)
    size_t v_comm = partitions[0]->membership(v);

    if (consider_comms == ALL_COMMS)
    {
      for(size_t comm = 0; comm < partitions[0]->n_communities(); comm++)
      {
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          if (partitions[layer]->cnodes(comm) > 0 && !comm_added[comm])
          {
            comms.push_back(comm);
            comm_added[comm] = true;
            break; // Break from for loop in layer
          }
        }

      }
    }
    else if (consider_comms == ALL_NEIGH_COMMS)
    {
      /****************************ALL NEIGH COMMS*****************************/
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        for (size_t comm : partitions[layer]->get_neigh_comms(v, IGRAPH_ALL))
        {
          if (!comm_added[comm])
          {
              comms.push_back(comm);
              comm_added[comm] = true;
          }
        }
      }
    }
    else if (consider_comms == RAND_COMM)
    {
      /****************************RAND COMM***********************************/
      size_t rand_comm = partitions[0]->membership(graphs[0]->get_random_node(&rng));
      // No need to check if random_comm is already added, we only add one comm
      comms.push_back(rand_comm);
      comm_added[rand_comm] = true;
    }
    else if (consider_comms == RAND_NEIGH_COMM)
    {
      /****************************RAND NEIGH COMM*****************************/
      size_t rand_layer = get_random_int(0, nb_layers - 1, &rng);
      if (graphs[rand_layer]->degree(v, IGRAPH_ALL) > 0)
      {
        size_t rand_comm = partitions[0]->membership(graphs[rand_layer]->get_random_neighbour(v, IGRAPH_ALL, &rng));
        // No need to check if random_comm is already added, we only add one comm
        comms.push_back(rand_comm);
        comm_added[rand_comm] = true;
      }
    }

    // [TRACE-LD-BRANCH] per-visit which consider_comms path was taken.
    // Closes P0 #20 (audit row F): branch silently visible only via
    // observed candidate count; emit decoded name + n_post_consider so
    // empty-comm gate flip vs n_unchanged is unambiguous.
    if (_ld_probes_enabled()) {
      const char* cc_tag =
        (consider_comms == ALL_COMMS) ? "ALL_COMMS" :
        (consider_comms == ALL_NEIGH_COMMS) ? "ALL_NEIGH_COMMS" :
        (consider_comms == RAND_COMM) ? "RAND_COMM" :
        (consider_comms == RAND_NEIGH_COMM) ? "RAND_NEIGH_COMM" : "UNKNOWN";
      fprintf(stderr, "[TRACE-LD-BRANCH] pass=%zu visit=%zu v=%zu vcomm=%zu consider_comms=%s n_after_cc=%zu cnodes_vcomm=%zu\n",
              pass_idx, gTrace.passes[pass_idx].moves.size(), v, v_comm,
              cc_tag, comms.size(), partitions[0]->cnodes(v_comm));
    }

    // Check if we should move to an empty community
    if (consider_empty_community)
    {
      if ( partitions[0]->cnodes(v_comm) > 1 )  // We should not move a node when it is already in its own empty community (this may otherwise create more empty communities than nodes)
      {
        size_t n_comms = partitions[0]->n_communities();
        size_t comm = partitions[0]->get_empty_community();
        #ifdef DEBUG
          cerr << "Checking empty community (" << comm << ") for partition " << partitions[0] << endl;
        #endif
        comms.push_back(comm);
        // [TRACE-LD-EMPTYGATE] consider_empty branch taken. Closes P0 #17
        // (add_empty_community use-site result) + part of #20 (empty-comm
        // gate branch). Emits pre/post n_communities + the empty id used.
        if (_ld_probes_enabled()) {
          fprintf(stderr, "[TRACE-LD-EMPTYGATE] site=move pass=%zu visit=%zu v=%zu vcomm=%zu cnodes_vcomm=%zu n_comms_pre=%zu empty_id=%zu n_comms_post=%zu grew=%d\n",
                  pass_idx, gTrace.passes[pass_idx].moves.size(), v, v_comm,
                  partitions[0]->cnodes(v_comm), n_comms, comm,
                  partitions[0]->n_communities(),
                  (partitions[0]->n_communities() > n_comms) ? 1 : 0);
        }
        if (partitions[0]->n_communities() > n_comms)
        {
          // If the empty community has just been added, we need to make sure
          // that is has also been added to the other layers
          for (size_t layer = 1; layer < nb_layers; layer++)
              partitions[layer]->add_empty_community();
          comm_added.push_back(true);
        }
      } else {
        if (_ld_probes_enabled()) {
          fprintf(stderr, "[TRACE-LD-EMPTYGATE] site=move pass=%zu visit=%zu v=%zu vcomm=%zu cnodes_vcomm=%zu skipped=1\n",
                  pass_idx, gTrace.passes[pass_idx].moves.size(), v, v_comm,
                  partitions[0]->cnodes(v_comm));
        }
      }
    }

    #ifdef DEBUG
      cerr << "Consider " << comms.size() << " communities for moving." << endl;
    #endif

    // [TRACE-LD-DEBUG] dump v=185 first-visit candidate order on top level.
    if (v == 185 && pass_idx == 0 && n == 906) {
      fprintf(stderr, "[TRACE-LD-DEBUG] v=185 candidates order: ");
      for (size_t i = 0; i < comms.size() && i < 20; i++) {
        fprintf(stderr, "%zu%s", comms[i], i + 1 == comms.size() || i == 19 ? "" : ",");
      }
      fprintf(stderr, " (n=%zu)\n", comms.size());
    }

    size_t max_comm = v_comm;
    bool _ld_max_comm_size_guard = (0 < max_comm_size && max_comm_size < partitions[0]->csize(v_comm));
    double max_improv = _ld_max_comm_size_guard ? -INFINITY : 10*DBL_EPSILON;
    double v_size = graphs[0]->node_size(v);
    // [TRACE-LD-CAND] Closes audit row H + row J + row F: dump full
    // candidate list pre-loop (Optimiser.cpp:701-789) + per-candidate
    // `possible_improv` (Optimiser.cpp:817) + running `max_improv` at
    // every `>` test (Optimiser.cpp:820) + initial-value branch tag
    // (Optimiser.cpp:799 max_improv branch — P0 #15).
    if (_ld_probes_enabled()) {
      const char* mi_branch = _ld_max_comm_size_guard ? "NEG_INF" : "10EPS";
      fprintf(stderr, "[TRACE-LD-CAND] CANDS pass=%zu visit=%zu v=%zu vcomm=%zu ncands=%zu max_improv_branch=%s max_improv_init=",
              pass_idx, gTrace.passes[pass_idx].moves.size(), v, v_comm, comms.size(), mi_branch);
      _ld_emit_hex(max_improv);
      fprintf(stderr, " cands=");
      for (size_t i = 0; i < comms.size(); i++) {
        fprintf(stderr, "%s%zu", i ? "," : "", comms[i]);
      }
      fprintf(stderr, "\n");
    }
    for (size_t comm : comms)
    {
      // reset comm_added to all false
      comm_added[comm] = false;

      // Do not create too-large communities.
      if (0 < max_comm_size && max_comm_size < partitions[0]->csize(comm) + v_size) {
        if (_ld_probes_enabled()) {
          fprintf(stderr, "[TRACE-LD-CAND] SKIP pass=%zu visit=%zu v=%zu comm=%zu reason=max_comm_size\n",
                  pass_idx, gTrace.passes[pass_idx].moves.size(), v, comm);
        }
        continue;
      }

      double possible_improv = 0.0;

      // Consider the improvement of moving to a community for all layers
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        // Make sure to multiply it by the weight per layer
        double dm = partitions[layer]->diff_move(v, comm);
        // [TRACE-LD-CAND] Per-layer diff_move return (Optimiser.cpp:817).
        // Single-layer default so layer=0 is the only emission, but keep
        // the loop probe for forward-compatibility with multiplex layers.
        if (_ld_probes_enabled()) {
          fprintf(stderr, "[TRACE-LD-CAND] DIFF pass=%zu visit=%zu v=%zu comm=%zu layer=%zu lw=",
                  pass_idx, gTrace.passes[pass_idx].moves.size(), v, comm, layer);
          _ld_emit_hex(layer_weights[layer]);
          fprintf(stderr, " diff_move=");
          _ld_emit_hex(dm);
          fprintf(stderr, "\n");
        }
        possible_improv += layer_weights[layer]*dm;
      }

      // [TRACE-LD-CAND] PRE-COMPARE: possible_improv + running max_improv
      // at the moment of the `>` test (Optimiser.cpp:820). Closes audit
      // row J — every tie-break decision visible.
      bool accepts = (possible_improv > max_improv);
      if (_ld_probes_enabled()) {
        fprintf(stderr, "[TRACE-LD-CAND] COMP pass=%zu visit=%zu v=%zu comm=%zu pimprov=",
                pass_idx, gTrace.passes[pass_idx].moves.size(), v, comm);
        _ld_emit_hex(possible_improv);
        fprintf(stderr, " max_improv=");
        _ld_emit_hex(max_improv);
        fprintf(stderr, " accepts=%d\n", accepts ? 1 : 0);
      }
      if (accepts)
      {
        max_comm = comm;
        max_improv = possible_improv;
      }
    }

    // Clear comms
    comms.clear();

    is_node_stable[v] = true;

    // [TRACE-LD] Capture this visit's decision (regardless of whether
    // we end up moving). pass_idx + visit_idx into gTrace.passes.back().
    {
        LeidenTraceMove m;
        m.pass = pass_idx;
        m.visit_idx = gTrace.passes[pass_idx].moves.size();
        m.v = v;
        m.from_comm = v_comm;
        m.to_comm = max_comm;
        m.dQ = (max_comm != v_comm) ? max_improv : 0.0;
        m.moved = (max_comm != v_comm);
        gTrace.passes[pass_idx].moves.push_back(m);
    }

    // If we actually plan to move the node
    if (max_comm != v_comm)
    {
        // Keep track of improvement
        total_improv += max_improv;

        #ifdef DEBUG
          // If we are debugging, calculate quality function
          double q_improv = 0;
        #endif

        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          MutableVertexPartition* partition = partitions[layer];

          #ifdef DEBUG
            // If we are debugging, calculate quality function
            double q1 = partition->quality();
          #endif

          // Actually move the node
          partition->move_node(v, max_comm);
          #ifdef DEBUG
            // If we are debugging, calculate quality function
            // and report difference
            double q2 = partition->quality();
            double q_delta = layer_weights[layer]*(q2 - q1);
            q_improv += q_delta;
            cerr << "Move node " << v
            << " from " << v_comm << " to " << max_comm << " for layer " << layer
            << " (diff_move=" << max_improv
            << ", q2 - q1=" << q_delta << ")" << endl;
          #endif
        }
        #ifdef DEBUG
          if (fabs(q_improv - max_improv) > 1e-6)
          {
            cerr << "ERROR: Inconsistency while moving nodes, improvement as measured by quality function did not equal the improvement measured by the diff_move function." << endl
                 << " (diff_move=" << max_improv
                 << ", q2 - q1=" << q_improv << ")" << endl;
          }
        #endif

        // Mark neighbours as unstable (if not in new community and not fixed)
        for (Graph* graph : graphs)
        {
          for (size_t u : graph->get_neighbours(v, IGRAPH_ALL))
          {
            // If the neighbour was stable and is not in the new community, we
            // should mark it as unstable, and add it to the queue, skipping
            // fixed nodes
            bool _ld_was_stable = is_node_stable[u];
            bool _ld_in_new = (partitions[0]->membership(u) == max_comm);
            bool _ld_fixed = is_membership_fixed[u];
            bool _ld_push = _ld_was_stable && !_ld_in_new && !_ld_fixed;
            // [TRACE-LD-RESTAB] Closes P0 #16. Per-neighbour decision tuple:
            // `(u, is_node_stable[u], membership[u], pushed_to_queue)`.
            // RNG-independent but queue-extension order matters for sub-
            // visit RNG draws downstream. cpp:889-903 / Optimiser.cpp ~720.
            if (_ld_probes_enabled()) {
              fprintf(stderr, "[TRACE-LD-RESTAB] pass=%zu visit=%zu v=%zu u=%zu was_stable=%d in_new=%d fixed=%d pushed=%d u_comm=%zu\n",
                      pass_idx, gTrace.passes[pass_idx].moves.size(), v, u,
                      _ld_was_stable ? 1 : 0, _ld_in_new ? 1 : 0,
                      _ld_fixed ? 1 : 0, _ld_push ? 1 : 0,
                      partitions[0]->membership(u));
            }
            if (_ld_push)
            {
              vertex_order.push_back(u);
              is_node_stable[u] = false;
            }
          }
        }
        // Keep track of number of moves
        nb_moves += 1;
      }
  }

  partitions[0]->renumber_communities();
  if (renumber_fixed_nodes)
    partitions[0]->renumber_communities(fixed_nodes, fixed_membership);
  vector<size_t> const& membership = partitions[0]->membership();
  for (size_t layer = 1; layer < nb_layers; layer++)
  {
    partitions[layer]->set_membership(membership);
    #ifdef DEBUG
      cerr << "Renumbered communities for layer " << layer << " for " << partitions[layer]->n_communities() << " communities." << endl;
    #endif //DEBUG
  }
  // [TRACE-LD] Capture pass-end totals (move_nodes return path).
  gTrace.passes[pass_idx].total_improv = total_improv;
  gTrace.passes[pass_idx].nb_moves = nb_moves;
  // Capture POST-renumber membership so JS replay can synchronize.
  // (libleidenalg's renumber_communities runs at line 791 below before
  // we return.) Use the public membership() accessor.
  {
    auto const& mem = partitions[0]->membership();
    gTrace.passes[pass_idx].post_membership.assign(mem.begin(), mem.end());
  }
  fprintf(stderr, "[TRACE-LD] PASS_END pass=%zu nb_moves=%zu improv=%.6f visits=%zu\n",
          pass_idx, nb_moves, total_improv, gTrace.passes[pass_idx].moves.size());
  return total_improv;
}

double Optimiser::merge_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, bool renumber_fixed_nodes)
{
  return this->merge_nodes(partitions, layer_weights, is_membership_fixed, this->consider_comms, renumber_fixed_nodes);
}

double Optimiser::merge_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes)
{
  return this->merge_nodes(partitions, layer_weights, is_membership_fixed, consider_comms, renumber_fixed_nodes, this->max_comm_size);
}

double Optimiser::merge_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes, size_t max_comm_size)
{
  #ifdef DEBUG
    cerr << "double Optimiser::merge_nodes(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, vector<bool> const& is_membership_fixed, int consider_comms, bool renumber_fixed_nodes, size_t max_comm)" << std::endl;
  #endif

  // Number of multiplex layers
  size_t nb_layers = partitions.size();
  if (nb_layers == 0)
    return -1.0;

  // Get graphs
  vector<Graph*> graphs(nb_layers);
  for (size_t layer = 0; layer < nb_layers; layer++)
    graphs[layer] = partitions[layer]->get_graph();
  // Number of nodes in the graph
  size_t n = graphs[0]->vcount();

  // Get the fixed membership for fixed nodes
  vector<size_t> fixed_nodes;
  vector<size_t> fixed_membership(n);
  if (renumber_fixed_nodes) {
    for (size_t v = 0; v < n; v++) {
      if (is_membership_fixed[v]) {
        fixed_nodes.push_back(v);
        fixed_membership[v] = partitions[0]->membership(v);
      }
    }
  }

  // Total improvement while merging nodes
  double total_improv = 0.0;

  for (Graph* graph : graphs)
    if (graph->vcount() != n)
      throw Exception("Number of nodes are not equal for all graphs.");

  // Establish vertex order, skipping fixed nodes
  // We normally initialize the normal vertex order
  // of considering node 0,1,...
  vector<size_t> vertex_order;
  for (size_t v = 0; v != n; v++)
    if (!is_membership_fixed[v])
      vertex_order.push_back(v);

  // But if we use a random order, we shuffle this order.
  if (_ld_probes_enabled()) _ld_traced_shuffle(vertex_order, &rng, "merge");
  else                      shuffle(vertex_order, &rng);

  vector<bool> comm_added(partitions[0]->n_communities(), false);
  vector<size_t> comms;

  // Iterate over all nodes
  for (size_t v : vertex_order)
  {
    // What is the current community of the node (this should be the same for all layers)
    size_t v_comm = partitions[0]->membership(v);
    // Clear comms
    for (size_t comm : comms)
      comm_added[comm] = false;
    comms.clear();

    #ifdef DEBUG
      cerr << "Consider moving node " << v << " from " << v_comm << "." << endl;
    #endif

    if (partitions[0]->cnodes(v_comm) == 1)
    {
      if (consider_comms == ALL_COMMS)
      {
        for(size_t comm = 0; comm < partitions[0]->n_communities(); comm++)
        {
          for (size_t layer = 0; layer < nb_layers; layer++)
          {
            if (partitions[layer]->cnodes(comm) > 0 && !comm_added[comm])
            {
              comms.push_back(comm);
              comm_added[comm] = true;
              break; // Break from for loop in layer
            }
          }

        }
      }
      else if (consider_comms == ALL_NEIGH_COMMS)
      {
        /****************************ALL NEIGH COMMS*****************************/
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          for (size_t comm : partitions[layer]->get_neigh_comms(v, IGRAPH_ALL))
          {
            if (!comm_added[comm])
            {
                comms.push_back(comm);
                comm_added[comm] = true;
            }
          }
        }
      }
      else if (consider_comms == RAND_COMM)
      {
        /****************************RAND COMM***********************************/
        size_t rand_comm = partitions[0]->membership(graphs[0]->get_random_node(&rng));
        // No need to check if random_comm is already added, we only add one comm
        comms.push_back(rand_comm);
        comm_added[rand_comm] = true;
      }
      else if (consider_comms == RAND_NEIGH_COMM)
      {
        /****************************RAND NEIGH COMM*****************************/
        size_t rand_layer = get_random_int(0, nb_layers - 1, &rng);
        size_t k = graphs[rand_layer]->degree(v, IGRAPH_ALL);
        if (k > 0)
        {
          // Make sure there is also a probability not to move the node
          if (get_random_int(0, k, &rng) > 0)
          {
            size_t rand_comm = partitions[0]->membership(graphs[rand_layer]->get_random_neighbour(v, IGRAPH_ALL, &rng));
            // No need to check if random_comm is already added, we only add one comm
            comms.push_back(rand_comm);
            comm_added[rand_comm] = true;
          }
        }
      }

      #ifdef DEBUG
        cerr << "Consider " << comms.size() << " communities for moving node " << v << "." << endl;
      #endif

      size_t max_comm = v_comm;
      double max_improv = (0 < max_comm_size && max_comm_size < partitions[0]->csize(v_comm)) ? -INFINITY : 0;
      double v_size = graphs[0]->node_size(v);
      for (size_t comm : comms)
      {
        // Do not create too-large communities.
        if (0 < max_comm_size && max_comm_size < partitions[0]->csize(comm) + v_size) {
          continue;
        }

        double possible_improv = 0.0;

        // Consider the improvement of moving to a community for all layers
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          // Make sure to multiply it by the weight per layer
          possible_improv += layer_weights[layer]*partitions[layer]->diff_move(v, comm);
        }
        #ifdef DEBUG
          cerr << "Improvement of " << possible_improv << " when move to " << comm << "." << endl;
        #endif

        if (possible_improv >= max_improv)
        {
          max_comm = comm;
          max_improv = possible_improv;
        }
      }

      // If we actually plan to move the node
      if (max_comm != v_comm)
      {
          // Keep track of improvement
          total_improv += max_improv;

          #ifdef DEBUG
            // If we are debugging, calculate quality function
            double q_improv = 0;
          #endif

          for (size_t layer = 0; layer < nb_layers; layer++)
          {
            MutableVertexPartition* partition = partitions[layer];

            #ifdef DEBUG
              // If we are debugging, calculate quality function
              double q1 = partition->quality();
            #endif

            // Actually move the node
            partition->move_node(v, max_comm);
            #ifdef DEBUG
              // If we are debugging, calculate quality function
              // and report difference
              double q2 = partition->quality();
              double q_delta = layer_weights[layer]*(q2 - q1);
              q_improv += q_delta;
              cerr << "Move node " << v
              << " from " << v_comm << " to " << max_comm << " for layer " << layer
              << " (diff_move=" << max_improv
              << ", q2 - q1=" << q_delta << ")" << endl;
            #endif
          }
          #ifdef DEBUG
            if (fabs(q_improv - max_improv) > 1e-6)
            {
              cerr << "ERROR: Inconsistency while moving nodes, improvement as measured by quality function did not equal the improvement measured by the diff_move function." << endl
                   << " (diff_move=" << max_improv
                   << ", q2 - q1=" << q_improv << ")" << endl;
            }
          #endif
        }
      }
  }

  partitions[0]->renumber_communities();
  if (renumber_fixed_nodes)
    partitions[0]->renumber_communities(fixed_nodes, fixed_membership);
  vector<size_t> const& membership = partitions[0]->membership();
  for (size_t layer = 1; layer < nb_layers; layer++)
  {
    partitions[layer]->set_membership(membership);
    #ifdef DEBUG
      cerr << "Renumbered communities for layer " << layer << " for " << partitions[layer]->n_communities() << " communities." << endl;
    #endif //DEBUG
  }
  return total_improv;
}

double Optimiser::move_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, MutableVertexPartition* constrained_partition)
{
  return this->move_nodes_constrained(partitions, layer_weights, this->refine_consider_comms, constrained_partition);
}

double Optimiser::move_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition)
{
  return this->move_nodes_constrained(partitions, layer_weights, refine_consider_comms, constrained_partition, this->max_comm_size);
}

double Optimiser::move_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)
{
  #ifdef DEBUG
    cerr << "double Optimiser::move_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)" << std::endl;
  #endif
  // Number of multiplex layers
  size_t nb_layers = partitions.size();
  if (nb_layers == 0)
    return -1.0;
  // Get graphs
  vector<Graph*> graphs(nb_layers);
  for (size_t layer = 0; layer < nb_layers; layer++)
    graphs[layer] = partitions[layer]->get_graph();
  // Number of nodes in the graph
  size_t n = graphs[0]->vcount();

  // Total improvement while moving nodes
  double total_improv = 0.0;

  for (size_t layer = 0; layer < nb_layers; layer++)
    if (graphs[layer]->vcount() != n)
      throw Exception("Number of nodes are not equal for all graphs.");
  // Number of moved nodes during one loop
  size_t nb_moves = 0;

  // Establish vertex order
  // We normally initialize the normal vertex order
  // of considering node 0,1,...
  vector<bool> is_node_stable(n, false);
  // But if we use a random order, we shuffle this order.
  vector<size_t> nodes = range(n);
  if (_ld_probes_enabled()) _ld_traced_shuffle(nodes, &rng, "move_constrained");
  else                      shuffle(nodes, &rng);
  deque<size_t> vertex_order(nodes.begin(), nodes.end());

  vector< vector<size_t> > constrained_comms = constrained_partition->get_communities();

  // Initialize the degree vector
  // If we want to debug the function, we will calculate some additional values.
  // In particular, the following consistencies could be checked:
  // (1) - The difference in the quality function after a move should match
  //       the reported difference when calling diff_move.
  // (2) - The quality function should be exactly the same value after
  //       aggregating/collapsing the graph.

  vector<bool> comm_added(partitions[0]->n_communities(), false);
  vector<size_t> comms;

  // As long as the queue is not empty
  while(!vertex_order.empty())
  {
    size_t v = vertex_order.front(); vertex_order.pop_front();

    // Clear comms
    for (size_t comm : comms)
      comm_added[comm] = false;
    comms.clear();

    // What is the current community of the node (this should be the same for all layers)
    size_t v_comm = partitions[0]->membership(v);

    if (consider_comms == ALL_COMMS)
    {
        // Add all communities to the set comms that are within the constrained community.
        size_t v_constrained_comm = constrained_partition->membership(v);
        for (size_t u : constrained_comms[v_constrained_comm])
        {
          size_t u_comm = partitions[0]->membership(u);
          if (!comm_added[u_comm])
          {
            comms.push_back(u_comm);
            comm_added[u_comm] = true;
          }
        }
    }
    else if (consider_comms == ALL_NEIGH_COMMS)
    {
        /****************************ALL NEIGH COMMS*****************************/
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          for (size_t comm : partitions[layer]->get_neigh_comms(v, IGRAPH_ALL, constrained_partition->membership()))
          {
            if (!comm_added[comm])
            {
                comms.push_back(comm);
                comm_added[comm] = true;
            }
          }
        }
    }
    else if (consider_comms == RAND_COMM)
    {
      /****************************RAND COMM***********************************/
        size_t v_constrained_comm = constrained_partition->membership(v);
        size_t random_idx = get_random_int(0, constrained_comms[v_constrained_comm].size() - 1, &rng);
        size_t rand_comm = constrained_comms[v_constrained_comm][random_idx];
        // No need to check if random_comm is already added, we only add one comm
        comms.push_back(rand_comm);
        comm_added[rand_comm] = true;
    }
    else if (consider_comms == RAND_NEIGH_COMM)
    {
      /****************************RAND NEIGH COMM*****************************/
        // Draw a random community among the neighbours, proportional to the
        // frequency of the communities among the neighbours. Notice this is no
        // longer
        vector<size_t> all_neigh_comms_incl_dupes;
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          vector<size_t> neigh_comm_layer = partitions[layer]->get_neigh_comms(v, IGRAPH_ALL, constrained_partition->membership());
          all_neigh_comms_incl_dupes.insert(all_neigh_comms_incl_dupes.end(), neigh_comm_layer.begin(), neigh_comm_layer.end());
        }
        if (all_neigh_comms_incl_dupes.size() > 0)
        {
          size_t random_idx = get_random_int(0, all_neigh_comms_incl_dupes.size() - 1, &rng);
          size_t rand_comm = all_neigh_comms_incl_dupes[random_idx];
          // No need to check if random_comm is already added, we only add one comm
          comms.push_back(rand_comm);
          comm_added[rand_comm] = true;
        }
    }

    #ifdef DEBUG
      cerr << "Consider " << comms.size() << " communities for moving." << endl;
    #endif

    size_t max_comm = v_comm;
    double max_improv = (0 < max_comm_size && max_comm_size < partitions[0]->csize(v_comm)) ? -INFINITY : 10*DBL_EPSILON;
    double v_size = graphs[0]->node_size(v);
    for (size_t comm : comms)
    {
      // Do not create too-large communities.
      if (0 < max_comm_size && max_comm_size < partitions[0]->csize(comm) + v_size) {
        continue;
      }

      double possible_improv = 0.0;

      // Consider the improvement of moving to a community for all layers
      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        // Make sure to multiply it by the weight per layer
        possible_improv += layer_weights[layer]*partitions[layer]->diff_move(v, comm);
      }

      // Check if improvement is best
      if (possible_improv > max_improv)
      {
        max_comm = comm;
        max_improv = possible_improv;
      }
    }

    is_node_stable[v] = true;

    // If we actually plan to move the nove
    if (max_comm != v_comm)
    {
      // Keep track of improvement
      total_improv += max_improv;

      #ifdef DEBUG
        // If we are debugging, calculate quality function
        double q_improv = 0;
      #endif

      for (size_t layer = 0; layer < nb_layers; layer++)
      {
        MutableVertexPartition* partition = partitions[layer];

        #ifdef DEBUG
          // If we are debugging, calculate quality function
          double q1 = partition->quality();
        #endif

        // Actually move the node
        partition->move_node(v, max_comm);
        #ifdef DEBUG
          // If we are debugging, calculate quality function
          // and report difference
          double q2 = partition->quality();
          double q_delta = layer_weights[layer]*(q2 - q1);
          q_improv += q_delta;
          cerr << "Move node " << v
          << " from " << v_comm << " to " << max_comm << " for layer " << layer
          << " (diff_move=" << max_improv
          << ", q2 - q1=" << q_delta << ")" << endl;
        #endif
      }
      #ifdef DEBUG
        if (fabs(q_improv - max_improv) > 1e-6)
        {
          cerr << "ERROR: Inconsistency while moving nodes, improvement as measured by quality function did not equal the improvement measured by the diff_move function." << endl
               << " (diff_move=" << max_improv
               << ", q2 - q1=" << q_improv << ")" << endl;
        }
      #endif

      // Mark neighbours as unstable (if not in new community and not fixed)
      for (Graph* graph : graphs)
      {
        for (size_t u : graph->get_neighbours(v, IGRAPH_ALL))
        {
          // If the neighbour was stable and is not in the new community, we
          // should mark it as unstable, and add it to the queue, skipping
          // fixed nodes
          if (is_node_stable[u] && partitions[0]->membership(u) != max_comm && constrained_partition->membership(u) == constrained_partition->membership(v))
          {
            vertex_order.push_back(u);
            is_node_stable[u] = false;
          }
        }
      }

      // Keep track of number of moves
      nb_moves += 1;
    }
    #ifdef DEBUG
      cerr << "Moved " << nb_moves << " nodes." << endl;
    #endif
  }
  partitions[0]->renumber_communities();
  vector<size_t> const& membership = partitions[0]->membership();
  for (size_t layer = 1; layer < nb_layers; layer++)
  {
    partitions[layer]->set_membership(membership);
    #ifdef DEBUG
      cerr << "Renumbered communities for layer " << layer << " for " << partitions[layer]->n_communities() << " communities." << endl;
    #endif //DEBUG
  }
  return total_improv;
}

double Optimiser::merge_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, MutableVertexPartition* constrained_partition)
{
  return this->merge_nodes_constrained(partitions, layer_weights, this->refine_consider_comms, constrained_partition);
}

double Optimiser::merge_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition)
{
  return this->merge_nodes_constrained(partitions, layer_weights, refine_consider_comms, constrained_partition, this->max_comm_size);
}

double Optimiser::merge_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)
{
  #ifdef DEBUG
    cerr << "double Optimiser::merge_nodes_constrained(vector<MutableVertexPartition*> partitions, vector<double> layer_weights, int consider_comms, MutableVertexPartition* constrained_partition, size_t max_comm_size)" << std::endl;
  #endif

  // Number of multiplex layers
  size_t nb_layers = partitions.size();
  if (nb_layers == 0)
    return -1.0;

  // Get graphs
  vector<Graph*> graphs(nb_layers);
  for (size_t layer = 0; layer < nb_layers; layer++)
    graphs[layer] = partitions[layer]->get_graph();
  // Number of nodes in the graph
  size_t n = graphs[0]->vcount();

  // Total improvement while merging nodes
  double total_improv = 0.0;

  for (size_t layer = 0; layer < nb_layers; layer++)
    if (graphs[layer]->vcount() != n)
      throw Exception("Number of nodes are not equal for all graphs.");

  // Establish vertex order
  // We normally initialize the normal vertex order
  // of considering node 0,1,...
  vector<size_t> vertex_order = range(n);


  // But if we use a random order, we shuffle this order.
  if (_ld_probes_enabled()) _ld_traced_shuffle(vertex_order, &rng, "merge_constrained");
  else                      shuffle(vertex_order, &rng);

  // [TRACE-LD] Capture refine pass init: phase=1, level, queue, pre_membership.
  gTrace.passes.push_back({});
  size_t pass_idx_refine = gTrace.passes.size() - 1;
  gTrace.passes[pass_idx_refine].pass = pass_idx_refine;
  gTrace.passes[pass_idx_refine].phase = 1;     // merge_nodes_constrained
  gTrace.passes[pass_idx_refine].level = n;     // current graph vcount
  gTrace.passes[pass_idx_refine].n_nodes_in_queue = vertex_order.size();
  gTrace.passes[pass_idx_refine].shuffled_nodes = vertex_order;
  {
    auto const& mem = partitions[0]->membership();
    gTrace.passes[pass_idx_refine].pre_membership.assign(mem.begin(), mem.end());
  }
  fprintf(stderr, "[TRACE-LD] PASS_BEGIN pass=%zu phase=refine level=%zu queue=%zu\n",
          pass_idx_refine, n, vertex_order.size());

  vector< vector<size_t> > constrained_comms = constrained_partition->get_communities();

  vector<bool> comm_added(partitions[0]->n_communities(), false);
  vector<size_t> comms;

  // For each node
  for (size_t v : vertex_order)
  {
    // What is the current community of the node (this should be the same for all layers)
    size_t v_comm = partitions[0]->membership(v);

    if (partitions[0]->cnodes(v_comm) == 1)
    {
      // Clear comms
      for (size_t comm : comms)
        comm_added[comm] = false;
      comms.clear();

      if (consider_comms == ALL_COMMS)
      {
          // Add all communities to the set comms that are within the constrained community.
          size_t v_constrained_comm = constrained_partition->membership(v);
          for (size_t u : constrained_comms[v_constrained_comm])
          {
            size_t u_comm = partitions[0]->membership(u);
            if (!comm_added[u_comm]) {
              comms.push_back(u_comm);
              comm_added[u_comm] = true;
            }
          }
      }
      else if (consider_comms == ALL_NEIGH_COMMS)
      {
          /****************************ALL NEIGH COMMS*****************************/
          for (size_t layer = 0; layer < nb_layers; layer++)
          {
            for (size_t u : partitions[layer]->get_graph()->get_neighbours(v, IGRAPH_ALL)) {
              if (constrained_partition->membership(v) == constrained_partition->membership(u)) {
                size_t comm = partitions[layer]->membership(u);
                if (!comm_added[comm]) {
                  comms.push_back(comm);
                  comm_added[comm] = true;
                }
              }
            }
          }
      }
      else if (consider_comms == RAND_COMM)
      {
        /****************************RAND COMM***********************************/
          size_t v_constrained_comm = constrained_partition->membership(v);
          size_t random_idx = get_random_int(0, constrained_comms[v_constrained_comm].size() - 1, &rng);
          size_t rand_comm = constrained_comms[v_constrained_comm][random_idx];
          // No need to check if random_comm is already added, we only add one comm
          comms.push_back(rand_comm);
          comm_added[rand_comm] = true;
      }
      else if (consider_comms == RAND_NEIGH_COMM)
      {
        /****************************RAND NEIGH COMM*****************************/
          // Draw a random community among the neighbours, proportional to the
          // frequency of the communities among the neighbours. Notice this is no
          // longer
          vector<size_t> all_neigh_comms_incl_dupes;
          for (size_t layer = 0; layer < nb_layers; layer++)
          {
            vector<size_t> neigh_comm_layer = partitions[layer]->get_neigh_comms(v, IGRAPH_ALL, constrained_partition->membership());
            all_neigh_comms_incl_dupes.insert(all_neigh_comms_incl_dupes.end(), neigh_comm_layer.begin(), neigh_comm_layer.end());
          }
          size_t k = all_neigh_comms_incl_dupes.size();
          if (k > 0)
          {
            // Make sure there is also a probability not to move the node
            if (get_random_int(0, k, &rng) > 0)
            {
              size_t random_idx = get_random_int(0, k - 1, &rng);
              size_t rand_comm = all_neigh_comms_incl_dupes[random_idx];
              // No need to check if random_comm is already added, we only add one comm
              comms.push_back(rand_comm);
              comm_added[rand_comm] = true;
            }
          }
      }

      #ifdef DEBUG
        cerr << "Consider " << comms.size() << " communities for moving." << endl;
      #endif

      size_t max_comm = v_comm;
      bool _ld_max_comm_size_guard_r = (0 < max_comm_size && max_comm_size < partitions[0]->csize(v_comm));
      double max_improv = _ld_max_comm_size_guard_r ? -INFINITY : 0;
      double v_size = graphs[0]->node_size(v);
      // [TRACE-LD-CAND] Refine path candidate list + per-cand `possible_improv`.
      // Mirrors merge_nodes_constrained (Optimiser.cpp:1374-1378). Same
      // probe shape as move_nodes site so the JS-side mirror diffs without
      // branching on phase. max_improv_branch=NEG_INF|ZERO mirrors P0 #15
      // (refine initial threshold differs from move: 0 not 10*EPS).
      if (_ld_probes_enabled()) {
        const char* mi_branch_r = _ld_max_comm_size_guard_r ? "NEG_INF" : "ZERO";
        fprintf(stderr, "[TRACE-LD-CAND] CANDS pass=%zu visit=%zu v=%zu vcomm=%zu ncands=%zu max_improv_branch=%s max_improv_init=",
                pass_idx_refine, gTrace.passes[pass_idx_refine].moves.size(), v, v_comm, comms.size(), mi_branch_r);
        _ld_emit_hex(max_improv);
        fprintf(stderr, " cands=");
        for (size_t i = 0; i < comms.size(); i++) {
          fprintf(stderr, "%s%zu", i ? "," : "", comms[i]);
        }
        fprintf(stderr, "\n");
      }
      for (size_t comm : comms)
      {
        // reset comm_added to all false
        comm_added[comm] = false;

        // Do not create too-large communities.
        if (0 < max_comm_size && max_comm_size < partitions[0]->csize(comm) + v_size) {
          if (_ld_probes_enabled()) {
            fprintf(stderr, "[TRACE-LD-CAND] SKIP pass=%zu visit=%zu v=%zu comm=%zu reason=max_comm_size\n",
                    pass_idx_refine, gTrace.passes[pass_idx_refine].moves.size(), v, comm);
          }
          continue;
        }

        double possible_improv = 0.0;

        // Consider the improvement of moving to a community for all layers
        for (size_t layer = 0; layer < nb_layers; layer++)
        {
          // Make sure to multiply it by the weight per layer
          double dm = partitions[layer]->diff_move(v, comm);
          if (_ld_probes_enabled()) {
            fprintf(stderr, "[TRACE-LD-CAND] DIFF pass=%zu visit=%zu v=%zu comm=%zu layer=%zu lw=",
                    pass_idx_refine, gTrace.passes[pass_idx_refine].moves.size(), v, comm, layer);
            _ld_emit_hex(layer_weights[layer]);
            fprintf(stderr, " diff_move=");
            _ld_emit_hex(dm);
            fprintf(stderr, "\n");
          }
          possible_improv += layer_weights[layer]*dm;
        }

        // [TRACE-LD-CAND] PRE-COMPARE: refine uses `>=` (Optimiser.cpp:1374).
        bool accepts = (possible_improv >= max_improv);
        if (_ld_probes_enabled()) {
          fprintf(stderr, "[TRACE-LD-CAND] COMP pass=%zu visit=%zu v=%zu comm=%zu pimprov=",
                  pass_idx_refine, gTrace.passes[pass_idx_refine].moves.size(), v, comm);
          _ld_emit_hex(possible_improv);
          fprintf(stderr, " max_improv=");
          _ld_emit_hex(max_improv);
          fprintf(stderr, " accepts=%d\n", accepts ? 1 : 0);
        }
        if (accepts)
        {
          max_comm = comm;
          max_improv = possible_improv;
        }
      }

      // [TRACE-LD] Capture this refine visit's decision (singletons only).
      {
          LeidenTraceMove m;
          m.pass = pass_idx_refine;
          m.visit_idx = gTrace.passes[pass_idx_refine].moves.size();
          m.v = v;
          m.from_comm = v_comm;
          m.to_comm = max_comm;
          m.dQ = (max_comm != v_comm) ? max_improv : 0.0;
          m.moved = (max_comm != v_comm);
          gTrace.passes[pass_idx_refine].moves.push_back(m);
      }

      // If we actually plan to move the node
      if (max_comm != v_comm)
      {
          // Keep track of improvement
          total_improv += max_improv;

          #ifdef DEBUG
            // If we are debugging, calculate quality function
            double q_improv = 0;
          #endif

          for (size_t layer = 0; layer < nb_layers; layer++)
          {
            MutableVertexPartition* partition = partitions[layer];

            #ifdef DEBUG
              // If we are debugging, calculate quality function
              double q1 = partition->quality();
            #endif

            // Actually move the node
            partition->move_node(v, max_comm);
            #ifdef DEBUG
              // If we are debugging, calculate quality function
              // and report difference
              double q2 = partition->quality();
              double q_delta = layer_weights[layer]*(q2 - q1);
              q_improv += q_delta;
              cerr << "Move node " << v
              << " from " << v_comm << " to " << max_comm << " for layer " << layer
              << " (diff_move=" << max_improv
              << ", q2 - q1=" << q_delta << ")" << endl;
            #endif
          }
          #ifdef DEBUG
            if (fabs(q_improv - max_improv) > 1e-6)
            {
              cerr << "ERROR: Inconsistency while moving nodes, improvement as measured by quality function did not equal the improvement measured by the diff_move function." << endl
                   << " (diff_move=" << max_improv
                   << ", q2 - q1=" << q_improv << ")" << endl;
            }
          #endif
        }
      }
  }

  partitions[0]->renumber_communities();
  vector<size_t> const& membership = partitions[0]->membership();
  for (size_t layer = 1; layer < nb_layers; layer++)
  {
    partitions[layer]->set_membership(membership);
    #ifdef DEBUG
      cerr << "Renumbered communities for layer " << layer << " for " << partitions[layer]->n_communities() << " communities." << endl;
    #endif //DEBUG
  }
  // [TRACE-LD] Capture refine pass-end totals + post-renumber membership.
  gTrace.passes[pass_idx_refine].total_improv = total_improv;
  gTrace.passes[pass_idx_refine].nb_moves = 0; // refine doesn't track nb_moves like move_nodes
  // Count actual moves from trace.
  for (auto const& mm : gTrace.passes[pass_idx_refine].moves)
      if (mm.moved) gTrace.passes[pass_idx_refine].nb_moves++;
  {
      auto const& mem = partitions[0]->membership();
      gTrace.passes[pass_idx_refine].post_membership.assign(mem.begin(), mem.end());
  }
  fprintf(stderr, "[TRACE-LD] PASS_END pass=%zu phase=refine nb_moves=%zu improv=%.6f visits=%zu\n",
          pass_idx_refine, gTrace.passes[pass_idx_refine].nb_moves,
          total_improv, gTrace.passes[pass_idx_refine].moves.size());
  return total_improv;
}
