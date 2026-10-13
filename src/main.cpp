#include "matching_engine.hpp"
#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
//  Pretty-print helpers
// ─────────────────────────────────────────────────────────────────────────────
static const char* status_str(OrderStatus s) {
    switch (s) {
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIAL_FILL";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
        case OrderStatus::REJECTED:         return "REJECTED";
    }
    return "?";
}

static std::string price_str(Price p) {
    if (p == INVALID_PRICE) return "---";
    char buf[16];
    snprintf(buf, sizeof(buf), "$%.4f", p / 10000.0);
    return buf;
}

int main() {
    std::cout << "════════════════════════════════════════════\n";
    std::cout << "  LOB Matching Engine – Live Demo\n";
    std::cout << "════════════════════════════════════════════\n\n";

    constexpr SymbolId AAPL = 1;

    // ── Callbacks ────────────────────────────────────────────────────────
    auto on_exec = [](const ExecutionReport& r) {
        std::cout << "[EXEC]"
                  << "  seq="     << std::setw(4)  << r.seq_no
                  << "  oid="     << std::setw(3)  << r.order_id
                  << "  "         << (r.side == Side::BUY ? "BUY " : "SELL")
                  << "  status="  << std::setw(12) << status_str(r.status)
                  << "  price="   << std::setw(10) << price_str(r.price)
                  << "  filled="  << std::setw(4)  << r.filled_qty
                  << "  leaves="  << std::setw(4)  << r.leaves_qty
                  << "\n";
    };

    auto on_bbo = [](const BBOUpdate& b) {
        std::cout << "[BBO] "
                  << " bid=" << price_str(b.bid_price)
                  << " x" << std::setw(4) << b.bid_qty
                  << "  |  ask=" << price_str(b.ask_price)
                  << " x" << std::setw(4) << b.ask_qty
                  << "\n";
    };

    // ── Setup ─────────────────────────────────────────────────────────────
    MatchingEngine engine(on_exec, on_bbo);
    engine.register_symbol(AAPL);
    engine.start();

    auto submit = [&](auto msg) {
        while (!engine.submit_new(msg)) {}
    };

    // Allow matching thread to spin up
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // ── Scenario 1: Build a simple order book ────────────────────────────
    std::cout << "\n--- Scenario 1: Resting orders (no match) ---\n";
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 1, AAPL, Side::BUY,  OrderType::GTC, 1001000, 100 });
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 2, AAPL, Side::BUY,  OrderType::GTC, 1000500, 200 });
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 3, AAPL, Side::SELL, OrderType::GTC, 1001500, 150 });
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 4, AAPL, Side::SELL, OrderType::GTC, 1002000, 100 });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ── Scenario 2: Aggressor sell hits best bid ──────────────────────────
    std::cout << "\n--- Scenario 2: Aggressive sell crosses spread ---\n";
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 5, AAPL, Side::SELL, OrderType::GTC, 1000500, 50 });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ── Scenario 3: IOC order ─────────────────────────────────────────────
    std::cout << "\n--- Scenario 3: IOC buy – partial fill, cancel remainder ---\n";
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 6, AAPL, Side::BUY, OrderType::IOC, 1001500, 300 });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // ── Scenario 4: FOK rejected ──────────────────────────────────────────
    std::cout << "\n--- Scenario 4: FOK sell – not enough book depth, rejected ---\n";
    submit(NewOrderMsg{ MsgType::NEW_ORDER, 7, AAPL, Side::SELL, OrderType::FOK, 999000, 1000 });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    engine.stop();
    std::cout << "\n════════════════════════════════════════════\n";
    std::cout << "  Total orders processed: " << engine.orders_processed() << "\n";
    std::cout << "════════════════════════════════════════════\n";
    return 0;
}
