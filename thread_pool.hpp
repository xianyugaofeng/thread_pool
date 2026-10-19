#define TINYSERVER_THREADPOOL_HPP

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <list>

#include <pthread.h>

class Threadpool {
    private:
        struct NWorker
        {
            pthread_t threadid;
            bool terminate;     //是否需要结束该worker的标志
            int isWorking;      //该worker是否在工作
            Threadpool *pool;   //隶属于线程池
        } *m_workers;

        struct NJOB {
            void (*func)(void *args);   //任务函数
            void *user_data;
        };
    public:
        //线程池初始化
        //numWorkers:线程数量
        ThreadPool(int numWorkers, int max_jobs);
        //销毁线程池
        ~ThreadPool();
        //面向用户的添加任务
        int pushJob(void (*func)(void *data), void *arg, int len);
    private:
        //向线程池中添加任务
        bool _addJob(NJOB* job);
        //回调函数
        static void* _run(void* arg);
        void _threadLoop(void *arg);
    private:
        std::list<NJOB*> m_job_lists;
        int m_max_jobs;         //任务队列中的最大任务数
        int m_sum_thread;       //worker总数量
        int m_free_thread;      //worker空闲数量
        pthread_cond_t m_jobs_cond;     //线程条件等待
        //当任务队列有新任务时唤醒worker的条件锁
        pthread_mutex_t m_jobs_mutex;   //为任务加锁防止一个任务被两个线程执行等其他情况
};

//run为static函数
void* ThreadPool:: _run(void *arg) {
    NWORKER *worker = (NWORKER *)arg;
    worker->pool->_threadLoop(arg);
}

//threadLoop为普通成员函数
void ThreadPool ::_threadLoop(void *arg) {
    NWORKER *worker = (NWORKER *)arg;
    while(1) {
        //线程只有两个状态：执行\等待
        //查看任务队列前先获取锁
        pthread_mutex_lock(&m_jobs_mutex);
        //当前没有任务
        while (m_jobs_lists.size()== 0) {
            if (worker->terminate) break;
            //释放 mutex，并把当前线程加入cond的等待队列
            //被pthread_cond_signal/broadcast唤醒后，重新获取mutex
            //返回时，线程持有mutex，可以安全地重新检查条件
            pthread_cond_wait(&m_jobs_cond, &m_jobs_mutex);
        };
        
        //检查worker是否需要结束生命
        if (worker->terminate) {
            pthread_mutex_unlock(&m_jobs_mutex);
            break;
        }
        //获取到job后将该job从任务队列移出，免得其他worker过来重复做这个任务
        struct NJOB *job = m_jobs_lists.front();
        ////对任务队列的操作结束，释放锁
        pthread_mutex_unlock(&m_jobs_mutex);
        m_jobs_lists.pop_front();

        m_free_thread--;
        worker->isWorking = true;
        //执行job中的func
        job->func(job->user_data);
        
        m_free_thread++;
        worker->isWorking = false;

        free(job->user_data);
        free(job);
    }

    free(worker);
    pthread_exit(NULL);
}

//添加任务
bool ThreadPool::_addJob(NJOB *job) {
	//尝试获取锁
    pthread_mutex_lock(&m_job_mutex);
    //判断队列是否超过任务数量上限
    if (m_jobs_list.size() >= m_max_jobs) {
        pthread_mutex_unlock(&m_jobs_mutex);
        return false;
    }
    //向任务队列添加job
    m_jobs_list.push_back(job);
    //唤醒休眠的线程
    pthread_mutex_signal(&m_jobs_cond);
    //释放锁
    pthread_mutex_unlock(&m_jobs_mutex);
    return true;
}