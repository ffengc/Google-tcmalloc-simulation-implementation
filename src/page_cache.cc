
#include "../include/page_cache.hpp"
#include "../include/log.hpp"

page_cache page_cache::__s_inst;

// cc向pc获取k页的span
span* page_cache::new_span(size_t k) {
    assert(k > 0 && k < PAGES_NUM);
    // 先检查第k个桶是否有span
    if (!__span_lists[k].empty())
        return __span_lists->pop_front();
    // 第k个桶是空的->去检查后面的桶里面有无span，如果有，可以把它进行切分
    for (size_t i = k + 1; i < PAGES_NUM; i++) {
        if (!__span_lists[i].empty()) {
            // 可以开始切了
            // 假设这个页是n页的，需要的是k页的
            // 1. 从__span_lists中拿下来 2. 切开 3. 一个返回给cc 4. 另一个挂到 n-k 号桶里面去
            span* n_span = __span_lists[i].pop_front();
            span* k_span = new span;
            // 在n_span头部切除k页下来
            k_span->__page_id = n_span->__page_id; // <1>
            k_span->__n = k; // <2>
            n_span->__page_id += k; // <3>
            n_span->__n -= k; // <4>
            /**
             * 这里要好好理解一下 100 ------ 101 ------- 102 ------
             * 假设n_span从100开始，大小是3
             * 切出来之后k_span就是从100开始了，所以<1>
             * 切出来之后k_span就有k页了，所以 <2>
             * 切出来之后n_span就是从102开始了，所以 <3>
             * 切出来之后n_span就变成__n-k页了，所以 <4>
             */
            // 剩下的挂到相应位置
            __span_lists[n_span->__n].push_front(n_span);
            LOG(DEBUG) << "page_cache::new_span() have span, return" << std::endl;
            return k_span;
        }
    }
    LOG(DEBUG) << "page_cache::new_span() cannot find span, goto os for mem" << std::endl;
    // 走到这里，说明找不到span了：找os要
    span* big_span = new span;
    void* ptr = system_alloc(PAGES_NUM - 1);
    big_span->__page_id = (PAGE_ID)ptr >> PAGE_SHIFT;
    big_span->__n = PAGES_NUM - 1;
    // 挂到上面去
    __span_lists[PAGES_NUM - 1].push_front(big_span);
    return new_span(k);
}