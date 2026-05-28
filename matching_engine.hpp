#pragma once
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include "types.hpp"
#include <unordered_map>
#include <variant>
#include <thread>
#include <atomic>
#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
//  InboundMsg  –  variant over all inbound message types
//  Stored in the SPSC ring buffer between gateway and matching threads
// ─────────────────────────────────────────────────────────────────────────────
using InboundMsg = std::variant<NewOrderMsg, CancelOrderMsg, ModifyOrderMsg>;

// ─────────────────────────────────────────────────────────────────────────────
//  MatchingEngine
//
//  • Owns one OrderBook per symbol
//  • One dedicated matching thread spins on the inbound SPSC queue
//  • Producer (gateway) thread submits via submit_*() – lock-free
// ─────────────────────────────────────────────────────────────────────────────
class MatchingEngine {
public:
    static constexpr std::size_t QUEUE_SIZE = 1 << 14;  // 16 384 slots

    explicit MatchingEngine(ExecCallback on_exec,
                            BBOCallback  on_bbo);

    ~MatchingEngine();

    // Call before starting the matching thread to register symbols
    void register_symbol(SymbolId id);

    // ── Gateway / producer API (any thread) ──────────────────────────────
    // Returns false only if ring buffer is momentarily full (back-pressure)
    bool submit_new   (const NewOrderMsg&    msg);
    bool submit_cancel(const CancelOrderMsg& msg);
    bool submit_modify(const ModifyOrderMsg& msg);

    // ── Lifecycle ────────────────────────────────────────────────────────
    void start();   // spawns matching thread
    void stop();    // signals thread to drain and exit

    // Statistics (approximate, read from any thread)
    uint64_t orders_processed() const {
        return orders_processed_.load(std::memory_order_relaxed);
    }

private:
    void matching_loop();

    SPSCQueue<InboundMsg, QUEUE_SIZE> queue_;
    std::unordered_map<SymbolId, OrderBook> books_;

    ExecCallback on_exec_;
    BBOCallback  on_bbo_;

    std::thread          matching_thread_;
    std::atomic<bool>    running_{ false };
    std::atomic<uint64_t> orders_processed_{ 0 };
};
