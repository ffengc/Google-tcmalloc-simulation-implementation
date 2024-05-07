
#ifndef __YUFC_CENTRAL_CACHE_HPP__
#define __YUFC_CENTRAL_CACHE_HPP__

#include "./common.hpp"

class central_cache {
private:
    span_list __span_lists[BUCKETS_NUM]; // 有多少个桶就多少个
private:
    static central_cache __s_inst;
    central_cache() = default; // 构造函数私有
    central_cache(const central_cache&) = delete; // 不允许拷贝
public:
    static central_cache* get_instance() { return &__s_inst; }
    // 将中心缓存获取一定数量的对象给threadCache
    size_t fetch_range_obj(void*& start, void*& end, size_t batch_num, size_t size);
    // 获取一个非空的span
    span* get_non_empty_span(span_list& list, size_t size);

public:
    // 将一定数量的对象释放到span中
    void release_list_to_spans(void* start, size_t byte_size);

public:
};

#endif