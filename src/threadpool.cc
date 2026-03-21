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


#include "threadpool.h"

#include <chrono>
#include <memory>
#include <vector>

namespace mvvmm {

void worker_thread::run()
{
    std::unique_lock<std::mutex> lk{m_lock};
    while (1) {
        while (!m_has_task) {
            if (m_pool->m_quit) {
                return;
            }
            m_cond.wait_for(lk, std::chrono::milliseconds(300));
        }
        m_task();
        m_has_task = false;
        std::unique_lock<std::mutex> plk{m_pool->m_lock};
        m_pool->m_is_working[m_id] = false;
    }
}

worker_thread *
worker_thread::make_instance(struct thread_pool *pool, int id)
{
    auto *self = new worker_thread{};
    self->m_pool = pool;
    self->m_id = id;
    self->m_has_task = false;
    self->m_th = std::thread{[=]() {
        self->run();
    }};
    return self;
}

thread_pool * thread_pool::make_instance(int thread_num)
{
    auto *self = new thread_pool{};
    self->m_quit = 0;
    self->m_worker_num = thread_num;
    self->m_workers.resize(self->m_worker_num);
    self->m_is_working.resize(self->m_worker_num);
    for (int i = 0; i < self->m_worker_num; i++) {
        self->m_workers[i].reset(worker_thread::make_instance(self, i));
        self->m_is_working[i] = false;
    }
    return self;
}

int thread_pool::run(std::function<void(void)> &&task)
{
    std::unique_lock<std::mutex> lk{m_lock};
    for (int i = 0; i < m_worker_num; i++) {
        if (!m_is_working[i]) {
            m_is_working[i] = true;
            auto &worker = m_workers[i];
            lk.unlock();
            std::unique_lock<std::mutex> wlk{worker->m_lock};
            if (m_quit) {
                return -1;
            }
            worker->m_task = std::move(task);
            worker->m_has_task = true;
            worker->m_cond.notify_one();
            return 0;
        }
    }
    return -1;
}

thread_pool::~thread_pool()
{
    m_quit = 1;
    for (int i = 0; i < m_worker_num; i++) {
        if (m_workers[i]->m_th.joinable()) {
            m_workers[i]->m_th.join();
        }
    }
}

} // namespace mvvmm
