

#ifndef __YUFC_THREAD_CACHE_HPP__
#define __YUFC_THREAD_CACHE_HPP__

#include "common.hpp"

class thread_cache {
private:
    free_list __free_lists[BUCKETS_NUM]; // 哈希表

public:
    void* allocate(size_t size);
    void deallocate(void* ptr, size_t size);

public:
    void* fetch_from_central_cache(size_t index, size_t size);
};

#endif