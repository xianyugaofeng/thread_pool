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
#include <unistd.h>
#endif

inline size_t AlignUp(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}


class MemoryPool {
    private:
        // 空闲块结构（嵌入式链表，无额外内存开销）
        // 单线程版本
        struct FreeBlock {
            FreeBlock *next;
        };
    public:
        explicit MemoryPool(size_t block_size, size_t align = 8, size_t prealloc = 1024)
            : m_align(align), m_free_list(nullptr), m_block_size(block_size) {
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
        size_t m_block_size;      // 内存块大小
        size_t m_align;           // 对齐方式
        FreeBlock *m_free_list;   // 空闲块链表头指针
        std::vector<void*> m_pools;  // 已分配的内存块列表(用于折构时释放)
        std::mutex m_mutex;  // 互斥锁，保护空闲块链表
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
            if (size > kMaxSmallObjSize) {
                // 大对象直接向Central Cache申请
                return CentralCache::Allocate(size);
            }
            size = AlignUp(size, k_align);  // 对齐到8B整数倍
            size_t index = GetSizeIndex(size);
            if (m_free_lists) {
                // 空闲链表为空，从Central Cache批量申请内存块
                FetchFromCentralCache(index, size);
            }
            FreeListNode* block = m_free_lists[0];
            return block;
        };
        void Deallocate(void* ptr, size_t size);  // 释放内存
    private:
        // 计算大小对应的链表索引
        size_t GetSizeIndex(size_t size) {
            return AlignUp(size, k_align) / k_align - 1;  // 向上取整并减1得到索引
        }
        //向Central Cache批量申请内存块
        void FetchFromCentralCache(size_t index, size_t size);
        // 计算批量申请的块数（小对象多申请，大对象少申请）
        size_t CalculateBatchSize(size_t size) {
            if (size <= 64) return 32;
            if (size <= 256) return 16;
            if (size <= 1024) return 8;
            return 4;
        }
};


class CentralCache {
    public:
        void* Allocate(size_t size);
};


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
            if (head_->next = tail_) return nullptr;
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
    private:
        Span* head_;
        Span* tail_;
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
        //批量申请块
        void* AllocateBatch(size_t size, size_t batch_num);
        //批量释放块
        void* DeallocateBatch(size_t size, void* start, size_t count);
        //计算大小所需要的索引
        size_t GetSizeIndex(size_t size) const {
            return AlignUp(size, k_align) / k_align - 1;
        }
        //批量向Page Cache申请内存页 创建Span
        void FetchFromPageCache(size_t page_count);
        //Span Split 构建空闲链表
        void *SplitSpan(Span* span, size_t size, size_t batch_num);
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
                SystemDeallocate(span, kMaxPageCount - span->page_count);
                span->page_count = kMaxPageCount;
            }
            m_span_lists[span->page_count].PushBack(span);
            //移除页号映射
            for(size_t i = 0; i < span->page_count; ++i) {
                page_id_to_span[span->page_id + i] == nullptr;
            }
        };   // 归还Span
        //根据地址找到对应的Span
        Span* FindSpanByAddr(void* addr) {
            size_t page_id = reinterpret_cast<size_t>(addr) / m_page_size;
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = page_id_to_span.find(page_id);
            return it != page_id_to_span.end() ? it->second : nullptr;
        }
    private:
        //合并Span与前后相邻的空闲Span
        void MergeSpan(Span* span);
        //向系统申请n页内存(mmap实现)
        void* SystemAllocate(size_t n) {
            //封装系统内存分配接口，通过条件编译适配不同平台
            size_t total_size = n * m_page_size;
            #ifdef _WIN32
                return VirtualAlloc(nullptr, total_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            #else
                void* ptr = mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (ptr == MAP_FAILED) return nullptr;
            #endif
        }
        //向系统释放内存munmap实现
        void SystemDeallocate(void* ptr, size_t n) {
            #ifdef _WIN32
                VirtualFree(ptr, 0, MEM_RELEASE);
            #else
                munmap(ptr, total_size);
            #endif
        }
};