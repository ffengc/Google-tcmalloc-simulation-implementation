

#ifndef __YUFC_COMM_HPP__
#define __YUFC_COMM_HPP__

#include <algorithm>
#include <assert.h>
#include <mutex>
#include <unordered_map>

#ifdef PROJECT_DEBUG
#include "log.hpp"
#include <iostream>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#elif defined(__aarch64__) // ...
#include <sys/mman.h>
#else
#include <iostream>
#endif

static const size_t MAX_BYTES = 256 * 1024; // 256kb
static const size_t BUCKETS_NUM = 208; // 一共208个桶
static const size_t PAGES_NUM = 129; // pageCahche设置128个桶
static const size_t PAGE_SHIFT = 13;

#if defined(_WIN64) || defined(__x86_64__) || defined(__ppc64__) || defined(__aarch64__)
typedef unsigned long long PAGE_ID;
#else
typedef size_t PAGE_ID;
#endif

inline static void* system_alloc(size_t kpage) {
    void* ptr = nullptr;
#if defined(_WIN32) || defined(_WIN64)
    ptr = VirtualAlloc(0, kpage << 13, MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);
#elif defined(__aarch64__) // ...
    ptr = mmap(NULL, kpage << 13, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#else
    std::cerr << "unknown system" << std::endl;
    throw std::bad_alloc();
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();
    return ptr;
}

inline static void system_free(void* ptr, size_t size = 0) {
    /**
     * linux的munmap需要给大小
     */
#if defined(_WIN32) || defined(_WIN64)
    VirtualFree(ptr, 0, MEM_RELEASE);
#elif defined(__linux__) // ...
    munmap(ptr, size);
#endif
}

// 管理切分好的小对象的自由链表
class free_list {
private:
    void* __free_list_ptr = nullptr;
    size_t __max_size = 1;
    size_t __size = 0;

public:
    void push(void* obj) {
        assert(obj);
        __next_obj(obj) = __free_list_ptr;
        __free_list_ptr = obj;
        ++__size;
    }
    void push(void* start, void* end, size_t n) {
        __next_obj(end) = __free_list_ptr;
        __free_list_ptr = start;
        __size += n;
    }
    void* pop() {
        assert(__free_list_ptr);
        void* obj = __free_list_ptr;
        __free_list_ptr = __next_obj(obj);
        --__size;
        return obj;
    }
    void pop(void*& start, void*& end, size_t n) {
// 这里是输出参数了
#ifdef PROJECT_DEBUG
        LOG(DEBUG) << "call here" << std::endl;
#endif
        assert(n <= __size);
        start = __free_list_ptr;
        end = start; // debug 20240507 miss this
        for (size_t i = 0; i < n - 1; i++)
            end = free_list::__next_obj(end);
        __free_list_ptr = free_list::__next_obj(end);
        free_list::__next_obj(end) = nullptr;
        __size -= n;
    }
    bool empty() { return __free_list_ptr == nullptr; }
    size_t& max_size() { return __max_size; }
    size_t size() { return __size; }

public:
    static void*& __next_obj(void* obj) {
        return *(void**)obj;
    }
};

// 计算对象大小的对齐映射规则
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
            // 大内存
            return __round_up(size, 1 << PAGE_SHIFT);
        }
    }
    // 计算映射的哪一个自由链表桶
    static inline size_t __bucket_index(size_t bytes, size_t align_shift) {
        return ((bytes + (1 << align_shift) - 1) >> align_shift) - 1;
        /*
            这个还是同一道理，bytes不是对齐数的倍数，那就是直接模就行了
            如果是，那就特殊规则一下即可，比如 1~128字节，对齐数字是8
            那就是 bytes / 8 + 1 就是几号桶了
            如果 bytes % 8 == 0 表示没有余数，刚好就是那个桶，就不用+1
            这个也很好理解
            当然这里的 align_shift 也不是对齐数，这种写法给的是 2^n = 对齐数，给的是这个n
        */
    }
    static inline size_t bucket_index(size_t bytes) {
        assert(bytes <= MAX_BYTES);
        // 每个区间有多少个链
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
    // 一次threadCache从centralCache获取多少个内存
    static inline size_t num_move_size(size_t size) {
        if (size == 0)
            return 0;
        // [2, 512], 一次批量移动多少个对象的（慢启动）上限制
        // 小对象一次批量上限高
        // 大对象一次批量上限低
        int num = MAX_BYTES / size;
        if (num < 2)
            num = 2;
        if (num > 512)
            num = 512;
        return num;
    }
    // 计算一次向pc获取几个页
    static inline size_t num_move_page(size_t size) {
        size_t num = num_move_size(size);
        size_t npage = num * size;
        npage >>= PAGE_SHIFT; // 相当于 /= 8kb
        if (npage == 0)
            npage = 1;
        return npage;
    }
};

// 管理大块内存
class span {
public:
    PAGE_ID __page_id; // 大块内存起始页的页号
    size_t __n = 0; // 页的数量
    // 双向链表结构
    span* __next = nullptr;
    span* __prev = nullptr;
    size_t __use_count = 0; // 切成段小块内存，被分配给threadCache的计数器
    void* __free_list = nullptr; // 切好的小块内存的自由链表
    bool __is_use = false; // 是否在被使用
};

// 带头双向循环链表
class span_list {
private:
    span* __head = nullptr;

public:
    std::mutex __bucket_mtx;

public:
    span_list() {
        __head = new span;
        __head->__next = __head;
        __head->__prev = __head;
    }
    void insert(span* pos, span* new_span) {
        // 插入的是一个完好的span
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
    void push_front(span* new_span) {
        insert(begin(), new_span);
    }
    span* pop_front() {
        span* front = __head->__next;
        erase(front); // erase只是解除连接，没有删掉front
        return front;
    }
    bool empty() { return __head->__next == __head; }

public:
    // 遍历相关
    span* begin() { return __head->__next; }
    span* end() { return __head; }
};

#endif