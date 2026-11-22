# thread_pool · C++ 多线程工具集

**生产级 C++ 线程池与无锁队列、内存池实现，轻量可嵌入，适合高并发服务器与底层组件开发**

[![License](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue?style=flat-square)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20WSL-lightgrey?style=flat-square)](.)
[![Threads](https://img.shields.io/badge/threads-pthread%20%7C%20std%3A%3Athread-lightgrey?style=flat-square)](.)

[快速开始](#快速开始) · [模块介绍](#模块介绍) · [核心特性](#核心特性) · [使用示例](#使用示例) · [项目结构](#项目结构) · [参与贡献](#参与贡献)

---

thread_pool 是一个面向 C++ 后端开发的轻量并发原语集合，包含两套线程池实现（pthread C 风格 v1 与 C++17 动态扩缩容 v2）、一套 SPSC 无锁队列以及一套 tcmalloc 风格的多级内存池。所有代码均为单文件自包含、零外部依赖，可直接编译运行，便于集成到服务器、游戏服务端或任何对延迟与吞吐敏感的 C++ 项目中。

## 快速开始

### 环境要求

- g++ / clang++，支持 C++11（v1）或 C++17（v2、无锁队列、内存池）
- pthread（Linux / WSL / MinGW 均可）

### 编译运行

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

## 模块介绍

| 文件 | 语言/风格 | 核心特点 | 适用场景 |
|------|-----------|----------|----------|
| `thread_pool.cpp` | C++11 + pthread，C 风格回调 | 线程数固定、`void(*)(void*)` 任务 | 旧代码兼容、嵌入式 / C 混合项目 |
| `thread_pool_v2.cpp` | C++17，头文件风格单文件 | 动态扩缩容、异常安全、`future` 取结果、优雅关闭、DAG 依赖 | 现代 C++ 后端、任务调度框架 |
| `lock_free_queue.cpp` | C++17 + pthread，SPSC 无锁 | 环形缓冲区、原子内存序、缓存行对齐防伪共享 | 单生产单消费高吞吐通道 |
| `memory_pool.cpp` | C++17，跨平台 | ThreadCache / CentralCache / PageCache 三级设计 | 高频小对象分配/释放 |

## 核心特性

### 1. 异常安全（v2）

任务体通过 `std::packaged_task` 包裹，用户代码抛出的异常被存入关联的 `std::future`，由调用方 `get()` 时重抛；工作线程执行层还设有 `catch(...)` 兜底，确保没有异常逃逸出线程入口。

### 2. 优雅关闭（v2）

析构默认执行 `shutdown(true)`：工作线程只有在任务队列已排空后才退出，已提交任务不丢失；随后析构统一 `join` 所有线程。支持 `shutdown(false)` 放弃模式。

### 3. 动态扩缩容（v2）

- `min_workers` 常驻线程，忙时按"未消费工作量 > 存活线程数"自动扩容，不超过 `max_workers`
- 弹性线程空闲超过 `keep_alive` 后自愿退出，缩回 `min_workers`
- 运行时可通过 `resize(min, max)` 调整上下限

### 4. 任务依赖（DAG，v2）

`submit_after({依赖句柄...}, fn)` 构造有向无环图：下游任务只有在全部上游完成后才入队，工作线程从不阻塞等待依赖。

### 5. SPSC 无锁队列

- 环形缓冲区，容量固定为 2 的幂，通过掩码快速取模
- 生产者/消费者索引分别位于独立缓存行，避免伪共享
- 内置空/满检测、队列大小、生产/消费/失败计数等统计

### 6. 多级内存池

- **ThreadCache**：线程本地空闲链表，小对象分配无锁
- **CentralCache**：按大小等级（8B~4096B，共 512 等级）细粒度锁，批量补给/回收
- **PageCache**：管理连续物理页，支持页拆分与相邻空闲页合并

## 使用示例

### v2 线程池（推荐）

```cpp
#include "thread_pool_v2.cpp"
#include <iostream>
#include <future>

int main() {
    ThreadPool pool(2, 8);
    auto r = pool.submit([](int x) { return x * 2; }, 21);
    std::cout << r.get() << std::endl;  // 42
}
```

### v1 线程池（pthread C 风格）

```cpp
#include "thread_pool.cpp"
#include <cstdio>

static void task(void *args) {
    int n = *(int *)args;
    printf("task done, n = %d\n", n);
}

int main() {
    ThreadPool pool;
    pool.init(4, 100);
    for (int i = 0; i < 10; i++) {
        pool.pushJob(task, &i, sizeof(i));
    }
}
```

### SPSC 无锁队列

```cpp
#include "lock_free_queue.cpp"

int main() {
    SPSC_QUEUE queue;
    spsc_queue_init(&queue);
    Task task{};
    spsc_push(&queue, &task);
    Task out{};
    spsc_pop(&queue, &out);
}
```

### 多级内存池

```cpp
#include "memory_pool.cpp"

int main() {
    void* p = PoolAllocate(32);
    PoolDeallocate(p, 32);
}
```

## API 参考

### ThreadPool（v2）

| 接口 | 说明 |
|------|------|
| `ThreadPool(size_t min, size_t max, ms keep_alive = 3s)` | 动态池构造 |
| `ThreadPool(size_t fixed)` | 静态池便捷构造 |
| `~ThreadPool()` | 默认优雅关闭 |
| `submit(f, args...)` | 提交任务，返回 `TaskResult<R>` |
| `submit_after({handles...}, f, args...)` | 提交带依赖任务 |
| `shutdown(bool drain = true)` | 优雅排空或放弃任务 |
| `resize(min, max)` | 调整线程数上下限 |

### ThreadPool（v1）

| 接口 | 说明 |
|------|------|
| `bool init(int numWorkers, int max_jobs = 10)` | 初始化线程池 |
| `int pushJob(void (*func)(void*), void* arg, int len)` | 提交任务 |

### 无锁队列

| 接口 | 说明 |
|------|------|
| `spsc_queue_init(queue)` | 初始化队列 |
| `spsc_push(queue, &task)` | 生产者推送 |
| `spsc_pop(queue, &task)` | 消费者弹出 |
| `spsc_queue_size(queue)` | 当前队列大小 |

### 内存池

| 接口 | 说明 |
|------|------|
| `PoolAllocate(size_t size)` | 分配内存块 |
| `PoolDeallocate(void* ptr, size_t size)` | 释放内存块 |

## 项目结构

```
thread_pool/
├── thread_pool.cpp          # v1 线程池（pthread + C 风格回调）
├── thread_pool_v2.cpp       # v2 线程池（C++17，动态扩缩容）
├── lock_free_queue.cpp      # SPSC 无锁队列
├── memory_pool.cpp          # 多级内存池
├── README.md                # 项目说明
└── .vscode/                 # VS Code 配置
```

## 常见问题

**Q：v1 与 v2 该如何选型？**  
A：新项目优先使用 v2，它提供现代 C++ 接口、异常安全、动态扩缩容与 DAG 依赖。

**Q：能否在 Windows MSVC 下编译？**  
A：建议在 MinGW / WSL / Linux 下编译。

## 参与贡献

欢迎通过 Issues 报告问题或提交 Pull Request 贡献代码。

## 开源许可

本项目采用 [MIT License](LICENSE) 开源。