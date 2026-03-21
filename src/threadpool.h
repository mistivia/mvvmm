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

#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

namespace mvvmm {

class worker_thread;

class thread_pool {
public:
    static thread_pool* make_instance(int thread_num);
    int run(std::function<void(void)> &&m_task);
    ~thread_pool();
private:
    explicit thread_pool() = default;
    thread_pool(const thread_pool &) = delete;
    thread_pool& operator=(const thread_pool &) = delete;
    std::vector<std::unique_ptr<worker_thread>> m_workers;
    std::vector<uint8_t> m_is_working;
    int m_worker_num = -1;
    std::mutex m_lock;
    bool m_quit = false;
    friend class worker_thread;
};

class worker_thread {
private:
    static worker_thread* make_instance(struct thread_pool *pool, int id);
    void run();
    
    explicit worker_thread() = default;
    worker_thread(const worker_thread &) = delete;
    worker_thread& operator=(const worker_thread &) = delete;

    std::function<void(void)> m_task;
    bool m_has_task = false;
    std::thread m_th;
    std::mutex m_lock;
    std::condition_variable m_cond;
    thread_pool *m_pool = nullptr;
    int m_id = -1;
    friend class thread_pool;
};

} // namespace mvvmm
