// thread_pool_v2 测试：异常安全 / 优雅关闭 / 动态扩缩容 / 任务依赖(DAG) / 静态池
// 编译（Linux / WSL / MinGW）：
//   g++ -std=c++17 -Wall -Wextra main_v2.cpp -o test_pool_v2 && ./test_pool_v2
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

#include "thread_pool_v2.hpp"

using namespace std::chrono_literals;

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) { ++g_pass; }                                              \
        else { ++g_fail;                                                     \
            std::printf("  [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #cond); }\
    } while (0)

// 1. 异常安全：异常经future传递，线程池不崩溃
static void test_exception_safety() {
    std::printf("== exception safety ==\n");
    ThreadPool pool(2, 8);

    auto r1 = pool.submit([] { throw std::runtime_error("boom"); return 1; });
    try {
        (void)r1.get();
        //如果没抛（异常丢失），r1.get()返回int，(void)忽略，此CHECK失败
        CHECK(false && "expected exception");
    } catch (const std::runtime_error& e) {
        CHECK(std::string(e.what()) == "boom");
    }

    // 异常任务的下游经依赖链收到同一异常；与失败任务无关的任务正常执行
    auto h  = pool.submit([]() -> int { throw std::logic_error("up fail"); });
    auto d1 = pool.submit_after({h.handle()}, [] { return 2; });
    auto d2 = pool.submit_after({h.handle()}, [] { return 3; });
    auto ok = pool.submit([] { return 42; });
    try {
        (void)d1.get(); CHECK(false);
    } catch (const std::logic_error& e) {
        CHECK(std::string(e.what()) == "up fail");
    }
    try {
        (void)d2.get(); CHECK(false);
    } catch (const std::logic_error&) { CHECK(true); }
    CHECK(ok.get() == 42);

    // 任务外抛任意异常也不崩溃(执行层 catch(...) 兜底)
    auto r2 = pool.submit([]() -> int { throw 42; });
    try { (void)r2.get(); CHECK(false); } catch (int v) { CHECK(v == 42); }

    // 崩溃后池仍可用
    CHECK(pool.submit([] { return 7; }).get() == 7);
}

// 2. 优雅关闭：析构时已提交任务不丢失
static void test_graceful_shutdown() {
    std::printf("== graceful shutdown ==\n");
    std::atomic<int> done{0};
    {
        ThreadPool pool(3, 3);
        for (int i = 0; i < 20; i++)
            //任务通过 [&done] 引用捕获去修改它
            pool.submit([&done] { std::this_thread::sleep_for(10ms); ++done; });
        // 无 wait、无显式 shutdown，直接析构：必须等 20 个任务全部跑完
    }   //pool 在这里析构
    CHECK(done.load() == 20);

    // 放弃模式：未开始的任务被丢弃，future收到broken_promise；已开始的会跑完
    std::atomic<int> ran{0};
    {
        ThreadPool pool(1, 1);
        pool.submit([&ran] { std::this_thread::sleep_for(150ms); ++ran; }); // 占住唯一线程
        std::this_thread::sleep_for(20ms);
        pool.submit([] { return 1; });   // 未开始，将被丢弃
        pool.shutdown(false);
        // 排空模式外丢弃任务的 future 行为在 test_abort_mode_future 中单独验证
    }
    CHECK(ran.load() == 1);   // 已开始执行的任务未被打断
}

// 上面放弃模式的future验证(拆出来避免宏干扰)：broken_promise 属于 future_error
static void test_abort_mode_future() {
    ThreadPool pool(1, 1);
    pool.submit([] { std::this_thread::sleep_for(100ms); });
    auto fut = pool.submit([] { return 1; });
    std::this_thread::sleep_for(20ms);
    pool.shutdown(false);
    try {
        (void)fut.get();
        CHECK(false && "expected broken_promise");
    } catch (const std::future_error& e) {
        CHECK(e.code() == std::future_errc::broken_promise);
    }
}

// 3. 动态扩缩容
static void test_dynamic_scaling() {
    std::printf("== dynamic scaling ==\n");
    ThreadPool pool(2, 8);   // min=2, max=8, keep_alive 默认 3s

    // 压满：8 个 300ms 任务 → 扩容到 8 线程，总耗时应接近 300ms 而非 8*300/2
    auto t0 = std::chrono::steady_clock::now();
    std::vector<TaskResult<int>> rs;
    for (int i = 0; i < 8; i++)
        rs.push_back(pool.submit([] { std::this_thread::sleep_for(300ms); return 1; }));
    int sum = 0;
    for (auto& r : rs) sum += r.get();
    //duration_cast把任意duration显式转换为目标单位
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now() - t0).count();
    CHECK(sum == 8);
    CHECK(ms < 900);   // 2线程至少需1200ms；扩到 8 线程应 ~300ms
    std::printf("  8 tasks x 300ms took %lld ms (expanded)\n", (long long)ms);

    // 空闲 keep_alive 后弹性线程退出，缩回 min
    std::this_thread::sleep_for(3600ms);
    CHECK(pool.active_workers() == 2);

    // resize 运行时调整
    pool.resize(4, 4);
    std::this_thread::sleep_for(50ms);
    CHECK(pool.active_workers() == 4);
    CHECK(pool.submit([] { return 9; }).get() == 9);
}

// 4. 任务依赖(DAG)：菱形依赖 + 工作线程永不阻塞等依赖
static void test_dependencies() {
    std::printf("== dag dependencies ==\n");
    ThreadPool pool(2, 2);
    std::atomic<int> a_val{0};

    auto a = pool.submit([&a_val] { std::this_thread::sleep_for(50ms); a_val = 1; return 1; });
    auto b = pool.submit_after({a.handle()}, [&a_val] { CHECK(a_val.load() == 1); return 2; });
    auto c = pool.submit_after({a.handle()}, [] { return 3; });
    std::vector<TaskHandle> bc{b.handle(), c.handle()};
    auto d = pool.submit_after(bc, [&a_val] { CHECK(a_val.load() == 1); return 4; });

    CHECK(d.get() == 4);   // get 等全链完成
    // 池只有 2 线程但 4 个任务全部完成：依赖等待不占用线程
    CHECK(pool.submit([] { return 5; }).get() == 5);
}

// 5. 压力：多生产者大量任务 + future 收集，结束无泄漏无死锁
static void test_stress() {
    std::printf("== stress ==\n");
    ThreadPool pool(2, 6);
    constexpr int P = 4, N = 500;
    std::vector<std::thread> producers;
    std::vector<long long> results(P, 0);
    for (int p = 0; p < P; p++) {
        producers.emplace_back([&pool, &results, p] {
            std::vector<TaskResult<int>> rs;
            rs.reserve(N);
            for (int i = 1; i <= N; i++) {
                rs.push_back(pool.submit([i] { return i; }));
                // 随取随收，队列不会无限增长
            }
            for (auto &r: rs) results[p] += r.get();
        });
    }
    for (auto& t : producers) t.join();
    long long total = 0;
    for (long long v : results) total += v;
    CHECK(total == (long long)P * N * (N + 1) / 2);
}

int main() {
    try {
        test_exception_safety();
        test_graceful_shutdown();
        test_abort_mode_future();
        test_dynamic_scaling();
        test_dependencies();
        test_stress();
    } catch (...) {
        std::printf("!! unexpected escape exception\n");
        ++g_fail;
    }
    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
