#pragma once
#include "types.hpp"
#include "memory_pool.hpp"
#include <map>
#include <unordered_map>
#include <list>
#include <functional>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Order node stored inside a PriceLevel's FIFO list
// ─────────────────────────────────────────────────────────────────────────────
struct Order {
    OrderId   order_id   = 0;
    SymbolId  symbol_id  = 0;
    Side      side       = Side::BUY;
    OrderType order_type = OrderType::GTC;
    Price     price      = 0;
    Quantity  open_qty   = 0;   // remaining unfilled quantity
    Quantity  orig_qty   = 0;
    Timestamp ts         = 0;
    std::list<Order*>::iterator level_pos{};  // where it sits in its level, while resting
};

// ─────────────────────────────────────────────────────────────────────────────
//  PriceLevel – all orders at one price, served FIFO
// ─────────────────────────────────────────────────────────────────────────────
struct PriceLevel {
    Price    price       = 0;
    Quantity total_qty   = 0;
    std::list<Order*> orders;   // FIFO queue; we hold pointers into pool

    void add(Order* o) {
        orders.push_back(o);
        o->level_pos = std::prev(orders.end());
        total_qty += o->open_qty;
    }

    // Remove a resting order (cancel path): O(1), using its stored position
    void remove(Order* o) {
        total_qty -= o->open_qty;
        orders.erase(o->level_pos);
    }

    bool empty() const { return orders.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
//  Callbacks fired by the order book / matching engine
// ─────────────────────────────────────────────────────────────────────────────
using ExecCallback = std::function<void(const ExecutionReport&)>;
using BBOCallback  = std::function<void(const BBOUpdate&)>;

// ─────────────────────────────────────────────────────────────────────────────
//  OrderBook  –  one per instrument
// ─────────────────────────────────────────────────────────────────────────────
class OrderBook {
public:
    static constexpr std::size_t MAX_ORDERS = 1 << 16;  // 65 536

    explicit OrderBook(SymbolId symbol_id,
                       ExecCallback on_exec,
                       BBOCallback  on_bbo);

    // Returns true if order was accepted (may be fully/partially filled)
    bool add_order(const NewOrderMsg& msg);

    // Returns true if order was found and cancelled
    bool cancel_order(const CancelOrderMsg& msg);

    // Cancel + re-enter at the back of the queue (always loses time priority).
    // new_price 0 keeps the price; new_qty 0 keeps the quantity still open.
    bool modify_order(const ModifyOrderMsg& msg);

    // Accessors
    SymbolId symbol_id() const { return symbol_id_; }
    Price    best_bid()  const;
    Price    best_ask()  const;
    void     print_top(int levels = 5) const;

private:
    // Bid side: highest price first  → std::map with std::greater
    // Ask side: lowest  price first  → std::map with std::less (default)
    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    SymbolId symbol_id_;
    BidMap   bids_;
    AskMap   asks_;

    // O(1) order lookup by id
    std::unordered_map<OrderId, Order*> order_index_;

    // Pool – no heap allocs in hot path
    MemoryPool<Order, MAX_ORDERS> pool_;

    // Sequence counter for execution reports
    uint64_t seq_no_ = 0;

    ExecCallback on_exec_;
    BBOCallback  on_bbo_;

    // ── Internal helpers ──────────────────────────────────────────────────
    // Try to match a newly submitted order against the opposing side
    // Returns leaves_qty (0 = fully filled)
    Quantity match(Order* aggressive, Timestamp exec_ts);

    // Emit exec report for a fill
    void emit_fill(const Order* aggressor,
                   const Order* passive,
                   Price        fill_price,
                   Quantity     fill_qty,
                   Timestamp    exec_ts);

    void emit_ack (const Order* o, OrderStatus status, Timestamp exec_ts);
    void emit_bbo (Timestamp ts);

    // Add resting order to book after (partial) match
    void rest_order(Order* o);

    Order* find_order(OrderId id);
};
