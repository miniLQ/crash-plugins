/**
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef MEM_DIAG_DEFS_H_
#define MEM_DIAG_DEFS_H_

#include "plugin.h"
#include <string>
#include <vector>
#include <memory>

struct MemZoneInfo {
    ulong addr = 0;
    std::string name;
    ulong spanned_pages = 0;
    ulong present_pages = 0;
    ulong managed_pages = 0;
    ulong free_pages = 0;
    ulong cma_free_pages = 0;
    ulong wmark_min = 0;
    ulong wmark_low = 0;
    ulong wmark_high = 0;
    ulong watermark_boost = 0;
    std::string status;
    std::vector<ulong> order_nr_free;
};

struct MemTaskInfo {
    ulong task_addr = 0;
    ulong mm_addr = 0;
    pid_t pid = 0;
    pid_t tgid = 0;
    pid_t ppid = 0;
    std::string comm;
    short oom_score_adj = 0;
    short oom_score_adj_min = 0;
    ulong total_vm_pages = 0;
    ulong anon_rss_pages = 0;
    ulong file_rss_pages = 0;
    ulong swap_rss_pages = 0;
    ulong shmem_rss_pages = 0;
    ulong total_rss_pages = 0;
    bool is_reclaiming = false;
    std::string reclaim_func;
};

struct SlabCacheInfo {
    ulong addr = 0;
    std::string name;
    uint size = 0;
    uint object_size = 0;
    uint flags = 0;
    uint order = 0;
    ulong nr_slabs = 0;
    ulong total_objects = 0;
    ulong total_bytes = 0;
    bool is_reclaimable = false;
};

struct MemSummary {
    ulong total_ram_pages = 0;
    ulong free_ram_pages = 0;
    ulong used_ram_pages = 0;
    ulong cached_pages = 0;
    ulong anon_pages = 0;
    ulong slab_reclaimable_pages = 0;
    ulong slab_unreclaimable_pages = 0;
    ulong total_slab_pages = 0;
    ulong swap_total_pages = 0;
    ulong swap_used_pages = 0;
    ulong swap_free_pages = 0;
    int total_user_tasks = 0;
    int unkillable_count = 0;
    int killable_count = 0;
    bool watermark_breached = false;
    std::string breached_zone_name;
};

class MemDiag : public ParserPlugin {
private:
    std::vector<MemZoneInfo> zones;
    std::vector<std::shared_ptr<MemTaskInfo>> tasks;
    std::vector<std::shared_ptr<MemTaskInfo>> reclaiming_tasks;
    std::vector<SlabCacheInfo> slab_caches;
    MemSummary summary;

    void collect_all_data();
    void collect_memory_summary();
    void collect_zones();
    void collect_tasks();
    void collect_slab_caches();

    void print_overview();
    void print_oom_deadlock();
    void print_top_processes(int top_n = 20);
    void print_task_detail(pid_t target_pid);
    void print_slab_caches(int top_n = 15);
    void print_reclaim_contention();
    void print_full_report();

public:
    MemDiag();
    void cmd_main(void) override;
    void init_offset(void) override;
    void init_command(void) override;
    DEFINE_PLUGIN_INSTANCE(MemDiag)
};

#endif // MEM_DIAG_DEFS_H_
