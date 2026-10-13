#include "order_book.hpp"
#include <iostream>
#include <iomanip>
#include <vector>

OrderBook::OrderBook(SymbolId symbol_id, ExecCallback on_exec, BBOCallback on_bbo)
    : symbol_id_(symbol_id)
    , on_exec_(std::move(on_exec))
    , on_bbo_(std::move(on_bbo))
{}

bool OrderBook::add_order(const NewOrderMsg& msg) {
    if (msg.qty == 0) return false;
    if (order_index_.count(msg.order_id)) return false;
    if (!pool_.available()) return false;   // every slot holds a resting order: refuse, don't crash

    Order* o = pool_.allocate();
    o->order_id   = msg.order_id;
    o->symbol_id  = msg.symbol_id;
    o->side       = msg.side;
    o->order_type = msg.order_type;
    o->price      = msg.price;
    o->open_qty   = msg.qty;
    o->orig_qty   = msg.qty;
    o->ts         = msg.ts ? msg.ts : now_ns();

    const Timestamp exec_ts = now_ns();

    // FOK pre-check: is there enough depth to fill entirely?
    if (o->order_type == OrderType::FOK) {
        Quantity available = 0;
        if (o->side == Side::BUY) {
            for (auto& [px, lvl] : asks_) {
                if (px > o->price) break;
                available += lvl.total_qty;
                if (available >= o->open_qty) break;
            }
        } else {
            for (auto& [px, lvl] : bids_) {
                if (px < o->price) break;
                available += lvl.total_qty;
                if (available >= o->open_qty) break;
            }
        }
        if (available < o->open_qty) {
            emit_ack(o, OrderStatus::REJECTED, exec_ts);
            pool_.deallocate(o);
            return false;
        }
    }

    order_index_[o->order_id] = o;
    emit_ack(o, OrderStatus::NEW, exec_ts);

    Quantity leaves = match(o, exec_ts);

    if (leaves == 0) {
        // Filled on arrival: it never rests, so free its slot now. The book still
        // changed (resting orders were used up), so publish the new top of book.
        order_index_.erase(o->order_id);
        pool_.deallocate(o);
        emit_bbo(now_ns());
    } else if (o->order_type == OrderType::IOC) {
        emit_ack(o, OrderStatus::CANCELLED, now_ns());
        order_index_.erase(o->order_id);
        pool_.deallocate(o);
        emit_bbo(now_ns());
    } else {
        rest_order(o);
        emit_bbo(now_ns());
    }
    return true;
}

bool OrderBook::cancel_order(const CancelOrderMsg& msg) {
    Order* o = find_order(msg.order_id);
    if (!o) return false;

    const Timestamp exec_ts = msg.ts ? msg.ts : now_ns();

    // Every indexed order is resting on its level, so these finds always succeed.
    if (o->side == Side::BUY) {
        auto it = bids_.find(o->price);
        it->second.remove(o);
        if (it->second.empty()) bids_.erase(it);
    } else {
        auto it = asks_.find(o->price);
        it->second.remove(o);
        if (it->second.empty()) asks_.erase(it);
    }

    emit_ack(o, OrderStatus::CANCELLED, exec_ts);
    order_index_.erase(o->order_id);
    pool_.deallocate(o);
    emit_bbo(exec_ts);
    return true;
}

bool OrderBook::modify_order(const ModifyOrderMsg& msg) {
    Order* o = find_order(msg.order_id);
    if (!o) return false;

    // Save side/type before cancel removes the order. A new_qty of 0 keeps what is
    // still open: re-entering orig_qty would bring back quantity that already traded.
    Side      saved_side = o->side;
    OrderType saved_type = o->order_type;
    Quantity  saved_qty  = o->open_qty;
    Price     saved_px   = o->price;

    CancelOrderMsg cancel;
    cancel.order_id  = msg.order_id;
    cancel.symbol_id = msg.symbol_id;
    cancel.ts        = msg.ts;
    cancel_order(cancel);

    NewOrderMsg neo;
    neo.order_id   = msg.order_id;
    neo.symbol_id  = msg.symbol_id;
    neo.side       = saved_side;
    neo.order_type = saved_type;
    neo.price      = msg.new_price ? msg.new_price : saved_px;
    neo.qty        = msg.new_qty   ? msg.new_qty   : saved_qty;
    neo.ts         = msg.ts;
    return add_order(neo);
}

Quantity OrderBook::match(Order* agg, Timestamp exec_ts) {
    auto try_match = [&](auto& levels, auto price_ok) {
        while (agg->open_qty > 0 && !levels.empty()) {
            auto it = levels.begin();
            if (!price_ok(it->first, agg->price)) break;

            PriceLevel& lvl = it->second;
            while (agg->open_qty > 0 && !lvl.orders.empty()) {
                Order* passive = lvl.orders.front();
                Quantity fill  = std::min(agg->open_qty, passive->open_qty);
                Price fill_px  = passive->price;

                agg->open_qty     -= fill;
                passive->open_qty -= fill;
                lvl.total_qty     -= fill;

                emit_fill(agg, passive, fill_px, fill, exec_ts);

                if (passive->open_qty == 0) {
                    lvl.orders.pop_front();
                    order_index_.erase(passive->order_id);
                    pool_.deallocate(passive);
                }
            }
            if (lvl.empty()) levels.erase(it);
        }
    };

    if (agg->side == Side::BUY)
        try_match(asks_, [](Price ask, Price bid) { return ask <= bid; });
    else
        try_match(bids_, [](Price bid, Price ask) { return bid >= ask; });

    return agg->open_qty;
}

void OrderBook::rest_order(Order* o) {
    if (o->side == Side::BUY) {
        bids_[o->price].price = o->price;
        bids_[o->price].add(o);
    } else {
        asks_[o->price].price = o->price;
        asks_[o->price].add(o);
    }
}

void OrderBook::emit_fill(const Order* agg, const Order* passive,
                           Price fill_px, Quantity fill_qty, Timestamp exec_ts) {
    auto make = [&](const Order* o, const Order* contra) {
        ExecutionReport r;
        r.seq_no     = ++seq_no_;
        r.order_id   = o->order_id;
        r.contra_id  = contra->order_id;
        r.symbol_id  = symbol_id_;
        r.side       = o->side;
        r.status     = (o->open_qty == 0) ? OrderStatus::FILLED
                                           : OrderStatus::PARTIALLY_FILLED;
        r.price      = fill_px;
        r.filled_qty = fill_qty;
        r.leaves_qty = o->open_qty;
        r.order_ts   = o->ts;
        r.exec_ts    = exec_ts;
        on_exec_(r);
    };
    make(agg, passive);
    make(passive, agg);
}

void OrderBook::emit_ack(const Order* o, OrderStatus status, Timestamp exec_ts) {
    ExecutionReport r;
    r.seq_no     = ++seq_no_;
    r.order_id   = o->order_id;
    r.symbol_id  = symbol_id_;
    r.side       = o->side;
    r.status     = status;
    r.price      = o->price;
    r.filled_qty = 0;
    r.leaves_qty = o->open_qty;
    r.order_ts   = o->ts;
    r.exec_ts    = exec_ts;
    on_exec_(r);
}

void OrderBook::emit_bbo(Timestamp ts) {
    BBOUpdate u;
    u.symbol_id = symbol_id_;
    u.ts        = ts;
    if (!bids_.empty()) { u.bid_price = bids_.begin()->first; u.bid_qty = bids_.begin()->second.total_qty; }
    if (!asks_.empty()) { u.ask_price = asks_.begin()->first; u.ask_qty = asks_.begin()->second.total_qty; }
    on_bbo_(u);
}

Price OrderBook::best_bid() const { return bids_.empty() ? INVALID_PRICE : bids_.begin()->first; }
Price OrderBook::best_ask() const { return asks_.empty() ? INVALID_PRICE : asks_.begin()->first; }
Order* OrderBook::find_order(OrderId id) {
    auto it = order_index_.find(id);
    return it != order_index_.end() ? it->second : nullptr;
}

void OrderBook::print_top(int levels) const {
    std::cout << "\n=== Order Book (symbol=" << symbol_id_ << ") ===\n";
    std::cout << std::setw(12) << "ASK QTY" << std::setw(14) << "PRICE\n";
    std::vector<std::pair<Price,Quantity>> ask_rows;
    for (auto& [px, lvl] : asks_) {
        ask_rows.emplace_back(px, lvl.total_qty);
        if ((int)ask_rows.size() >= levels) break;
    }
    for (auto it = ask_rows.rbegin(); it != ask_rows.rend(); ++it)
        std::cout << std::setw(12) << it->second << std::setw(14) << (it->first/10000.0) << "\n";
    std::cout << "         ──── spread ────\n";
    int cnt = 0;
    for (auto& [px, lvl] : bids_) {
        std::cout << std::setw(12) << lvl.total_qty << std::setw(14) << (px/10000.0) << "\n";
        if (++cnt >= levels) break;
    }
    std::cout << "\n";
}
