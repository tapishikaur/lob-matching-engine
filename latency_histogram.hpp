#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <string>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Portable leading-zero count (MSYS2/MinGW and Linux)
// ─────────────────────────────────────────────────────────────────────────────
#if defined(_MSC_VER)
  #include <intrin.h>
  inline int clz64(uint64_t v) {
      unsigned long idx = 0;
      return _BitScanReverse64(&idx, v) ? (63 - (int)idx) : 64;
  }
#elif defined(__GNUC__) || defined(__clang__)
  inline int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
#else
  inline int clz64(uint64_t v) {
      int n = 0;
      if (!v) return 64;
      if (!(v >> 32)) { n += 32; v <<= 32; }
      if (!(v >> 48)) { n += 16; v <<= 16; }
      if (!(v >> 56)) { n +=  8; v <<=  8; }
      if (!(v >> 60)) { n +=  4; v <<=  4; }
      if (!(v >> 62)) { n +=  2; v <<=  2; }
      if (!(v >> 63)) { n +=  1; }
      return n;
  }
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Log2-bucketed latency histogram
//  64 buckets: bucket i covers [2^i, 2^(i+1)) nanoseconds
//  O(1) record, O(64) percentile query, zero heap allocation
// ─────────────────────────────────────────────────────────────────────────────
class LatencyHistogram {
public:
    static constexpr int BUCKETS = 64;

    void record(uint64_t ns) {
        if (ns == 0) ns = 1;
        int b = 63 - clz64(ns);
        b = std::clamp(b, 0, BUCKETS - 1);
        ++counts_[b];
        ++total_;
        if (ns > max_ns_) max_ns_ = ns;
        if (ns < min_ns_) min_ns_ = ns;
        sum_ns_ += ns;
    }

    uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        uint64_t target = static_cast<uint64_t>(total_ * p / 100.0);
        uint64_t cum    = 0;
        for (int i = 0; i < BUCKETS; ++i) {
            cum += counts_[i];
            if (cum > target)
                return (1ULL << i) + (i > 0 ? (1ULL << (i - 1)) : 0);
        }
        return max_ns_;
    }

    uint64_t min_val() const { return total_ ? min_ns_ : 0; }
    uint64_t max_val() const { return max_ns_; }
    uint64_t mean()    const { return total_ ? sum_ns_ / total_ : 0; }
    uint64_t count()   const { return total_; }

    void print(const char* label = "Latency") const {
        std::cout << "\n\u2500\u2500 " << label << " (" << total_ << " samples) \u2500\u2500\n"
                  << "  min    : " << fmt(min_val())          << "\n"
                  << "  mean   : " << fmt(mean())             << "\n"
                  << "  p50    : " << fmt(percentile(50))     << "\n"
                  << "  p90    : " << fmt(percentile(90))     << "\n"
                  << "  p99    : " << fmt(percentile(99))     << "\n"
                  << "  p99.9  : " << fmt(percentile(99.9))   << "\n"
                  << "  max    : " << fmt(max_val())          << "\n";
    }

    void reset() {
        counts_.fill(0);
        total_  = 0;
        max_ns_ = 0;
        min_ns_ = UINT64_MAX;
        sum_ns_ = 0;
    }

private:
    static std::string fmt(uint64_t ns) {
        char buf[32];
        if      (ns < 1'000ULL)       snprintf(buf, sizeof(buf), "%llu ns",        (unsigned long long)ns);
        else if (ns < 1'000'000ULL)   snprintf(buf, sizeof(buf), "%.2f us", ns / 1e3);
        else                          snprintf(buf, sizeof(buf), "%.2f ms", ns / 1e6);
        return buf;
    }

    std::array<uint64_t, BUCKETS> counts_{};
    uint64_t total_  = 0;
    uint64_t max_ns_ = 0;
    uint64_t min_ns_ = UINT64_MAX;
    uint64_t sum_ns_ = 0;
};
