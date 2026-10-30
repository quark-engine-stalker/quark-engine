#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
volatile std::uint64_t benchmark_sink = 0;

struct shared_manager
{
    explicit shared_manager(const std::size_t target_count) : targets(target_count)
    {
        std::iota(targets.begin(), targets.end(), std::uint16_t{1});
    }

    void remove_links(const std::uint16_t object_id)
    {
        for (std::uint16_t& target : targets)
        {
            if (target == object_id)
                target = 0;
        }
    }

    std::vector<std::uint16_t> targets;
};

std::uint64_t old_broadcast(const std::size_t broadcaster_count, const std::size_t destroyed_count)
{
    shared_manager manager(4096);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t broadcaster = 0; broadcaster < broadcaster_count; ++broadcaster)
    {
        for (std::uint16_t id = 1; id <= destroyed_count; ++id)
            manager.remove_links(id);
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    benchmark_sink += std::accumulate(manager.targets.begin(), manager.targets.end(), std::uint64_t{0});
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::uint64_t deduplicated_broadcast(const std::size_t broadcaster_count, const std::size_t destroyed_count)
{
    shared_manager manager(4096);
    std::vector<std::uint16_t> processed;
    processed.reserve(destroyed_count);
    const auto started = std::chrono::steady_clock::now();
    for (std::size_t broadcaster = 0; broadcaster < broadcaster_count; ++broadcaster)
    {
        for (std::uint16_t id = 1; id <= destroyed_count; ++id)
        {
            const auto found = std::lower_bound(processed.begin(), processed.end(), id);
            if (found != processed.end() && *found == id)
                continue;
            processed.insert(found, id);
            manager.remove_links(id);
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    benchmark_sink += std::accumulate(manager.targets.begin(), manager.targets.end(), std::uint64_t{0});
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::uint64_t median(std::vector<std::uint64_t>& samples)
{
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}
}

int main()
{
    // First occurrence order remains intact; duplicate broadcasters no longer
    // repeat the same shared-manager mutation.
    const std::vector<std::uint16_t> input{4, 2, 4, 1, 2, 3};
    std::vector<std::uint16_t> processed;
    std::vector<std::uint16_t> committed;
    for (const std::uint16_t id : input)
    {
        const auto found = std::lower_bound(processed.begin(), processed.end(), id);
        if (found != processed.end() && *found == id)
            continue;
        processed.insert(found, id);
        committed.push_back(id);
    }
    const std::vector<std::uint16_t> expected{4, 2, 1, 3};
    if (committed != expected)
    {
        std::puts("semantic_first_commit_order=FAIL");
        return 1;
    }

    // A new frame starts a new relcase batch. Every object must be committed
    // once again rather than remaining suppressed by the previous frame.
    processed.clear();
    committed.clear();
    for (const std::uint16_t id : input)
    {
        const auto found = std::lower_bound(processed.begin(), processed.end(), id);
        if (found != processed.end() && *found == id)
            continue;
        processed.insert(found, id);
        committed.push_back(id);
    }
    if (committed != expected)
    {
        std::puts("semantic_frame_reset=FAIL");
        return 1;
    }

    constexpr std::size_t broadcaster_count = 64;
    constexpr std::size_t destroyed_count = 64;
    constexpr std::size_t sample_count = 7;
    std::vector<std::uint64_t> old_samples;
    std::vector<std::uint64_t> deduplicated_samples;
    old_samples.reserve(sample_count);
    deduplicated_samples.reserve(sample_count);

    old_broadcast(4, 8);
    deduplicated_broadcast(4, 8);
    for (std::size_t sample = 0; sample < sample_count; ++sample)
    {
        old_samples.push_back(old_broadcast(broadcaster_count, destroyed_count));
        deduplicated_samples.push_back(deduplicated_broadcast(broadcaster_count, destroyed_count));
    }

    const std::uint64_t old_ns = median(old_samples);
    const std::uint64_t deduplicated_ns = median(deduplicated_samples);
    std::printf("semantic_first_commit_order=PASS semantic_frame_reset=PASS broadcasters=%zu destroyed=%zu old_median_ms=%.3f deduplicated_median_ms=%.3f speedup=%.2fx sink=%llu\n",
        broadcaster_count, destroyed_count,
        static_cast<double>(old_ns) / 1000000.0,
        static_cast<double>(deduplicated_ns) / 1000000.0,
        static_cast<double>(old_ns) / static_cast<double>(deduplicated_ns),
        static_cast<unsigned long long>(benchmark_sink));
    return 0;
}
