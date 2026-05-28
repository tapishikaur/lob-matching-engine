#pragma once
#include <atomic>
#include <array>
#include <optional>
#include <cstddef>

// Single-Producer / Single-Consumer lock-free ring buffer.
// N must be a power of two.
template <typename T, std::size_t N>
class SPSCQueue {
    static_assert((N & (N - 1)) == 0, "N must be a power of two");
    static constexpr std::size_t MASK       = N - 1;
    static constexpr std::size_t CACHE_LINE = 64;

public:
    bool push(const T& item) {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }
    bool push(T&& item) {
        const std::size_t h    = head_.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) & MASK;
        if (next == tail_.load(std::memory_order_acquire)) return false;
        buffer_[h] = std::move(item);
        head_.store(next, std::memory_order_release);
        return true;
    }
    std::optional<T> pop() {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) return std::nullopt;
        T item = buffer_[t];
        tail_.store((t + 1) & MASK, std::memory_order_release);
        return item;
    }
    bool        empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire); }
    std::size_t size()  const { return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)) & MASK; }

private:
    alignas(CACHE_LINE) std::atomic<std::size_t> head_{ 0 };
    alignas(CACHE_LINE) std::atomic<std::size_t> tail_{ 0 };
    std::array<T, N> buffer_{};
};
