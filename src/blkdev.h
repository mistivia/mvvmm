// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Mistivia <i@mistivia.com>
#pragma once
namespace mvvmm {

int
mvvm_init_virtio_blk(struct mvvm *self, const char *disk_path);

void mvvm_destroy_virtio_blk(struct mvvm *self);

} // namespace mvvmm
