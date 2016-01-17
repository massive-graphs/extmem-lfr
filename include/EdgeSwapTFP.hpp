#pragma once

#include <stxxl/vector>
#include <stxxl/sorter>
#include <stxxl/bits/unused.h>

#include <defs.h>
#include "Swaps.h"
#include "GenericComparator.h"
#include "TupleHelper.h"

#include "EdgeSwapBase.h"

namespace EdgeSwapTFP {
    struct DependencyChainEdgeMsg {
        swapid_t swap_id;
        edgeid_t edge_id;
        edge_t edge;

        DependencyChainEdgeMsg() { }

        DependencyChainEdgeMsg(const swapid_t &swap_id_, const edgeid_t &edge_id_, const edge_t &edge_)
              : swap_id(swap_id_), edge_id(edge_id_), edge(edge_) { }

        DECL_LEX_COMPARE_OS(DependencyChainEdgeMsg, swap_id, edge_id, edge);
    };

    struct DependencyChainSuccessorMsg {
        swapid_t swap_id;
        edgeid_t edge_id;
        swapid_t successor;

        DependencyChainSuccessorMsg() { }

        DependencyChainSuccessorMsg(const swapid_t &swap_id_, const edgeid_t &edge_id_, const swapid_t &successor_) :
              swap_id(swap_id_), edge_id(edge_id_), successor(successor_) { }

        DECL_LEX_COMPARE_OS(DependencyChainSuccessorMsg, swap_id, edge_id, successor);
    };

    struct ExistenceRequestMsg {
        edge_t edge;
        swapid_t swap_id;
        bool forward_only;

        ExistenceRequestMsg() { }

        ExistenceRequestMsg(const edge_t &edge_, const swapid_t &swap_id_, const bool &forward_only_) :
              edge(edge_), swap_id(swap_id_),  forward_only(forward_only_) { }

        bool operator< (const ExistenceRequestMsg& o) const {
            return (edge < o.edge || (edge == o.edge && (swap_id > o.swap_id || (swap_id == o.swap_id && forward_only < o.forward_only))));
        }
        DECL_TO_TUPLE(edge, swap_id, forward_only);
        DECL_TUPLE_OS(ExistenceRequestMsg);
    };

    struct ExistenceInfoMsg {
        swapid_t swap_id;
        edge_t edge;
      #ifndef NDEBUG
        bool exists;
      #endif

        ExistenceInfoMsg() { }

        ExistenceInfoMsg(const swapid_t &swap_id_, const edge_t &edge_, const bool &exists_ = true) :
            swap_id(swap_id_), edge(edge_)
            #ifndef NDEBUG
            , exists(exists_)
            #endif
        { stxxl::STXXL_UNUSED(exists_); }

        DECL_LEX_COMPARE_OS(ExistenceInfoMsg, swap_id, edge
            #ifndef NDEBUG
            , exists
            #endif
        );
    };

    struct ExistenceSuccessorMsg {
        swapid_t swap_id;
        edge_t edge;
        swapid_t successor;

        ExistenceSuccessorMsg() { }

        ExistenceSuccessorMsg(const swapid_t &swap_id_, const edge_t &edge_, const swapid_t &successor_) :
              swap_id(swap_id_), edge(edge_), successor(successor_) { }

        DECL_LEX_COMPARE_OS(ExistenceSuccessorMsg, swap_id, edge, successor);
    };

    struct EdgeUpdateMsg {
        edgeid_t edge_id;
        swapid_t sender;
        edge_t updated_edge;

        EdgeUpdateMsg() { }

        EdgeUpdateMsg(const edgeid_t &edge_id_, const swapid_t &sender_, const edge_t &updated_edge_) :
              edge_id(edge_id_), sender(sender_), updated_edge(updated_edge_) { }

        DECL_LEX_COMPARE_OS(EdgeUpdateMsg, edge_id, sender, updated_edge);
    };

    template<class EdgeVector = stxxl::vector<edge_t>, class SwapVector = stxxl::vector<SwapDescriptor>, bool compute_stats = false>
    class EdgeSwapTFP : public EdgeSwapBase {
    public:
        using debug_vector = stxxl::vector<SwapResult>;
        using edge_vector = EdgeVector;
        using swap_vector = SwapVector;

    protected:
        constexpr static size_t _pq_mem = PQ_INT_MEM;
        constexpr static size_t _pq_pool_mem = PQ_POOL_MEM;
        constexpr static size_t _sorter_mem = SORTER_MEM;

        constexpr static bool _deduplicate_before_insert = false;

        EdgeVector &_edges;
        SwapVector &_swaps;

        typename SwapVector::iterator _swaps_begin;
        typename SwapVector::iterator _swaps_end;


        debug_vector _result;

// dependency chain
        // we need to use a desc-comparator since the pq puts the largest element on top
        using DependencyChainEdgeComparatorSorter = typename GenericComparatorStruct<DependencyChainEdgeMsg>::Ascending;
        using DependencyChainEdgeSorter = stxxl::sorter<DependencyChainEdgeMsg, DependencyChainEdgeComparatorSorter>;
        DependencyChainEdgeSorter _depchain_edge_sorter;

        using DependencyChainSuccessorComparator = typename GenericComparatorStruct<DependencyChainSuccessorMsg>::Ascending;
        using DependencyChainSuccessorSorter = stxxl::sorter<DependencyChainSuccessorMsg, DependencyChainSuccessorComparator>;
        DependencyChainSuccessorSorter _depchain_successor_sorter;

// existence requests
        using ExistenceRequestComparator = typename GenericComparatorStruct<ExistenceRequestMsg>::Ascending;
        using ExistenceRequestSorter = stxxl::sorter<ExistenceRequestMsg, ExistenceRequestComparator>;
        ExistenceRequestSorter _existence_request_sorter;

// existence information and dependencies
        using ExistenceInfoComparator = typename GenericComparatorStruct<ExistenceInfoMsg>::Ascending;
        using ExistenceInfoSorter = stxxl::sorter<ExistenceInfoMsg, ExistenceInfoComparator>;
        ExistenceInfoSorter _existence_info_sorter;

        using ExistenceSuccessorComparator = typename GenericComparatorStruct<ExistenceSuccessorMsg>::Ascending;
        using ExistenceSuccessorSorter = stxxl::sorter<ExistenceSuccessorMsg, ExistenceSuccessorComparator>;
        ExistenceSuccessorSorter _existence_successor_sorter;

// edge updates
        using EdgeUpdateComparator = typename GenericComparatorStruct<EdgeUpdateMsg>::Ascending;
        using EdgeUpdateSorter = stxxl::sorter<EdgeUpdateMsg, EdgeUpdateComparator>;
        EdgeUpdateSorter _edge_update_sorter;

// algos
        void _compute_dependency_chain();
        void _compute_conflicts();
        void _process_existence_requests();
        void _perform_swaps();
        void _apply_updates();

        void _reset() {
            _depchain_edge_sorter.clear();
            _depchain_successor_sorter.clear();
            _existence_request_sorter.clear();
            _existence_info_sorter.clear();
            _existence_successor_sorter.clear();
            _edge_update_sorter.clear();
        }

    public:
        EdgeSwapTFP() = delete;

        EdgeSwapTFP(const EdgeSwapTFP &) = delete;

        //! Swaps are performed during constructor.
        //! @param edges  Edge vector changed in-place
        //! @param swaps  Read-only swap vector
        EdgeSwapTFP(edge_vector &edges, swap_vector &swaps) :
              EdgeSwapBase(),
              _edges(edges),
              _swaps(swaps),

              _depchain_edge_sorter(DependencyChainEdgeComparatorSorter{}, _sorter_mem),
              _depchain_successor_sorter(DependencyChainSuccessorComparator{}, _sorter_mem),
              _existence_request_sorter(ExistenceRequestComparator{}, _sorter_mem),
              _existence_info_sorter(ExistenceInfoComparator{}, _sorter_mem),
              _existence_successor_sorter(ExistenceSuccessorComparator{}, _sorter_mem),
              _edge_update_sorter(EdgeUpdateComparator{}, _sorter_mem) { }

        void run(uint64_t swaps_per_iteration = 0);

        //! The i-th entry of this vector corresponds to the i-th
        //! swap provided to the constructor
        debug_vector &debugVector() {
           return _result;
        }
    };
}
