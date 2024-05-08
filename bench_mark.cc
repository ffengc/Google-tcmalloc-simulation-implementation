

#include "./include/tcmalloc.hpp"
#include <atomic>
#include <thread>

// ntimes 一轮申请和释放内存的次数
// rounds 轮次
void BenchmarkMalloc(size_t ntimes, size_t nworks, size_t rounds) {
    std::vector<std::thread> vthread(nworks);
    std::atomic<size_t> malloc_costtime(0);
    std::atomic<size_t> free_costtime(0);
    for (size_t k = 0; k < nworks; ++k) {
        vthread[k] = std::thread([&, k]() {
            std::vector<void*> v;
            v.reserve(ntimes);
            for (size_t j = 0; j < rounds; ++j) {
                size_t begin1 = clock();
                for (size_t i = 0; i < ntimes; i++) {
                    v.push_back(malloc(16));
                    // v.push_back(malloc((16 + i) % 8192 + 1));
                }
                size_t end1 = clock();
                size_t begin2 = clock();
                for (size_t i = 0; i < ntimes; i++) {
                    free(v[i]);
                }
                size_t end2 = clock();
                v.clear();
                malloc_costtime += (end1 - begin1);
                free_costtime += (end2 - begin2);
            }
        });
    }
    for (auto& t : vthread) {
        t.join();
    }
    std::cout << nworks << "threads run" << rounds << " times, each round malloc " << ntimes << " times, cost: " << malloc_costtime.load() << "ms\n";
    std::cout << nworks << "threads run" << rounds << " times, each round free " << ntimes << " times,  cost: " << free_costtime.load() << " ms\n";
    std::cout << nworks << "threads run malloc and free " << nworks * rounds * ntimes << " time, total cost: " << malloc_costtime.load() + free_costtime.load() << " ms\n";
}

// 单轮次申请释放次数 线程数 轮次
void BenchmarkConcurrentMalloc(size_t ntimes, size_t nworks, size_t rounds) {
    std::vector<std::thread> vthread(nworks);
    std::atomic<size_t> malloc_costtime(0);
    std::atomic<size_t> free_costtime(0);
    for (size_t k = 0; k < nworks; ++k) {
        vthread[k] = std::thread([&]() {
            std::vector<void*> v;
            v.reserve(ntimes);
            for (size_t j = 0; j < rounds; ++j) {
                size_t begin1 = clock();
                for (size_t i = 0; i < ntimes; i++) {
                    v.push_back(tcmalloc(16));
                    // v.push_back(ConcurrentAlloc((16 + i) % 8192 + 1));
                }
                size_t end1 = clock();
                size_t begin2 = clock();
                for (size_t i = 0; i < ntimes; i++) {
                    tcfree(v[i]);
                }
                size_t end2 = clock();
                v.clear();
                malloc_costtime += (end1 - begin1);
                free_costtime += (end2 - begin2);
            }
        });
    }
    for (auto& t : vthread) {
        t.join();
    }
    std::cout << nworks << "threads run" << rounds << " times, each round malloc " << ntimes << " times, cost: " << malloc_costtime.load() << "ms\n";
    std::cout << nworks << "threads run" << rounds << " times, each round free " << ntimes << " times,  cost: " << free_costtime.load() << " ms\n";
    std::cout << nworks << "threads run tcmalloc and tcfree " << nworks * rounds * ntimes << " time, total cost: " << malloc_costtime.load() + free_costtime.load() << " ms\n";
}

int main() {
    size_t n = 1000;
    BenchmarkConcurrentMalloc(n, 4, 10);
    std::cout << std::endl
              << std::endl;
    BenchmarkMalloc(n, 4, 10);
    return 0;
}