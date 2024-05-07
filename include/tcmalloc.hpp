
#ifndef __YUFC_TCMALLOC_HPP__
#define __YUFC_TCMALLOC_HPP__

#include "common.hpp"
#include "log.hpp"
#include "page_cache.hpp"
#include "thread_cache.hpp"

static void* tcmalloc(size_t size) {
    if (size > MAX_BYTES) {
        // 处理申请大内存的情况
        size_t align_size = size_class::round_up(size);
        size_t k_page = align_size >> PAGE_SHIFT;
        page_cache::get_instance()->__page_mtx.lock();
        span* cur_span = page_cache::get_instance()->new_span(k_page); // 直接找pc
        page_cache::get_instance()->__page_mtx.unlock();
        void* ptr = (void*)(cur_span->__page_id << PAGE_SHIFT); // span转化成地址
        return ptr;
    }
    if (p_tls_thread_cache == nullptr)
        // 相当于单例
        p_tls_thread_cache = new thread_cache;
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "tcmalloc find tc from mem" << std::endl;
#endif
    return p_tls_thread_cache->allocate(size);
}

static void tcfree(void* ptr, size_t size) {
    if (size > MAX_BYTES) {
        span* s = page_cache::get_instance()->map_obj_to_span(ptr); // 找到这个span
        page_cache::get_instance()->__page_mtx.lock();
        page_cache::get_instance()->release_span_to_page(s, size); // 直接调用pc的
        page_cache::get_instance()->__page_mtx.unlock();
        return;
    }
    assert(p_tls_thread_cache);
    p_tls_thread_cache->deallocate(ptr, size);
}

#endif