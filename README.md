# LOB Matching Engine

[![CI](https://github.com/tapishikaur/lob-matching-engine/actions/workflows/ci.yml/badge.svg)](https://github.com/tapishikaur/lob-matching-engine/actions/workflows/ci.yml)

A C++20 limit order book matching engine with price-time priority, GTC / IOC / FOK orders,
a lock-free queue between threads, and no memory allocation on the matching path.
Builds with CMake on Linux, macOS and Windows (MSYS2 / MinGW-w64).

---

## How it works

```
 your thread                          matching thread
 ───────────                          ───────────────
 submit_new / submit_cancel   ──►  SPSC ring buffer  ──►  matching_loop: pop, std::visit
 submit_modify                     (16,384 slots,          │
 returns false if full             lock-free)              ▼
                                                         OrderBook for that symbol
                                                           │  price levels: std::map (bids high→low, asks low→high)
                                                           │  each level: FIFO list of orders
                                                           │  order slots: fixed MemoryPool, 65,536 per book
                                                           ▼
                                                         callbacks, on the matching thread:
                                                           on_exec(ExecutionReport), on_bbo(BBOUpdate)
```

- **Price-time priority.** The best price trades first; at the same price, the earliest order. A trade
  always happens at the resting order's price.
- **Order types.** GTC rests whatever doesn't trade. IOC cancels whatever doesn't trade at once. FOK checks
  first that the whole quantity can trade, and is rejected otherwise.
- **Cancel** is O(1) after the hash lookup: each resting order remembers its position in its level.
- **Modify** is cancel + re-enter at the back of the queue, so it always loses time priority.
  `new_price = 0` keeps the price; `new_qty = 0` keeps the quantity still open.
- **Memory.** Order slots come from a fixed pool allocated once per book, so matching never calls `new`.
  When all 65,536 slots hold resting orders, `add_order` refuses the next order (returns `false`) instead
  of crashing.
- **Prices** are fixed-point integers, ×10,000 (so $100.10 is `1001000`).

## Project layout

```
include/
  types.hpp               messages, enums, portable nanosecond clock
  memory_pool.hpp         fixed-size pool allocator
  spsc_queue.hpp          lock-free single-producer / single-consumer ring buffer
  order_book.hpp          OrderBook
  matching_engine.hpp     MatchingEngine: matching thread + one book per symbol
  latency_histogram.hpp   log-linear histogram for p50 / p99 / p99.9
src/
  order_book.cpp          matching, GTC / IOC / FOK, cancel, modify
  matching_engine.cpp     the matching thread's loop
  main.cpp                lob_demo: four scenarios, printed
bench/
  benchmark.cpp           lob_bench: throughput run, then latency run
tests/
  test_engine.cpp         lob_tests: doctest unit tests
CMakeLists.txt            fetches doctest v2.5.3 (pinned) for the tests
```

## Build and run

You need a C++20 compiler (GCC 11+ or Clang 14+), CMake 3.25+ and Ninja.

**Linux / macOS**

```bash
sudo apt install -y g++ cmake ninja-build git          # Ubuntu / Debian
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/lob_demo
./build/bin/lob_bench
```

**Windows (MSYS2)**

Install [MSYS2](https://www.msys2.org), then in its MinGW64 terminal:

```bash
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja git
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
./build/bin/lob_demo.exe
./build/bin/lob_bench.exe
```

The programs are linked statically on MinGW, so they also run from a normal Windows prompt.

**Options**

| Option | Default | Effect |
|---|---|---|
| `-DCMAKE_BUILD_TYPE=Release` | yes | `-O3 -DNDEBUG`: what to benchmark |
| `-DCMAKE_BUILD_TYPE=Debug` | | `-O0 -g`, `assert()` checks on |
| `-DLOB_NATIVE=OFF` | `ON` | don't tune for this machine's CPU (`-march=native`) |
| `-DLOB_BUILD_TESTS=OFF` | `ON` | skip the tests (and the doctest download) |

## Demo

`lob_demo` runs four scenarios on one symbol:

```
--- Scenario 2: Aggressive sell crosses spread ---
[EXEC]  seq=   5  oid=  5  SELL  status=         NEW  price= $100.0500  filled=   0  leaves=  50
[EXEC]  seq=   6  oid=  5  SELL  status=      FILLED  price= $100.1000  filled=  50  leaves=   0
[EXEC]  seq=   7  oid=  1  BUY   status=PARTIAL_FILL  price= $100.1000  filled=  50  leaves=  50
[BBO]  bid=$100.1000 x  50  |  ask=$100.1500 x 150
```

1. Two bids and two asks that don't cross: all rest.
2. A sell at 100.05 crosses and fills 50 at the best bid, 100.10: the resting order's price.
3. An IOC buy of 300 at 100.15 fills the 150 offered; the other 150 is cancelled.
4. A FOK sell of 1,000 at 99.90 finds only 250 bid in total, so it's rejected.

## Tests

`lob_tests`: 24 test cases, run in Release and Debug on every push (GitHub Actions, Ubuntu 24.04).

| Area | What is checked |
|---|---|
| Matching | full and partial fills, price-time priority, no trade when prices don't cross |
| Order types | IOC partial and no-match, FOK reject and full fill |
| Cancel and modify | cancel, cancel of unknown order, cancel in the middle of a level keeps the others in order, modify loses priority, modify without a new quantity keeps only what is still open |
| Memory pool | 66,536 orders filled on arrival (more than the pool's 65,536 slots) all work, so every slot is given back; a full pool refuses new orders and recovers after a cancel |
| Top of book | BBO after every change, including after a full fill empties the book |
| SPSC queue | push / pop, full, empty |
| Latency histogram | percentiles never exceed the max, are within 12.5% of the true value, small values exact |
| Matching engine | 10,000 orders through the queue and the matching thread: all processed, every buy filled by the sell after it |

## Benchmark

`lob_bench` makes two separate measurements, because one run can't give both honestly:

1. **Throughput.** 200,000 pre-built random orders (5,000 warm-up), pushed as fast as possible, so the queue
   stays full and the matching thread never waits. Prices are within ±40 ticks of 100.00, quantities 1–100,
   80% GTC / 15% IOC / 5% FOK, fixed seed.
2. **Latency.** 100,000 orders from the same generator, **one in flight at a time**: stamp, submit, wait
   until the matching thread has finished it. Each sample is submit → that order's first execution report, so
   it covers the hand-off between threads plus matching. Fills that happen later, while an order rests,
   aren't counted as its latency.

**Results** from three runs on a Windows 11 laptop (Intel Core Ultra 7 165U, MinGW-w64 GCC 16, Release,
`-march=native`, other programs running):

| Measure | Result |
|---|---|
| Throughput | 2.8 – 3.4 million orders/s |
| Latency p50 | 207 – 319 ns |
| Latency p99 | 415 – 511 ns |
| Latency p99.9 | 703 ns |

Read these with care:

- Windows' `steady_clock` ticks every 100 ns, so latencies this small are close to the clock's resolution.
  The histogram itself is accurate to 12.5%.
- Both threads spin, so the benchmark needs at least two free cores. Results move with CPU frequency, other
  load and which cores the threads land on. Run it on your own machine rather than comparing numbers across
  machines.

## Limitations and next steps

- One matching thread for all symbols; a real engine would shard symbols across threads.
- Modify always loses time priority; exchanges usually let a quantity decrease at the same price keep it.
- Callbacks run on the matching thread, so a slow callback slows matching.
- No market orders, no self-trade prevention, no persistence or recovery.
- The price levels use `std::map` and `std::list`; a flat array of levels would be faster.
