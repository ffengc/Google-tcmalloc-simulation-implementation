
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
    size_t fetch_range_obj(void*& start, void*& end, size_t batch_num, size_t size);
    span* get_non_empty_span(span_list& list, size_t size);
public:
};

#endif