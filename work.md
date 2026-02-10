# Detailed Project Implementation

**English** | [中文](./work-cn.md)

- [Detailed Project Implementation](#detailed-project-implementation)
  - [ThreadCache Overall Framework](#threadcache-overall-framework)
  - [Start Writing ThreadCache Code](#start-writing-threadcache-code)
  - [Hash Bucket Mapping Rules](#hash-bucket-mapping-rules)
  - [ThreadCache TLS Lock-Free Access](#threadcache-tls-lock-free-access)
  - [A Legacy Issue When Writing tcfree](#a-legacy-issue-when-writing-tcfree)
  - [Central Cache Overall Structure](#central-cache-overall-structure)
  - [Central Cache Core Logic](#central-cache-core-logic)
  - [The Logic of fetch\_range\_obj in Central Cache](#the-logic-of-fetch_range_obj-in-central-cache)
  - [Page Cache Overall Framework](#page-cache-overall-framework)
  - [Detailed Explanation of Getting a Span](#detailed-explanation-of-getting-a-span)
    - [About How new\_span Handles Locking (Important / Easy to Bug)](#about-how-new_span-handles-locking-important--easy-to-bug)
  - [Memory Allocation Flow Integration Testing](#memory-allocation-flow-integration-testing)
  - [Thread Cache Memory Deallocation](#thread-cache-memory-deallocation)
  - [Central Cache Memory Deallocation](#central-cache-memory-deallocation)
  - [Page Cache Memory Deallocation](#page-cache-memory-deallocation)
  - [Handling Allocations Larger Than 256KB](#handling-allocations-larger-than-256kb)
  - [Handling the `new` Problem in the Code](#handling-the-new-problem-in-the-code)
  - [Making free Work Without Passing Size](#making-free-work-without-passing-size)
  - [Deep Testing in Multi-threaded Scenarios](#deep-testing-in-multi-threaded-scenarios)
  - [Analyzing Performance Bottlenecks](#analyzing-performance-bottlenecks)
  - [Optimization with Radix Tree](#optimization-with-radix-tree)


## ThreadCache Overall Framework

**An important concept: the free list links small chunks of memory together. The first 4 bytes (or 8 bytes on 64-bit systems) of each chunk serve as a pointer to the next chunk in the list.**

If this were a fixed-size memory pool, a single free list would suffice. But our pool is not fixed-size.

Should we create one free list for 1 byte, one for 2 bytes, one for 3 bytes, and so on? That would be way too many.

Rule: requests smaller than 256KB go to threadCache; requests larger than 256KB will be handled separately later.

If we hung a separate list for every byte size from 1 to 256K, that would be over two hundred thousand lists — far too many!

So as shown in the diagram below, we design it this way:

![](./assets/2.png)

This is a compromise and trade-off.

## Start Writing ThreadCache Code

First, we obviously need to provide these two interfaces:

thread_cache.hpp
```cpp
class thread_cache {
private:
public:
    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);
};
```

We notice that the free list class is needed not only by threadCache but also by the upper layers, so we put it in common.hpp.

```cpp
class free_list {
private:
    void* __free_list_ptr;
public:
    void push(void* obj);
    void* pop();
};
```

The push and pop implementations are straightforward — just head insertion and head deletion.

```cpp
    void push(void* obj) {
        *(void**)obj = __free_list_ptr;
        __free_list_ptr = obj;
    }
```

The expression `*(void**)obj` needs some explanation: since we don't know whether a pointer is 4 bytes or 8 bytes in the current environment, this cast ensures we always read/write exactly one pointer's worth of bytes.

Then we can also encapsulate it:

```cpp
class free_list {
private:
    void* __free_list_ptr;

public:
    void push(void* obj) {
        assert(obj);
        __next_obj(obj) = __free_list_ptr;
        __free_list_ptr = obj;
    }
    void* pop() {
        assert(__free_list_ptr);
        void* obj = __free_list_ptr;
        __free_list_ptr = __next_obj(obj);
    }

private:
    static void*& __next_obj(void* obj) {
        return *(void**)obj;
    }
};
```

## Hash Bucket Mapping Rules

We can write a class to compute the size alignment and mapping rules for objects.

The mapping rules in tcmalloc are quite complex; here we simplify them.

The mapping rules are as follows:

Overall, internal fragmentation is controlled to at most ~10%.

| Request Size (bytes) | Alignment | Free List Range |
|-------|-------|-------|
| [1, 128] | 8-byte aligned | freelist[0, 16) |
| [128+1, 1024] | 16-byte aligned | freelist[16, 72) |
| [1024+1, 8*1024] | 128-byte aligned | freelist[72, 128) |
| [8*1024+1, 64*1024] | 1024-byte aligned | freelist[128, 184) |
| [64*1024+1, 256*1024] | 8*1024-byte aligned | freelist[184, 208) |

**This way we can control internal fragmentation to at most 10%. If you request more memory, we allow slightly more waste — which is reasonable. (These rules are specific to this project; tcmalloc's rules are more complex.)**

So we first determine what to align to, then find the alignment number.

common.hpp
```cpp
// Compute object size alignment mapping rules
class size_class {
public:
    static inline size_t __round_up(size_t bytes, size_t align_number) {
        return (((bytes) + align_number - 1) & ~(align_number - 1));
    }
    static inline size_t round_up(size_t size) {
        if (size <= 128)
            return __round_up(size, 8);
        else if (size <= 1024)
            return __round_up(size, 16);
        else if (size <= 8 * 1024)
            return __round_up(size, 128);
        else if (size <= 64 * 1024)
            return __round_up(size, 1024);
        else if (size <= 256 * 1024)
            return __round_up(size, 8 * 1024);
        else {
            assert(false);
            return -1;
        }
    }
};
```

How to understand this code:

```cpp
    size_t __round_up(size_t size, size_t align_number) {
        return (((bytes) + align_number - 1) & ~(align_number - 1));
    }
```

This is a clever trick. You can verify it with a few test cases.

thread_cache.cc
```cpp
void* thread_cache::allocate(size_t size) {
    assert(size <= MAX_BITES);
    size_t align_size = size_class::round_up(size);
}
```
Now we can get the aligned size! That is, if you request `size` bytes, we will give you `align_size` bytes.
But which bucket does this memory come from? So we also need a method to find the bucket.


```cpp
    // Compute which free list bucket to map to
    static inline size_t __bucket_index(size_t bytes, size_t align_shift) {
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
        /*
            Same idea: if bytes is not a multiple of the alignment,
            just divide and round up.
            If it is, apply a special rule. For example, for 1~128 bytes
            with alignment 8: bucket = bytes / 8 + 1
            If bytes % 8 == 0, there's no remainder, it's exactly that bucket,
            so no +1 is needed.
            This is also easy to understand.
        */
    }
    static inline size_t bucket_index(size_t bytes) {
        assert(bytes <= MAX_BYTES);
        // Number of lists in each range
        static int group_array[4] = { 16, 56, 56, 56 };
        if (bytes <= 128) {
            return __bucket_index(bytes, 3);
        } else if (bytes <= 1024) {
            return __bucket_index(bytes - 128, 4) + group_array[0];
        } else if (bytes <= 8 * 1024) {
            return __bucket_index(bytes - 1024, 7) + group_array[1] + group_array[0];
        } else if (bytes <= 64 * 1024) {
            return __bucket_index(bytes - 8 * 1024, 10) + group_array[2] + group_array[1]
                + group_array[0];
        } else if (bytes <= 256 * 1024) {
            return __bucket_index(bytes - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
        } else {
            assert(false);
        }
        return -1;
    }
```

This is also quite easy to understand, because the number of buckets in each alignment range is already determined:

If aligned by 8: 16 buckets
If aligned by 16: 56 buckets
...

In `bucket_index`, why do we add `group_array` numbers at the end? Because `__bucket_index` only computes which bucket you are *within your group*, not the global bucket index.

> For example, if you're in the 16-byte aligned group, your bucket number is definitely greater than 16, because the 8-byte aligned group already used 16 buckets. So you start from the 17th bucket. `__bucket_index` tells you that you're the first bucket in this group of 56, but globally you're the 17th bucket.

Then thread_cache.cc can be completed:

```cpp
void* thread_cache::allocate(size_t size) {
    assert(size <= MAX_BYTES);
    size_t align_size = size_class::round_up(size);
    size_t bucket_index = size_class::bucket_index(size);
    if (!__free_lists[bucket_index].empty()) {
        return __free_lists[bucket_index].pop();
    } else {
        // This bucket has no memory! Go to centralCache
        return fetch_from_central_cache(bucket_index, align_size);
    }
}
```

## ThreadCache TLS Lock-Free Access

First, if we understand operating system concepts, we know that within a process (including threads), everything is shared. This means that without special handling, the threadCache we create would be accessible to all threads.

That's not what we want — we need each thread to have its own threadCache!

> Thread Local Storage (TLS) is a storage mechanism where a variable is globally accessible within its owning thread but cannot be accessed by other threads. This maintains data isolation between threads. In contrast, well-known global variables are accessible to all threads, which inevitably requires locks for access control, increasing complexity and overhead.

On Linux, we just do this:

thread_cache.hpp
```cpp
__thread thread_cache* p_tls_thread_cache = nullptr;
```

On Windows:

```cpp
__thread static thread_cache* p_tls_thread_cache = nullptr;
```

This is easy to understand: by declaring the variable this way, each thread gets its own copy of `p_tls_thread_cache`.

When calling, we can't expect users to directly call thread_cache.cc's alloc. So we create another file to provide a calling interface.

tcmalloc.hpp
```cpp
static void* tcmalloc(size_t size) {
    if (p_tls_thread_cache == nullptr)
        // Essentially a singleton per thread
        p_tls_thread_cache = new thread_cache;
    return p_tls_thread_cache->allocate(size);
}

static void tcfree(size_t size) {
}
#endif
```

## A Legacy Issue When Writing tcfree

tcmalloc.hpp
```cpp
static void tcfree(void* ptr, size_t size) {
    assert(p_tls_thread_cache);
    p_tls_thread_cache->deallocate(ptr, size);
}
```

thread_cache.cc
```cpp
void thread_cache::deallocate(void* ptr, size_t size) {
    assert(ptr);
    assert(size <= MAX_BYTES);
    size_t index = size_class::bucket_index(size);
    __free_lists[index].push(ptr);
}
```

Here we need to pass the size, because `p_tls_thread_cache->deallocate()` needs `size` to know which bucket to return memory to. But the standard `free` doesn't require passing a size — how do we solve this?

We can't solve it right now. Let's keep this issue and revisit it later.

## Central Cache Overall Structure

CentralCache is also a hash bucket structure. Its hash bucket mapping is the same as threadCache's. The difference is that each hash bucket position holds a SpanList linked list structure, and each span's large memory block is cut into small memory block objects according to the mapping relation, linked in the span's free list.

Locking is needed here, but it uses bucket locks. If different threads access different buckets, no locking is required.

![](./assets/3.png)

**Memory allocation:**
1. When thread cache runs out of memory, it batch-requests memory objects from central cache. The batch quantity uses a slow-start algorithm similar to TCP congestion control. Central cache also has a hash-mapped spanlist; spanlist contains spans, and objects are taken from spans to give to thread cache. This process requires locking, but bucket locks are used here to maximize efficiency.
2. When all spans in central cache's mapped spanlist have no memory left, a new span object needs to be requested from page cache. After getting the span, the memory it manages is cut into fixed-size blocks linked together as a free list. Then objects are taken from the span to give to thread cache.
3. The `use_count` in each span of central cache tracks how many objects have been distributed. Each time an object is given to thread cache, `++use_count`.


**Memory deallocation:**

1. When threadCache's free list gets too long or the thread is destroyed, memory is released back to centralCache, and `--use_count`. When use_count drops to 0, it means all objects have returned to the span, and the span is released back to pageCache. PageCache will then merge adjacent pages to form larger pages, alleviating memory fragmentation.

**The small objects in centralCache are cut from large objects, and the large objects are Spans.**

The span list is a doubly-linked list.

Since span is needed by both central_cache and page_cache, let's define it in common.

There's a problem here: `size_t` is not large enough on 64-bit systems, so we need conditional compilation.

common.hpp
```cpp
#if defined(_WIN64) || defined(__x86_64__) || defined(__ppc64__) || defined(__aarch64__)
typedef unsigned long long PAGE_ID;
#else
typedef size_t PAGE_ID;
#endif
```

Note a pitfall on Windows: Win64 defines both win64 and win32, so we must check 64-bit first to avoid bugs.

```cpp
// Manages large memory blocks
class span {
public:
    PAGE_ID __page_id; // Page number of the starting page of the large memory block
    size_t __n; // Number of pages
    // Doubly-linked list structure
    span* __next;
    span* __prev;
    size_t __use_count; // Counter for small blocks distributed to threadCache
    void* __free_list; // Free list of cut small memory blocks
};
```

Then we need to implement a doubly-linked list by hand — straightforward, not much to say. Each bucket needs to maintain a lock!

common.hpp
```cpp
// Doubly-linked circular list with head node
class span_list {
private:
    span* __head = nullptr;
    std::mutex __bucket_mtx;
public:
    span_list() {
        __head = new span;
        __head->__next = __head;
        __head->__prev = __head;
    }
    void insert(span* pos, span* new_span) {
        // Insert a complete span
        assert(pos);
        assert(new_span);
        span* prev = pos->__prev;
        prev->__next = new_span;
        new_span->__prev = prev;
        new_span->__next = pos;
        pos->__prev = new_span;
    }
    void erase(span* pos) {
        assert(pos);
        assert(pos != __head);
        span* prev = pos->__prev;
        span* next = pos->__next;
        prev->__next = next;
        next->__prev = prev;
    }
};
```

central_cache.hpp
```cpp
#include "../common.hpp"

class central_cache {
private:
    span_list __span_lists[BUCKETS_NUM]; // One per bucket
public:

};
```

As many buckets as there are, that's how many locks we have!


## Central Cache Core Logic

**Clearly, this is well suited for the singleton pattern, since each process only needs one central_cache. For detailed explanation of the singleton pattern, see my blog: [Singleton Pattern](https://blog.csdn.net/Yu_Cblog/article/details/131787131)**


Here we use the eager initialization approach.

```cpp
class central_cache {
private:
    span_list __span_lists[BUCKETS_NUM]; // One per bucket
private:
    static central_cache __s_inst;
    central_cache() = default; // Private constructor
    central_cache(const central_cache&) = delete; // No copying allowed
public:
    central_cache* get_instance() { return &__s_inst; }
public:
};
```


So when threadCache asks for memory, how much do we give?

Here we use a slow-start feedback algorithm similar to TCP. We can put this algorithm in size_class.

common.hpp::size_class
```cpp
    // How many objects threadCache fetches from centralCache at once
    static inline size_t num_move_size(size_t size) {
        if (size == 0)
            return 0;
        // [2, 512], upper limit for batch object transfer (slow start)
        // Small objects: higher batch upper limit
        // Large objects: lower batch upper limit
        int num = MAX_BYTES / size;
        if (num < 2)
            num = 2;
        if (num > 512)
            num = 512;
        return num;
    }
```

This method tells threadCache how many objects to fetch from centralCache this time.

To control the slow start, free_list also needs a `max_size` field that increments over time.

thread_cache.cc
```cpp
void* thread_cache::fetch_from_central_cache(size_t index, size_t size) {
    // Slow-start feedback adjustment algorithm
    size_t batch_num = std::min(__free_lists[index].max_size(), size_class::num_move_size(size));
    if (__free_lists[index].max_size() == batch_num)
        __free_lists[index].max_size() += 1; // Grows up to 512 at most
    // 1. Initially don't request too much from centralCache, because excess may go unused
    // 2. If you keep requesting this bucket size, we gradually give you more, up to the limit (size_class::num_move_size(size))
    //      This limit is determined by the memory block size of this bucket
    // 3. The larger the size, the fewer we fetch from centralCache at once; the smaller the size, the opposite
    return nullptr;
}
```


Then we call the fetch_range_obj function.

Parameter meanings: get a range of memory from `start` to `end` blocks, fetching `batch_num` blocks total, each of size `size`. `end - start` should equal `batch_num`.

Return value meaning: we request `batch_num` objects from central_cache's span, but does the span necessarily have that many? Not necessarily. If the span doesn't have enough, it gives all it has. `actual_n` represents how many were actually obtained. `1 <= actual_n <= batch_num`.



thread_cache.cc
```cpp
void* thread_cache::fetch_from_central_cache(size_t index, size_t size) {
    // Slow-start feedback adjustment algorithm
    size_t batch_num = std::min(__free_lists[index].max_size(), size_class::num_move_size(size));
    if (__free_lists[index].max_size() == batch_num)
        __free_lists[index].max_size() += 1; // Grows up to 512 at most
    // 1. Initially don't request too much from centralCache, because excess may go unused
    // 2. If you keep requesting this bucket size, we gradually give you more, up to the limit (size_class::num_move_size(size))
    //      This limit is determined by the memory block size of this bucket
    // 3. The larger the size, the fewer we fetch from centralCache at once; the smaller the size, the opposite

    // Start fetching memory
    void* start = nullptr;
    void* end = nullptr;
    size_t actual_n = central_cache::get_instance()->fetch_range_obj(start, end, batch_num, size);
    return nullptr;
}
```

After fetching memory from cc (centralCache), there are two cases:

1. cc gave tc only one memory block (`actual_n == 1`): just return it directly. In this case, `thread_cache::allocate` will hand this block directly to the user without going through tc's hash bucket.
2. If cc gives us a range (`actual_n >= 1`), we only give one block to the user and insert the rest into tc! So we need to provide free_list with a method to insert a range (multiple blocks of size `size`) — just head insertion.

We can overload:

common.hpp::free_list
```cpp
    void push(void* obj) {
        assert(obj);
        __next_obj(obj) = __free_list_ptr;
        __free_list_ptr = obj;
    }
    void push(void* start, void* end) {
        __next_obj(end) = __free_list_ptr;
        __free_list_ptr = start;
    }
```

thread_cache.cc
```cpp
    if (actual_n == 1) {
        assert(start == end);
        return start;
    } else {
        __free_lists[index].push(free_list::__next_obj(start), end);
        return start;
    }

```

Here push starts from `start`'s next position — `start` doesn't need to go through tc; `start` is returned directly to the user. The blocks from `start+1` to `end` are inserted into tc.

## The Logic of fetch_range_obj in Central Cache

```cpp
size_t central_cache::fetch_range_obj(void*& start, void*& end, size_t batch_num, size_t size) {
    size_t index = size_class::bucket_index(size); // Find which bucket

}
```

After finding the bucket, we need to handle different cases.

First, if no span is hanging in this bucket, we need to go to the next level — ask pc (pageCache).

If some spans exist, there are also different cases.

We need to find a non-empty span first.

So let's write a method for this, though we can implement it later.

Note here: the free list is a singly-linked list. If we take a segment out, we must remember to set nullptr at the end.

**Important details:**
1. To take `batch_num` blocks, the end pointer only needs to walk `batch_num` steps (provided the span has enough)!
2. If the span doesn't have enough, special handling is needed!


```cpp
size_t central_cache::fetch_range_obj(void*& start, void*& end, size_t batch_num, size_t size) {
    size_t index = size_class::bucket_index(size); // Find which bucket
    __span_lists[index].__bucket_mtx.lock(); // Lock (consider RAII)
    span* cur_span = get_non_empty_span(__span_lists[index], size); // Find a non-empty span (might not find one)
    assert(cur_span);
    assert(cur_span->__free_list); // This non-empty span must have memory hanging below, so assert

    start = cur_span->__free_list;
    // Draw a diagram to understand this
    end = start;
    // Start pointer traversal, get objects from span; if not enough, take as many as available
    size_t i = 0;
    size_t actual_n = 1;
    while (i < batch_num - 1 && free_list::__next_obj(end) != nullptr) {
        end = free_list::__next_obj(end);
        ++i;
        ++actual_n;
    }
    cur_span->__free_list = free_list::__next_obj(end);
    free_list::__next_obj(end) = nullptr;
    __span_lists[index].__bucket_mtx.unlock(); // Unlock
    return actual_n;
}
```

Of course, cc is not fully complete yet. We need to write pc first before we can complete the remaining parts here.

## Page Cache Overall Framework

![](./assets/4.png)

**Memory allocation:**
1. When central cache requests memory from page cache, page cache first checks if there's a span at the corresponding position. If not, it looks for a larger span further along and splits it. For example: if requesting 4 pages and no span is available at the 4-page position, look further for a larger span. Say we find a span at the 10-page position — we split the 10-page span into a 4-page span and a 6-page span.
2. If no suitable span is found even at `_spanList[128]`, request a 128-page span from the system using mmap, brk, or VirtualAlloc and add it to the free list, then repeat step 1.
3. Note that while both central cache and page cache use spanlist hash buckets as their core structure, they are fundamentally different. Central cache's hash buckets use the same size-alignment mapping as thread cache, and the memory in its spans is cut into small blocks linked as a free list. Page cache's spanlist, however, is mapped by bucket index number — the i-th bucket contains spans of exactly i pages.


**Memory deallocation:**
1. When central cache releases a span back, page cache searches for free (unused) spans at adjacent page IDs (before and after), checking if they can be merged. If merged, it continues looking further. This way, small memory fragments can be merged back into larger spans, reducing memory fragmentation.

**The mapping here is different from before: there are 128 buckets total, the first for 1 page, the second for 2 pages!**

**pc only cares about how many pages cc wants! Note: the unit is pages!**

page_cache.hpp
```cpp
class page_cache {
private:
    span_list __span_lists[PAGES_NUM];
    std::mutex __page_mtx;
    static page_cache __s_inst;
    page_cache() = default;
    page_cache(const page_cache&) = delete;

public:
    static page_cache* get_instance() { return &__s_inst; }
public:
    // Get a span of K pages
};
```

This also needs to be designed as a singleton.

How do we initialize? Start with everything empty, then request a 128-page span from the OS (heap). Later, when a request comes (say for 2 pages), we split the 128-page span into a 126-page span and a 2-page span. The 2-page span goes to cc, and the 126-page span hangs on bucket 126.

When cc has memory it doesn't need, it returns it to the corresponding span. Then pc checks whether adjacent pages (by page number) are free. If so, they are merged into larger pages, solving the memory fragmentation problem.

## Detailed Explanation of Getting a Span

To traverse spans in cc, we can write some list traversal helpers in common.hpp.

common.hpp
```cpp
public:
    // Traversal helpers
    span* begin() { return __head->__next; }
    span* end() { return __head; }
```

central_cache.cc
```cpp
span* central_cache::get_non_empty_span(span_list& list, size_t size) {
    // First check if there's a non-empty span in the current spanlist
    span* it = list.begin();
    while (it != list.end()) {
        if (it->__free_list != nullptr) // Found a non-empty one
            return it;
        it = it->__next;
    }
    // If we reach here, there are no free spans — need to ask pc
    page_cache::get_instance()->new_span();
    return nullptr;
}
```

The question is: how many pages do we need? There's a calculation method for this too; let's put it in size_class!

common.hpp
```cpp
    static inline size_t num_move_page(size_t size) {
        size_t num = num_move_size(size);
        size_t npage = num * size;
        npage >>= PAGE_SHIFT; // Equivalent to /= 8KB
        if (npage == 0)
            npage = 1;
        return npage;
    }
```

So:
central_cache.cc
```cpp
span* central_cache::get_non_empty_span(span_list& list, size_t size) {
    // First check if there's a non-empty span in the current spanlist
    span* it = list.begin();
    while (it != list.end()) {
        if (it->__free_list != nullptr) // Found a non-empty one
            return it;
        it = it->__next;
    }
    // If we reach here, there are no free spans — need to ask pc
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    // Cutting logic
    return nullptr;
}
```

Now comes the cutting logic.

How do we find the memory address?

If the page number is 100, then the starting address of the page is `100 << PAGE_SHIFT`.

central_cache.cc
```cpp
span* central_cache::get_non_empty_span(span_list& list, size_t size) {
    // First check if there's a non-empty span in the current spanlist
    span* it = list.begin();
    while (it != list.end()) {
        if (it->__free_list != nullptr) // Found a non-empty one
            return it;
        it = it->__next;
    }
    // If we reach here, there are no free spans — need to ask pc
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    // Cutting logic
    // 1. Compute the starting address and byte size of the span's large memory block
    char* addr_start = (char*)(cur_span->__page_id << PAGE_SHIFT);
    size_t bytes = cur_span->__n << PAGE_SHIFT; // << PAGE_SHIFT means multiply by 8KB
    char* addr_end = addr_start + bytes;
    // 2. Cut the large memory block into a free list
    cur_span->__free_list = addr_start; // Cut the first block as the head
    addr_start += size;
    void* tail = cur_span->__free_list;
    while(addr_start < addr_end) {
        free_list::__next_obj(tail) = addr_start;
        tail = free_list::__next_obj(tail);
        addr_start += size;
    }
    list.push_front(cur_span);
    return cur_span;
}
```

Note: the "cutting" here means cutting the large memory block into a free list of small blocks — not the page cache splitting large pages into smaller pages.

After writing the above, we need to implement `span* page_cache::new_span(size_t k)`. This is where large pages are split into smaller pages.

page_cache.cc
```cpp
// cc requests a k-page span from pc
span* page_cache::new_span(size_t k) {
    assert(k > 0 && k < PAGES_NUM);
    // First check if the k-th bucket has a span
    if (!__span_lists[k].empty())
        return __span_lists->pop_front();
    // The k-th bucket is empty -> check subsequent buckets for a span to split
    for (size_t i = k + 1; i < PAGES_NUM; i++) {
        if (!__span_lists[i].empty()) {
            // We can start splitting
            // Suppose this span has n pages and we need k pages
            // 1. Remove from __span_lists  2. Split  3. Return one to cc  4. Hang the other on bucket n-k
            span* n_span = __span_lists[i].pop_front();
            span* k_span = new span;
            // Cut k pages from the front of n_span
            k_span->__page_id = n_span->__page_id; // <1>
            k_span->__n = k; // <2>
            n_span->__page_id += k; // <3>
            n_span->__n -= k; // <4>
            /**
             * Let's understand this: 100 ------ 101 ------- 102 ------
             * Suppose n_span starts at page 100 with size 3
             * After splitting: k_span starts at 100, hence <1>
             * After splitting: k_span has k pages, hence <2>
             * After splitting: n_span now starts at 102, hence <3>
             * After splitting: n_span becomes __n-k pages, hence <4>
             */
            // Hang the remainder at the appropriate position
            __span_lists[n_span->__n].push_front(n_span);
            return k_span;
        }
    }
    // If we reach here, no span was found: request from OS
    span* big_span = new span;
    big_span =
}
```

The splitting logic is explained clearly in the code comments!

If nothing is found even up to 128 pages, we request directly from the system!

Here we need to distinguish between Windows and Linux.

common.hpp
```cpp
inline static void* system_alloc(size_t kpage) {
    void* ptr = nullptr;
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
    *ptr = VirtualAlloc(0, kpage * (1 << 12), MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
#elif defined(__aarch64__) // ...
#include <sys/mman.h>
    void* ptr = mmap(NULL, kpage << 13, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
#include <iostream>
    std::cerr << "unknown system" << std::endl;
    throw std::bad_alloc();
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();
    return ptr;
}
```

Then the end of new_span:

```cpp
    // If we reach here, no span was found: request from OS
    span* big_span = new span;
    void* ptr = system_alloc(PAGES_NUM - 1);
    big_span->__page_id = (PAGE_ID)ptr >> PAGE_SHIFT;
    big_span->__n = PAGES_NUM - 1;
    // Hang it up
    __span_lists[PAGES_NUM - 1].push_front(big_span);
    return new_span(k);
```

After insertion, don't duplicate the splitting logic — just call itself recursively!

### About How new_span Handles Locking (Important / Easy to Bug)

The most critical step here: the entire method needs to be locked!

There's a key issue to think about.

`get_non_empty_span` is called by `fetch_range_obj` (in cc.cc).

But `get_non_empty_span` calls pc's `new_span`.
Now here's the critical problem: if we don't handle this properly, inside pc's `new_span`, we're actually still holding cc's bucket lock.
This is bad, because this bucket might have memory that needs to be released! If you hold the lock, others can't get in.
(Honestly, I don't fully understand this part either — need to study it more.)

So, in `span* central_cache::get_non_empty_span(span_list& list, size_t size) {`, before the line `span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));`, we should release the bucket lock first.

Then pc's `new_span`'s global lock — we add it in cc.cc's `span* central_cache::get_non_empty_span(span_list& list, size_t size) {`.

cc.cc
```cpp
    // If we reach here, there are no free spans — need to ask pc
    page_cache::get_instance()->__page_mtx.lock();
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    page_cache::get_instance()->__page_mtx.unlock();
```

**Now the question is: we got this new span from cc, and we still need to cut it. We released the lock before getting new_span — do we need to re-lock now?**

No!

Because this span was just obtained from pc — it's new and hasn't been hung on cc yet, so no other thread can access it! Therefore, no locking is needed!

But the final step `list.push_front(span)` accesses the cc object — so we need the lock. Let's restore it.

central_cache.cc
```cpp
span* central_cache::get_non_empty_span(span_list& list, size_t size) {
    // First check if there's a non-empty span in the current spanlist
    span* it = list.begin();
    while (it != list.end()) {
        if (it->__free_list != nullptr) // Found a non-empty one
            return it;
        it = it->__next;
    }
    // Release the bucket lock here first
    list.__bucket_mtx.unlock();

    // If we reach here, there are no free spans — need to ask pc
    page_cache::get_instance()->__page_mtx.lock();
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    page_cache::get_instance()->__page_mtx.unlock();

    // Cutting logic
    // 1. Compute the starting address and byte size of the span's large memory block
    char* addr_start = (char*)(cur_span->__page_id << PAGE_SHIFT);
    size_t bytes = cur_span->__n << PAGE_SHIFT; // << PAGE_SHIFT means multiply by 8KB
    char* addr_end = addr_start + bytes;
    // 2. Cut the large memory block into a free list
    cur_span->__free_list = addr_start; // Cut the first block as the head
    addr_start += size;
    void* tail = cur_span->__free_list;
    while(addr_start < addr_end) {
        free_list::__next_obj(tail) = addr_start;
        tail = free_list::__next_obj(tail);
        addr_start += size;
    }
    // Restore the lock
    list.__bucket_mtx.lock();
    list.push_front(cur_span);
    return cur_span;
}
```

## Memory Allocation Flow Integration Testing

First, add logs to each step to trace the call flow.

Then call tcmalloc multiple times and check the logs.

unit_test.cc
```cpp
void test_alloc() {
    std::cout << "call tcmalloc(1)" << std::endl;
    void* ptr = tcmalloc(8 * 1024);
    std::cout << "call tcmalloc(2)" << std::endl;
    ptr = tcmalloc(10);
    std::cout << "call tcmalloc(3)" << std::endl;
    ptr = tcmalloc(2);
    std::cout << "call tcmalloc(4)" << std::endl;
    ptr = tcmalloc(1);
    std::cout << "call tcmalloc(5)" << std::endl;
    ptr = tcmalloc(1);
    std::cout << "call tcmalloc(6)" << std::endl;
    ptr = tcmalloc(5);
    std::cout << "call tcmalloc(7)" << std::endl;
    ptr = tcmalloc(1);
}
```

Log output:
```bash
call tcmalloc(1)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
[DEBUG][src/thread_cache.cc][16] thread_cache::allocate call thread_cache::fetch_from_central_cache
[DEBUG][src/thread_cache.cc][43] thread_cache::fetch_from_central_cache call  central_cache::get_instance()->fetch_range_obj()
[DEBUG][src/central_cache.cc][12] central_cache::fetch_range_obj() call central_cache::get_non_empty_span()
[DEBUG][src/central_cache.cc][45] central_cache::get_non_empty_span() cannot find non-null span in cc, goto pc for mem
[DEBUG][src/central_cache.cc][52] central_cache::get_non_empty_span() call page_cache::get_instance()->new_span()
[DEBUG][src/page_cache.cc][43] page_cache::new_span() cannot find span, goto os for mem
[DEBUG][src/page_cache.cc][37] page_cache::new_span() have span, return
[DEBUG][src/central_cache.cc][58] central_cache::get_non_empty_span() get new span success
[DEBUG][src/central_cache.cc][70] central_cache::get_non_empty_span() cut span
[DEBUG][src/thread_cache.cc][47] actual_n:1
call tcmalloc(2)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
[DEBUG][src/thread_cache.cc][16] thread_cache::allocate call thread_cache::fetch_from_central_cache
[DEBUG][src/thread_cache.cc][43] thread_cache::fetch_from_central_cache call  central_cache::get_instance()->fetch_range_obj()
[DEBUG][src/central_cache.cc][12] central_cache::fetch_range_obj() call central_cache::get_non_empty_span()
[DEBUG][src/central_cache.cc][45] central_cache::get_non_empty_span() cannot find non-null span in cc, goto pc for mem
[DEBUG][src/central_cache.cc][52] central_cache::get_non_empty_span() call page_cache::get_instance()->new_span()
[DEBUG][src/page_cache.cc][37] page_cache::new_span() have span, return
[DEBUG][src/central_cache.cc][58] central_cache::get_non_empty_span() get new span success
[DEBUG][src/central_cache.cc][70] central_cache::get_non_empty_span() cut span
[DEBUG][src/thread_cache.cc][47] actual_n:1
call tcmalloc(3)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
[DEBUG][src/thread_cache.cc][16] thread_cache::allocate call thread_cache::fetch_from_central_cache
[DEBUG][src/thread_cache.cc][43] thread_cache::fetch_from_central_cache call  central_cache::get_instance()->fetch_range_obj()
[DEBUG][src/central_cache.cc][12] central_cache::fetch_range_obj() call central_cache::get_non_empty_span()
[DEBUG][src/central_cache.cc][45] central_cache::get_non_empty_span() cannot find non-null span in cc, goto pc for mem
[DEBUG][src/central_cache.cc][52] central_cache::get_non_empty_span() call page_cache::get_instance()->new_span()
[DEBUG][src/page_cache.cc][37] page_cache::new_span() have span, return
[DEBUG][src/central_cache.cc][58] central_cache::get_non_empty_span() get new span success
[DEBUG][src/central_cache.cc][70] central_cache::get_non_empty_span() cut span
[DEBUG][src/thread_cache.cc][47] actual_n:1
call tcmalloc(4)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
[DEBUG][src/thread_cache.cc][16] thread_cache::allocate call thread_cache::fetch_from_central_cache
[DEBUG][src/thread_cache.cc][43] thread_cache::fetch_from_central_cache call  central_cache::get_instance()->fetch_range_obj()
[DEBUG][src/central_cache.cc][12] central_cache::fetch_range_obj() call central_cache::get_non_empty_span()
[DEBUG][src/thread_cache.cc][47] actual_n:2
call tcmalloc(5)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
call tcmalloc(6)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
[DEBUG][src/thread_cache.cc][16] thread_cache::allocate call thread_cache::fetch_from_central_cache
[DEBUG][src/thread_cache.cc][43] thread_cache::fetch_from_central_cache call  central_cache::get_instance()->fetch_range_obj()
[DEBUG][src/central_cache.cc][12] central_cache::fetch_range_obj() call central_cache::get_non_empty_span()
[DEBUG][src/thread_cache.cc][47] actual_n:3
call tcmalloc(7)
[DEBUG][./include/tcmalloc.hpp][14] tcmalloc find tc from mem
```


Let's also test again:

```cpp
void test_alloc2() {
    for (size_t i = 0; i < 1024; ++i) {
        void* p1 = tcmalloc(6);
    }
    void* p2 = tcmalloc(6); // This time it will definitely need a new span
}
```

If we allocate 6 bytes 1024 times (aligned to 8 bytes), the 1025th allocation will definitely require a new span from the system — all previous ones won't! So the expected output should have only two `goto os for mem` messages.

The output log is saved in `./test/test1.log`.


## Thread Cache Memory Deallocation

When the list length exceeds the batch allocation size, start returning a segment of the list to cc.

thread_cache.cc
```cpp
void thread_cache::deallocate(void* ptr, size_t size) {
    assert(ptr);
    assert(size <= MAX_BYTES);
    size_t index = size_class::bucket_index(size);
    __free_lists[index].push(ptr);
    // When the list length exceeds the batch allocation size, return a segment to cc
    if (__free_lists[index].size() >= __free_lists[index].max_size()) {
        list_too_long(__free_lists[index], size);
    }
}
```

thread_cache.cc
```cpp
void thread_cache::list_too_long(free_list& list, size_t size) {
    void* start = nullptr;
    void* end = nullptr;
    list.pop(start, end, list.max_size());
    central_cache::get_instance()->release_list_to_spans(start, size);
}
```

tcmalloc's rules are more complex — it might also control memory size, releasing when exceeding a threshold, etc.


## Central Cache Memory Deallocation

```cpp
void central_cache::release_list_to_spans(void* start, size_t size) {
    size_t index = size_class::bucket_index(size); // Find which bucket
    __span_lists[index].__bucket_mtx.lock();
    // Note: one bucket may hold multiple spans; which span these blocks belong to is uncertain

    __span_lists[index].__bucket_mtx.unlock();
}
```

**The question here is: how do we determine which span each memory block should go back to?**


We need to determine which span these memory blocks came from. Spans are cut from pages, pages have addresses, and spans have addresses too.

So ideally, when memory is still in page cache, we should map page IDs to span addresses beforehand.

Add this to pc.hpp:
```cpp
std::unordered_map<PAGE_ID, span*> __id_span_map;
```

**Then in new_span, when distributing the new span to cc, record the mapping.**

Provide a method in pc to get the object-to-span mapping:

page_cache.cc
```cpp
span* page_cache::map_obj_to_span(void* obj) {
    // First compute the page number
    PAGE_ID id = (PAGE_ID)obj >> PAGE_SHIFT; // You can derive this formula yourself
    auto ret = __id_span_map.find(id);
    if (ret != __id_span_map.end())
        return ret->second;
    LOG(FATAL);
    assert(false);
    return nullptr;
}
```

Now we can get the corresponding span from an object.

Now we can continue writing `release_list_to_spans`:

```cpp
void central_cache::release_list_to_spans(void* start, size_t size) {
    size_t index = size_class::bucket_index(size); // Find which bucket
    __span_lists[index].__bucket_mtx.lock();
    // Note: one bucket may hold multiple spans; which span these blocks belong to is uncertain
    while (start) {
        // Traverse this list
        void* next = free_list::__next_obj(start); // Save next first to avoid losing it
        span* cur_span = page_cache::get_instance()->map_obj_to_span(start);
        free_list::__next_obj(start) = cur_span->__free_list;
        cur_span->__free_list = start;
        // Handle use_count
        cur_span->__use_count--;
        if (cur_span->__use_count == 0) {
            // All small blocks cut from this span have returned
            // Return to page cache
            // 1. Remove this span from cc's bucket spanlist
            __span_lists[index].erase(cur_span); // Remove from bucket
            // 2. No need to care about this span's freelist now, because this memory
            //    is originally after the span's start address, and the order is scrambled,
            //    so just set it to null
            //    (I don't fully understand this part yet)
            cur_span->__free_list = nullptr;
            cur_span->__next = cur_span->__prev = nullptr;
            // Page number and page count must not be modified!
            // 3. Release the bucket lock
            __span_lists[index].__bucket_mtx.unlock();
            // 4. Return to pc
            page_cache::get_instance()->__page_mtx.lock();
            page_cache::get_instance()->release_span_to_page(cur_span);
            page_cache::get_instance()->__page_mtx.unlock();
            // 5. Restore the bucket lock
            __span_lists[index].__bucket_mtx.lock();
        }
        start = next;
    }
    __span_lists[index].__bucket_mtx.unlock();
}
```

The details are explained clearly in the comments.

Remember: when calling pc's interface, release the bucket lock first.

## Page Cache Memory Deallocation

This is the function:

```cpp
void page_cache::release_span_to_page(span* s) {
    // Try to merge adjacent pages before and after the span to alleviate memory fragmentation
}
```

The map we wrote earlier can help us find adjacent pages.

When searching before and after, we need to distinguish whether the page is on centralCache. If it's on cc, it cannot be merged.

We can't use `use_count == 0` as the condition for this check. A span might have just been taken from pc and not yet given to anyone — its use_count would be 0, but pc shouldn't reclaim and merge it.

So we can add an `is_use` field to span:

```cpp
// Manages large memory blocks
class span {
public:
    PAGE_ID __page_id; // Page number of the starting page of the large memory block
    size_t __n = 0; // Number of pages
    // Doubly-linked list structure
    span* __next = nullptr;
    span* __prev = nullptr;
    size_t __use_count = 0; // Counter for small blocks distributed to threadCache
    void* __free_list = nullptr; // Free list of cut small memory blocks
    bool is_use = false; // Whether the span is in use
};
```

Then modify cc.cc — set it to true after obtaining the span:

cc.cc
```cpp
    page_cache::get_instance()->__page_mtx.lock();
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    cur_span->is_use = true; // Mark as in use
    page_cache::get_instance()->__page_mtx.unlock();
```


Then continue writing the logic:

page_cache.cc
```cpp
void page_cache::release_span_to_page(span* s) {
    // Try to merge adjacent pages before and after the span to alleviate memory fragmentation
    PAGE_ID prev_id = s->__page_id - 1; // The previous span's ID is always current span's ID - 1
    // Given the ID, can we find the span using the map we wrote earlier?
}
```

The current problem is: can the previous map find it? Not yet, because our map only recorded the mappings of spans distributed to cc, not the spans remaining in pc.
So we need to add mappings in `span* page_cache::new_span(size_t k) {` for the blocks that stay in pageCache.

```cpp
            // Store the mapping of n_span's first and last page numbers to n_span,
            // to facilitate merge lookups during pc memory reclamation
            __id_span_map[n_span->__page_id] = n_span;
            __id_span_map[n_span->__page_id + n_span->__n - 1] = n_span;
```

Why don't we need to store the mapping in a loop here?

Because pc's memory is just hung on spans — it won't be cut up. So knowing the address is enough!
The memory given to cc gets cut into many fixed-size memory blocks! That's why we don't need to store mappings in a loop here.

## Handling Allocations Larger Than 256KB

1. <= 256KB -> Follow the three-tier cache approach described above
2. \> 256KB:
   a. 128\*8KB > size > 32\*8KB: Can still go to pageCache
   b. Otherwise: Go directly to the system

This part requires modifications in multiple places, but they're all simple and easy to find — you can look at the code directly. After handling this, test with large memory allocations.


## Handling the `new` Problem in the Code

Some places in the code use `new span`. This is incorrect. We're building tcmalloc to replace malloc, and since it's a replacement, our code shouldn't contain `new` — `new` also calls `malloc` internally. So we need to change this.

We previously wrote a fixed-size memory pool that can replace `new`.

**Blog post: [What's the Principle Behind Memory Pools? | Simple Memory Pool Implementation | Preparation for Learning High-Concurrency Memory Pool tcmalloc](https://blog.csdn.net/Yu_Cblog/article/details/131741601)**

page_cache.hpp
```cpp
class page_cache {
private:
    span_list __span_lists[PAGES_NUM];
    static page_cache __s_inst;
    page_cache() = default;
    page_cache(const page_cache&) = delete;
    std::unordered_map<PAGE_ID, span*> __id_span_map;
    object_pool<span> __span_pool;
```
Add an `object_pool<span> __span_pool;` object.

Then replace all `new span` occurrences. Replace `delete` calls too.

Also modify here:

tcmalloc.hpp
```cpp
static void* tcmalloc(size_t size) {
    if (size > MAX_BYTES) {
        // Handle large memory allocation
        size_t align_size = size_class::round_up(size);
        size_t k_page = align_size >> PAGE_SHIFT;
        page_cache::get_instance()->__page_mtx.lock();
        span* cur_span = page_cache::get_instance()->new_span(k_page); // Go directly to pc
        page_cache::get_instance()->__page_mtx.unlock();
        void* ptr = (void*)(cur_span->__page_id << PAGE_SHIFT); // Convert span to address
        return ptr;
    }
    if (p_tls_thread_cache == nullptr) {
        // Essentially a singleton per thread
        // p_tls_thread_cache = new thread_cache;
        static object_pool<thread_cache> tc_pool;
        p_tls_thread_cache = tc_pool.new_();
    }
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "tcmalloc find tc from mem" << std::endl;
#endif
    return p_tls_thread_cache->allocate(size);
}
```

## Making free Work Without Passing Size

Since we already have a page-number-to-span mapping, we just add an `obj_size` field to span.

## Deep Testing in Multi-threaded Scenarios

**First, let's be clear: we're not trying to build a production wheel. We're comparing against malloc, not trying to be much faster — in many details, we're still far from the real tcmalloc.**

The test code can be found in bench\_mark.cc.

Results:
```bash
parallels@ubuntu-linux-22-04-desktop:~/Project/Google-tcmalloc-simulation-implementation$ ./out
==========================================================
4个线程并发执行10轮次，每轮次concurrent alloc 1000次: 花费：27877 ms
4个线程并发执行10轮次，每轮次concurrent dealloc 1000次: 花费：52190 ms
4个线程并发concurrent alloc&dealloc 40000次，总计花费：80067 ms


4个线程并发执行10次，每轮次malloc 1000次: 花费：2227ms
4个线程并发执行10轮次，每轮次free 1000次: 花费：1385 ms
4个线程并发malloc&free 40000次，总计花费：3612 ms
==========================================================
parallels@ubuntu-linux-22-04-desktop:~/Project/Google-tcmalloc-simulation-implementation$
```

Worse than malloc.

## Analyzing Performance Bottlenecks

Both Linux and Windows (VS Studio) have many performance analysis tools to detect where time is spent.

Here we jump straight to the conclusion: locking is consuming a lot of time.

The radix tree can be used for optimization.

## Optimization with Radix Tree

We can directly use the radix tree from tcmalloc's source code: `page_map.hpp`.
