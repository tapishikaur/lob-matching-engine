#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

// Fixed-size pool allocator — O(1) alloc/free, zero heap in hot path.
// The slots are allocated once, on the heap, when the pool is built: at N = 65,536
// they are several MB, far too big to live on a thread's stack.
// NOT thread-safe: one pool per thread.
template <typename T, std::size_t N>
class MemoryPool {
public:
    MemoryPool() : storage_(std::make_unique<Slot[]>(N)) {
        for (std::size_t i = 0; i < N - 1; ++i)
            node_at(i)->next = node_at(i + 1);
        node_at(N - 1)->next = nullptr;
        head_ = node_at(0);
    }

    // Returns nullptr when every slot is in use; check available() first.
    template <typename... Args>
    T* allocate(Args&&... args) {
        assert(head_ && "MemoryPool exhausted");
        if (!head_) return nullptr;
        FreeNode* n = head_;
        head_ = head_->next;
        ++in_use_;
        return new (n) T(std::forward<Args>(args)...);
    }

    void deallocate(T* ptr) {
        ptr->~T();
        auto* n = reinterpret_cast<FreeNode*>(ptr);
        n->next = head_;
        head_   = n;
        --in_use_;
    }

    std::size_t in_use()    const { return in_use_; }
    std::size_t capacity()  const { return N; }
    bool        available() const { return head_ != nullptr; }

private:
    struct FreeNode { FreeNode* next; };
    static constexpr std::size_t SLOT =
        sizeof(T) > sizeof(FreeNode) ? sizeof(T) : sizeof(FreeNode);
    static constexpr std::size_t ALIGN =
        alignof(T) > alignof(FreeNode) ? alignof(T) : alignof(FreeNode);
    struct alignas(ALIGN) Slot { std::byte bytes[SLOT]; };

    FreeNode* node_at(std::size_t i) {
        return reinterpret_cast<FreeNode*>(&storage_[i]);
    }

    std::unique_ptr<Slot[]> storage_;
    FreeNode*   head_   = nullptr;
    std::size_t in_use_ = 0;
};
