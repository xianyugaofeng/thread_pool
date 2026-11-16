// 线程池测试
// 编译（Linux / WSL / MinGW）：
//   g++ -std=c++11 -Wall -Wextra main.cpp -pthread -o test_pool && ./test_pool
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>
#include "thread_pool.hpp"

static void task(void *args) {
    int n = *(int *)args;
    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 模拟耗时任务
    printf("task done, n = %d\n", n);
}

int main() {
    ThreadPool pool;

    if (!pool.init(4, 100)) {
        fprintf(stderr, "pool init fail\n");
        return 1;
    }

    // 连续提交10个任务（多于worker数，覆盖排队与唤醒逻辑）
    for (int i = 0; i < 10; i++) {
        int ret = pool.pushJob(task, &i, sizeof(i));
        printf("pushJob(%d) -> %d\n", i, ret);
    }

    // 等任务执行完
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // 析构：worker 应全部退出、无内存泄漏/崩溃
    printf("shutting down...\n");
    return 0;
}
