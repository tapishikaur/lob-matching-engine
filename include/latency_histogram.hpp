#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
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
//  Log-linear latency histogram
//  Each power of two [2^e, 2^(e+1)) is split into 8 equal sub-buckets, so a
//  reported percentile is within 12.5% of the true value (exact below 8 ns).
//  O(1) record, O(buckets) percentile query, zero heap allocation.
//  Percentiles report the bucket's upper edge, clamped to the observed min/max,
//  so a percentile can never exceed the max.
// ─────────────────────────────────────────────────────────────────────────────
class LatencyHistogram {
public:
    static constexpr int SUB_BITS = 3;                  // 8 sub-buckets per power of two
    static constexpr int SUB      = 1 << SUB_BITS;
    static constexpr int BUCKETS  = 64 * SUB;

    void record(uint64_t ns) {
        ++counts_[index(ns)];
        ++total_;
        if (ns > max_ns_) max_ns_ = ns;
        if (ns < min_ns_) min_ns_ = ns;
        sum_ns_ += ns;
    }

    // p in [0, 100]: the smallest recorded value v such that at least p% of samples are <= v
    // (to the histogram's resolution).
    uint64_t percentile(double p) const {
        if (total_ == 0) return 0;
        if (p <= 0.0)   return min_ns_;   // exact, not a bucket edge
        if (p >= 100.0) return max_ns_;
        uint64_t target = static_cast<uint64_t>(static_cast<double>(total_) * p / 100.0 + 0.999999);
        target = std::clamp<uint64_t>(target, 1, total_);
        uint64_t cum = 0;
        for (int i = 0; i < BUCKETS; ++i) {
            cum += counts_[i];
            if (cum >= target) return std::clamp(upper_edge(i), min_ns_, max_ns_);
        }
        return max_ns_;
    }

    uint64_t min_val() const { return total_ ? min_ns_ : 0; }
    uint64_t max_val() const { return max_ns_; }
    uint64_t mean()    const { return total_ ? sum_ns_ / total_ : 0; }
    uint64_t count()   const { return total_; }

    void print(const char* label = "Latency") const {
        std::cout << "\n── " << label << " (" << total_ << " samples) ──\n"
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
    // Values below 2^SUB_BITS get a bucket each; above that, bucket = (exponent, top 3 bits after the leading 1).
    static int index(uint64_t ns) {
        if (ns < static_cast<uint64_t>(SUB)) return static_cast<int>(ns);
        const int e   = 63 - clz64(ns);
        const int sub = static_cast<int>((ns >> (e - SUB_BITS)) & (SUB - 1));
        return e * SUB + sub;
    }
    static uint64_t upper_edge(int i) {
        const int e = i / SUB, sub = i % SUB;
        if (e < SUB_BITS) return static_cast<uint64_t>(i);   // exact small values
        const int shift = e - SUB_BITS;
        return ((static_cast<uint64_t>(SUB + sub + 1)) << shift) - 1;
    }

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
