#include "EdgeSwapParallelTFP.h"

#include <algorithm>
#include <array>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include <stx/btree_map>
#include <stxxl/priority_queue>

#include <ParallelBufferedPQSorterMerger.h>
#include "PQSorterMerger.h"
#include "EdgeVectorUpdateStream.h"
#include <EdgeExistenceInformation.h>

namespace EdgeSwapParallelTFP {

        struct edge_information_t {
            std::array<std::atomic<bool>, 2> is_set;
            std::array<edge_t, 2> edge;
            std::array<edge_t*, 2> edges_begin;
            std::array<edge_t*, 2> edges_end;

            template <typename F>
            void forEach(unsigned char spos, F f) {
                assert(is_set[spos]);

                f(edge[spos]);
                if (edges_begin[spos] != nullptr) {
                    for (edge_t* e = edges_begin[spos]; e < edges_end[spos]; ++e) {
                        f(*e);
                    }
                }
            };

            std::size_t num_edges(unsigned char spos) {
                assert(is_set[spos]);

                if (edges_begin[spos] == nullptr) {
                    return 1;
                } else {
                    return (edges_end[spos] - edges_begin[spos]) + 1;
                }
            };

            void reset(unsigned char spos) {
                is_set[spos] = false;
                edge[spos] = edge_t::invalid();
                edges_begin[spos] = nullptr;
                edges_end[spos] = nullptr;
            };

            void wait(unsigned char spos) {
                while (!is_set[spos].load(std::memory_order_seq_cst)) {
                    std::this_thread::yield();
                }
            };
        };


        struct edge_buffer_t {
            std::vector<std::vector<edge_t>> buffers;
            std::size_t cur_buffer, cur_pos;
            std::size_t buffer_size;

            edge_buffer_t(size_t buffer_size) : cur_buffer(0), cur_pos(0), buffer_size(buffer_size) {
                buffers.emplace_back(buffer_size);
            };

            void reset() {
                cur_buffer = 0;
                cur_pos = 0;
            };

            edge_t* start_push(std::size_t num_items) {
                if (UNLIKELY(cur_pos + num_items > buffers[cur_buffer].size())) {
                    ++cur_buffer;
                    cur_pos = 0;
                    if (buffers.size() > cur_buffer) {
                        if (UNLIKELY(buffers[cur_buffer].size() < num_items)) {
                            buffers[cur_buffer].resize(num_items);
                        }
                    } else {
                        buffers.emplace_back(std::max(buffer_size, num_items));
                    }
                }
                return &buffers[cur_buffer][cur_pos];
            };

            void push(const edge_t& e) {
                assert(buffers[cur_buffer].size() > cur_pos);
                buffers[cur_buffer][cur_pos++] = e;
            };

            edge_t* end_push() {
                return &buffers[cur_buffer][cur_pos];
            };
        };

    EdgeSwapParallelTFP::EdgeSwapParallelTFP(EdgeStream &edges, EdgeSwapBase::swap_vector &, swapid_t swaps_per_iteration) : EdgeSwapParallelTFP(edges, swaps_per_iteration) { }

    EdgeSwapParallelTFP::EdgeSwapParallelTFP(EdgeStream &edges, swapid_t swaps_per_iteration, int num_threads) :
              EdgeSwapBase(),
              _edges(edges),
              _num_swaps_per_iteration(swaps_per_iteration),
              _num_swaps_in_run(0),
#ifdef EDGE_SWAP_DEBUG_VECTOR
              _debug_vector_writer(_result),
#endif

              _swap_direction(num_threads),
              _edge_swap_sorter(GenericComparatorStruct<EdgeLoadRequest>::Ascending(), _sorter_mem),
              _edge_state(num_threads),
              _needs_writeback(false),
              _existence_info(num_threads),
              _edge_update_merger(EdgeUpdateComparator{}, _sorter_mem),
              _num_threads(num_threads) {

        _start_stats();
        omp_set_nested(1);
        for (int i = 0; i < _num_threads; ++i) {
            _swap_direction[i].reset(new BoolStream);
        }
    } // FIXME actually _edge_update_merger isn't needed all the time. If memory is an issue, we could safe memory here

    void EdgeSwapParallelTFP::run() {
        process_swaps();
        process_swaps();
#ifdef EDGE_SWAP_DEBUG_VECTOR
        _debug_vector_writer.finish();
#endif
    }


    void EdgeSwapParallelTFP::process_swaps() {
        // if we have no swaps to load and no edges to write back, do nothing (might happen by calling process_swaps several times)
        if (_num_swaps_in_run == 0 && !_needs_writeback) return;
        _report_stats("_push_swaps");

        std::vector<std::unique_ptr<DependencyChainSuccessorSorter>> swap_edge_dependencies_sorter(_num_threads);

        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistenceSuccessorSorter > > existence_successor_sorter(_num_threads);
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistencePlaceholderSorter > > existence_placeholder_sorter(_num_threads);

        // allocate sorters only if there is actually something to do!
        if (_num_swaps_in_run > 0) {
            _edge_state.clear();
            _existence_info.clear();

            // allocate sorters in parallel because of NUMA
            #pragma omp parallel num_threads(_num_threads)
            {
                auto tid = omp_get_thread_num();
                swap_edge_dependencies_sorter[tid].reset(new DependencyChainSuccessorSorter(DependencyChainSuccessorComparator(), _sorter_mem));

                existence_successor_sorter[tid].reset(new ExistenceSuccessorSorter(ExistenceSuccessorComparator(), _sorter_mem));
                existence_placeholder_sorter[tid].reset(new ExistencePlaceholderSorter(ExistencePlaceholderComparator(), _sorter_mem));
            }
        }
        _report_stats("_init_process_swaps");

        _load_and_update_edges(swap_edge_dependencies_sorter);

        _report_stats("_load_and_update_edges");

        if (_num_swaps_in_run > 0) {
            for (auto &w : _swap_direction) {
                w->consume();
            }

            {
                ExistenceRequestMerger existence_merger(ExistenceRequestComparator(), SORTER_MEM);

                _compute_conflicts(swap_edge_dependencies_sorter, existence_merger);
                _report_stats("_compute_conflicts");
                _process_existence_requests(existence_merger, existence_successor_sorter, existence_placeholder_sorter);
                _report_stats("_process_existence_requests");
            }

            _perform_swaps(swap_edge_dependencies_sorter, existence_successor_sorter, existence_placeholder_sorter);
            _report_stats("_perform_swaps");

            for (auto &w : _swap_direction) {
                w->clear();
            }
        }

        // re-initialize data structures for new swaps
        _num_swaps_in_run = 0;
        _edge_swap_sorter.clear();
        _report_stats("_cleanup");
    }

    void EdgeSwapParallelTFP::_load_and_update_edges(std::vector<std::unique_ptr<DependencyChainSuccessorSorter>> &dependency_output) {
        uint64_t numSwaps = _num_swaps_in_run;
        _edge_swap_sorter.sort();

        bool loaded_edges = !_edge_swap_sorter.empty();


        if (compute_stats) {
            std::cout << "Requesting " << _edge_swap_sorter.size() << " non-unique edges for internal swaps" << std::endl;
        }

        BoolStream next_valid_edges;


        { // load edges from EM. Generates successor information and swap_edges information (for the first edge in the chain).
            auto use_edge = [&] (const edge_t & cur_e, edgeid_t id) {
                swapid_t sid;
                int tid;

                auto match_request = [&]() {
                    if (!_edge_swap_sorter.empty() && _edge_swap_sorter->eid == id) {
                        sid = _edge_swap_sorter->sid;
                        tid = _thread(get_swap_id(sid));
                        assert(tid < _num_threads);
                        ++_edge_swap_sorter;
                        return true;
                    } else {
                        return false;
                    }
                };

                if (match_request()) {
                    assert(dependency_output[tid]);
                    next_valid_edges.push(false);
                    _edge_state.push_sorter(DependencyChainEdgeMsg {sid, cur_e});

                    auto lastSid = sid;
                    auto lastTid = tid;


                    // further requests for the same swap - store successor information
                    while (match_request()) {
                        // set edge id to internal edge id
                        assert(dependency_output[lastTid]);
                        dependency_output[lastTid]->push(DependencyChainSuccessorMsg {lastSid, sid});
                        lastSid = sid;
                        lastTid = tid;
                    }
                } else {
                    next_valid_edges.push(true);
                }
            };

            edgeid_t id = 0;

            if (!_needs_writeback) {
                // just read edges
                for (; !_edges.empty(); ++id, ++_edges) {
                    use_edge(*_edges, id);
                }
                _edges.rewind();
            } else {
                EdgeVectorUpdateStream<EdgeStream, BoolStream, EdgeUpdateMerger, true> edge_update_stream(_edges, _valid_edges, _edge_update_merger);

                for (; !edge_update_stream.empty(); ++id, ++edge_update_stream) {
                    use_edge(*edge_update_stream, id);
                }

                assert(static_cast<std::size_t>(id) == _edges.size());

                edge_update_stream.finish();
                _edge_update_merger.deallocate();
                _edges.rewind();
            }

            _needs_writeback = loaded_edges;

            std::swap(_valid_edges, next_valid_edges);
            _valid_edges.consume();
        }


        if (numSwaps > 0) {
            _edge_state.finish_sorter_input();

            for (int tid = 0; tid < _num_threads; ++tid) {
                dependency_output[tid]->sort();
            }

            _edge_swap_sorter.finish_clear();
        }
    }

    /*
     * Since we do not yet know whether an swap can be performed, we keep for
     * every edge id a set of possible states. Initially this state is only the
     * edge as fetched in _compute_dependency_chain(), but after the first swap
     * the set contains at least two configurations, i.e. the original state
     * (in case the swap cannot be performed) and the swapped state.
     *
     * These configurations are kept in depchain_edge_pq: Each swap receives
     * the complete state set of both edges and computes the cartesian product
     * of both. If there exists a successor swap (info stored in
     * _depchain_successor_sorter), i.e. a swap that will be  affect by the
     * current one, these information are forwarded.
     *
     * We further request information whether the edge exists by pushing requests
     * into _existence_request_sorter.
     */
    void EdgeSwapParallelTFP::_compute_conflicts(std::vector< std::unique_ptr< EdgeSwapParallelTFP::DependencyChainSuccessorSorter > > &dependencies, ExistenceRequestMerger &requestOutputMerger) {

        // FIXME make sure that this leads to useful sort buffer sizes!
        const auto existence_request_buffer_size = SORTER_MEM/sizeof(ExistenceRequestMsg)/2;
        swapid_t batch_size_per_thread = IntScale::Mi;
        swapid_t num_batches_till_sorter_run = std::max<swapid_t>(1, existence_request_buffer_size / (batch_size_per_thread * 6)); // assume 6 messages per swap - 4 are minimum
        STXXL_MSG("Batch size per thread in _compute_conflicts: " << batch_size_per_thread << ", perform sorter run every " << num_batches_till_sorter_run << " batches");

        std::vector<std::unique_ptr<edge_buffer_t>> edge_forward_buffer(_num_threads);

        // pointers are used to make sure that everything is in the memory region of the specific thread
        std::vector<std::unique_ptr<std::vector<edge_information_t>>> edge_information(_num_threads);

        stxxl::stream::runs_creator<stxxl::stream::from_sorted_sequences<ExistenceRequestMsg>,
        ExistenceRequestComparator, STXXL_DEFAULT_BLOCK_SIZE(ExistenceRequestMsg), STXXL_DEFAULT_ALLOC_STRATEGY> existence_request_runs_creator (ExistenceRequestComparator(), SORTER_MEM);
        using runs_creator_thread_t = RunsCreatorThread<decltype(existence_request_runs_creator)>;

        std::unique_ptr<runs_creator_thread_t> existence_request_runs_creator_thread(new runs_creator_thread_t(existence_request_runs_creator));
        using runs_creator_buffer_t = RunsCreatorBuffer<decltype(existence_request_runs_creator)>;

        std::vector<std::unique_ptr<runs_creator_buffer_t>> existence_request_buffer(_num_threads);

        #pragma omp parallel num_threads(_num_threads)
        {

            int tid = omp_get_thread_num();

            edge_information[tid].reset(new std::vector<edge_information_t>(batch_size_per_thread));
            existence_request_buffer[tid].reset(new runs_creator_buffer_t(*existence_request_runs_creator_thread, existence_request_buffer_size));
            edge_forward_buffer[tid].reset(new edge_buffer_t(batch_size_per_thread));
        }

        swapid_t loop_limit = _num_swaps_in_run;
        {
            swapid_t remainder = _num_swaps_in_run % _num_threads;
            if (remainder != 0) loop_limit += (_num_threads - remainder);
        }

        for (swapid_t sid_in_batch_base = 0, batch_num = 0; sid_in_batch_base < loop_limit; sid_in_batch_base += batch_size_per_thread * _num_threads, ++batch_num) { // execution of batch starts
            swapid_t sid_in_batch_limit = std::min<swapid_t>(_num_swaps_in_run, sid_in_batch_base + batch_size_per_thread * _num_threads);

            { // todo: put in extra thread!
                DependencyChainEdgeMsg limit({pack_swap_id_spos(sid_in_batch_limit, 0), edge_t{-1, -1}});
                _edge_state.start_batch(limit);

                std::vector<edge_t> edge_buf;
                edge_buf.reserve(8);

                // read information from sorter, should be most edges
                for (swapid_t swap_id = sid_in_batch_base, pos = 0; swap_id < sid_in_batch_limit; ++pos) {
                    for (int tid = 0; tid < _num_threads; ++tid, ++swap_id) {
                        edge_information_t& current_edge_info = (*edge_information[tid])[pos];
                        for (unsigned char spos = 0; spos < 2; ++spos) {
                            current_edge_info.reset(spos);

                            swapid_t s = pack_swap_id_spos(swap_id, spos);
                            assert(_edge_state.empty() || _edge_state->sid >= s);

                            if (!_edge_state.empty() && _edge_state->sid == s) {
                                current_edge_info.edge[spos] = _edge_state->edge;
                                current_edge_info.is_set[spos] = true;
                                ++_edge_state;
                            }

                            while (!_edge_state.empty() && _edge_state->sid == s) {
                                edge_buf.push_back(_edge_state->edge);
                                ++_edge_state;
                            }

                            if (!edge_buf.empty()) {
                                current_edge_info.edges_begin[spos] = edge_forward_buffer[tid]->start_push(edge_buf.size());

                                for (const auto& e : edge_buf) {
                                    edge_forward_buffer[tid]->push(e);
                                }

                                current_edge_info.edges_end[spos] = edge_forward_buffer[tid]->end_push();

                                edge_buf.clear();
                            }
                        }
                    }
                }

                assert(_edge_state.empty());

                _edge_state.start_push();
            }

            #pragma omp parallel num_threads(_num_threads)
            {
                int tid = omp_get_thread_num();

                auto &my_edge_information = *edge_information[tid];

                auto &my_swap_direction =  *_swap_direction[tid];

                auto &dep = *dependencies[tid];

                auto &my_existence_request_buffer = *existence_request_buffer[tid];

                edge_buffer_t & my_edge_forward_buffer = *edge_forward_buffer[tid];

                std::array<std::vector<edge_t>, 2> dd_new_edges;


                swapid_t sid = sid_in_batch_base + tid;
                for (swapid_t i = 0; i < batch_size_per_thread && sid < loop_limit; ++i, sid += _num_threads) {
                    if (UNLIKELY(sid >= _num_swaps_in_run)) continue;

                    std::array<swapid_t, 2> successor_sid = {0, 0};

                    assert(!my_swap_direction.empty());

                    bool direction = *my_swap_direction;
                    ++my_swap_direction;

                    edge_information_t& current_edge_info = my_edge_information[i];

                    // fetch messages sent to this edge
                    for (unsigned int spos = 0; spos < 2; spos++) {
                        // get successor
                        if (!dep.empty()) {
                            auto &msg = *dep;

                            assert(get_swap_id(msg.sid) >= sid);
                            assert(get_swap_id(msg.sid) > sid || get_swap_spos(msg.sid) >= spos);

                            if (msg.sid == pack_swap_id_spos(sid, spos)) {
                                DEBUG_MSG(_display_debug, "Got successor for S" << sid << ", E" << spos << ": " << msg);
                                successor_sid[spos] = msg.successor;
                                assert(get_swap_id(msg.successor) > sid);
                                ++dep;
                            }
                        }


                        // ensure that we received at least one state of the edge before the swap
                        current_edge_info.wait(spos);

                        DEBUG_MSG(_display_debug, "SWAP " << sid << " Edge " << spos << " Successor: " << successor_sid[spos] << " States: " << current_edge_info.num_edges(spos));

                        assert(current_edge_info.num_edges(spos) > 0);

                        // ensure that dependent swap is in fact a successor (i.e. has larger index)
                        assert(!successor_sid[spos] || get_swap_id(successor_sid[spos]) > sid);
                    }

                    #ifndef NDEBUG
                        if (_display_debug) {
                            std::cout << "Swap " << sid << " edges[0] = [";
                            current_edge_info.forEach(0, [&](const edge_t&e) {
                                std::cout << e << " ";
                            });
                            std::cout << "] edges[1]= [";
                            current_edge_info.forEach(1, [&](const edge_t&e) {
                                std::cout << e << " ";
                            });
                            std::cout << "]" << std::endl;
                        }
                    #endif

                    // compute "cartesian" product between possible edges to determine all possible new edges
                    dd_new_edges[0].clear();
                    dd_new_edges[1].clear();

                    current_edge_info.forEach(0, [&](const edge_t&e1) {
                        current_edge_info.forEach(1, [&](const edge_t&e2) {
                            edge_t new_edges[2];
                            std::tie(new_edges[0], new_edges[1]) = _swap_edges(e1, e2, direction);

                            for (unsigned int i = 0; i < 2; i++) {
                                auto &new_edge = new_edges[i];
                                auto &queue = dd_new_edges[i];

                                queue.push_back(new_edge);

                                DEBUG_MSG(_display_debug, "Swap " << sid << " may yield " << new_edge << " at " << i);
                            }
                        });
                    });

                    for (unsigned char spos = 0; spos < 2; spos++) {
                        auto & dd = dd_new_edges[spos];

                        // sort to support binary search and linear time deduplication
                        if (dd.size() > 1) {
                            std::sort(dd.begin(), dd.end());
                        }

                        bool has_successor_in_batch = false;
                        bool has_successor_in_other_batch = false;
                        edge_information_t* t_information = nullptr;
                        unsigned char successor_spos = 0;
                        swapid_t successor_pos = 0;
                        int_t successor_tid = 0;
                        swapid_t num_forwarded_edges = 0;
                        if (successor_sid[spos]) {
                            successor_tid = _thread(get_swap_id(successor_sid[spos]));
                            if (get_swap_id(successor_sid[spos]) < sid_in_batch_limit) {
                                has_successor_in_batch = true;

                                successor_pos = (get_swap_id(successor_sid[spos]) - sid_in_batch_base)/_num_threads;
                                successor_spos = get_swap_spos(successor_sid[spos]);
                                t_information = &(*edge_information[successor_tid])[successor_pos];

                                t_information->edges_begin[successor_spos] = my_edge_forward_buffer.start_push(current_edge_info.num_edges(spos) + dd.size());
                            } else {
                                has_successor_in_other_batch = true;
                            }
                        }

                        auto new_it = dd.begin(), new_end = dd.end();

                        auto forward_edge = [&](const edge_t &e, bool is_source) {
                            my_existence_request_buffer.push(ExistenceRequestMsg {e, sid, is_source});

                            if (UNLIKELY(has_successor_in_other_batch)) {
                                _edge_state.push_pq(tid, DependencyChainEdgeMsg {successor_sid[spos], e});
                            }
                            if (UNLIKELY(has_successor_in_batch)) {
                                if (num_forwarded_edges == 0) {
                                    t_information->edge[successor_spos] = e;
                                } else {
                                    my_edge_forward_buffer.push(e);
                                }
                                ++num_forwarded_edges;
                            }
                        };

                        if (current_edge_info.edges_begin[spos] != nullptr) {
                            assert(std::is_sorted(current_edge_info.edges_begin[spos], current_edge_info.edges_end[spos]));
                            assert(std::unique(current_edge_info.edges_begin[spos], current_edge_info.edges_end[spos]) == current_edge_info.edges_end[spos]);
                        }

                        assert(std::is_sorted(new_it, new_end));

                        current_edge_info.forEach(spos, [&](const edge_t& e) {
                            auto last_e = edge_t::invalid();

                            while (new_it != new_end && *new_it <= e) {
                                forward_edge(*new_it, false);

                                last_e = *new_it;

                                do {
                                    ++new_it;
                                } while (new_it != new_end && *new_it == last_e); // skip duplicates
                            }

                            if (e != last_e) { // do not forward current edge if equivalent to target edge
                                forward_edge(e, true);
                            }
                        });

                        // forward new edges past the current edges
                        while (new_it != new_end) {
                            forward_edge(*new_it, false);

                            auto last_e = *new_it;

                            do {
                                ++new_it;
                            } while (new_it != new_end && *new_it == last_e); // skip duplicates
                        }

                        if (has_successor_in_batch) {
                            // make sure that the vector is flushed before is_set is updated!
                            if (LIKELY(num_forwarded_edges > 1)) {
                                t_information->edges_end[successor_spos] = my_edge_forward_buffer.end_push();
                            } else {
                                t_information->edges_begin[successor_spos] = nullptr;
                            }
                            t_information->is_set[successor_spos].store(true, std::memory_order_seq_cst);
                        }
                    }

                }

                my_edge_forward_buffer.reset(); // reset doesn't delete any data, so we do not invalidate data of other threads

                // finished batch.

                // sort buffer and enqueue sorted buffer to be written out
                if (batch_num % num_batches_till_sorter_run == 0 || sid_in_batch_limit == _num_swaps_in_run)
                    my_existence_request_buffer.finish();

            } // end of parallel section

            _edge_state.end_batch();

        } // finished processing all swaps of the current run

        #pragma omp parallel
        {
            edge_information[omp_get_thread_num()].reset(nullptr);
        }

        for (int tid = 0; tid < _num_threads; ++tid) {
            assert(_swap_direction[tid]->empty());
            _swap_direction[tid]->rewind();
            existence_request_buffer[tid]->flush(); // make sure all requests are processed!
            existence_request_buffer[tid].reset(nullptr);

            dependencies[tid]->rewind();
        }

        existence_request_runs_creator_thread.reset(nullptr);

        _edge_state.rewind_sorter();

        #pragma omp flush

        requestOutputMerger.initialize(existence_request_runs_creator.result());
    }

    /*
     * We parallel stream through _edges and requestMerger
     * to check whether a requested edge exists in the input graph.
     * The result is sent to the first swap requesting using
     * existence_info_output. We additionally compute a dependency chain
     * by informing every swap about the next one requesting the info and inform each swap how many edges it will get using placeholders.
     */
    void EdgeSwapParallelTFP::_process_existence_requests(ExistenceRequestMerger &requestMerger,
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistenceSuccessorSorter > > &successor_output,
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistencePlaceholderSorter > > &existence_placeholder_output) {

        while (!requestMerger.empty()) {
            auto &request = *requestMerger;
            edge_t current_edge = request.edge;

            // find edge in graph
            bool exists = false;
            for (; !_edges.empty(); ++_edges) {
                const auto &edge = *_edges;
                if (edge > current_edge) break;
                exists = (edge == current_edge);
            }

            // build depencency chain (i.e. inform earlier swaps about later ones) and find the earliest swap
            swapid_t last_swap = request.get_swap_id();
            bool foundTargetEdge = false; // if we already found a swap where the edge is a target
            for (; !requestMerger.empty(); ++requestMerger) {
                const ExistenceRequestMsg &request = *requestMerger;
                if (request.edge != current_edge)
                    break;

                swapid_t swap_id = request.get_swap_id();

                if (last_swap != swap_id && foundTargetEdge) {
                    // inform an earlier swap about later swaps that need the new state
                    assert(last_swap > swap_id);
                    successor_output[_thread(swap_id)]->push(ExistenceSuccessorMsg{swap_id, current_edge, last_swap});
                    existence_placeholder_output[_thread(last_swap)]->push(last_swap);
                    DEBUG_MSG(_display_debug, "Inform swap " << swap_id << " that " << last_swap << " is a successor for edge " << current_edge);
                }

                last_swap = swap_id;
                foundTargetEdge = (foundTargetEdge || !request.get_forward_only());
            }

            // inform earliest swap whether edge exists
            if (foundTargetEdge && exists) {
                auto tid = _thread(last_swap);
                _existence_info.push_sorter(ExistenceInfoMsg{last_swap, current_edge});
                existence_placeholder_output[tid]->push(last_swap);
                DEBUG_MSG(_display_debug, "Inform swap " << last_swap << " edge " << current_edge << " exists " << exists);
            }
        }

        _existence_info.finish_sorter_input();
        _edges.rewind();


        for (int tid = 0; tid < _num_threads; ++tid) {
            existence_placeholder_output[tid]->sort();
            successor_output[tid]->sort();
        }
    }

    /*
     * Information sources:
     *  _swaps contains definition of swaps
     *  _depchain_successor_sorter stores swaps we need to inform about our actions
     */
    void EdgeSwapParallelTFP::_perform_swaps(
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::DependencyChainSuccessorSorter > > &edge_dependencies,
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistenceSuccessorSorter > > &existence_successor,
        std::vector< std::unique_ptr< EdgeSwapParallelTFP::ExistencePlaceholderSorter > > &existence_placeholder) {

        // FIXME make sure that this leads to useful sort buffer sizes!
        const auto merger_buffer_size = SORTER_MEM/sizeof(edge_t)/2; // buffer size should be SORTER_MEM/2 and each swap produces up to two edge updates
        constexpr swapid_t batch_size_per_thread = IntScale::Mi;
        swapid_t num_batches_till_sorter_run = std::max<swapid_t>(1, merger_buffer_size / (batch_size_per_thread * 2));
        STXXL_MSG("Batch size per thread in _perform_swaps: " << batch_size_per_thread << ", perform sorter run every " << num_batches_till_sorter_run << " batches");

#ifdef EDGE_SWAP_DEBUG_VECTOR
        // debug only
        // this is not good for NUMA, but hey, this is debug mode (+ this is write once + read once in a single thread, so either writing or reading is bad anyway)
        std::vector<std::vector<debug_vector::value_type>> debug_output_buffer(_num_threads);
        for (auto & v : debug_output_buffer) {
            v.reserve(batch_size_per_thread);
        }
#endif

        stxxl::stream::runs_creator<stxxl::stream::from_sorted_sequences<edge_t>,
        EdgeUpdateComparator, STXXL_DEFAULT_BLOCK_SIZE(edge_t), STXXL_DEFAULT_ALLOC_STRATEGY> edge_update_runs_creator (EdgeUpdateComparator(), SORTER_MEM);

        using runs_creator_thread_t = RunsCreatorThread<decltype(edge_update_runs_creator)>;

        std::unique_ptr<runs_creator_thread_t> edge_update_runs_creator_thread(new runs_creator_thread_t(edge_update_runs_creator));

        std::vector<std::unique_ptr<std::vector<std::array<edge_t, 2>>>> source_edges(_num_threads);

        std::vector<std::unique_ptr<EdgeExistenceInformation>> existence_information(_num_threads);

        using runs_creator_buffer_t = RunsCreatorBuffer<decltype(edge_update_runs_creator)>;
        std::vector<std::unique_ptr<runs_creator_buffer_t>> edge_update_buffer(_num_threads);


        #pragma omp parallel num_threads(_num_threads)
        {
            const auto tid = omp_get_thread_num();

            source_edges[tid].reset(new std::vector<std::array<edge_t, 2>>(batch_size_per_thread, std::array<edge_t, 2>{edge_t::invalid(), edge_t::invalid()}));
            existence_information[tid].reset(new EdgeExistenceInformation(batch_size_per_thread));
            edge_update_buffer[tid].reset(new runs_creator_buffer_t(*edge_update_runs_creator_thread, merger_buffer_size));
        }

        swapid_t loop_limit = _num_swaps_in_run;
        {
            swapid_t remainder = _num_swaps_in_run % _num_threads;
            if (remainder != 0) loop_limit += (_num_threads - remainder);
        }

        for (swapid_t sid_in_batch_base = 0, batch_num = 0; sid_in_batch_base < loop_limit; sid_in_batch_base += batch_size_per_thread * _num_threads, ++batch_num) { // execution of batch starts
            swapid_t sid_in_batch_limit = std::min<swapid_t>(_num_swaps_in_run, sid_in_batch_base + batch_size_per_thread * _num_threads);

            #pragma omp parallel num_threads(_num_threads)
            {
                const auto tid = omp_get_thread_num();

                EdgeExistenceInformation &my_existence_information = *existence_information[tid];
                my_existence_information.start_initialization();

                auto &my_existence_placeholder = *existence_placeholder[tid];

                for (swapid_t s = sid_in_batch_base + tid, i = 0; i < batch_size_per_thread && s < _num_swaps_in_run; ++i, s += _num_threads) {
                    size_t c = 0;
                    while (!my_existence_placeholder.empty() && *my_existence_placeholder == s) {
                            ++c;
                        ++my_existence_placeholder;
                    }

                    my_existence_information.add_possible_info(i, c);
                }

                my_existence_information.finish_initialization();
            }

            {
                _edge_state.start_batch(DependencyChainEdgeMsg {pack_swap_id_spos(sid_in_batch_limit, 0), edge_t{-1, -1}});
                _existence_info.start_batch(ExistenceInfoMsg {sid_in_batch_limit, edge_t{-1, -1}});

                for (swapid_t swap_id = sid_in_batch_base, pos = 0; swap_id < sid_in_batch_limit; ++pos) {
                    for (int tid = 0; tid < _num_threads; ++tid, ++swap_id) {
                        std::array<edge_t, 2>& current_edges = (*source_edges[tid])[pos];
                        while (!_existence_info.empty() && _existence_info->swap_id == swap_id) {
                            if (_existence_info->edge == edge_t::invalid()) {
                                existence_information[tid]->push_missing(pos);
                            } else {
                                existence_information[tid]->push_exists(pos, _existence_info->edge);
                            }
                            ++_existence_info;
                        }

                        for (unsigned char spos = 0; spos < 2; ++spos) {
                            assert(_edge_state.empty() || get_swap_id(_edge_state->sid) > swap_id || (get_swap_id(_edge_state->sid) == swap_id && get_swap_spos(_edge_state->sid) >= spos));
                            if (!_edge_state.empty() && _edge_state->sid == pack_swap_id_spos(swap_id, spos)) {
                                current_edges[spos] = _edge_state->edge;
                                ++_edge_state;
                            }
                        }
                    }
                }

                assert(_existence_info.empty());
                assert(_edge_state.empty());

                _edge_state.start_push();
                _existence_info.start_push();
            }

            #pragma omp parallel num_threads(_num_threads)
            {
                const auto tid = omp_get_thread_num();

                auto &my_source_edges = *source_edges[tid];

                auto &my_edge_update_buffer = *edge_update_buffer[tid];

                EdgeExistenceInformation &my_existence_information = *existence_information[tid];

                auto &my_edge_dependencies = *edge_dependencies[tid];

                auto &my_existence_sucessors = *existence_successor[tid];


                auto & my_swap_direction = *_swap_direction[tid];

                swapid_t sid = sid_in_batch_base + tid;

                for (swapid_t i = 0; i < batch_size_per_thread && sid < loop_limit; ++i, sid += _num_threads) {
                    if (UNLIKELY(sid >= _num_swaps_in_run)) continue;
                    auto & cur_edges = my_source_edges[i];

                    std::array<edge_t, 2> new_edges;

                    assert(!my_swap_direction.empty());

                    bool direction = *my_swap_direction;
                    ++my_swap_direction;


                    for (unsigned int spos = 0; spos < 2; spos++) {
                        // get edge successors
                        // possibly wait for another thread to set the information
                        while (cur_edges[spos].first == INVALID_NODE || cur_edges[spos].second == INVALID_NODE) { // busy waiting for other thread to supply information
                            std::this_thread::yield();
                            // this adds an empty assembler instruction that acts as memory fence and tells the compiler that all memory contents may be changed, i.e. forces it to re-load cur_edges
                            __asm__ __volatile__ ("":::"memory");
                        }
                    }

                    // compute swapped edges
                    std::tie(new_edges[0], new_edges[1]) = _swap_edges(cur_edges[0], cur_edges[1], direction);

                    #ifndef NDEBUG
                        if (_display_debug) {
                            std::cout << "State in " << sid << ": ";
                            std::cout << cur_edges[0] << ", " << cur_edges[1] << " ";
                            std::cout << new_edges[0] << ", " << new_edges[1] <<  " ";
                            std::cout << std::endl;
                        }
                    #endif

                    // gather all edge states that have been sent to this swap
                    my_existence_information.wait_for_missing(i);

                    // check if there's a conflicting edge
                    bool conflict_exists[2];
                    for (unsigned int spos = 0; spos < 2; spos++) {
                        conflict_exists[spos] = my_existence_information.exists(i, new_edges[spos]);
                    }

                    // can we perform the swap?
                    const bool loop = new_edges[0].is_loop() || new_edges[1].is_loop();
                    const bool perform_swap = !(conflict_exists[0] || conflict_exists[1] || loop);


#ifdef EDGE_SWAP_DEBUG_VECTOR
                    // write out debug message
                    {
                        SwapResult res;
                        res.performed = perform_swap;
                        res.loop = loop;
                        for(unsigned int spos=0; spos < 2; spos++) {
                            res.edges[spos] = new_edges[spos];
                            res.conflictDetected[spos] = conflict_exists[spos];
                        }
                        res.normalize();

                        debug_output_buffer[tid].push_back(res);
                        DEBUG_MSG(_display_debug, "Swap " << sid << " " << res);
                    }
#endif

                    if (!perform_swap) {
                        new_edges[0] = cur_edges[0];
                        new_edges[1] = cur_edges[1];
                    }

                    // forward edge state to successor swap
                    std::array<bool, 2> successor_found = {false, false};
                    while (!my_edge_dependencies.empty() && get_swap_id(my_edge_dependencies->sid) == sid) {
                        auto &msg = *my_edge_dependencies;
                        DEBUG_MSG(_display_debug, "Got successor for S" << sid << ", E" << get_swap_spos(msg.sid) << ": " << msg);

                        successor_found[get_swap_spos(msg.sid)] = true;

                        swapid_t successor_swap_id = get_swap_id(msg.successor);

                        if (successor_swap_id < sid_in_batch_limit) {
                            auto successor_tid = _thread(successor_swap_id);
                            auto pos = (successor_swap_id - sid_in_batch_base)/_num_threads;
                            (*source_edges[successor_tid])[pos][get_swap_spos(msg.successor)] = new_edges[get_swap_spos(msg.sid)];
                        } else {
                            _edge_state.push_pq(tid, DependencyChainEdgeMsg {msg.successor, new_edges[get_swap_spos(msg.sid)]});
                        }

                        ++my_edge_dependencies;
                    }

                    // send current state of edge iff there are no successors to this edge
                    for(unsigned int spos=0; spos <2; spos++) {
                        if (!successor_found[spos]) {
                            my_edge_update_buffer.push(new_edges[spos]);
                        }
                    }

                    auto push_existence_info = [&](swapid_t target_sid, edge_t e, bool exists) {
                        // if the edge does not exist send invalid edge so it won't find it (but still gets enough messages)
                        if (!exists) e = edge_t::invalid();

                        if (target_sid < sid_in_batch_limit) {
                            auto successor_tid = _thread(target_sid);
                            auto pos = (target_sid - sid_in_batch_base)/_num_threads;
                            if (exists) {
                                existence_information[successor_tid]->push_exists(pos, e);
                            } else {
                                existence_information[successor_tid]->push_missing(pos);
                            }
                        } else {
                            _existence_info.push_pq(tid, ExistenceInfoMsg{target_sid, e});
                        }
                    };

                    // forward existence information
                    for (; !my_existence_sucessors.empty(); ++my_existence_sucessors) {
                        auto &succ = *my_existence_sucessors;

                        assert(succ.swap_id >= sid);
                        if (succ.swap_id > sid) break;

                        if (succ.edge == new_edges[0] || succ.edge == new_edges[1]) {
                            // target edges always exist (or source if no swap has been performed)
                            push_existence_info(succ.successor, succ.edge, true);
                            DEBUG_MSG(_display_debug, "Send " << succ.edge << " exists: " << true << " to " << succ.successor);
                        } else if (succ.edge == cur_edges[0] || succ.edge == cur_edges[1]) {
                            // source edges never exist (if no swap has been performed, this has been handled above)
                            push_existence_info(succ.successor, succ.edge, false);
                            DEBUG_MSG(_display_debug, "Send " << succ.edge << " exists: " << false << " to " << succ.successor);
                        } else {
                            bool exists = my_existence_information.exists(i, succ.edge);
                            push_existence_info(succ.successor, succ.edge, exists);
                            DEBUG_MSG(_display_debug, "Send " << succ.edge << " exists: " << exists << " to " << succ.successor);
                        }
                    }

                    cur_edges[0] = edge_t::invalid();
                    cur_edges[1] = edge_t::invalid();
                }
                // finished batch

                if (batch_num % num_batches_till_sorter_run == 0 ||  sid_in_batch_limit == _num_swaps_in_run) {
                        my_edge_update_buffer.finish();
                }

#ifdef EDGE_SWAP_DEBUG_VECTOR
                #pragma omp barrier

                #pragma omp single
                {
                    for (swapid_t i = 0, s = sid_in_batch_base; i < batch_size_per_thread && s < _num_swaps_in_run; ++i) {
                        for (int t = 0; t < _num_threads && s < _num_swaps_in_run; ++t, ++s) {
                            _debug_vector_writer << debug_output_buffer[t][i];
                        }
                    }
                }

                debug_output_buffer[tid].clear();
#endif
            } // end of parallel region

            {
                _edge_state.end_batch();
                _existence_info.end_batch();
            }
        }// end of loop over all batches

        for (int tid = 0; tid < _num_threads; ++tid) {
            edge_update_buffer[tid]->flush();
            edge_update_buffer[tid].reset(nullptr);

            // check message data structures are empty
            assert(edge_dependencies[tid]->empty());
            assert(existence_successor[tid]->empty());
            assert(_swap_direction[tid]->empty());
        }

        edge_update_runs_creator_thread.reset(nullptr);

        #pragma omp flush

        _edge_update_merger.initialize(edge_update_runs_creator.result());
    }
};
