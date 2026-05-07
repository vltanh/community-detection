/******************************************************************************
 * container_swap.h
 *
 * Compile-time swap between hash-bucket-ordered and id-ASC-ordered
 * associative containers (audit row H closure for VieCut).
 *
 * -DCANONICAL_MODE -> std::unordered_set/map (matches unmodified upstream).
 * -DTRACER_MODE    -> std::set/map (id-ASC iteration; matches the JS port's
 *                     sort-on-demand mirror at the eight row-H call sites).
 *
 * Aliased into the eight unordered_* sites listed in viecut/audit.md row H.
 *****************************************************************************/

#pragma once

#if !defined(CANONICAL_MODE) && !defined(TRACER_MODE)
#error "must define -DCANONICAL_MODE or -DTRACER_MODE"
#endif
#if defined(CANONICAL_MODE) && defined(TRACER_MODE)
#error "exactly one of CANONICAL_MODE / TRACER_MODE must be defined"
#endif

#ifdef TRACER_MODE
  #include <map>
  #include <set>
  template<class K>            using TracerSet = std::set<K>;
  template<class K, class V>   using TracerMap = std::map<K, V>;
  static constexpr const char* k_build_mode = "TRACER_MODE";
#else
  #include <unordered_map>
  #include <unordered_set>
  template<class K>            using TracerSet = std::unordered_set<K>;
  template<class K, class V>   using TracerMap = std::unordered_map<K, V>;
  static constexpr const char* k_build_mode = "CANONICAL_MODE";
#endif
