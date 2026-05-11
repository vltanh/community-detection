// [TRACER FORK] Verbatim copy of libleidenalg/src/ModularityVertexPartition.cpp +
// trace injection inside `diff_move` for term-by-term breakdown. Closes
// P0 #13 (audit row E). Probe gate `LEIDEN_DUMP_PROBES=1`.
//
// Mirrors canonical `#ifdef DEBUG` block at ModularityVertexPartition.cpp:
// 37-113 (cerr lines after each term).
#include "ModularityVertexPartition.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern bool _ld_probes_enabled();
static void _ld_mod_emit_hex(double x) {
  unsigned long long u;
  std::memcpy(&u, &x, 8);
  fprintf(stderr, "%016llx", u);
}

ModularityVertexPartition::ModularityVertexPartition(Graph* graph,
      vector<size_t> const& membership) :
        MutableVertexPartition(graph,
        membership)
{ }

ModularityVertexPartition::ModularityVertexPartition(Graph* graph) :
        MutableVertexPartition(graph)
{ }

ModularityVertexPartition::~ModularityVertexPartition()
{ }

ModularityVertexPartition* ModularityVertexPartition::create(Graph* graph)
{
  return new ModularityVertexPartition(graph);
}

ModularityVertexPartition* ModularityVertexPartition::create(Graph* graph, vector<size_t> const& membership)
{
  return new ModularityVertexPartition(graph, membership);
}

double ModularityVertexPartition::diff_move(size_t v, size_t new_comm)
{
  size_t old_comm = this->_membership[v];
  double diff = 0.0;
  double total_weight = this->graph->total_weight()*(2.0 - this->graph->is_directed());
  if (total_weight == 0.0)
    return 0.0;
  if (new_comm != old_comm)
  {
    double w_to_old = this->weight_to_comm(v, old_comm);
    double w_from_old = this->weight_from_comm(v, old_comm);
    double w_to_new = this->weight_to_comm(v, new_comm);
    double w_from_new = this->weight_from_comm(v, new_comm);
    double k_out = this->graph->strength(v, IGRAPH_OUT);
    double k_in = this->graph->strength(v, IGRAPH_IN);
    double self_weight = this->graph->node_self_weight(v);
    double K_out_old = this->total_weight_from_comm(old_comm);
    double K_in_old = this->total_weight_to_comm(old_comm);
    double K_out_new = this->total_weight_from_comm(new_comm) + k_out;
    double K_in_new = this->total_weight_to_comm(new_comm) + k_in;
    double diff_old = (w_to_old - k_out*K_in_old/total_weight) + \
               (w_from_old - k_in*K_out_old/total_weight);
    double diff_new = (w_to_new + self_weight - k_out*K_in_new/total_weight) + \
               (w_from_new + self_weight - k_in*K_out_new/total_weight);
    diff = diff_new - diff_old;
    double m;
    if (this->graph->is_directed())
      m = this->graph->total_weight();
    else
      m = 2*this->graph->total_weight();
    // [TRACE-LD-MOD] Per-call 15-term breakdown. Probe fires AFTER all
    // terms computed, BEFORE return. cpp source lines: 35-119.
    if (_ld_probes_enabled()) {
      fprintf(stderr, "[TRACE-LD-MOD] v=%zu old=%zu new=%zu", v, old_comm, new_comm);
      fprintf(stderr, " total_weight="); _ld_mod_emit_hex(total_weight);
      fprintf(stderr, " w_to_old=");     _ld_mod_emit_hex(w_to_old);
      fprintf(stderr, " w_from_old=");   _ld_mod_emit_hex(w_from_old);
      fprintf(stderr, " w_to_new=");     _ld_mod_emit_hex(w_to_new);
      fprintf(stderr, " w_from_new=");   _ld_mod_emit_hex(w_from_new);
      fprintf(stderr, " k_out=");        _ld_mod_emit_hex(k_out);
      fprintf(stderr, " k_in=");         _ld_mod_emit_hex(k_in);
      fprintf(stderr, " self_weight=");  _ld_mod_emit_hex(self_weight);
      fprintf(stderr, " K_out_old=");    _ld_mod_emit_hex(K_out_old);
      fprintf(stderr, " K_in_old=");     _ld_mod_emit_hex(K_in_old);
      fprintf(stderr, " K_out_new=");    _ld_mod_emit_hex(K_out_new);
      fprintf(stderr, " K_in_new=");     _ld_mod_emit_hex(K_in_new);
      fprintf(stderr, " diff_old=");     _ld_mod_emit_hex(diff_old);
      fprintf(stderr, " diff_new=");     _ld_mod_emit_hex(diff_new);
      fprintf(stderr, " diff=");         _ld_mod_emit_hex(diff);
      fprintf(stderr, " m=");            _ld_mod_emit_hex(m);
      fprintf(stderr, " result=");       _ld_mod_emit_hex(diff/m);
      fprintf(stderr, "\n");
    }
    return diff/m;
  }
  double m;
  if (this->graph->is_directed())
    m = this->graph->total_weight();
  else
    m = 2*this->graph->total_weight();
  return diff/m;
}


double ModularityVertexPartition::quality()
{
  double mod = 0.0;

  double m;
  if (this->graph->is_directed())
    m = this->graph->total_weight();
  else
    m = 2*this->graph->total_weight();

  if (m == 0)
    return 0.0;

  for (size_t c = 0; c < this->n_communities(); c++)
  {
    double w = this->total_weight_in_comm(c);
    double w_out = this->total_weight_from_comm(c);
    double w_in = this->total_weight_to_comm(c);
    mod += w - w_out*w_in/((this->graph->is_directed() ? 1.0 : 4.0)*this->graph->total_weight());
  }
  double q = (2.0 - this->graph->is_directed())*mod;
  return q/m;
}
