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

#include "virtio.h"
#include "utils.h"
#include <memory>

namespace mvvmm {

class mvvm;

class tap_net_impl: public ethernet_device
                  , public std::enable_shared_from_this<tap_net_impl> {
public:
    explicit tap_net_impl() = default;
    virtual ~tap_net_impl() override;
    virtual void write_packet_to_ether(const uint8_t *buf, int len) override;
    int init(mvvm *vm, const char *tap_name);
private:
    static void rx_thread_run(std::shared_ptr<tap_net_impl> net);
    int m_fd = -1;
    char m_ifname[IFNAMSIZ] = {0};
    std::thread m_rx_thread{};
    int m_quit = 0;
    std::mutex m_lock{};
    move_only mo;
};

} // namespace mvvmm
