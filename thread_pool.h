#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdlib.h>


struct NWORKER {
    pthread_t threadid;     //线程id
    bool terminate;     //是否需要结束该worker的标志
    int isWorking;      //该worker是否在工作
    Threadpool *pool;   //隶属于线程池
};

struct NJOB {
    void (*func)(void *args);    //任务函数
    void *user_data;
};

