#pragma once
#include <cstdint>
#include <chrono>
#include <string>

// Portable nanosecond clock — works on Windows MSYS2/MinGW and Linux
inline uint64_t now_ns() {
    return static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

// All prices are fixed-point x10000: $10.25 -> 102500
using OrderId   = uint64_t;
using SymbolId  = uint32_t;
using Price     = int64_t;
using Quantity  = uint64_t;
using Timestamp = uint64_t;

constexpr Price INVALID_PRICE = -1;

enum class Side      : uint8_t { BUY = 0, SELL = 1 };
enum class OrderType : uint8_t { GTC, IOC, FOK };
enum class OrderStatus : uint8_t { NEW, PARTIALLY_FILLED, FILLED, CANCELLED, REJECTED };
enum class MsgType   : uint8_t { NEW_ORDER, CANCEL_ORDER, MODIFY_ORDER };

struct NewOrderMsg {
    MsgType   type       = MsgType::NEW_ORDER;
    OrderId   order_id   = 0;
    SymbolId  symbol_id  = 0;
    Side      side       = Side::BUY;
    OrderType order_type = OrderType::GTC;
    Price     price      = 0;
    Quantity  qty        = 0;
    Timestamp ts         = 0;
};

struct CancelOrderMsg {
    MsgType   type      = MsgType::CANCEL_ORDER;
    OrderId   order_id  = 0;
    SymbolId  symbol_id = 0;
    Timestamp ts        = 0;
};

struct ModifyOrderMsg {
    MsgType   type      = MsgType::MODIFY_ORDER;
    OrderId   order_id  = 0;
    SymbolId  symbol_id = 0;
    Price     new_price = 0;
    Quantity  new_qty   = 0;
    Timestamp ts        = 0;
};

struct ExecutionReport {
    uint64_t    seq_no     = 0;
    OrderId     order_id   = 0;
    OrderId     contra_id  = 0;
    SymbolId    symbol_id  = 0;
    Side        side       = Side::BUY;
    OrderStatus status     = OrderStatus::NEW;
    Price       price      = 0;
    Quantity    filled_qty = 0;
    Quantity    leaves_qty = 0;
    Timestamp   order_ts   = 0;
    Timestamp   exec_ts    = 0;
};

struct BBOUpdate {
    SymbolId  symbol_id = 0;
    Price     bid_price = INVALID_PRICE;
    Quantity  bid_qty   = 0;
    Price     ask_price = INVALID_PRICE;
    Quantity  ask_qty   = 0;
    Timestamp ts        = 0;
};
