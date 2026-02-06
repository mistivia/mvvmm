#ifndef MVVMM_THREADPOOL_H_
#define MVVMM_THREADPOOL_H_

#include <stdbool.h>
#include <pthread.h>

struct thread_pool {
    struct worker_thread **workers;
    bool *is_working;
    int worker_num;
    pthread_mutex_t lock;
};

struct worker_thread {
    void* (*task_fn)(void*);
    void *arg;
    pthread_t th;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    struct thread_pool *pool;
    int id;
};

struct thread_pool* new_thread_pool(int thread_num);

int
thread_pool_run(struct thread_pool *self, void* (*task_fn)(void*), void *arg);

#endif