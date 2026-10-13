#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "latency_histogram.hpp"
#include "matching_engine.hpp"
#include "order_book.hpp"
#include "spsc_queue.hpp"
#include <cstddef>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Test helpers
// ─────────────────────────────────────────────────────────────────────────────
struct TestHarness {
    std::vector<ExecutionReport> reports;
    std::vector<BBOUpdate>       bbos;

    OrderBook book;

    TestHarness()
        : book(1,
               [this](const ExecutionReport& r){ reports.push_back(r); },
               [this](const BBOUpdate&      b){ bbos.push_back(b); })
    {}

    ExecutionReport last_exec() { return reports.back(); }

    NewOrderMsg make_buy(OrderId id, Price price, Quantity qty,
                         OrderType type = OrderType::GTC) {
        NewOrderMsg m;
        m.order_id   = id;
        m.symbol_id  = 1;
        m.side       = Side::BUY;
        m.price      = price;
        m.qty        = qty;
        m.order_type = type;
        return m;
    }

    NewOrderMsg make_sell(OrderId id, Price price, Quantity qty,
                          OrderType type = OrderType::GTC) {
        auto m = make_buy(id, price, qty, type);
        m.side = Side::SELL;
        return m;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Order Book – basics") {

    TEST_CASE("Resting buy then matching sell – full fill") {
        TestHarness h;
        h.book.add_order(h.make_buy (1, 100000, 10));  // $10.00
        h.book.add_order(h.make_sell(2, 100000, 10));

        // Should have: ack for buy, ack for sell, 2 fills
        REQUIRE(h.reports.size() >= 4);

        // Verify both orders filled
        bool buyer_filled = false, seller_filled = false;
        for (auto& r : h.reports) {
            if (r.order_id == 1 && r.status == OrderStatus::FILLED) buyer_filled  = true;
            if (r.order_id == 2 && r.status == OrderStatus::FILLED) seller_filled = true;
        }
        CHECK(buyer_filled);
        CHECK(seller_filled);
        CHECK(h.book.best_bid() == INVALID_PRICE);
        CHECK(h.book.best_ask() == INVALID_PRICE);
    }

    TEST_CASE("Partial fill – remainder rests") {
        TestHarness h;
        h.book.add_order(h.make_buy (1, 100000, 10));
        h.book.add_order(h.make_sell(2, 100000, 6));

        bool partial = false;
        for (auto& r : h.reports)
            if (r.order_id == 1 && r.status == OrderStatus::PARTIALLY_FILLED)
                { partial = true; CHECK(r.leaves_qty == 4); }
        CHECK(partial);
        CHECK(h.book.best_bid() == 100000);   // remainder stays
        CHECK(h.book.best_ask() == INVALID_PRICE);
    }

    TEST_CASE("Price-time priority – earlier order fills first") {
        TestHarness h;
        // Two buys at same price
        h.book.add_order(h.make_buy(1, 100000, 5));
        h.book.add_order(h.make_buy(2, 100000, 5));
        // Sell only 5 – should hit order #1 first
        h.book.add_order(h.make_sell(3, 100000, 5));

        bool order1_filled = false;
        for (auto& r : h.reports)
            if (r.order_id == 1 && r.status == OrderStatus::FILLED)
                order1_filled = true;
        CHECK(order1_filled);
        CHECK(h.book.best_bid() == 100000);   // order 2 still resting
    }

    TEST_CASE("No match when prices don't cross") {
        TestHarness h;
        h.book.add_order(h.make_buy (1, 99000, 10));
        h.book.add_order(h.make_sell(2, 101000, 10));

        CHECK(h.book.best_bid() == 99000);
        CHECK(h.book.best_ask() == 101000);
        for (auto& r : h.reports)
            CHECK(r.status != OrderStatus::FILLED);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Order types") {

    TEST_CASE("IOC – fills partially, cancels remainder") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 5));  // resting
        h.book.add_order(h.make_sell(2, 100000, 10, OrderType::IOC));

        bool cancelled = false;
        for (auto& r : h.reports)
            if (r.order_id == 2 && r.status == OrderStatus::CANCELLED)
                cancelled = true;
        CHECK(cancelled);
        CHECK(h.book.best_ask() == INVALID_PRICE);  // remainder cancelled
    }

    TEST_CASE("IOC – no match, fully cancelled") {
        TestHarness h;
        h.book.add_order(h.make_sell(1, 200000, 10, OrderType::IOC));

        bool cancelled = false;
        for (auto& r : h.reports)
            if (r.order_id == 1 && r.status == OrderStatus::CANCELLED)
                cancelled = true;
        CHECK(cancelled);
    }

    TEST_CASE("FOK – rejects when book can't fill entirely") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 5));
        h.book.add_order(h.make_sell(2, 100000, 10, OrderType::FOK));

        bool rejected = false;
        for (auto& r : h.reports)
            if (r.order_id == 2 && r.status == OrderStatus::REJECTED)
                rejected = true;
        CHECK(rejected);
        CHECK(h.book.best_bid() == 100000);  // resting order untouched
    }

    TEST_CASE("FOK – fills entirely when book has enough") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 10));
        h.book.add_order(h.make_sell(2, 100000, 10, OrderType::FOK));

        bool filled = false;
        for (auto& r : h.reports)
            if (r.order_id == 2 && r.status == OrderStatus::FILLED)
                filled = true;
        CHECK(filled);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Cancel and Modify") {

    TEST_CASE("Cancel resting order") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 10));
        CHECK(h.book.best_bid() == 100000);

        CancelOrderMsg cancel;
        cancel.order_id  = 1;
        cancel.symbol_id = 1;
        h.book.cancel_order(cancel);

        CHECK(h.book.best_bid() == INVALID_PRICE);
    }

    TEST_CASE("Cancel non-existent order returns false") {
        TestHarness h;
        CancelOrderMsg cancel;
        cancel.order_id  = 999;
        cancel.symbol_id = 1;
        CHECK_FALSE(h.book.cancel_order(cancel));
    }

    TEST_CASE("Modify – new price loses time priority") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 10));
        h.book.add_order(h.make_buy(2, 100000, 10));

        ModifyOrderMsg mod;
        mod.order_id   = 1;
        mod.symbol_id  = 1;
        mod.new_price  = 100000;   // same price – but re-enters at tail
        mod.new_qty    = 10;

        h.book.modify_order(mod);

        // Now a sell should hit order #2 first (time priority)
        h.book.add_order(h.make_sell(3, 100000, 10));

        bool order2_filled = false;
        for (auto& r : h.reports)
            if (r.order_id == 2 && r.status == OrderStatus::FILLED)
                order2_filled = true;
        CHECK(order2_filled);
    }

    TEST_CASE("Cancel in the middle of a level keeps the others in order") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 5));
        h.book.add_order(h.make_buy(2, 100000, 5));
        h.book.add_order(h.make_buy(3, 100000, 5));

        CancelOrderMsg cancel;
        cancel.order_id  = 2;
        cancel.symbol_id = 1;
        CHECK(h.book.cancel_order(cancel));
        CHECK(h.bbos.back().bid_qty == 10);    // the level's total dropped by 5

        h.reports.clear();
        h.book.add_order(h.make_sell(4, 100000, 10));
        std::vector<OrderId> filled;
        for (auto& r : h.reports)
            if (r.status == OrderStatus::FILLED && r.order_id != 4) filled.push_back(r.order_id);
        CHECK(filled == std::vector<OrderId>{1, 3});
        CHECK(h.book.best_bid() == INVALID_PRICE);
    }

    TEST_CASE("Modify without a new qty keeps only what is still open") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 10));
        h.book.add_order(h.make_sell(2, 100000, 4));   // 6 left open

        ModifyOrderMsg mod;
        mod.order_id  = 1;
        mod.symbol_id = 1;
        mod.new_price = 99000;
        mod.new_qty   = 0;                             // keep the quantity
        CHECK(h.book.modify_order(mod));

        CHECK(h.bbos.back().bid_price == 99000);
        CHECK(h.bbos.back().bid_qty   == 6);           // not the original 10
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Memory pool") {

    TEST_CASE("Orders filled on arrival give their slot back") {
        // Many more full fills than the pool has slots: every one must still work.
        TestHarness h;
        const OrderId pairs = OrderBook::MAX_ORDERS + 1000;
        std::size_t filled = 0;
        for (OrderId i = 0; i < pairs; ++i) {
            REQUIRE(h.book.add_order(h.make_buy (2 * i + 1, 100000, 1)));
            REQUIRE(h.book.add_order(h.make_sell(2 * i + 2, 100000, 1)));
            h.reports.clear();
            h.bbos.clear();
            ++filled;
        }
        CHECK(filled == pairs);
        CHECK(h.book.best_bid() == INVALID_PRICE);
        CHECK(h.book.best_ask() == INVALID_PRICE);
    }

    TEST_CASE("A full pool refuses new orders instead of crashing") {
        TestHarness h;
        for (OrderId i = 1; i <= OrderBook::MAX_ORDERS; ++i)
            REQUIRE(h.book.add_order(h.make_buy(i, 100000, 1)));   // all rest
        CHECK_FALSE(h.book.add_order(h.make_buy(OrderBook::MAX_ORDERS + 1, 100000, 1)));

        // Freeing one slot makes room again.
        CancelOrderMsg cancel;
        cancel.order_id  = 1;
        cancel.symbol_id = 1;
        CHECK(h.book.cancel_order(cancel));
        CHECK(h.book.add_order(h.make_buy(OrderBook::MAX_ORDERS + 1, 100000, 1)));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("BBO Updates") {

    TEST_CASE("BBO emitted on each order event") {
        TestHarness h;
        h.book.add_order(h.make_buy(1, 100000, 5));
        REQUIRE(!h.bbos.empty());
        CHECK(h.bbos.back().bid_price == 100000);
        CHECK(h.bbos.back().bid_qty   == 5);
        CHECK(h.bbos.back().ask_price == INVALID_PRICE);
    }

    TEST_CASE("BBO clears after full fill") {
        TestHarness h;
        h.book.add_order(h.make_buy (1, 100000, 5));
        h.book.add_order(h.make_sell(2, 100000, 5));
        CHECK(h.bbos.back().bid_price == INVALID_PRICE);
        CHECK(h.bbos.back().ask_price == INVALID_PRICE);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("SPSC Queue") {

    TEST_CASE("Basic push/pop") {
        SPSCQueue<int, 16> q;
        CHECK(q.empty());
        CHECK(q.push(42));
        CHECK(!q.empty());
        auto v = q.pop();
        REQUIRE(v.has_value());
        CHECK(*v == 42);
        CHECK(q.empty());
    }

    TEST_CASE("Full queue rejects push") {
        SPSCQueue<int, 4> q;
        CHECK(q.push(1));
        CHECK(q.push(2));
        CHECK(q.push(3));
        CHECK_FALSE(q.push(4));  // full (capacity = N-1 = 3)
    }

    TEST_CASE("Empty queue returns nullopt") {
        SPSCQueue<int, 8> q;
        CHECK_FALSE(q.pop().has_value());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Latency histogram") {

    TEST_CASE("Percentiles stay between min and max") {
        LatencyHistogram h;
        h.record(1'000);
        h.record(9'250'000);   // in the upper half of its power-of-two range
        CHECK(h.percentile(50)  >= h.min_val());
        CHECK(h.percentile(99)  <= h.max_val());
        CHECK(h.percentile(100) == h.max_val());
        CHECK(h.percentile(0)   == h.min_val());
    }

    TEST_CASE("Percentiles are within 12.5% of the true value") {
        LatencyHistogram h;
        for (uint64_t v = 1; v <= 100'000; ++v) h.record(v);   // true p-th percentile = p * 1000
        for (double p : {10.0, 50.0, 90.0, 99.0, 99.9}) {
            const double truth = p * 1'000.0;
            const double got   = static_cast<double>(h.percentile(p));
            CHECK(got >= truth);           // the upper edge of the bucket holding it
            CHECK(got <= truth * 1.125);
        }
    }

    TEST_CASE("Small values are exact") {
        LatencyHistogram h;
        for (uint64_t v : {1, 2, 3, 4, 5}) h.record(v);
        CHECK(h.percentile(20) == 1);
        CHECK(h.percentile(60) == 3);
        CHECK(h.percentile(100) == 5);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
TEST_SUITE("Matching engine (two threads)") {

    TEST_CASE("Orders sent through the queue are all processed, in order") {
        std::vector<ExecutionReport> reports;   // written by the matching thread only
        MatchingEngine engine([&](const ExecutionReport& r) { reports.push_back(r); },
                              [](const BBOUpdate&) {});
        engine.register_symbol(1);
        engine.start();

        constexpr int N = 10'000;
        for (int i = 0; i < N; ++i) {
            NewOrderMsg m;
            m.order_id  = static_cast<OrderId>(i + 1);
            m.symbol_id = 1;
            m.side      = i % 2 == 0 ? Side::BUY : Side::SELL;
            m.price     = 100000;
            m.qty       = 1;
            while (!engine.submit_new(m)) {}
        }
        engine.stop();   // drains the queue, then joins: reports is safe to read now

        CHECK(engine.orders_processed() == static_cast<uint64_t>(N));
        // Every buy rests and the next sell fills it: NEW + NEW + 2 fills per pair.
        CHECK(reports.size() == static_cast<std::size_t>(2 * N));
        std::size_t filled = 0;
        for (auto& r : reports)
            if (r.status == OrderStatus::FILLED) ++filled;
        CHECK(filled == static_cast<std::size_t>(N));
    }
}
