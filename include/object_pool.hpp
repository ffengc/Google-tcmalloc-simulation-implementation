

#ifndef __YUFC_OBJECT_POOL_HPP__
#define __YUFC_OBJECT_POOL_HPP__

#include <iostream>
#include <vector>
#include "./common.hpp"

#define __DEFAULT_KB__ 128



template <class T>
class object_pool {
private:
    char* __memory = nullptr; // char 方便切
    size_t __remain_bytes = 0; // 大块内存在切的过程中剩余的字节数
    void* __free_list = nullptr; // 还回来的时候形成的自由链表
public:
    T* new_() {
        T* obj = nullptr;
        // 不够空间 首选是把还回来的内存块对象进行再次利用
        if (__free_list) {
            // 头删
            void* next = *((void**)__free_list);
            obj = (T*)__free_list;
            __free_list = next;
            return obj;
        }
        if (__remain_bytes < sizeof(T)) {
            // 空间不够了，要重新开一个空间
            __remain_bytes = __DEFAULT_KB__ * 1024;
            __memory = (char*)malloc(__remain_bytes);
            if (__memory == nullptr) {
                throw std::bad_alloc();
            }
        }
        obj = (T*)__memory;
        size_t obj_size = sizeof(T) < sizeof(void*) ? sizeof(void*) : sizeof(T);
        __memory += obj_size;
        __remain_bytes -= obj_size;
        new (obj) T;
        return obj;
    }
    void delete_(T* obj) {
        obj->~T();
        *(void**)obj = __free_list;
        __free_list = obj;
    }
};

#endif