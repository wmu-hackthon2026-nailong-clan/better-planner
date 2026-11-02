#include "core/layout.hpp"

#include <algorithm>

namespace bp {

namespace {

struct Cluster {
    std::vector<std::size_t> indices;
    std::int64_t start = 0;
    std::int64_t end = 0;
};

bool spansOverlap(const Cluster& a, const Cluster& b) {
    return a.start < b.end && b.start < a.end;
}

}  // namespace

std::vector<LayoutEngine::Block> LayoutEngine::layout(
    const std::vector<Event>& dayEvents) const {
    std::vector<Cluster> clusters;
    for (std::size_t i = 0; i < dayEvents.size(); ++i) {
        const Event& event = dayEvents[i];
        bool merged = false;
        for (Cluster& cluster : clusters) {
            if (cluster.start == event.start && cluster.end == event.end) {
                cluster.indices.push_back(i);
                merged = true;
                break;
            }
        }
        if (!merged) {
            clusters.push_back(Cluster{{i}, event.start, event.end});
        }
    }

    const std::size_t n = clusters.size();
    std::vector<std::vector<std::size_t>> adjacency(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = i + 1; j < n; ++j) {
            if (spansOverlap(clusters[i], clusters[j])) {
                adjacency[i].push_back(j);
                adjacency[j].push_back(i);
            }
        }
    }

    std::vector<std::size_t> order(n);
    for (std::size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (clusters[a].start != clusters[b].start) {
            return clusters[a].start < clusters[b].start;
        }
        if (clusters[a].end != clusters[b].end) {
            return clusters[a].end > clusters[b].end;
        }
        return a < b;
    });

    std::vector<int> lane(n, -1);
    for (std::size_t idx : order) {
        int assigned = 0;
        for (;;) {
            bool taken = false;
            for (std::size_t other : adjacency[idx]) {
                if (lane[other] == assigned &&
                    spansOverlap(clusters[idx], clusters[other])) {
                    taken = true;
                    break;
                }
            }
            if (!taken) {
                break;
            }
            ++assigned;
        }
        lane[idx] = assigned;
    }

    std::vector<Block> blocks;
    blocks.reserve(n);
    for (std::size_t idx : order) {
        Block block;
        block.eventIndices = clusters[idx].indices;
        block.lane = lane[idx];
        block.summary = !adjacency[idx].empty();
        blocks.push_back(std::move(block));
    }
    return blocks;
}

}  // namespace bp
