/**
 * Copyright (c) 2026 Mistivia <i@mistivia.com>
 * 
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once
#include <stdbool.h>
#include <pthread.h>

namespace mvvmm {

struct thread_pool {
    struct worker_thread **workers;
    bool *is_working;
    int worker_num;
    pthread_mutex_t lock;
    int quit;
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

void delete_thread_pool(struct thread_pool *self);

} // namespace mvvmm
