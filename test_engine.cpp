#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "order_book.hpp"
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
#include "spsc_queue.hpp"

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
