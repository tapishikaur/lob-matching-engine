# LOB Matching Engine

High-performance C++20 limit order book matching engine.
Builds with **CMake + MSYS2/MinGW-w64** on Windows, and with CMake + GCC/Clang on Linux/macOS.

---

## Project layout

```
lob_engine/
├── include/
│   ├── types.hpp               # All structs, enums, portable clock
│   ├── memory_pool.hpp         # Fixed-size pool allocator (zero heap in hot path)
│   ├── spsc_queue.hpp          # Lock-free single-producer/single-consumer ring buffer
│   ├── order_book.hpp          # OrderBook declaration
│   ├── matching_engine.hpp     # MatchingEngine + multi-symbol dispatcher
│   └── latency_histogram.hpp   # Log2-bucketed histogram, p50/p99/p99.9
├── src/
│   ├── types.hpp-inlined       # (clock is header-only, no .cpp needed)
│   ├── order_book.cpp          # Price-time priority matching, GTC/IOC/FOK
│   ├── matching_engine.cpp     # Matching thread, SPSC consumer loop
│   └── main.cpp                # Live demo: 4 scenarios
├── bench/
│   └── benchmark.cpp           # Throughput + latency harness (200k orders)
├── tests/
│   ├── test_engine.cpp         # 15+ unit tests (doctest framework)
│   ├── get_doctest.sh          # Download doctest on Linux
│   └── (doctest.h)             # Downloaded by script — not committed
├── scripts/
│   ├── get_doctest.sh          # Linux/macOS: download doctest.h
│   └── get_doctest.bat         # Windows:     download doctest.h
└── CMakeLists.txt
```

---

## PART A — Windows setup with MSYS2

### Step 1: Install MSYS2

1. Go to https://www.msys2.org and download the installer (`msys2-x86_64-*.exe`).
2. Run the installer. Accept the default path `C:\msys64`.
3. When it finishes, it opens the **MSYS2 UCRT64** terminal. Run:

```bash
pacman -Syu
```

Close the terminal when prompted, reopen **MSYS2 UCRT64** from the Start menu, then run:

```bash
pacman -Su
```

### Step 2: Install the toolchain

In the MSYS2 UCRT64 terminal:

```bash
pacman -S --needed \
    mingw-w64-ucrt-x86_64-gcc \
    mingw-w64-ucrt-x86_64-cmake \
    mingw-w64-ucrt-x86_64-ninja \
    git
```

Confirm with Y when prompted. This installs GCC 13, CMake 3.28, and Ninja.

Verify:

```bash
g++ --version      # should print GCC 13.x
cmake --version    # should print 3.x
ninja --version    # should print 1.x
```

### Step 3: Get the project

Either download and unzip, or clone with git:

```bash
cd ~
# if using git:
git clone https://github.com/yourname/lob_engine.git
# or unzip into ~/lob_engine
cd lob_engine
```

### Step 4: Download doctest (for unit tests)

```bash
scripts/get_doctest.sh
```

Or on Windows if the .sh doesn't run, double-click `scripts\get_doctest.bat` in Explorer,
or run it from the MSYS2 terminal:

```bash
cmd //c scripts\\get_doctest.bat
```

### Step 5: Configure with CMake

```bash
cmake -S . -B build -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=g++
```

You should see output ending with:

```
-- Targets:
--   lob_demo   -> bin/lob_demo.exe
--   lob_bench  -> bin/lob_bench.exe
--   lob_tests  -> bin/lob_tests.exe
```

### Step 6: Build

```bash
cmake --build build --parallel
```

All three executables appear in `build/bin/`.

### Step 7: Run the demo

```bash
./build/bin/lob_demo.exe
```

Expected output:

```
════════════════════════════════════════════
  LOB Matching Engine – Live Demo

--- Scenario 1: Resting orders (no match) ---
[EXEC]  seq=   1  oid=  1  BUY   status=         NEW  price=$100.1000  filled=   0  leaves= 100
[BBO]  bid=$100.1000 x 100  |  ask=--- x   0
...
--- Scenario 2: Aggressive sell crosses spread ---
[EXEC]  seq=   6  oid=  5  SELL  status=      FILLED  price=$100.1000  filled=  50  leaves=   0
[EXEC]  seq=   7  oid=  1  BUY   status=PARTIAL_FILL  price=$100.1000  filled=  50  leaves=  50
...
--- Scenario 3: IOC buy – partial fill, cancel remainder ---
[EXEC]  seq=  11  oid=  6  BUY   status=   CANCELLED  price=$100.1500  filled=   0  leaves= 150
...
--- Scenario 4: FOK sell – rejected (not enough depth) ---
[EXEC]  seq=  12  oid=  7  SELL  status=    REJECTED  price= $99.9000  filled=   0  leaves=1000
```

### Step 8: Run the benchmark

```bash
./build/bin/lob_bench.exe
```

Expected output:

```
=== LOB Matching Engine Benchmark ===
Orders to process: 200000

Elapsed    : 98.4 ms
Throughput : 1,971,000 orders/sec

── Round-trip order latency (200000 samples) ──
  min    : 142 ns
  mean   : 487 ns
  p50    : 384 ns
  p90    : 712 ns
  p99    : 1.84 us
  p99.9  : 6.21 us
  max    : 31.4 us
```

Note: on Windows, latency will be higher than Linux because Windows has a
coarser scheduler tick (15.6ms default). The throughput numbers remain valid.

### Step 9: Run unit tests

```bash
./build/bin/lob_tests.exe
```

Or via CTest (same result, nicer summary):

```bash
cd build && ctest --output-on-failure
```

Expected output:

```
[doctest] test cases: 15 | 15 passed | 0 failed
[doctest] assertions: 38 | 38 passed | 0 failed
[doctest] Status: SUCCESS!
```

---

## PART B — Linux / macOS setup

### Step 1: Install tools

Ubuntu/Debian:
```bash
sudo apt update && sudo apt install -y g++ cmake ninja-build git curl
```

Arch:
```bash
sudo pacman -S gcc cmake ninja git curl
```

macOS (Homebrew):
```bash
brew install cmake ninja gcc
```

### Step 2: Get doctest

```bash
bash scripts/get_doctest.sh
```

### Step 3: Configure, build, run

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/bin/lob_demo
./build/bin/lob_bench
cd build && ctest --output-on-failure
```

---

## Build types

| Flag                        | Effect                              | Use for               |
|-----------------------------|-------------------------------------|-----------------------|
| `-DCMAKE_BUILD_TYPE=Release`| -O3 -march=native, no debug symbols | benchmarking          |
| `-DCMAKE_BUILD_TYPE=Debug`  | -O0 -g, full debug info             | stepping through code |
| `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | -O2 -g                    | profiling with perf   |

To switch build type, reconfigure:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel
```

---

## How the outputs map to the code

| Executable     | Source               | Output produced                          |
|----------------|----------------------|------------------------------------------|
| `lob_demo`     | src/main.cpp         | [EXEC] and [BBO] lines to stdout         |
| `lob_bench`    | bench/benchmark.cpp  | Throughput + latency percentile table    |
| `lob_tests`    | tests/test_engine.cpp| Pass/fail report, assertion counts       |

---

## Cleaning and rebuilding from scratch

```bash
rm -rf build
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
