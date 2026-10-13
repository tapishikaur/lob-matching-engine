#include "matching_engine.hpp"
#include <stdexcept>
#include <iostream>

MatchingEngine::MatchingEngine(ExecCallback on_exec, BBOCallback on_bbo)
    : on_exec_(std::move(on_exec))
    , on_bbo_ (std::move(on_bbo))
{}

MatchingEngine::~MatchingEngine() {
    stop();
}

void MatchingEngine::register_symbol(SymbolId id) {
    if (running_.load()) throw std::runtime_error("Cannot register while running");
    // Construct in-place; avoids copy (OrderBook is not copyable)
    books_.emplace(std::piecewise_construct,
                   std::forward_as_tuple(id),
                   std::forward_as_tuple(id, on_exec_, on_bbo_));
}

// ── Producer API ─────────────────────────────────────────────────────────────
bool MatchingEngine::submit_new(const NewOrderMsg& msg) {
    return queue_.push(InboundMsg{ msg });
}
bool MatchingEngine::submit_cancel(const CancelOrderMsg& msg) {
    return queue_.push(InboundMsg{ msg });
}
bool MatchingEngine::submit_modify(const ModifyOrderMsg& msg) {
    return queue_.push(InboundMsg{ msg });
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
void MatchingEngine::start() {
    running_.store(true, std::memory_order_release);
    matching_thread_ = std::thread(&MatchingEngine::matching_loop, this);
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (matching_thread_.joinable())
        matching_thread_.join();
}

// ── Matching loop (dedicated thread) ─────────────────────────────────────────
void MatchingEngine::matching_loop() {
    // Optional: pin to a core for deterministic scheduling
    // cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(1, &cpuset);
    // pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);

    while (running_.load(std::memory_order_acquire)) {
        auto msg_opt = queue_.pop();
        if (!msg_opt) {
            // Spin-wait (busy loop) – appropriate for ultra-low latency
            // In a power-constrained environment replace with:
            //   std::this_thread::yield();
            continue;
        }

        std::visit([&](auto& msg) {
            auto it = books_.find(msg.symbol_id);
            if (it == books_.end()) {
                std::cerr << "[engine] unknown symbol " << msg.symbol_id << "\n";
                return;
            }
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, NewOrderMsg>)
                it->second.add_order(msg);
            else if constexpr (std::is_same_v<T, CancelOrderMsg>)
                it->second.cancel_order(msg);
            else if constexpr (std::is_same_v<T, ModifyOrderMsg>)
                it->second.modify_order(msg);
        }, *msg_opt);

        orders_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    // Drain remaining messages after stop() signal
    while (auto msg_opt = queue_.pop()) {
        std::visit([&](auto& msg) {
            auto it = books_.find(msg.symbol_id);
            if (it == books_.end()) return;
            using T = std::decay_t<decltype(msg)>;
            if constexpr (std::is_same_v<T, NewOrderMsg>)
                it->second.add_order(msg);
            else if constexpr (std::is_same_v<T, CancelOrderMsg>)
                it->second.cancel_order(msg);
            else if constexpr (std::is_same_v<T, ModifyOrderMsg>)
                it->second.modify_order(msg);
        }, *msg_opt);
        orders_processed_.fetch_add(1, std::memory_order_relaxed);
    }
}
