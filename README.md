# thread_pool

两个线程池实现：

| 文件 | 风格 | 特点 |
|------|------|------|
| `thread_pool.hpp` | pthread + C 风格回调 | 静态线程数，`void(*)(void*)` 任务，malloc 拷贝参数 |
| `thread_pool_v2.hpp` | C++17，header-only | 动态扩缩容、异常安全、优雅关闭、任务依赖(DAG)、`future` 取结果 |

## 编译测试

v2（Linux / WSL / MinGW）：

```bash
g++ -std=c++17 -Wall -Wextra -O1 -pthread main_v2.cpp -o test_pool_v2 && ./test_pool_v2
```

v1（保持不变）：

```bash
g++ -std=c++11 -Wall -Wextra main.cpp -pthread -o test_pool && ./test_pool
```

建议在 WSL/Linux 下配合 AddressSanitizer / ThreadSanitizer / valgrind 检查
（MinGW 的 GCC 无法链接 sanitizer 运行时）：

```bash
g++ -std=c++17 -g -fsanitize=address,undefined main_v2.cpp -pthread -o test_pool_v2 && ./test_pool_v2
g++ -std=c++11 -g -fsanitize=address,thread main.cpp -pthread -o test_pool && ./test_pool
valgrind --tool=memcheck --leak-check=full ./test_pool_v2
```

## thread_pool_v2 用法

```cpp
ThreadPool pool(2, 8);                      // min=2 常驻, 忙时扩到 8, 空闲 3s 缩回
auto r = pool.submit([](int x) { return x * 2; }, 21);
int v = r.get();                            // 42；任务抛出的异常在 get() 处重抛

auto a  = pool.submit([]() -> int { throw std::runtime_error("up"); });
auto b  = pool.submit_after({a.handle()}, [] { return 1; });  // DAG 依赖
// a 失败 → b 不执行用户代码，get() 收到 a 的原始异常

pool.resize(4, 16);                         // 运行时调整上下限
pool.shutdown(true);                        // 优雅：排空已提交任务再退出(析构默认)
pool.shutdown(false);                       // 放弃：丢弃未开始的任务(broken_promise)
```

## 三个核心问题的实现位置

1. **异常安全**：任务体包在 `packaged_task` 里，异常存入 future 由 `get()` 重抛；
   执行层 `execute_job` 的 `catch(...)` 保证无异常逃逸线程入口（逃逸即 `std::terminate`）。
   失败异常同时记入 `Job::error`，沿 DAG 向下游级联传播。
2. **优雅关闭**：worker 唯一退出条件是 `stop_ && queue_ 空已排空`，析构后统一 `join`，
   已提交任务不丢失；放弃模式销毁 `packaged_task` 让未运行任务的 future 收到
   `broken_promise`。构造函数半途抛异常时手动回收已创建线程（joinable 线程析构会 terminate）。
3. **静态 vs 动态池 & 依赖**：静态池=单参构造（min==max，永不扩缩）；动态池按
   "未消费工作量 > 线程数"扩容（与线程唤醒时序无关），弹性线程空闲超 keep_alive 自愿缩容。
   依赖任务寄存在 `waiting_` 表，最后一个上游完成时才入队，工作线程从不阻塞等依赖。
