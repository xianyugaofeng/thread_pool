#include <iostream>
#include <cstdlib>
#include <cstddef>
#include <mutex>
#include <thread>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
// 跨平台支持
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

inline size_t AlignUp(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}


// 管理连续的物理页，分割为多个小块
struct Span {
    size_t page_id; // 起始页号
    size_t page_count; // 页数量
    size_t block_size; // 每个小块的大小
    size_t free_count; // 空闲块数量
    void* free_list; // 空闲块链表
    Span* prev;  // 双向链表指针
    Span* next; 
    bool is_used;   // 是否被使用
    Span(): page_id(0), page_count(0), block_size(0), free_count(0), 
            free_list(nullptr), prev(nullptr), next(nullptr), is_used(false) {};
};


// SpanList：管理同大小等级的Span（双向链表）
// 每个SpanList对应一个互斥锁，避免不同大小等级的分配操作互相阻塞
class SpanList {
    public:
        SpanList() : head_(new Span), tail_(new Span) {
            head_->next = tail_;
            tail_->prev = head_;
        }
        ~SpanList() {
            Span* cur = head_;
            while (cur != nullptr) {
                Span* next = cur->next;
                delete cur;
                cur = next;
            }
        }
        //插入尾部
        void PushBack(Span* span) {
            span->next = tail_;
            span->prev = tail_->prev;
            tail_->prev->next = span;
            tail_->prev = span;
        }
        //弹出头部
        Span* PopFront() {
            if (head_->next == tail_) return nullptr;
            Span* span = head_->next;
            RemoveSpan(span);
            return span;
        }
        // 移除指定Span
        void RemoveSpan(Span* span) {
            span->next->prev = span->prev;
            span->prev->next = span->next;
            span->prev = nullptr;
            span->next = nullptr;
        }
        bool Empty() const {
            return head_->next == tail_;
        }
        Span* head_;
        Span* tail_;
};


class MemoryPool {
    private:
        // 空闲块结构（嵌入式链表，无额外内存开销）
        // 单线程版本
        struct FreeBlock {
            FreeBlock *next;
        };
        FreeBlock* m_free_list;   // 空闲块链表头指针
        size_t m_block_size;      // 内存块大小
    public:
        explicit MemoryPool(size_t block_size, size_t align = 8, size_t prealloc = 1024)
            : m_free_list(nullptr), m_block_size(block_size), m_align(align) {
            // 块大小向上对齐到align整数倍
            m_block_size = AlignUp(m_block_size, m_align);
            // 预分配第一批块
            if (prealloc > 0) {
                PreallocateBlocks(prealloc);
            }
        }
        // 折构函数：释放所有预分配的大内存块
        ~MemoryPool() {
            //遍历所有预分配的大内存块，调用free释放
            for (void *pool : m_pools) {
                std::free(pool);
            }
        };
        // 分配内存块
        void* Allocate() {
            std::lock_guard<std::mutex> lock(m_mutex);  //多线程安全
            if (!m_free_list) {
                // 空闲链表为空，动态扩容(指数扩容，当前总块数的2倍)
                size_t expand_size = m_pools.empty() ? 1024 : m_pools.size() * 1024;
                PreallocateBlocks(expand_size);
            }
            FreeBlock *block = m_free_list;
            m_free_list = block->next;
            return block;
        };
        // 释放内存块
        void Deallocate(void *ptr) {
            std::lock_guard<std::mutex> lock(m_mutex);   // 多线程安全

            FreeBlock* block = static_cast<FreeBlock*>(ptr);
            block->next = m_free_list;
            m_free_list = block;
        };   
    private:
        // 预分配内存块
        void PreallocateBlocks(size_t num_blocks) {
            // 计算需要申请的连续内存大小
            size_t pool_size = m_block_size * num_blocks;
            // 向系统申请连续内存
            void *pool = std::malloc(pool_size);
            if (!pool) {
                throw std::bad_alloc();   // 申请失败，抛出异常
            }
            // 保存大内存块指针，便于析构时释放
            m_pools.push_back(pool);     
            // 将连续内存分割为num_blocks个块，构建空闲链表
            FreeBlock* current = static_cast<FreeBlock*>(pool);
            FreeBlock* start = current;
            for (size_t i = 0; i < num_blocks - 1; ++i) {
                // 下一个块的地址 = 当前块地址 + 块大小
                current->next = reinterpret_cast<FreeBlock*>(reinterpret_cast<char*>(current) + m_block_size);
                current = current->next;
            }
            current->next = nullptr;  // 最后一个块的next指针置空
            if (m_free_list) {
                // 将新分配的块链表插入到现有空闲链表的前面
                current->next = m_free_list;
            }
            m_free_list = start;  // 更新空闲链表头指针
            std::cout << "Preallocated " << num_blocks << " blocks (size: " << m_block_size 
                      << "B), total pools: " << m_pools.size() << std::endl;
        };
        // 禁止拷贝构造和赋值操作
        MemoryPool(const MemoryPool&) = delete;
        MemoryPool& operator=(const MemoryPool&) = delete;
    private:
        size_t m_align;           // 对齐方式
        std::vector<void*> m_pools;  // 已分配的内存块列表(用于折构时释放)
        std::mutex m_mutex;  // 互斥锁，保护空闲块链表
};


class PageCache {
    private:
        static const size_t kMaxPageCount = 128;   //最大支持128页
        SpanList m_span_lists[kMaxPageCount + 1];   // 索引0未使用，1~128对应页数
        std::mutex mutex_;      // 全局锁(竞争度小)
        size_t m_page_size;     // 系统页的大小
        std::unordered_map<size_t, Span*> page_id_to_span;      //页号到Span的映射
        PageCache() {
            //获取系统页大小
            #ifdef _WIN32
            SYSTEM_INFO si;
            GetSystemInfo(&si);
            m_page_size = si.dwPageSize;
        #else
            m_page_size = sysconf(_SC_PAGESIZE);
        #endif    
        }
        PageCache(const PageCache&) = delete;
        PageCache& operator=(const PageCache&) = delete;
    public:
        static PageCache& GetInstance() {
            static PageCache instance;
            return instance;
        }
        // 申请n页内存
        Span* AllocatePages(size_t n) {
            if (n == 0 || n > kMaxPageCount) {
                throw std::bad_alloc();
            }
            std::lock_guard<std::mutex> lock(mutex_);
            // 1. 查找对应页数的SpanList
            if (!m_span_lists[n].Empty()) {
                Span* span = m_span_lists[n].PopFront();
                for (size_t i = 0; i < span->page_count; ++i) {
                    page_id_to_span[span->page_id + i] = span;
                }
                span->is_used = true;
                return span;
            }
            // 2. 查找更大的Span，分割使用
            for(size_t j = n + 1; j <= kMaxPageCount; ++j) {
                if (!m_span_lists[j].Empty()) {
                    Span* big_span = m_span_lists[j].PopFront();
                    Span* small_span = new Span();
                    small_span->page_count = n;
                    small_span->page_id = big_span->page_id;
                    // 剩余Span
                    big_span->page_id = small_span->page_id + n;
                    big_span->page_count -= n;
                    m_span_lists[big_span->page_count].PushBack(big_span);
                    //记录页号到Span的映射
                    for(size_t k = 0; k < n; ++k) {
                        page_id_to_span[small_span->page_id + k] = small_span;
                    }
                    small_span->is_used = true;
                    return small_span;
                }
            }
            // 3. 向系统申请内存
            void* ptr = SystemAllocate(n);
            if (!ptr) throw std::bad_alloc();
            // 创建新Span
            Span* span = new Span();
            span->page_count = n;
            span->page_id = reinterpret_cast<size_t>(ptr) / m_page_size;
            // 记录页号映射
            for(size_t i = 0; i < span->page_count; ++i) {
                page_id_to_span[span->page_id + i] = span;
            }
            span->is_used = true;
            return span;
        };      
        // 归还Span
        void DeallocatePages(Span* span) {
            if (!span || !span->is_used) return;
            std::lock_guard<std::mutex> lock(mutex_);
            span->is_used = false;
            span->free_count = 0;
            span->free_list = nullptr;
            //合并相邻Span
            MergeSpan(span);
            //超过最大物理页数，选择向系统释放部分内存
            if (span->page_count > kMaxPageCount) {
                SystemDeallocate(span);
                span->page_count = kMaxPageCount;
            }
            m_span_lists[span->page_count].PushBack(span);
            //移除页号映射
            for(size_t i = 0; i < span->page_count; ++i) {
                page_id_to_span.erase(span->page_id + i);
            }
        };   
        //根据地址找到对应的Span
        Span* FindSpanByAddr(void* addr) {
            if (!addr) return nullptr;
            size_t page_id = reinterpret_cast<size_t>(addr) / m_page_size;
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = page_id_to_span.find(page_id);
            return it != page_id_to_span.end() ? it->second : nullptr;
        }
        size_t GetPageSize() const { return m_page_size; }
    private:
        //合并Span与前后相邻的空闲Span
        void MergeSpan(Span* span) {
            //合并前一个Span
            size_t prev_page_id = span->page_id - 1;
            auto prev_it = page_id_to_span.find(prev_page_id);
            if (prev_it != page_id_to_span.end()) {
                Span* prev_span = prev_it->second;
                if (!prev_span->is_used && prev_span->page_id + prev_span->page_count == span->page_id) {
                    //从SpanList中移除Span
                    m_span_lists[span->page_count].RemoveSpan(span);
                    //合并Span
                    prev_span->page_count += span->page_count;
                    delete span;
                    span = prev_span;
                    //更新页号映射
                    for (size_t i = 0; i < span->page_count; ++i) {
                        page_id_to_span[span->page_id + i] = span;
                    }
                }
            }
            //合并后一个Span
            size_t next_page_id = span->page_id + span->page_count;
            auto next_it = page_id_to_span.find(next_page_id);
            if (next_it != page_id_to_span.end()) {
                Span* next_span = next_it->second;
                if (!next_span->is_used && span->page_id + span->page_count == next_span->page_id) {
                    //从SpanList中移除Span
                    m_span_lists[next_span->page_count].RemoveSpan(next_span);
                    //合并Span
                    span->page_count += next_span->page_count;
                    delete next_span;
                    for (size_t i = 0; i < span->page_count; ++i) {
                        page_id_to_span[span->page_id + i] = span;
                    }
                }
            }
        };
        //向系统申请n页内存(mmap实现)
        void* SystemAllocate(size_t n) {
            //封装系统内存分配接口，通过条件编译适配不同平台
            size_t total_size = n * m_page_size;
            #ifdef _WIN32
                //nullptr: 让系统选择地址
                //total_size: 申请字节数，系统会向上对齐到页边界
                //MEM_COMMIT | MEM_RESERVE: 同时保留地址空间并提交物理页，返回的内存可直接读写
                //PAGE_READWRITE：可读可写
                //成功返回页对齐地址，失败返回nullptr
                return VirtualAlloc(nullptr, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            #else
                void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (ptr == MAP_FAILED) return nullptr;
                return ptr;
            #endif
        }
        //向系统释放内存munmap实现
        void SystemDeallocate(void* ptr) {
            #ifdef _WIN32
                //MEM_RELEASE：释放整个保留区域，地址空间和物理页一起归还。
                VirtualFree(ptr, 0, MEM_RELEASE);
            #endif
        }
};


//CentralCache单例模式
class CentralCache {
    private:
        // 小对象大小等级：8B、16B、...、4096B（共 512 个等级）
        static const size_t kMaxSmallObjSize = 4096;
        static const size_t k_align = 8;
        static const size_t kFreeListCount = kMaxSmallObjSize / k_align;
        SpanList m_span_lists[kFreeListCount];   //SpanList数组
        std::mutex mutexs[kFreeListCount];      //细粒度锁(每个等级一个锁)
        CentralCache() = default;
        CentralCache(const CentralCache&) = delete;
        CentralCache& operator=(const CentralCache&) = delete;
    public:
        static CentralCache& GetInstance() {
            static CentralCache instance;
            return instance;
        }
        //批量申请内存块
        void* AllocateBatch(size_t size, size_t batch_num) {
            if (size == 0 || batch_num == 0 || size > kMaxSmallObjSize) {
                return nullptr;
            }
            size_t align_size = AlignUp(size, k_align);
            size_t index = align_size / k_align - 1;
            std::lock_guard<std::mutex> lock(mutexs[index]);
            //查找有空闲块的Span
            Span* span = nullptr;
            for(Span* cur = m_span_lists[index].head_->next; cur != m_span_lists[index].tail_; cur=cur->next) {
                if (cur->free_count > 0) {
                    span = cur;
                    break;
                }
            }
            //没有可用Span，向Page Cache申请
            if (!span) {
                size_t page_count = (align_size * batch_num + PageCache::GetInstance().GetPageSize() - 1) / PageCache::GetInstance().GetPageSize();
                page_count = std::max(page_count, static_cast<size_t>(1));
                Span* new_span = PageCache::GetInstance().AllocatePages(page_count);
                new_span->block_size = align_size;
                new_span->free_count = page_count * PageCache::GetInstance().GetPageSize() / align_size;
                //分割Span为小块，构建空闲链表
                SplitSpan(new_span, align_size);
                //将新Span插入SpanList
                m_span_lists[index].PushBack(new_span);
                span = new_span;
            }
            //从Span中取出batch_num个块
            void* start = span->free_list;
            void* end = start;
            size_t actual_num = std::min(batch_num, span->free_count);
            for(size_t i = 0; i < actual_num - 1 && end; ++i) {
                end = *reinterpret_cast<void**>(end);
            }
            //更新Span的空闲链表
            if (end) {
                span->free_list = *reinterpret_cast<char**>(end);
                *reinterpret_cast<char**>(end) = nullptr;
            } else {
                span->free_list = nullptr;
            }
            span->free_count -= actual_num;
            return start;
        };
        //批量归还内存块
        void DeallocateBatch(size_t size, void* start, size_t count) {
            if (!start || size == 0 || count == 0 || size > kMaxSmallObjSize) {
                return;
            }
            size_t align_size = AlignUp(size, k_align);
            size_t index = (align_size / k_align) - 1;
            std::lock_guard<std::mutex> lock(mutexs[index]);
            //找到对应的Span
            Span* span = PageCache::GetInstance().FindSpanByAddr(start);
            if (!span) return;
            // 将块插入该Span的空闲链表
            *reinterpret_cast<void**>(reinterpret_cast<char*>(start) + (count - 1) * align_size) = span->free_list;
            span->free_list = start;
            span->free_count += count;
            // 所有块都空闲，构建空闲链表
            size_t total_blocks = (span->page_count * PageCache::GetInstance().GetPageSize()) / align_size;
            if (span->free_count == total_blocks) {
                m_span_lists[index].RemoveSpan(span);
                PageCache::GetInstance().DeallocatePages(span);
            }
        };
        //Span Split 构建空闲链表
        void SplitSpan(Span* span, size_t align_size) {
            size_t page_size = PageCache::GetInstance().GetPageSize();
            char* start = reinterpret_cast<char*>(span->page_id * page_size);
            char* end = reinterpret_cast<char*>(start + page_size * span->page_count);
            char *cur = start;
            while(cur + align_size < end) {
                *reinterpret_cast<char**>(cur) = reinterpret_cast<char*>(cur) + align_size;
                cur += align_size;
            }
            *reinterpret_cast<char**>(cur) = nullptr;
            span->free_list = start;
        };
};


class ThreadCache {
    private:
        struct FreeListNode {
            FreeListNode *next;
        };
        // 小对象大小等级：8B、16B、...、4096B（共 512 个等级）
        static const size_t kMaxSmallObjSize = 4096;
        static const size_t k_align = 8;
        static const size_t kFreeListCount = kMaxSmallObjSize / k_align;
        // 空闲链表数组: 每个索引对应一种大小的块
        FreeListNode* m_free_lists[kFreeListCount] = {nullptr};
    public:
        // 分配内存
        void* Allocate(size_t size) {
            if (size == 0) return nullptr;
            if (size > kMaxSmallObjSize) {
                // 大对象直接向Page Cache申请
                size_t page_count = (size + PageCache::GetInstance().GetPageSize() - 1) / PageCache::GetInstance().GetPageSize();
                Span* span = PageCache::GetInstance().AllocatePages(page_count);
                return reinterpret_cast<void*>(span->page_id * PageCache::GetInstance().GetPageSize());
            }
            size_t align_size = AlignUp(size, k_align);  // 对齐到8B整数倍
            size_t index = align_size / k_align - 1;;
            if (m_free_lists[index]) {
                FreeListNode* node = m_free_lists[index];
                m_free_lists[index] = node->next;
                return node;
            }
            // 空闲链表为空，从Central Cache批量申请内存块
            FetchFromCentralCache(index, align_size);
            // 再次分配
            FreeListNode* node = m_free_lists[index];
            if (!node) {
                throw std::bad_alloc();
            }
            m_free_lists[index] = node->next;
            return node;
        }
        // 释放内存
        void Deallocate(void* ptr, size_t size) {
            if (!ptr || size == 0) return;
            // 大对象直接归还给PageCache
            if (size > kMaxSmallObjSize) {
                Span* span = PageCache::GetInstance().FindSpanByAddr(ptr);
                if (span) {
                    PageCache::GetInstance().DeallocatePages(span);
                    return;
                }
            }
            size_t align_size = AlignUp(size, k_align);
            size_t index = align_size / k_align - 1;
            //插入空闲链表
            FreeListNode* node = static_cast<FreeListNode*>(ptr);
            node->next = m_free_lists[index];
            m_free_lists[index] = node;
            //空闲链表过长，归还给Central Cache
            size_t batch_num = CalculateBatchNum(align_size);
            if (GetListLength(m_free_lists[index]) > 2 * batch_num) {
                ReleaseToCentralCache(index, align_size, batch_num);
            }
        }
    private:
        size_t GetListLength(FreeListNode* head) {
            size_t len = 0;
            while(head) {
                len++;
                head = head->next;
            }
            return len;
        }
        //向Central Cache批量申请内存块
        void FetchFromCentralCache(size_t index, size_t size) {
            size_t batch_num = CalculateBatchNum(size);
            void* start = CentralCache::GetInstance().AllocateBatch(size, batch_num);
            if(!start) throw std::bad_alloc();
            FreeListNode* cur = static_cast<FreeListNode*>(start);
            FreeListNode* end = cur;
            for(size_t i = 0; i < batch_num - 1; ++i) {
                end = end->next;
            }
            end->next = m_free_lists[index];
            m_free_lists[index] = cur; 
        };
        // 计算批量申请的块数（小对象多申请，大对象少申请）
        size_t CalculateBatchNum(size_t size) const {
            if (size <= 64) return 32;
            if (size <= 256) return 16;
            if (size <= 1024) return 8;
            return 4;
        }
        // 归还给Central Cache
        void ReleaseToCentralCache(size_t index, size_t size, size_t batch_num) {
            FreeListNode* cur = m_free_lists[index];
            if (!cur) return;
            // 取出batch_num个块
            FreeListNode* end = cur;
            for (size_t i = 0; i < batch_num - 1 && end; ++i) {
                end = end->next;
            }
            if (!end) return;
            //更新空闲链表
            m_free_lists[index] = end->next;
            end->next = nullptr;
            //归还给CentralCache
            CentralCache::GetInstance().DeallocateBatch(size, cur, batch_num);
        }
};


// 线程本地存储：每个线程独立的ThreadCache
thread_local ThreadCache tls_thread_cache;
// 对外接口
void* PoolAllocate(size_t size) {
    try {
        return tls_thread_cache.Allocate(size);
    } catch (const std::bad_alloc& e) {
        std::cerr << "PoolAllocate failed:" << e.what() << std::endl;
        return nullptr;
    }
}
void PoolDeallocate(void* ptr, size_t size) {
    tls_thread_cache.Deallocate(ptr, size);
}
//测试代码
struct TestObj {
    int data[2];   //8B
};

//多线程并发测试
void TestConcurrentAlloc() {
    const int KThreadNum = 8;
    const int KAllocNum = 100000;
    std::vector<std::thread> threads;
    for (int t=0;t < KThreadNum; ++t) {
        //表示Lambda内部会按值拷贝外部作用域中用到的所有局部变量
        threads.emplace_back([=]() {
            std::vector<void*> ptrs;
            ptrs.reserve(KAllocNum);
            //分配
            for(size_t i = 0; i < KAllocNum; ++i) {
                ptrs.push_back(PoolAllocate(sizeof(TestObj)));
                if(!ptrs.back()) {
                    std::cerr << "Thread" << t << "Alloc failed" << std::endl;
                    return;
                }
            }
            //释放
            for(void* ptr : ptrs) {
                PoolDeallocate(ptr, sizeof(TestObj));
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    std::cout << "Concurrent allocation test passed!" << std::endl;
}


// 单线程测试
void TestSingleThreadAlloc() {
    TestObj* obj1 = static_cast<TestObj*>(PoolAllocate(sizeof(TestObj)));
    TestObj* obj2 = static_cast<TestObj*>(PoolAllocate(sizeof(TestObj)));
    TestObj* obj3 = static_cast<TestObj*>(PoolAllocate(sizeof(TestObj)));
    if (!obj1 || !obj2 || !obj3) {
        std::cerr << "Single thread allocate failed" << std::endl;
        return;
    }
    obj1->data[0] = 1;
    obj2->data[0] = 2;
    obj3->data[0] = 3;
    std::cout << "Single thread alloc: " << obj1 << " (data: " << obj1->data[0] << ")" << std::endl;
    std::cout << "Single thread alloc: " << obj2 << " (data: " << obj2->data[0] << ")" << std::endl;
    std::cout << "Single thread alloc: " << obj3 << " (data: " << obj3->data[0] << ")" << std::endl;
    PoolDeallocate(obj1, sizeof(TestObj));
    PoolDeallocate(obj2, sizeof(TestObj));
    PoolDeallocate(obj3, sizeof(TestObj));
    std::cout << "Single thread test passed!" << std::endl;
}


int main() {
    
    try {
        std::cout << "Start TestSingleThreadAlloc" << std::endl;  // endl 会刷新缓冲区
        TestSingleThreadAlloc();
        std::cout << "End TestSingleThreadAlloc" << std::endl;
        TestConcurrentAlloc();
    } catch (const std::exception& e) {
        std::cerr << "Test failed: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
