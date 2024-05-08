

#include "./include/tcmalloc.hpp"
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <thread>

void alloc1() {
    for (size_t i = 0; i < 5; i++) {
        void* ptr = tcmalloc(6);
        std::cout << "thread id: " << std::this_thread::get_id() << std::endl;
    }
}
void tls_test() {
    std::thread t1(alloc1);
    std::thread t2(alloc1);
    t1.join();
    t2.join();
}

void test_alloc1() {
    std::cout << "call tcmalloc(1)" << std::endl;
    void* ptr = tcmalloc(8 * 1024);
    std::cout << "call tcmalloc(2)" << std::endl;
    ptr = tcmalloc(10);
    std::cout << "call tcmalloc(3)" << std::endl;
    ptr = tcmalloc(2);
    std::cout << "call tcmalloc(4)" << std::endl;
    ptr = tcmalloc(1);
    std::cout << "call tcmalloc(5)" << std::endl;
    ptr = tcmalloc(1);
    std::cout << "call tcmalloc(6)" << std::endl;
    ptr = tcmalloc(5);
    std::cout << "call tcmalloc(7)" << std::endl;
    ptr = tcmalloc(1);
}

void test_alloc2() {
    for (size_t i = 0; i < 1024; ++i) {
        void* p1 = tcmalloc(6);
    }
    void* p2 = tcmalloc(6); // 这一次一定会找新的span
}

void test_dealloc(int alloc_times = 10) {
    // 创建随机数生成器
    std::random_device rd;
    std::mt19937 gen(rd()); // 以随机设备作为种子
    std::uniform_int_distribution<> distrib(1, 127 * 100); // 设置随机数分布范围1-127
    // 生成并输出随机数
    std::map<void*, size_t> s;
    for (int i = 0; i < alloc_times; i++) {
        size_t sz = distrib(gen);
        std::cout << sz << std::endl;
        void* ptr = tcmalloc(sz);
        std::cout << "malloc successful" << std::endl;
        s.insert({ ptr, sz }); // 申请随机值
    }
    for (auto& e : s) {
        tcfree(e.first /*, e.second*/);
    }
}

void test_multi_thread() {
    std::thread t1(std::bind(test_dealloc, 1000));
    // std::thread t2(std::bind(test_dealloc, 20));
    t1.join();
    // t2.join();
    std::cout << "run successful" << std::endl;
}

void run() {
    void* ptr = tcmalloc(5949);
    tcfree(ptr);
    std::cout << "run successful" << std::endl;
}

void big_alloc() {
    std::thread t1(run);
    t1.join();
}

int main() {
// std::cout << "haha" << std::endl;
// big_alloc();
#ifdef __aarch64__
    std::cout << "64" << std::endl;
#elif defined(__arm__)
    std::cout << "32" << std::endl;
#else
    std::cout << "unknown sys" << std::endl;
#endif
    return 0;
}