#pragma once
#include <algorithm>
#include <cstddef>
#include <thread>
#include <vector>

namespace facet {

// Splits [0, n) across hardware threads. Falls back to a serial loop for small n.
template <class F>
void parallelFor(size_t n, F&& fn, size_t grain = 8192)
{
    unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) hw = 4;
    if (n < grain * 2 || hw < 2) {
        for (size_t i = 0; i < n; ++i) fn(i);
        return;
    }
    size_t chunks = std::min<size_t>(hw, (n + grain - 1) / grain);
    size_t per = (n + chunks - 1) / chunks;
    std::vector<std::thread> threads;
    threads.reserve(chunks);
    for (size_t c = 0; c < chunks; ++c) {
        threads.emplace_back([&, c] {
            size_t b = c * per, e = std::min(n, b + per);
            for (size_t i = b; i < e; ++i) fn(i);
        });
    }
    for (auto& t : threads) t.join();
}

} // namespace facet
