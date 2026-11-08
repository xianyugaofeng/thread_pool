#include <atomic>
#include <cstdint>
#include <cstring>
#include <cassert>
#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <sys/time.h>
#ifdef _WIN32
#include <windows.h>
#endif


//配置常量
const std::size_t QUEUE_SIZE = 1024;
const std::size_t QUEUE_MASK = QUEUE_SIZE - 1;
const std::size_t CACHE_LINE_SIZE = 64;
const std::size_t MAX_Tasks = 1000;
const std::size_t TASK_DATA_SIZE = 6;   // 任务数据数组大小


// 编译器检查队列大小是否为2的幂
static_assert((QUEUE_SIZE & QUEUE_MASK) == 0, "QUEUE_SIZE must be a power of 2");


// 任务结构体 - 缓存行对齐，避免伪共享
struct alignas(CACHE_LINE_SIZE) Task {
    std::uint32_t task_id; // 任务ID
    std::uint64_t timestamp; // 时间戳
    std::uint64_t data[TASK_DATA_SIZE]; // 任务数据负载
    std::uint32_t reserved; // 保留字段，用来未来扩展
};


// SPSC无锁队列结构体 - 精心设计的缓存行布局
struct SPSC_QUEUE {
    //生产者独占的缓存行（避免伪共享)
    alignas(CACHE_LINE_SIZE) 
    Task task_t[QUEUE_SIZE];          //环形缓冲区

    // 生产者索引
    alignas(CACHE_LINE_SIZE) 
    std::atomic<unsigned int> head; 
    char producer_padding[CACHE_LINE_SIZE - sizeof(std::atomic<unsigned int>)];

    // 消费者索引
    alignas(CACHE_LINE_SIZE) 
    std::atomic<unsigned int> tail; 
    char consumer_padding[CACHE_LINE_SIZE - sizeof(std::atomic<unsigned int>)];

    // 共享统计信息（单独缓存行）
    alignas(CACHE_LINE_SIZE)
    std::atomic<unsigned int> produced_count;
    std::atomic<unsigned int> consumed_count;
    std::atomic<unsigned int> push_fail_count;
    std::atomic<unsigned int> pop_fail_count;
};


//队列状态信息结构体
struct Queue_Status {
    std::uint32_t size; // 队列大小
    std::uint32_t capacity; // 队列容量
    std::uint32_t head; // 生产者索引
    std::uint32_t tail; // 消费者索引

    std::uint64_t produced_count; // 生产的任务总数
    std::uint64_t consumed_count; // 消费的任务总数
    std::uint64_t push_fail_count; // 推送失败的次数
    std::uint64_t pop_fail_count; // 弹出失败的次数
    float utilization; // 队列利用率
};


//初始化无锁队列
static void spsc_queue_init(SPSC_QUEUE* queue) {
    if(queue == nullptr) {
        perror("init queue fail");
        return;
    }

    queue->head.store(0, std::memory_order_relaxed);
    queue->tail.store(0, std::memory_order_relaxed);
    queue->produced_count.store(0, std::memory_order_relaxed);
    queue->consumed_count.store(0, std::memory_order_relaxed);
    queue->push_fail_count.store(0, std::memory_order_relaxed);
    queue->pop_fail_count.store(0, std::memory_order_relaxed);
}


//初始化无锁队列统计计数器
static void spsc_queue_reset_stats(SPSC_QUEUE* queue) {
    if(queue == nullptr) {
        perror("reset stats fail");
        return;
    }

    queue->produced_count.store(0, std::memory_order_relaxed);
    queue->consumed_count.store(0, std::memory_order_relaxed);
    queue->push_fail_count.store(0, std::memory_order_relaxed);
    queue->pop_fail_count.store(0, std::memory_order_relaxed);
}


//完全重置队列
static void spsc_queue_clear(SPSC_QUEUE* queue) {
    if(queue == nullptr) {
        perror("reset queue fail");
        return;
    }

    queue->head.store(0, std::memory_order_relaxed);
    queue->tail.store(0, std::memory_order_relaxed);
    spsc_queue_reset_stats(queue);
}


//生产者从队列中获取任务
static int spsc_push(SPSC_QUEUE* queue, const Task* task) {
    if (queue == nullptr || task == nullptr) {
        perror("push fail");
        return -1;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_relaxed);
    const unsigned int next_head = (current_head + 1) & QUEUE_MASK;
    const unsigned int current_tail = queue->tail.load(std::memory_order_acquire);

    if (next_head == current_tail) {
        //队列已满
        queue->push_fail_count.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
    // 将任务复制到队列中
    queue->task_t[current_head] = *task;
    queue->head.store(next_head, std::memory_order_release);
    queue->produced_count.fetch_add(1, std::memory_order_relaxed);
    
    return 0;
}


//消费者从队列中获取任务
static int spsc_pop(SPSC_QUEUE* queue, Task* task) {
    if (queue == nullptr || task == nullptr) {
        perror("pop fail");
        return -1;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_acquire);
    const unsigned int current_tail = queue->tail.load(std::memory_order_relaxed);

    if (current_head == current_tail) {
        // 队列为空
        queue->pop_fail_count.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }

    // 从队列中弹出任务
    *task = queue->task_t[current_tail];
    const unsigned int next_tail = (current_tail + 1) & QUEUE_MASK;
    queue->tail.store(next_tail, std::memory_order_release);
    queue->consumed_count.fetch_add(1, std::memory_order_relaxed);

    return 0;
}


//检查队列是否为空
static bool spsc_is_empty(SPSC_QUEUE* queue) {
    if (queue == nullptr) {
        perror("check empty fail");
        return false;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_acquire);
    const unsigned int current_tail = queue->tail.load(std::memory_order_acquire);
    return current_head == current_tail;
}


//检查队列是否为满
[[mabye_unused]] static bool spsc_is_full(SPSC_QUEUE* queue) {
    if (queue == nullptr) {
        perror("check full fail");
        return false;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_acquire);
    const unsigned int next_head = (current_head + 1) & QUEUE_MASK;
    const unsigned int current_tail = queue->tail.load(std::memory_order_acquire);
    return next_head == current_tail;
}


//获取当前队列详细数量
static int spsc_queue_size(SPSC_QUEUE* queue) {
    if (queue == nullptr) {
        perror("get size fail");
        return -1;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_acquire);
    const unsigned int current_tail = queue->tail.load(std::memory_order_acquire);
    if (current_head >= current_tail) {
        return current_head - current_tail;
    } else {
        return QUEUE_SIZE - (current_tail - current_head);
    }
}

//获取队列状态信息
static Queue_Status spsc_get_status(SPSC_QUEUE* queue) {
    Queue_Status status;
    if (queue == nullptr) {
        perror("get status fail");
        return status;
    }

    const unsigned int current_head = queue->head.load(std::memory_order_acquire);
    const unsigned int current_tail = queue->tail.load(std::memory_order_acquire);
    status.size = spsc_queue_size(queue);
    status.capacity = QUEUE_SIZE - 1; // 队列容量为 QUEUE_SIZE - 1
    status.head = current_head;
    status.tail = current_tail;
    status.produced_count = queue->produced_count.load(std::memory_order_relaxed);
    status.consumed_count = queue->consumed_count.load(std::memory_order_relaxed);
    status.push_fail_count = queue->push_fail_count.load(std::memory_order_relaxed);
    status.pop_fail_count = queue->pop_fail_count.load(std::memory_order_relaxed);
    status.utilization = static_cast<float>(status.size) / static_cast<float>(status.capacity);

    return status;
}


void performance_test(void) {
    SPSC_QUEUE test_queue;
    spsc_queue_init(&test_queue);

    printf("Queue initialized\n");

    Task test_task;
    unsigned int i;
    struct timeval start, end;
    long long elapsed_time;

    //空队列检查性能
    gettimeofday(&start, NULL);
    for (i = 0; i < 1000000; ++i) {
        spsc_is_empty(&test_queue);
    }
    gettimeofday(&end, NULL);
    elapsed_time = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("空队列检查性能: %lld microseconds\n", elapsed_time);

    //队列大小检查性能
    gettimeofday(&start, NULL);
    for (i = 0; i < 1000000; ++i) {
        spsc_queue_size(&test_queue);
    }

    gettimeofday(&end, NULL);
    elapsed_time = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("队列大小检查性能: %lld microseconds\n", elapsed_time);

    //测试单线程推送性能
    gettimeofday(&start, NULL);
    unsigned int push_count = 0;
    for (i = 0; i < QUEUE_SIZE - 1; ++i) {
        test_task.task_id = i;
        test_task.timestamp = static_cast<std::uint64_t>(time(NULL));
        if (spsc_push(&test_queue, &test_task) == 0) {
            push_count++;
        }
    }
    gettimeofday(&end, NULL);
    elapsed_time = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("单线程推送性能:%u操作成功 %lld microseconds\n", push_count, elapsed_time);

    //测试单线程弹出性能
    gettimeofday(&start, NULL);
    unsigned int pop_count = 0;
    Task temp_task;
    for(i = 0; i < QUEUE_SIZE - 1; ++i) {
        if (spsc_pop(&test_queue, &temp_task) == 0) {
            pop_count++;
        }
    }
    gettimeofday(&end, NULL);
    elapsed_time = (end.tv_sec - start.tv_sec) * 1000000LL + (end.tv_usec - start.tv_usec);
    printf("单线程弹出性能:%u操作成功 %lld microseconds\n", pop_count, elapsed_time);

    //验证性能测试后队列的状态
    Queue_Status queue_status = spsc_get_status(&test_queue);
    printf("队列状态: size=%u, produced_count=%llu, consumed_count=%llu, push_fail_count=%llu, pop_fail_count=%llu, utilization=%.2f\n",
           queue_status.size,
           queue_status.produced_count,
           queue_status.consumed_count,
           queue_status.push_fail_count,
           queue_status.pop_fail_count,
           queue_status.utilization);
    
    printf("性能测试完成\n");
}


void* produce_thread(void* arg) {
    SPSC_QUEUE* queue = (SPSC_QUEUE*)arg;
    Task task;
    unsigned int i;
    
    for(i = 0; i < MAX_Tasks; ++i) {
        task.task_id = i;
        task.timestamp = static_cast<std::uint64_t>(time(NULL));
        for (unsigned int j = 0; j < TASK_DATA_SIZE; ++j) {
            task.data[j] = i * 10 + j; // 填充任务数据
        }
        
        while (spsc_push(queue, &task) != 0) {
            // 队列满时，等待一段时间再尝试
            asm volatile("pause" ::: "memory"); // CPU友好等待
        }

        if (i % 100 == 0) {
            printf("Produced task %u\n", i);
        }
    }
    printf("✅生产者线程完成，共处理%u个任务\n", i);
    return NULL;
}


void* consume_thread(void* arg) {
    SPSC_QUEUE* queue = (SPSC_QUEUE*)arg;
    Task task;
    unsigned int i;

    
    for(i = 0; i < MAX_Tasks; ++i) {
        while(spsc_is_empty(queue)) {
            // 等待队列非空
            asm volatile("pause" ::: "memory"); // CPU友好等待
        }

        for(unsigned int j = 0; j < TASK_DATA_SIZE; ++j) {
            // 验证任务数据
            task.data[j] = task.task_id * 10 + j;
        }

        if (i % 100 == 0) {
            printf("Consumed task %u with timestamp %llu\n", task.task_id, task.timestamp);
        }
    }

    printf("✅消费者线程完成，共处理%u个任务\n", i);
    return NULL;
}

int main(void) {
    SPSC_QUEUE queue;
    spsc_queue_init(&queue);

    #ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    #endif
    performance_test(); // 执行性能测试
    printf("性能测试完成，开始生产者-消费者测试\n");

    spsc_queue_clear(&queue); // 清空队列，重置状态

    pthread_t producer_thread, consumer_thread;

    // 创建生产者线程
    if (pthread_create(&producer_thread, NULL, (void* (*)(void*))produce_thread, &queue) != 0) {
        perror("Failed to create producer thread");
        return -1;
    }

    // 创建消费者线程
    if (pthread_create(&consumer_thread, NULL, (void* (*)(void*))consume_thread, &queue) != 0) {
        perror("Failed to create consumer thread");
        return -1;
    }

    // 等待生产者线程完成
    pthread_join(producer_thread, NULL);
    // 等待消费者线程完成
    pthread_join(consumer_thread, NULL);

    // 获取最终队列状态
    Queue_Status final_status = spsc_get_status(&queue);
    printf("Final Queue Status: size=%u, produced_count=%llu, consumed_count=%llu, push_fail_count=%llu, pop_fail_count=%llu, utilization=%.2f\n",
           final_status.size,
           final_status.produced_count,
           final_status.consumed_count,
           final_status.push_fail_count,
           final_status.pop_fail_count,
           final_status.utilization
    );

    return 0;
}