#include <iostream>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <mutex>


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
                // 大对象直接向Page Cache申请
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
            return (size + k_align - 1) / k_align - 1;  // 向上取整并减1得到索引
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
};