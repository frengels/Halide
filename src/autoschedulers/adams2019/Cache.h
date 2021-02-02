#ifndef BLOCK_CACHE_H
#define BLOCK_CACHE_H

#include "ASLog.h"
#include "CostModel.h"
#include "Featurization.h"
#include "FunctionDAG.h"
#include "Halide.h"
#include "LoopNest.h"
#include "PerfectHashMap.h"

namespace Halide {
namespace Internal {
namespace Autoscheduler {

struct State;

// true iff HL_USE_MEMOIZED_FEATURES=1
bool use_memoized_features();

// true iff HL_VERIFY_MEMOIZED_FEATURES=1
bool verify_memoized_features();

// true iff HL_MEMOIZE_BLOCKS=1
bool is_memoize_blocks_enabled();

/*
Object stores caching options for autoscheduling.
*/
struct CachingOptions {
    bool cache_blocks = false;
    bool cache_features = false;
    bool verify_feature_caching = false;

    static CachingOptions MakeOptionsFromEnviron() {
        CachingOptions options;
        options.cache_blocks = is_memoize_blocks_enabled();
        options.cache_features = use_memoized_features();
        options.verify_feature_caching = verify_memoized_features();
        return options;
    }
};

// Node -> (vector_dim -> vector<tilings>)
using BlockCache = NodeMap<std::map<int, std::vector<IntrusivePtr<const LoopNest>>>>;

// Cache for memoizing possible tilings.
// Tracks hit/miss statistics for both block caching
// and for feature caching (self-contained by LoopNests).
struct Cache {
    CachingOptions options;
    BlockCache memoized_compute_root_blocks;

    mutable size_t cache_hits = 0;
    mutable size_t cache_misses = 0;

    Cache() = delete;
    Cache(const CachingOptions &_options, size_t nodes_size)
        : options(_options) {
        if (options.cache_blocks) {
            memoized_compute_root_blocks.make_large(nodes_size);
        }
    }

    ~Cache() = default;

    // check if we generated tilings for the current func on a previous pass
    // if so, add them and return true.
    // otherwise, return false (also return false if memoization is turned off).
    bool add_memoized_blocks(const State *state,
                             std::function<void(IntrusivePtr<State> &&)> &accept_child,
                             const FunctionDAG::Node *node,
                             int &num_children,
                             const FunctionDAG &dag,
                             const MachineParams &params,
                             CostModel *cost_model,
                             int64_t memory_limit) const;

    // Generate tilings for a specific vector dimension and memoize them.
    void memoize_blocks(const FunctionDAG::Node *node, LoopNest *new_root);
};

}  // namespace Autoscheduler
}  // namespace Internal
}  // namespace Halide

#endif  // BLOCK_CACHE_H
