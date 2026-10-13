#include "matching_engine.hpp"
#include "latency_histogram.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <random>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Two separate measurements, because one run can't give both honestly:
//
//  1. Throughput: the producer pushes orders as fast as it can, so the queue
//     stays full and the matching thread never waits. Orders per second is
//     meaningful here; per-order latency is not (it is mostly time spent waiting
//     behind thousands of queued orders).
//  2. Latency: one order in flight at a time. The producer stamps the order,
//     submits it, and waits until the matching thread has finished it before
//     sending the next. Each sample is submit → that order's first execution
//     report, so it covers the queue hand-off between threads plus matching.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr SymbolId SYMBOL          = 1;
static constexpr int      THROUGHPUT_ORDERS = 200'000;
static constexpr int      LATENCY_ORDERS    = 100'000;
static constexpr int      WARMUP            = 5'000;
static constexpr int      PRICE_BASE  = 1000000;   // $100.00 in fixed-point
static constexpr int      PRICE_RANGE = 40;        // ±$0.0040 around it (40 ticks of 0.0001)

// Pre-generate orders so load generation isn't part of what is measured.
static std::vector<NewOrderMsg> build_order_stream(int count) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int>  price_dist(-PRICE_RANGE, PRICE_RANGE);
    std::uniform_int_distribution<int>  qty_dist(1, 100);
    std::uniform_int_distribution<int>  side_dist(0, 1);
    std::uniform_int_distribution<int>  type_dist(0, 99);

    std::vector<NewOrderMsg> orders;
    orders.reserve(count);

    for (int i = 0; i < count; ++i) {
        NewOrderMsg o;
        o.order_id   = static_cast<OrderId>(i + 1);
        o.symbol_id  = SYMBOL;
        o.side       = side_dist(rng) ? Side::BUY : Side::SELL;
        o.price      = PRICE_BASE + price_dist(rng);
        o.qty        = static_cast<Quantity>(qty_dist(rng));
        // 80% GTC, 15% IOC, 5% FOK: one draw per order, so the split is exact
        const int t = type_dist(rng);
        if      (t < 5)   o.order_type = OrderType::FOK;
        else if (t < 20)  o.order_type = OrderType::IOC;
        else              o.order_type = OrderType::GTC;
        orders.push_back(o);
    }
    return orders;
}

static Timestamp stamp() {
    return static_cast<Timestamp>(std::chrono::steady_clock::now().time_since_epoch().count());
}

static void wait_until_processed(const MatchingEngine& engine, uint64_t n) {
    // yield, not sleep: a sleep can last a whole scheduler tick (up to ~15 ms on Windows)
    while (engine.orders_processed() < n) std::this_thread::yield();
}

// ── 1. Throughput ────────────────────────────────────────────────────────────
static void run_throughput() {
    std::atomic<uint64_t> reports{0};
    MatchingEngine engine([&](const ExecutionReport&) { reports.fetch_add(1, std::memory_order_relaxed); },
                          [](const BBOUpdate&) {});
    engine.register_symbol(SYMBOL);
    engine.start();

    auto orders = build_order_stream(THROUGHPUT_ORDERS);
    for (int i = 0; i < WARMUP; ++i) {
        while (!engine.submit_new(orders[i])) {}
    }
    wait_until_processed(engine, WARMUP);

    const auto t0 = std::chrono::steady_clock::now();
    for (int i = WARMUP; i < THROUGHPUT_ORDERS; ++i) {
        while (!engine.submit_new(orders[i])) {}   // queue full: spin until there's room
    }
    wait_until_processed(engine, THROUGHPUT_ORDERS);
    const auto t1 = std::chrono::steady_clock::now();
    engine.stop();

    const double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    const int timed = THROUGHPUT_ORDERS - WARMUP;
    std::cout << "\n── 1. Throughput: " << timed << " orders, queue kept full ──\n"
              << "  elapsed    : " << std::fixed << std::setprecision(1) << elapsed_s * 1000 << " ms\n"
              << "  throughput : " << static_cast<uint64_t>(timed / elapsed_s) << " orders/sec\n"
              << "  reports    : " << reports.load() << "\n";
}

// ── 2. Latency ───────────────────────────────────────────────────────────────
static void run_latency() {
    LatencyHistogram hist;                                        // used by the matching thread only
    std::vector<uint8_t> first_report_seen(LATENCY_ORDERS + 1, 0);
    std::atomic<bool> recording{false};

    MatchingEngine engine(
        [&](const ExecutionReport& r) {
            // Later reports for an order (fills while it rests) carry its original
            // submit time, so only its first report is a latency sample.
            if (first_report_seen[r.order_id]) return;
            first_report_seen[r.order_id] = 1;
            if (recording.load(std::memory_order_relaxed) && r.exec_ts >= r.order_ts)
                hist.record(r.exec_ts - r.order_ts);
        },
        [](const BBOUpdate&) {});
    engine.register_symbol(SYMBOL);
    engine.start();

    auto orders = build_order_stream(LATENCY_ORDERS);
    for (int i = 0; i < LATENCY_ORDERS; ++i) {
        if (i == WARMUP) recording.store(true, std::memory_order_relaxed);
        orders[i].ts = stamp();
        while (!engine.submit_new(orders[i])) {}
        wait_until_processed(engine, static_cast<uint64_t>(i + 1));  // one in flight
    }
    engine.stop();   // joins the matching thread: hist is safe to read now

    std::cout << "\n── 2. Latency: one order in flight at a time ──";
    hist.print("submit → first execution report");
}

int main() {
    std::cout << "=== LOB Matching Engine Benchmark ===\n";
    run_throughput();
    run_latency();
    return 0;
}
