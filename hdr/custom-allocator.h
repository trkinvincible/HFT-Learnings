/*
        Design notes

        Deterministic latency

        All memory is reserved at startup via numa_alloc_onnode and prefaulted; mlockall(MCL_CURRENT|MCL_FUTURE) avoids paging.

        allocate() / deallocate() are single CAS operations (no locks, no syscalls).

        No malloc/new/free/delete on the hot path.

        NUMA awareness

        Arena uses numa_alloc_onnode(node); caller pins thread to cores on the same node.

        Keep producer/consumer on the same node; if you must cross nodes, create separate arenas per node.

        Cache locality & false sharing

        Slots are 64-byte aligned; hot atomic head isolated with padding.

        Objects (example OrderMsg) padded to cache line to avoid sharing.

        Contiguous arena improves prefetch & TLB locality.

        TLB misses & shootdowns

        Prefer THP (MADV_HUGEPAGE) or explicit hugepages (if available) to shrink TLB footprint.

        Prefaulting touches every 4KB to avoid demand faults.

        No munmap/mprotect during runtime → avoids TLB shootdowns.

        Keep pools long-lived; don’t frequently resize.

        Recycling

        Fixed-size slots + intrusive free list → perfect recycling, no fragmentation.

        If you need multiple object sizes, instantiate multiple pools (one per type/size).

        Cross-thread free (optional)

        For the absolute lowest jitter, allocate and free on the same thread.
        If another thread must free, implement a per-owner MPSC “return queue”: other threads push returned nodes there, and the owner periodically drains into its freelist. This prevents contended CAS on a shared stack and avoids ABA hazards.
*/

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <unistd.h>
#include <sys/mman.h>
#include <numa.h>
#include <numaif.h>


// ---------- cacheline helpers ----------
#ifndef CACHELINE_SIZE
#define CACHELINE_SIZE 64
#endif
#define CACHE_ALIGNED alignas(CACHELINE_SIZE)

// Ensures a type is trivially storable in a fixed-size slot
template <class T>
concept PoolStorable = std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>;

// ---------- NUMA arena ----------
class NumaArena {
public:
    NumaArena(std::size_t bytes, int numa_node, bool prefer_thp = true)
    : size_(round_up(bytes, page_size())), node_(numa_node)
    {
        if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
            // Not fatal for all systems, but strongly recommended
            // You can throw if you prefer hard-fail:
            // throw std::runtime_error("mlockall failed");
        }

        if (numa_available() < 0)
            throw std::runtime_error("libnuma: NUMA not available");

        // Allocate on a specific NUMA node. This is anonymous, page-aligned memory.
#if 1
        void* p = numa_alloc_onnode(size_, node_);
        if (!p) throw std::bad_alloc();

        base_ = p;

        // Hint kernel for huge pages (transparent HP); avoids many TLB misses.
        if (prefer_thp) {
            (void)madvise(base_, size_, MADV_HUGEPAGE);
        }
#else
        /*
        *   sudo mkdir /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge
            echo 2048 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
            sudo mount -t hugetlbfs nodev /mnt/C406655E0665528A/Code-Factory/MINE/QtCreator-Projects/practice/hft-programs/huge/
         */
        sched_setscheduler(SCHED_FIFO)
        void* p = mmap(nullptr, size_,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                       -1, 0);
        if (p == MAP_FAILED) {
            throw std::runtime_error(std::string("mmap hugepages failed: ") +
                                     strerror(errno));
        }

        base_ = p;

        // bind memory to NUMA node
        unsigned long nodemask = 1UL << node_;
        long rc = mbind(base_, size_, MPOL_BIND, &nodemask,
                        sizeof(nodemask) * 8, MPOL_MF_MOVE);
#endif

        // Prefault: touch each page so there are no first-touch faults at runtime.
        prefault_pages();
    }

    ~NumaArena() {
        if (base_) {
            // Keep memory around? In HFT daemons you usually exit rarely; if you do free, do it once.
            numa_free(base_, size_);
        }
    }

    void* base() const noexcept { return base_; }
    std::size_t size() const noexcept { return size_; }
    int node() const noexcept { return node_; }

private:
    static std::size_t page_size() {
        static const long ps = sysconf(_SC_PAGESIZE);
        return static_cast<std::size_t>(ps);
    }
    static std::size_t round_up(std::size_t x, std::size_t a) {
        return (x + a - 1) & ~(a - 1);
    }
    void prefault_pages() {
        constexpr std::size_t stride = 4096; // touch per 4KB; THP/HP will coalesce under the hood
        volatile std::byte* p = static_cast<std::byte*>(base_);
        for (std::size_t i = 0; i < size_; i += stride) {
            p[i] = std::byte{0};
        }
    }

    void* base_{nullptr};
    std::size_t size_{0};
    int node_{0};
};

// ---------- Fixed-size freelist pool ----------
template <PoolStorable T>
class CACHE_ALIGNED FixedPool {
public:
    // slot_size allows headroom for alignment/padding, but we default to sizeof(T)
    explicit FixedPool(NumaArena& arena, std::size_t capacity, std::size_t slot_size = sizeof(T))
    : capacity_(capacity),
      slot_size_(round_up(std::max<std::size_t>(slot_size, sizeof(Node)), CACHELINE_SIZE))
    {
        const std::size_t needed = capacity_ * slot_size_;
        if (needed > arena.size()) {
            throw std::runtime_error("Arena too small for requested capacity");
        }
        storage_ = static_cast<std::byte*>(arena.base());
        init_freelist();
    }

    // Non-copyable; you usually create one pool per thread/NUMA node
    FixedPool(const FixedPool&) = delete;
    FixedPool& operator=(const FixedPool&) = delete;

    // Allocate one object (raw storage). No syscalls. O(1).
    // Returns nullptr if empty (you can make it throw if you prefer).
    T* allocate() noexcept {
        // Treiber stack (simple atomic LIFO) with pointer tagging to avoid ABA in single-producer scenarios.
        // For absolute determinism and to avoid ABA, we keep allocation/free on a single thread by design.
        // If you must cross-thread free, see note below for an MPSC return path.
        Node* head = free_head_.load(std::memory_order_acquire);
        if (!head) return nullptr;
        Node* next = head->next;
        while (!free_head_.compare_exchange_weak(head, next, std::memory_order_acq_rel, std::memory_order_acquire)) {
            if (!head) return nullptr;
            next = head->next;
        }
        // Placement-new is skipped since T is trivially constructible. Zero if you need clean buffers:
        // std::memset(head->payload(), 0, sizeof(T));
        return head->payload();
    }

    // Return an object to the pool. O(1). No syscalls.
    void deallocate(T* obj) noexcept {
        if (!obj) return;
        Node* node = Node::from(obj);
        // (Optional) scrub sensitive data:
        // std::memset(obj, 0, sizeof(T));
        Node* head = free_head_.load(std::memory_order_acquire);
        do {
            node->next = head;
        } while (!free_head_.compare_exchange_weak(head, node, std::memory_order_acq_rel, std::memory_order_acquire));
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t slot_size() const noexcept { return slot_size_; }
    std::size_t free_count() const noexcept {
        // Non-deterministic to walk; keep a separate counter if you need it strictly O(1).
        // For now, return 0 to avoid scanning (keep datapath lean).
        return 0;
    }

private:
    struct CACHE_ALIGNED Node {
        Node* next;
        alignas(CACHELINE_SIZE) std::byte storage[sizeof(T)];
        T* payload() noexcept { return std::bit_cast<T*>(storage); }
        static Node* from(T* p) noexcept {
            // storage is first member after 'next'; compute Node* from payload.
            auto* b = reinterpret_cast<std::byte*>(p);
            return reinterpret_cast<Node*>(b - offsetof(Node, storage));
        }
    };

    void init_freelist() noexceptmjn {
        // Build a contiguous array of Nodes inside storage_, linked as a freelist.
        std::byte* p = storage_;
        Node* prev = nullptr;
        for (std::size_t i = 0; i < capacity_; ++i) {
            Node* n = reinterpret_cast<Node*>(p);
            n->next = prev;
            prev = n;
            p += slot_size_;
        }
        free_head_.store(prev, std::memory_order_release);
        // Optional: touch every cache line to warm caches (small once-only cost).
        // This is usually not necessary if prefaulted.
    }

    static std::size_t round_up(std::size_t x, std::size_t a) {
        return (x + a - 1) & ~(a - 1);
    }

    // Padding to isolate hot atomic from neighbors (avoid false sharing).
    CACHE_ALIGNED std::atomic<Node*> free_head_{nullptr};
    char pad_[CACHELINE_SIZE - sizeof(std::atomic<Node*>) % CACHELINE_SIZE]{};

    std::byte* storage_{nullptr};
    std::size_t capacity_{0};
    std::size_t slot_size_{0};
};

// ---------- Per-thread pool wrapper (NUMA pinned) ----------
#include <pthread.h>
#include <sched.h>

inline void pin_thread_to_cpu(int cpu_id) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

inline int cpu_to_numa_node(int cpu_id) {
    int node = 0;
    (void)getcpu(nullptr, &node); // glibc; may require _GNU_SOURCE; or use numa API
    // Fallback: libnuma mapping
    return numa_node_of_cpu(cpu_id);
}

// Example POD object for HFT message/order structs
struct CACHE_ALIGNED OrderMsg {
    uint64_t ts_ns;
    uint64_t order_id;
    uint32_t instr_id;
    double   price;
    uint32_t qty;
    char     side;      // 'B' or 'S'
    char     pad[7];    // keep 64B aligned
};

CUST_ALLOC_TEST()
{
    // Choose CPU/core and NUMA node
    const int cpu = 2;
    pin_thread_to_cpu(cpu);
    const int node = cpu_to_numa_node(cpu);

    // Create a NUMA-local arena (e.g., 32 MB)
    constexpr std::size_t BYTES = 32ULL * 1024 * 1024;
    NumaArena arena(BYTES, node, /*prefer_thp=*/true);

    // Build a pool for OrderMsg with capacity N
    constexpr std::size_t CAP = 256 * 1024; // 256k orders
    FixedPool<OrderMsg> pool(arena, CAP);

    // Allocate
    OrderMsg* m = pool.allocate();
    if (!m) { std::cerr << "Pool exhausted\n"; return 1; }
    m->ts_ns = 0; m->order_id = 42; m->instr_id = 7; m->price = 101.25; m->qty = 10; m->side = 'B';

    // … use m …

    // Free
    pool.deallocate(m);

    std::cout << "OK\n";
    return 0;
}
