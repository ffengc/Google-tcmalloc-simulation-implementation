
#include "../include/central_cache.hpp"
#include "../include/log.hpp"
#include "../include/page_cache.hpp"

central_cache central_cache::__s_inst;

size_t central_cache::fetch_range_obj(void*& start, void*& end, size_t batch_num, size_t size) {
    size_t index = size_class::bucket_index(size); // 算出在哪个桶找
    __span_lists[index].__bucket_mtx.lock(); // 加锁（可以考虑RAII）
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "central_cache::fetch_range_obj() call central_cache::get_non_empty_span()" << std::endl;
#endif
    span* cur_span = get_non_empty_span(__span_lists[index], size); // 找一个非空的span（有可能找不到）
    assert(cur_span);
    assert(cur_span->__free_list); // 这个非空的span一定下面挂着内存了，所以断言一下

    start = cur_span->__free_list;
    // 这里要画图理解一下
    end = start;
    // 开始指针遍历，从span中获取对象，如果不够，有多少拿多少
    size_t i = 0;
    size_t actual_n = 1;
    while (i < batch_num - 1 && free_list::__next_obj(end) != nullptr) {
        end = free_list::__next_obj(end);
        ++i;
        ++actual_n;
    }
    cur_span->__free_list = free_list::__next_obj(end);
    free_list::__next_obj(end) = nullptr;
    cur_span->__use_count += actual_n; // 拿走了几个，use_count记得加上去
    __span_lists[index].__bucket_mtx.unlock(); // 解锁
    return actual_n;
}

span* central_cache::get_non_empty_span(span_list& list, size_t size) {
    // 先查看当前的spanlist中是否还有非空的span
    span* it = list.begin();
    while (it != list.end()) {
        if (it->__free_list != nullptr) // 找到非空的了
            return it;
        it = it->__next;
    }
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "central_cache::get_non_empty_span() cannot find non-null span in cc, goto pc for mem" << std::endl;
#endif
    // 这里先解开桶锁
    list.__bucket_mtx.unlock();

// 如果走到这里，说明没有空闲的span了，就要找pc了
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "central_cache::get_non_empty_span() call page_cache::get_instance()->new_span()" << std::endl;
#endif
    page_cache::get_instance()->__page_mtx.lock();
    span* cur_span = page_cache::get_instance()->new_span(size_class::num_move_page(size));
    page_cache::get_instance()->__page_mtx.unlock();
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "central_cache::get_non_empty_span() get new span success" << std::endl;
#endif
    // 切分的逻辑
    // 1. 计算span的大块内存的起始地址和大块内存的大小（字节数）
    char* addr_start = (char*)(cur_span->__page_id << PAGE_SHIFT);
    size_t bytes = cur_span->__n << PAGE_SHIFT; // << PAGE_SHIFT 就是乘8kb的意思
    char* addr_end = addr_start + bytes;
    // 2. 把大块内存切成自由链表链接起来
    cur_span->__free_list = addr_start; // 先切一块下来做头
    addr_start += size;
    void* tail = cur_span->__free_list;
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "central_cache::get_non_empty_span() cut span" << std::endl;
#endif
    while (addr_start < addr_end) {
        free_list::__next_obj(tail) = addr_start;
        tail = free_list::__next_obj(tail);
        addr_start += size;
    }
    // 恢复锁
    list.__bucket_mtx.lock();
    list.push_front(cur_span);
    return cur_span;
}

void central_cache::release_list_to_spans(void* start, size_t size) {
    size_t index = size_class::bucket_index(size); // 先算一下在哪一个桶里面
    __span_lists[index].__bucket_mtx.lock();
    // 这里要注意，一个桶挂了多个span，这些内存块挂到哪一个span是不确定的
    while (start) {
        // 遍历这个链表
        void* next = free_list::__next_obj(start); // 先记录一下下一个，避免等下找不到了
        span* cur_span = page_cache::get_instance()->map_obj_to_span(start);
        free_list::__next_obj(start) = cur_span->__free_list;
        cur_span->__free_list = start;
        // 处理usecount
        cur_span->__use_count--;
        if (cur_span->__use_count == 0) {
            // 说明这个span切分出去的所有小块都回来了
            // 归还给pagecache
            // 1. 把这一页从cc的这个桶的spanlist中拿掉
            __span_lists[index].erase(cur_span); // 从桶里面拿走
            // 2. 此时不用管这个span的freelist了，因为这些内存本来就是span初始地址后面的，然后顺序也是乱的，直接置空即可
            //      (这里还不太理解)
            cur_span->__free_list = nullptr;
            cur_span->__next = cur_span->__prev = nullptr;
            // 页号，页数是不能动的！
            // 3. 解开桶锁
            __span_lists[index].__bucket_mtx.unlock();
            // 4. 还给pc
            page_cache::get_instance()->__page_mtx.lock();
            page_cache::get_instance()->release_span_to_page(cur_span);
            page_cache::get_instance()->__page_mtx.unlock();
            // 5. 恢复桶锁
            __span_lists[index].__bucket_mtx.lock();
        }
        start = next;
    }
    __span_lists[index].__bucket_mtx.unlock();
}