#include "threadpool.h"

#include <bits/pthreadtypes.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

void *worker_thread_fn(void* arg)
{
    struct worker_thread *worker = arg;
    pthread_mutex_lock(&worker->lock);
    while (1) {
        while (worker->task_fn == NULL) {
            if (worker->pool->quit && worker->task_fn == NULL) {
                pthread_mutex_unlock(&worker->pool->lock);
                return NULL;
            }
            struct timespec ts = {0};
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 300000LL;
            pthread_cond_timedwait(&worker->cond, &worker->lock, &ts);
        }
        worker->task_fn(worker->arg);
        worker->task_fn = NULL;
        worker->arg = NULL;
        pthread_mutex_lock(&worker->pool->lock);
        worker->pool->is_working[worker->id] = false;
        pthread_mutex_unlock(&worker->pool->lock);
    }
}

static struct worker_thread *
new_worker_thread(struct thread_pool *pool, int id)
{
    struct worker_thread *worker = malloc(sizeof(struct worker_thread));
    *worker = (struct worker_thread){0};
    pthread_mutex_init(&worker->lock, NULL);
    pthread_cond_init(&worker->cond, NULL);
    worker->pool = pool;
    worker->id = id;
    pthread_create(&worker->th, NULL, worker_thread_fn, worker);
    return worker;
}

struct thread_pool*
new_thread_pool(int thread_num)
{
    struct thread_pool *pool = malloc(sizeof(struct thread_pool));
    memset(pool, 0, sizeof(struct thread_pool));
    pthread_mutex_init(&pool->lock, NULL);
    pool->quit = 0;
    pool->worker_num = thread_num;
    pool->workers = malloc(sizeof(struct worker_thread*) * pool->worker_num);
    memset(pool->workers, 0, sizeof(struct worker_thread*) * pool->worker_num);
    pool->is_working = malloc(sizeof(bool) * pool->worker_num);
    memset(pool->is_working, 0, sizeof(bool) * pool->worker_num);
    for (int i = 0; i < pool->worker_num; i++) {
        pool->workers[i] = new_worker_thread(pool, i);
    }
    return pool;
}

int
thread_pool_run(struct thread_pool *self, void* (*task_fn)(void*), void *arg)
{
    pthread_mutex_lock(&self->lock);
    for (int i = 0; i < self->worker_num; i++) {
        if (!self->is_working[i]) {
            self->is_working[i] = true;
            pthread_mutex_unlock(&self->lock);
            struct worker_thread *worker = self->workers[i];
            pthread_mutex_lock(&worker->lock);
            if (self->quit) {
                pthread_mutex_unlock(&worker->lock);
                return -1;
            }
            worker->task_fn = task_fn;
            worker->arg = arg;
            pthread_cond_signal(&worker->cond);
            pthread_mutex_unlock(&worker->lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&self->lock);
    return -1;
}

void delete_thread_pool(struct thread_pool* pool) {
    pool->quit = 1;
    for (int i = 0; i < pool->worker_num; i++) {
        void *ret;
        pthread_join(pool->workers[i]->th, &ret);
        free(pool->workers[i]);
    }
    free(pool->workers);
    free(pool->is_working);
    free(pool);
}