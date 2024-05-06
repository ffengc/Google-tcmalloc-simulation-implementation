

#ifndef __YUFC_PAGE_CACHE_HPP__
#define __YUFC_PAGE_CACHE_HPP__

#include "./common.hpp"

class page_cache {
private:
    span_list __span_lists[PAGES_NUM];
    static page_cache __s_inst;
    page_cache() = default;
    page_cache(const page_cache&) = delete;

public:
    std::mutex __page_mtx;

public:
    static page_cache* get_instance() { return &__s_inst; }

public:
    // 获取一个K页的span
    span* new_span(size_t k);
};

#endif