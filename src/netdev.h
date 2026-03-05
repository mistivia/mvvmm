// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mistivia <i@mistivia.com>
#pragma once
namespace mvvmm {

struct mvvm;

int
mvvm_init_virtio_net(struct mvvm *self, const char *tap_name);

void mvvm_destroy_virtio_net(struct mvvm *self);

} // namespace mvvmm
