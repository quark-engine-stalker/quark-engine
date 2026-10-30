#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

namespace
{
volatile std::uint64_t benchmark_sink = 0;

std::uint64_t old_erase_pass(const std::size_t count)
{
    std::vector<std::uint32_t> calls(count);
    std::iota(calls.begin(), calls.end(), 1u);

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < calls.size();)
    {
        if ((calls[index] & 1u) == 0)
            calls.erase(calls.begin() + index);
        else
            ++index;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    benchmark_sink += calls.size() + calls.front() + calls.back();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

std::uint64_t compact_once_pass(const std::size_t count)
{
    std::vector<std::uint32_t> calls(count);
    std::iota(calls.begin(), calls.end(), 1u);

    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t& call : calls)
    {
        if ((call & 1u) == 0)
            call = 0;
    }
    calls.erase(std::remove(calls.begin(), calls.end(), 0u), calls.end());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    benchmark_sink += calls.size() + calls.front() + calls.back();
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
    // Model the production loop's important semantics: survivor order is stable,
    // a re-entrant removal is a tombstone, and a newly appended call is checked
    // in the same pass because the loop observes the dynamic vector size.
    std::vector<std::uint32_t> semantic{1, 2, 3, 4, 5, 6};
    std::vector<std::uint32_t> checked;
    for (std::size_t index = 0; index < semantic.size(); ++index)
    {
        const std::uint32_t call = semantic[index];
        if (!call)
            continue;
        checked.push_back(call);
        if (call == 1)
        {
            semantic[4] = 0; // callback removes call 5 before its turn
            semantic.push_back(7); // callback appends a call for this same pass
        }
        if ((call & 1u) == 0)
            semantic[index] = 0;
    }
    semantic.erase(std::remove(semantic.begin(), semantic.end(), 0u), semantic.end());
    const std::vector<std::uint32_t> expected_survivors{1, 3, 7};
    const std::vector<std::uint32_t> expected_checked{1, 2, 3, 4, 6, 7};
    if (semantic != expected_survivors || checked != expected_checked)
    {
        std::puts("semantic_order=FAIL");
        return 1;
    }

    constexpr std::size_t call_count = 100000;
    constexpr std::size_t sample_count = 7;
    std::vector<std::uint64_t> old_samples;
    std::vector<std::uint64_t> compact_samples;
    old_samples.reserve(sample_count);
    compact_samples.reserve(sample_count);

    // Warm both paths before collecting medians.
    old_erase_pass(4096);
    compact_once_pass(4096);
    for (std::size_t sample = 0; sample < sample_count; ++sample)
    {
        old_samples.push_back(old_erase_pass(call_count));
        compact_samples.push_back(compact_once_pass(call_count));
    }

    const std::uint64_t old_ns = median(old_samples);
    const std::uint64_t compact_ns = median(compact_samples);
    std::printf("semantic_order=PASS calls=%zu removed=%zu old_median_ms=%.3f compact_median_ms=%.3f speedup=%.2fx sink=%llu\n",
        call_count, call_count / 2,
        static_cast<double>(old_ns) / 1000000.0,
        static_cast<double>(compact_ns) / 1000000.0,
        static_cast<double>(old_ns) / static_cast<double>(compact_ns),
        static_cast<unsigned long long>(benchmark_sink));
    return 0;
}
