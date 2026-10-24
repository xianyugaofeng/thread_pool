#ifndef TINYSERVER_THREADPOOL_HPP
#define TINYSERVER_THREADPOOL_HPP

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <new>
#include <pthread.h>


class ThreadPool {
    private:
        struct NWorker
        {
            pthread_t threadid;
            bool terminate;     //是否需要结束该worker的标志
            int isWorking;      //该worker是否在工作
            ThreadPool *pool;   //隶属于线程池
        } *m_workers = nullptr;

        struct NJOB {
            void (*func)(void *args);   //任务函数
            void *user_data;
        };
    
    public:
        //线程池初始化
        //numWorkers:线程数量  max_jobs:任务队列中的最大任务数
        bool init(int numWorkers, int max_jobs = 10);
        //销毁线程池
        ~ThreadPool();
        //面向用户的添加任务
        //user_data由pushJob内部malloc拷贝一份，任务执行完后由worker释放
        //返回值：1成功  -1队列已满  -2内存分配失败  -3参数非法
        int pushJob(void (*func)(void *data), void *arg, int len);
    
    private:
        //向线程池中添加任务
        bool _addJob(NJOB *job);
        //回调函数
        static void* _run(void *arg);
        void _threadLoop(NWorker *worker);
        //让前created个已创建的worker退出，并等待其全部结束
        void shutdown_workers(int created);
    private:
        std::list<NJOB*> m_job_lists;
        std::size_t m_max_jobs = 0;         //任务队列中的最大任务数
        std::size_t m_sum_thread = 0;       //worker总数量
        std::size_t m_free_thread = 0;      //worker空闲数量
        std::size_t m_alive_threads = 0;    //存活(尚未退出)的worker数量
        //worker为detached线程无法join，退出前--m_alive_threads并signal
        //m_workers_cond，析构/回滚时等待该计数归零后再释放worker数组
        pthread_cond_t m_jobs_cond;     //当任务队列有新任务时唤醒worker的条件锁
        pthread_cond_t m_workers_cond;  //worker退出时唤醒等待回收的线程
        pthread_mutex_t m_jobs_mutex;   //为任务加锁防止一个任务被两个线程执行等其他情况
};

//run为static函数
void* ThreadPool::_run(void *arg) {
    NWorker *worker = (NWorker *)arg;
    worker->pool->_threadLoop(worker);
    return NULL;
}

bool ThreadPool::init(int numWorkers, int max_jobs) {
    if (numWorkers < 1 || max_jobs < 1) return false;

    //初始化jobs_cond
    if (pthread_cond_init(&m_jobs_cond, NULL) != 0) {
        perror("init m_jobs_cond fail");
        return false;
    }

    //初始化workers_cond
    if (pthread_cond_init(&m_workers_cond, NULL) != 0) {
        perror("init m_workers_cond fail");
        pthread_cond_destroy(&m_jobs_cond);
        return false;
    }

    //初始化jobs_mutex
    if (pthread_mutex_init(&m_jobs_mutex, NULL) != 0) {
        perror("init m_jobs_mutex fail");
        pthread_cond_destroy(&m_jobs_cond);
        pthread_cond_destroy(&m_workers_cond);
        return false;
    }

    //初始化workers
    try { m_workers = new NWorker[numWorkers](); }
    catch (const std::bad_alloc&) {
        perror("create workers fail");
        pthread_mutex_destroy(&m_jobs_mutex);
        pthread_cond_destroy(&m_jobs_cond);
        pthread_cond_destroy(&m_workers_cond);
        return false;
    }

    m_max_jobs = max_jobs;
    m_sum_thread = numWorkers;
    m_free_thread = numWorkers;

    int created = 0;
    for (int i = 0; i < numWorkers; i++) {
        m_workers[i].pool = this;
        //int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void *(*start_routine)(void *), void *arg);
        //pthread_t* 输出参数，创建成功后线程ID写回这里
        //const pthread_attr_t*	线程属性，NULL=默认（joinable、默认栈大小、默认调度）
        //线程入口函数，新线程从这里开始执行
        //NWorker* → 隐式转void* 传给入口函数的参数指针，原样出现在_run(void *arg)里
        int ret = pthread_create(&(m_workers[i].threadid), NULL, _run, &m_workers[i]);
        if (ret) {
            //把 pthread_create 返回的错误码转成可读字符串并打印到标准错误
            fprintf(stderr, "create worker fail: %s\n", strerror(ret));
            goto fail;
        }
        //detached线程结束时，TCB和retval被自己立即释放
        ret = pthread_detach(m_workers[i].threadid);
        if (ret != 0) {
            fprintf(stderr, "detach worker fail: %s\n", strerror(ret));
            //向目标线程发送一个取消请求（detach失败时线程仍是joinable的，可以join）
            pthread_cancel(m_workers[i].threadid);
            //等待目标线程终止并回收其资源
            pthread_join(m_workers[i].threadid, NULL);
            goto fail;
        }
        ++m_alive_threads;   //创建成功，存活计数+1（取消/失败路径不计入）
        ++created;
    }
    return true;

    fail:
        shutdown_workers(created);           // ★ 先让已建线程退出并等待结束
        delete[] m_workers;
        m_workers = nullptr;
        m_sum_thread = 0;
        m_free_thread = 0;
        //回滚cond和mutex
        pthread_mutex_destroy(&m_jobs_mutex);
        pthread_cond_destroy(&m_jobs_cond);
        pthread_cond_destroy(&m_workers_cond);
        return false;
}

//让前created个worker退出并等待其全部结束
void ThreadPool::shutdown_workers(int created) {
    if (created <= 0) return;
    pthread_mutex_lock(&m_jobs_mutex);
    for (int i = 0; i < created; i++)
        m_workers[i].terminate = true;
    pthread_cond_broadcast(&m_jobs_cond);
    //detached线程无法join，只能等待存活计数归零
    while (m_alive_threads > 0)
        pthread_cond_wait(&m_workers_cond, &m_jobs_mutex);
    pthread_mutex_unlock(&m_jobs_mutex);
}

ThreadPool::~ThreadPool() {
    //置terminate=1 → pthread_cond_broadcast → 等待计数归零
    //(每个worker退出前 --m_alive_threads并signal m_workers_cond) → 返回
    //没有join的detached线程，必须靠计数器同步，否则无法安全释放
    if (m_workers == nullptr) return;    //init失败过或未调用init，无需清理

    shutdown_workers(m_sum_thread);

    //释放仍未执行的任务，避免内存泄漏
    for (NJOB *job : m_job_lists) {
        free(job->user_data);
        free(job);
    }
    m_job_lists.clear();

    delete[] m_workers;
    m_workers = nullptr;
    pthread_mutex_destroy(&m_jobs_mutex);
    pthread_cond_destroy(&m_jobs_cond);
    pthread_cond_destroy(&m_workers_cond);
    return;
}

//threadLoop为普通成员函数
void ThreadPool::_threadLoop(NWorker *worker) {
    while (1) {
        //线程只有两个状态：执行\等待
        //查看任务队列前先获取锁
        pthread_mutex_lock(&m_jobs_mutex);
        //当前没有任务且未要求退出时等待
        while (m_job_lists.size() == 0 && !worker->terminate) {
            //释放mutex，并把当前线程加入cond的等待队列
            //被pthread_cond_signal/broadcast唤醒后，重新获取mutex
            //返回时，线程持有mutex，可以安全地重新检查条件
            pthread_cond_wait(&m_jobs_cond, &m_jobs_mutex);
        }

        //检查worker是否需要结束生命
        if (worker->terminate) {
            //退出前存活计数-1并通知等待回收的线程
            --m_alive_threads;
            pthread_cond_signal(&m_workers_cond);
            pthread_mutex_unlock(&m_jobs_mutex);
            break;
        }
        //任务队列不为空，获取到任务
        //获取到job后将该job从任务队列移出，免得其他worker过来重复做这个任务
        struct NJOB *job = m_job_lists.front();
        m_job_lists.pop_front();
        --m_free_thread;
        worker->isWorking = 1;
        //对任务队列的操作结束，释放锁
        pthread_mutex_unlock(&m_jobs_mutex);

        //执行job中的func
        job->func(job->user_data);

        pthread_mutex_lock(&m_jobs_mutex);
        worker->isWorking = 0;
        ++m_free_thread;
        pthread_mutex_unlock(&m_jobs_mutex);

        //user_data由pushJob malloc拷贝，执行完后由worker释放
        free(job->user_data);
        free(job);
    }
    //worker数组是new[]分配的，这里不能free(worker)，统一由析构函数delete[]
}

//添加任务
bool ThreadPool::_addJob(NJOB *job) {
    //尝试获取锁
    pthread_mutex_lock(&m_jobs_mutex);
    //判断队列是否超过任务数量上限
    if (m_job_lists.size() >= m_max_jobs) {
        pthread_mutex_unlock(&m_jobs_mutex);
        return false;
    }
    //向任务队列添加job
    m_job_lists.push_back(job);
    //唤醒休眠的线程
    pthread_cond_signal(&m_jobs_cond);
    //释放锁
    pthread_mutex_unlock(&m_jobs_mutex);
    return true;
}

//面向用户的添加任务
int ThreadPool::pushJob(void (*func)(void *), void *arg, int len) {
    if (func == NULL || arg == NULL || len <= 0) return -3;

    struct NJOB *job = (struct NJOB *)malloc(sizeof(struct NJOB));
    if (job == NULL) {
        perror("malloc");
        return -2;
    }
    memset(job, 0, sizeof(struct NJOB));

    job->user_data = malloc(len);
    if (job->user_data == NULL) {
        free(job);
        perror("malloc");
        return -2;
    }
    memcpy(job->user_data, arg, len);
    job->func = func;

    if (!_addJob(job)) {
        //队列已满：释放资源并报告失败，调用方通过返回值得知
        free(job->user_data);
        free(job);
        return -1;
    }

    return 1;
}
#endif
