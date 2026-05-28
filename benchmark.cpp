#include "matching_engine.hpp"
#include "latency_histogram.hpp"
#include <chrono>
#include <iostream>
#include <random>
#include <vector>
#include <atomic>
#include <thread>

// ─────────────────────────────────────────────────────────────────────────────
//  Benchmark configuration
// ─────────────────────────────────────────────────────────────────────────────
static constexpr SymbolId SYMBOL      = 1;
static constexpr int      NUM_ORDERS  = 200'000;
static constexpr int      PRICE_BASE  = 1000000;   // $100.00 in fixed-point
static constexpr int      PRICE_RANGE = 40;       // ±$0.02 spread

// ─────────────────────────────────────────────────────────────────────────────
//  Pre-generate orders so load-gen doesn't pollute latency measurement
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<NewOrderMsg> build_order_stream() {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int>  price_dist(-PRICE_RANGE, PRICE_RANGE);
    std::uniform_int_distribution<int>  qty_dist(1, 100);
    std::uniform_int_distribution<int>  side_dist(0, 1);
    std::uniform_int_distribution<int>  type_dist(0, 9);

    std::vector<NewOrderMsg> orders;
    orders.reserve(NUM_ORDERS);

    for (int i = 0; i < NUM_ORDERS; ++i) {
        NewOrderMsg o;
        o.order_id   = static_cast<OrderId>(i + 1);
        o.symbol_id  = SYMBOL;
        o.side       = side_dist(rng) ? Side::BUY : Side::SELL;
        o.price      = PRICE_BASE + price_dist(rng);
        o.qty        = static_cast<Quantity>(qty_dist(rng));
        // 80% GTC, 15% IOC, 5% FOK
        if      (type_dist(rng) < 2)  o.order_type = OrderType::FOK;
        else if (type_dist(rng) < 4)  o.order_type = OrderType::IOC;
        else                           o.order_type = OrderType::GTC;
        orders.push_back(o);
    }
    return orders;
}

// ─────────────────────────────────────────────────────────────────────────────
int main() {
    using Clock = std::chrono::steady_clock;

    std::cout << "=== LOB Matching Engine Benchmark ===\n";
    std::cout << "Orders to process: " << NUM_ORDERS << "\n\n";

    // ── Latency tracking ──────────────────────────────────────────────────
    // We record per-order latency: submission ts → execution report ts
    LatencyHistogram hist;

    // Atomic to count reports received by callback
    std::atomic<uint64_t> reports_rcvd{ 0 };

    // ── Callbacks ────────────────────────────────────────────────────────
    auto on_exec = [&](const ExecutionReport& r) {
        if (r.status == OrderStatus::NEW) return;   // skip ack
        if (r.exec_ts > r.order_ts)
            hist.record(r.exec_ts - r.order_ts);
        reports_rcvd.fetch_add(1, std::memory_order_relaxed);
    };
    auto on_bbo = [&](const BBOUpdate&) { /* discard in benchmark */ };

    // ── Engine setup ──────────────────────────────────────────────────────
    MatchingEngine engine(on_exec, on_bbo);
    engine.register_symbol(SYMBOL);
    engine.start();

    // ── Pre-build order stream ────────────────────────────────────────────
    auto orders = build_order_stream();

    // Warmup: prime the CPU caches with a small batch
    constexpr int WARMUP = 5000;
    for (int i = 0; i < WARMUP; ++i) {
        auto o = orders[i];
        o.ts = static_cast<Timestamp>(Clock::now().time_since_epoch().count());
        while (!engine.submit_new(o)) { /* spin on back-pressure */ }
    }
    // Let warmup drain
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    hist.reset();
    reports_rcvd.store(0);

    // ── Timed run ─────────────────────────────────────────────────────────
    const int BENCH_ORDERS = NUM_ORDERS - WARMUP;
    const auto t0 = Clock::now();

    for (int i = WARMUP; i < NUM_ORDERS; ++i) {
        auto& o = orders[i];
        o.ts = static_cast<Timestamp>(Clock::now().time_since_epoch().count());
        while (!engine.submit_new(o)) { /* spin */ }
    }

    // Wait for engine to process everything
    while (engine.orders_processed() < static_cast<uint64_t>(NUM_ORDERS))
        std::this_thread::sleep_for(std::chrono::microseconds(100));

    const auto t1 = Clock::now();
    engine.stop();

    // ── Results ───────────────────────────────────────────────────────────
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    double throughput = BENCH_ORDERS / elapsed_s;

    std::cout << "Elapsed : " << std::fixed << std::setprecision(3)
              << elapsed_s * 1000 << " ms\n";
    std::cout << "Throughput : "
              << static_cast<uint64_t>(throughput) << " orders/sec\n";

    hist.print("Round-trip order latency (submit → first exec report)");

    return 0;
}
