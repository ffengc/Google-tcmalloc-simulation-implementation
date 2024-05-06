
#ifndef __YUFC_TCMALLOC_HPP__
#define __YUFC_TCMALLOC_HPP__

#include "common.hpp"
#include "log.hpp"
#include "thread_cache.hpp"

static void* tcmalloc(size_t size) {
    if (p_tls_thread_cache == nullptr)
        // 相当于单例
        p_tls_thread_cache = new thread_cache;
    LOG(DEBUG) << "tcmalloc find tc from mem" << std::endl;
    return p_tls_thread_cache->allocate(size);
}

static void tcfree(void* ptr, size_t size) {
    assert(p_tls_thread_cache);
    p_tls_thread_cache->deallocate(ptr, size);
}

#endif