<div align="center">

# thread_pool · C++ 多线程工具集

**生产级 C++ 线程池与无锁队列、内存池实现，轻量可嵌入，适合高并发服务器与底层组件开发**

[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20WSL-lightgrey?style=flat-square)](.)
[![Threads](https://img.shields.io/badge/threads-pthread%20%7C%20std%3A%3Athread-lightgrey?style=flat-square)](.)

[快速开始](#快速开始) · [模块介绍](#模块介绍) · [核心特性](#核心特性) · [使用示例](#使用示例) · [项目结构](#项目结构) · [参与贡献](#参与贡献)

</div>

---

thread_pool 是一个面向 C++ 后端开发的轻量并发原语集合，包含两套线程池实现（pthread C 风格 v1 与 C++17 动态扩缩容 v2）、一套 SPSC 无锁队列以及一套 tcmalloc 风格的多级内存池。所有代码均为单文件自包含、零外部依赖，可直接编译运行，便于集成到服务器、游戏服务端或任何对延迟与吞吐敏感的 C++ 项目中。

- **双线程池**：`thread_pool.cpp` 采用 pthread + C 风格回调，线程数固定；`thread_pool_v2.cpp` 采用 C++17，支持动态扩缩容、异常安全、`std::future` 结果获取、优雅关闭和 DAG 任务依赖。
- **SPSC 无锁队列**：`lock_free_queue.cpp` 采用环形缓冲区 + 原子索引 + 缓存行对齐，避免伪共享，适用于单生产者—单消费者的高性能数据传递。
- **多级内存池**：`memory_pool.cpp` 参考 tcmalloc 设计，按 ThreadCache / CentralCache / PageCache 分层，支持跨平台（Windows VirtualAlloc / POSIX mmap），适合小对象高频分配场景。
- **零依赖**：仅依赖 C++ 标准库与 pthread（Windows 下 MinGW 亦可编译），无需第三方框架。

> **项目状态**：仓库处于维护期，核心组件已通过异常安全、优雅关闭、动态扩缩容、依赖 DAG、多生产者并发分配等场景测试，适合作为学习参考或嵌入轻量级项目。

> **已知限制**：
> - `thread_pool.cpp`（v1）为 pthread 实现，原生不支持 Windows MSVC，需在 MinGW/WSL/Linux 下编译；
> - 各 `.cpp` 文件内置 `main()` 与测试代码，编译即可直接运行自测，引入项目时建议提取相应类/函数并自行剥离测试 `main`；
> - 内存池当前为演示级实现，生产环境使用前建议按需补充内存回收与线程退出清理逻辑。

**目录**：[快速开始](#快速开始) · [模块介绍](#模块介绍) · [核心特性](#核心特性) · [使用示例](#使用示例) · [API 参考](#api-参考) · [项目结构](#项目结构) · [编译与测试](#编译与测试) · [常见问题](#常见问题) · [参与贡献](#参与贡献) · [开源许可](#开源许可)

## 快速开始

### 环境要求

- g++ / clang++，支持 C++11（v1）或 C++17（v2、无锁队列、内存池）
- pthread（Linux / WSL / MinGW 均可）

### 编译运行

全部源码为单文件自包含，直接编译即得可执行文件并自带测试：

```bash
# v2 线程池（C++17，推荐）
g++ -std=c++17 -Wall -Wextra -O2 -pthread thread_pool_v2.cpp -o thread_pool_v2
./thread_pool_v2

# v1 线程池（pthread C 风格）
g++ -std=c++11 -Wall -Wextra -O2 -pthread thread_pool.cpp -o thread_pool
./thread_pool

# SPSC 无锁队列
g++ -std=c++17 -Wall -Wextra -O2 -pthread lock_free_queue.cpp -o lock_free_queue
./lock_free_queue

# 多级内存池
g++ -std=c++17 -Wall -Wextra -O2 memory_pool.cpp -o memory_pool
./memory_pool
```

带 AddressSanitizer / ThreadSanitizer 检查（Linux / WSL 推荐；MinGW 无法链接 sanitizer 运行时）：

```bash
g++ -std=c++17 -g -fsanitize=address,undefined -pthread thread_pool_v2.cpp -o thread_pool_v2_asan
./thread_pool_v2_asan
```

## 模块介绍

| 文件 | 语言/风格 | 核心特点 | 适用场景 |
|------|-----------|----------|----------|
| `thread_pool.cpp` | C++11 + pthread，C 风格回调 | 线程数固定、`void(*)(void*)` 任务，`malloc` 拷贝参数，队列有上限 | 旧代码兼容、嵌入式 / C 混合项目 |
| `thread_pool_v2.cpp` | C++17，头文件风格单文件 | 动态扩缩容、异常安全、`future` 取结果、优雅关闭、DAG 依赖、任务句柄 | 现代 C++ 后端、任务调度框架 |
| `lock_free_queue.cpp` | C++17 + pthread，SPSC 无锁 | 环形缓冲区、原子内存序、缓存行对齐防伪共享、生产者/消费者性能测试 | 单生产单消费高吞吐通道 |
| `memory_pool.cpp` | C++17，跨平台 | ThreadCache / CentralCache / PageCache 三级设计，8B~4KB 小对象等级，支持 VirtualAlloc / mmap | 高频小对象分配/释放 |

## 核心特性

### 1. 异常安全（v2）

任务体通过 `std::packaged_task` 包裹，用户代码抛出的异常被存入关联的 `std::future`，由调用方 `get()` 时重抛；工作线程执行层还设有 `catch(...)` 兜底，确保没有异常逃逸出线程入口（逃逸会导致 `std::terminate` 进程崩溃）。失败异常还会沿 DAG 向下游任务级联传播。

### 2. 优雅关闭（v2）

析构默认执行 `shutdown(true)`：工作线程只有在任务队列已排空后才退出，已提交任务不丢失；随后析构统一 `join` 所有线程。支持 `shutdown(false)` 放弃模式，丢弃未开始任务，其 `future` 收到 `std::future_errc::broken_promise`。构造函数若半途抛异常，会先手动回收已创建的 joinable 线程，避免 `std::terminate`。

### 3. 动态扩缩容（v2）

- `min_workers` 常驻线程，忙时按“未消费工作量 > 存活线程数”自动扩容，不超过 `max_workers`；
- 弹性线程空闲超过 `keep_alive` 后自愿退出，缩回 `min_workers`；
- 运行时可通过 `resize(min, max)` 调整上下限。

### 4. 任务依赖（DAG，v2）

`submit_after({依赖句柄...}, fn)` 构造有向无环图：下游任务只有在全部上游完成后才入队，工作线程从不阻塞等待依赖，因此依赖不会饿死线程池。上游任一失败，原始异常会传播给所有下游任务的 `future`，下游用户代码不再执行。

### 5. SPSC 无锁队列

- 环形缓冲区，容量固定为 2 的幂，通过掩码快速取模；
- 生产者/消费者索引分别位于独立缓存行，并附加填充，避免伪共享；
- `head` 释放序 / `tail` 获取序等内存序配合，保证单生产单消费下无锁正确；
- 内置空/满检测、队列大小、生产/消费/失败计数等统计。

### 6. 多级内存池

- **ThreadCache**：线程本地空闲链表，小对象分配无锁；
- **CentralCache**：按大小等级（8B~4096B，共 512 等级）细粒度锁，批量补给/回收；
- **PageCache**：管理连续物理页，支持页拆分与相邻空闲页合并，跨平台申请/释放内存（Windows `VirtualAlloc` / POSIX `mmap`）。

## 使用示例

### v2 线程池（推荐）

```cpp
#include "thread_pool_v2.cpp"   // 单文件自包含，直接包含或编译链接均可
#include <iostream>
#include <future>

int main() {
    // min=2 常驻，忙时扩到 8，空闲 3s 后缩回
    ThreadPool pool(2, 8);

    // 带返回值的任务，异常在 get() 处重抛
    auto r = pool.submit([](int x) { return x * 2; }, 21);
    std::cout << r.get() << std::endl;  // 42

    // DAG 依赖：上游失败时下游不执行用户代码，future 收到原始异常
    auto a  = pool.submit([]() -> int { throw std::runtime_error("up"); });
    auto b  = pool.submit_after({a.handle()}, [] { return 1; });
    try { b.get(); } catch (const std::runtime_error&) {}

    // 运行时调整线程上下限
    pool.resize(4, 16);

    // 析构时自动优雅关闭：排空队列后 join 所有线程
}
```

### v1 线程池（pthread C 风格）

```cpp
#include "thread_pool.cpp"
#include <cstdio>
#include <chrono>
#include <thread>

static void task(void *args) {
    int n = *(int *)args;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printf("task done, n = %d\n", n);
}

int main() {
    ThreadPool pool;
    if (!pool.init(4, 100)) {
        fprintf(stderr, "pool init fail\n");
        return 1;
    }
    for (int i = 0; i < 10; i++) {
        pool.pushJob(task, &i, sizeof(i));
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));  // 等待任务完成
    return 0;  // 析构会回收所有 worker
}
```

### SPSC 无锁队列

```cpp
#include "lock_free_queue.cpp"

int main() {
    SPSC_QUEUE queue;
    spsc_queue_init(&queue);

    Task task{};
    task.task_id = 1;
    task.timestamp = 1000;
    spsc_push(&queue, &task);

    Task out{};
    spsc_pop(&queue, &out);  // task_id == 1
    return 0;
}
```

### 多级内存池

```cpp
#include "memory_pool.cpp"

int main() {
    // 小对象：走 ThreadCache -> CentralCache -> PageCache
    void* p = PoolAllocate(32);
    PoolDeallocate(p, 32);

    // 并发安全：每个线程独立的 ThreadCache，无锁路径
    return 0;
}
```

## API 参考

### ThreadPool（v2）

| 接口 | 说明 |
|------|------|
| `ThreadPool(size_t min, size_t max, ms keep_alive = 3s)` | 动态池构造，要求 `1 <= min <= max` |
| `ThreadPool(size_t fixed)` | 静态池便捷构造（`min == max`，不扩缩容） |
| `~ThreadPool()` | 默认 `shutdown(true)` 优雅关闭 |
| `submit(f, args...)` | 提交任务，返回 `TaskResult<R>`（`get()` 取结果/重抛异常） |
| `submit_after({handles...}, f, args...)` | 提交带依赖任务，全部上游完成后才执行 |
| `shutdown(bool drain = true)` | `true` 优雅排空；`false` 放弃未开始任务，`future` 得 `broken_promise` |
| `resize(min, max)` | 运行时调整线程数上下限 |
| `active_workers() / busy_workers() / pending_tasks()` | 观测接口，用于监控与测试 |

### ThreadPool（v1）

| 接口 | 说明 |
|------|------|
| `bool init(int numWorkers, int max_jobs = 10)` | 初始化线程池与任务队列上限 |
| `int pushJob(void (*func)(void*), void* arg, int len)` | 提交任务，`1` 成功；`-1` 队列满；`-2` 分配失败；`-3` 参数非法 |
| `~ThreadPool()` | 回收所有 worker 并释放未执行任务 |

### 无锁队列

| 接口 | 说明 |
|------|------|
| `spsc_queue_init(queue)` | 初始化队列与计数器 |
| `spsc_push(queue, &task)` | 生产者推送，队列满返回 `-1` |
| `spsc_pop(queue, &task)` | 消费者弹出，队列空返回 `-1` |
| `spsc_queue_size(queue)` | 当前队列中任务数 |
| `spsc_get_status(queue)` | 获取含利用率、计数等完整状态 |

### 内存池

| 接口 | 说明 |
|------|------|
| `PoolAllocate(size_t size)` | 分配内存块，失败返回 `nullptr` |
| `PoolDeallocate(void* ptr, size_t size)` | 释放内存块 |

## 项目结构

```
thread_pool/
├── thread_pool.cpp          # v1 线程池（pthread + C 风格回调，含 main 测试）
├── thread_pool_v2.cpp       # v2 线程池（C++17，动态扩缩容/异常安全/DAG/future，含 main 测试）
├── lock_free_queue.cpp      # SPSC 无锁队列（含生产者-消费者与性能测试）
├── memory_pool.cpp          # 多级内存池（ThreadCache/CentralCache/PageCache，含并发测试）
├── README.md                # 项目说明
├── output/                  # 编译输出示例（.exe）
└── .vscode/                 # VS Code 调试与任务配置
```

## 编译与测试

所有模块自带 `main()`，编译后直接运行即为自测：

```bash
g++ -std=c++17 -Wall -Wextra -O2 -pthread thread_pool_v2.cpp -o thread_pool_v2 && ./thread_pool_v2
g++ -std=c++11 -Wall -Wextra -O2 -pthread thread_pool.cpp -o thread_pool && ./thread_pool
g++ -std=c++17 -Wall -Wextra -O2 -pthread lock_free_queue.cpp -o lock_free_queue && ./lock_free_queue
g++ -std=c++17 -Wall -Wextra -O2 memory_pool.cpp -o memory_pool && ./memory_pool
```

建议在 Linux / WSL 下配合 AddressSanitizer / ThreadSanitizer 检查：

```bash
g++ -std=c++17 -g -fsanitize=address,undefined -pthread thread_pool_v2.cpp -o tp_v2_asan && ./tp_v2_asan
valgrind --tool=memcheck --leak-check=full ./thread_pool_v2
```

> MinGW 的 GCC 无法链接 sanitizer 运行时，sanitizer 检查请在 Linux / WSL 下进行。

## 常见问题

**Q：v1 与 v2 该如何选型？**  
A：新项目优先使用 v2（`thread_pool_v2.cpp`），它提供现代 C++ 接口、异常安全、动态扩缩容与 DAG 依赖；v1 仅适用于已有 pthread / C 风格回调代码的兼容场景。

**Q：能否在 Windows MSVC 下编译？**  
A：`thread_pool.cpp` 与 `lock_free_queue.cpp` 依赖 pthread，建议在 MinGW / WSL / Linux 下编译；`memory_pool.cpp` 已通过 `_WIN32` 条件编译支持 Windows `VirtualAlloc`，可在 MSVC 环境下尝试编译。

**Q：v2 提交任务后线程池析构，任务会丢失吗？**  
A：不会。析构默认执行 `shutdown(true)`，工作线程会执行完队列中已提交的全部任务后再退出；如需丢弃未开始任务，显式调用 `shutdown(false)`。

**Q：依赖任务会占用工作线程等待吗？**  
A：不会。未就绪的依赖任务寄存在 `waiting_` 表中，工作线程从不阻塞等待依赖，最后一个上游完成时下游任务才入队执行。

**Q：内存池支持多线程并发吗？**  
A：支持。`ThreadCache` 使用 `thread_local`，每个线程独立空闲链表；`CentralCache` 按大小等级加细粒度锁，不同等级分配不互相阻塞。

## 参与贡献

欢迎通过 Issues 报告问题或提交 Pull Request 贡献代码。在提交 PR 前，请确保：

1. 新增功能附带对应测试用例；
2. 现有自测 `main` 仍能正常编译运行；
3. 代码风格与现有文件保持一致（缩进、注释、命名约定）。

## 开源许可

本项目采用 [MIT License](LICENSE) 开源。
