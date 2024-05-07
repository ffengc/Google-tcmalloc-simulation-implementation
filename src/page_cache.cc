
#include "../include/page_cache.hpp"
#include "../include/log.hpp"

page_cache page_cache::__s_inst;

// cc向pc获取k页的span
span* page_cache::new_span(size_t k) {
    assert(k > 0 && k < PAGES_NUM);
    // 先检查第k个桶是否有span
    // #ifdef PROJECT_DEBUG
    //     LOG(DEBUG) << "before ***" << std::endl;
    // #endif
    if (!__span_lists[k].empty())
        return __span_lists[k].pop_front(); // ? __span_lists->pop_front();
    // #ifdef PROJECT_DEBUG
    //     LOG(DEBUG) << "after ***" << std::endl;
    // #endif
    // 第k个桶是空的->去检查后面的桶里面有无span，如果有，可以把它进行切分
    for (size_t i = k + 1; i < PAGES_NUM; i++) {
        if (!__span_lists[i].empty()) {
// 可以开始切了
// 假设这个页是n页的，需要的是k页的
// 1. 从__span_lists中拿下来 2. 切开 3. 一个返回给cc 4. 另一个挂到 n-k 号桶里面去
#ifdef PROJECT_DEBUG
            LOG(DEBUG) << "before ***" << std::endl;
#endif
            span* n_span = __span_lists[i].pop_front();
#ifdef PROJECT_DEBUG
            LOG(DEBUG) << "after ***" << std::endl;
#endif
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
            // 存储n_span的首尾页号跟n_span的映射，方便pc回收内存时进行合并查找
            __id_span_map[n_span->__page_id] = n_span;
            __id_span_map[n_span->__page_id + n_span->__n - 1] = n_span;
            // 这里记录映射(简历id和span的映射，方便cc回收小块内存时，查找对应的span)
            for (PAGE_ID j = 0; j < k_span->__n; j++) {
                __id_span_map[k_span->__page_id + j] = k_span;
            }
#ifdef PROJECT_DEBUG
            LOG(DEBUG) << "page_cache::new_span() have span, return" << std::endl;
#endif
            return k_span;
        }
    }
#ifdef PROJECT_DEBUG
    LOG(DEBUG) << "page_cache::new_span() cannot find span, goto os for mem" << std::endl;
#endif
    // 走到这里，说明找不到span了：找os要
    span* big_span = new span;
    void* ptr = system_alloc(PAGES_NUM - 1);
    big_span->__page_id = (PAGE_ID)ptr >> PAGE_SHIFT;
    big_span->__n = PAGES_NUM - 1;
    // 挂到上面去
    __span_lists[PAGES_NUM - 1].push_front(big_span);
    return new_span(k);
}

span* page_cache::map_obj_to_span(void* obj) {
    // 先把页号算出来
    PAGE_ID id = (PAGE_ID)obj >> PAGE_SHIFT; // 这个理论推导可以自行推导一下
    auto ret = __id_span_map.find(id);
    if (ret != __id_span_map.end())
        return ret->second;
    LOG(FATAL) << std::endl;
    assert(false);
    return nullptr;
}

void page_cache::release_span_to_page(span* s) {
    // 对span前后对页尝试进行合并，缓解内存碎片问题
    while (true) {
        PAGE_ID prev_id = s->__page_id - 1; // 前一块span的id一定是当前span的id-1
        // 拿到id如何找span: 之前写好的map能拿到吗？
        // 找到了，如果isuse是false，就能合并了（向前合并+向后合并）
        // 如果遇到了合并大小超过了128页了，也要停止了
        auto ret = __id_span_map.find(prev_id);
        if (ret == __id_span_map.end()) // 前面的页号没有了，不合并了
            break;
        span* prev_span = ret->second;
        if (prev_span->__is_use == true) // 前面相邻页的span在使用，不合并了
            break;
        if (prev_span->__n + s->__n > PAGES_NUM - 1) // 合并出超过128页的span没办法管理，不合并了
            break;
        s->__page_id = prev_span->__page_id;
        s->__n += prev_span->__n;
        __span_lists[prev_span->__n].erase(prev_span); // 防止野指针，删掉
        delete prev_span; // 删掉这个span
    } // 向前合并的逻辑 while end;
    while (true) {
        PAGE_ID next_id = s->__page_id + s->__n - 1; // 注意这里的页号是+n-1了
        auto ret = __id_span_map.find(next_id);
        if (ret == __id_span_map.end()) // 后面的页号没有了
            break;
        span* next_span = ret->second;
        if (next_span->__is_use == true) // 后面相邻页的span在使用，不合并了
            break;
        if (next_span->__n + s->__n > PAGES_NUM - 1) // 合并出超过128页的span没办法管理，不合并了
            break;
        s->__page_id; // 起始页号不用变了，因为是向后合并
        s->__n += next_span->__n;
        __span_lists[next_span->__n].erase(next_span); // 防止野指针，删掉
        delete next_span;
    }
    // 已经合并完成了，把东西挂起来
    __span_lists[s->__n].push_front(s);
    s->__is_use = false;
    // 处理一下映射，方便别人找到我
    __id_span_map[s->__page_id] = s;
    __id_span_map[s->__page_id + s->__n - 1] = s;
}