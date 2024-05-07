

#include "./include/tcmalloc.hpp"
#include <iostream>
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

int main() {
    test_alloc2();
    return 0;
}