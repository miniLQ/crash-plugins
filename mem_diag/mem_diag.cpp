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

#include "mem_diag.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wwrite-strings"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(MemDiag)
#endif

MemDiag::MemDiag() {}

void MemDiag::init_command(void) {
    cmd_name = "mem_diag";
    help_str_list = {
        "mem_diag",
        "deep diagnosis for memory deadlocks, OOM killable processes, and slab leaks",
        "[-o] [-d] [-p [pid]] [-s] [-r] [-a]\n"
        "  This command analyzes Linux kernel and Android memory state, evaluates\n"
        "  zone watermarks, diagnoses OOM killer deadlocks (e.g. all tasks with\n"
        "  oom_score_adj=-1000), ranks top memory-consuming processes and slab\n"
        "  caches, and inspects direct reclaim contention.\n\n"
        "  -o            System memory overview and zone watermarks health. (Default)\n"
        "  -d            Diagnose OOM deadlock & audit unkillable (-1000) tasks.\n"
        "  -p [pid]      Rank top memory processes (or deep-dive specific PID).\n"
        "  -s            Rank kernel Slab caches & detect unreclaimable slab leaks.\n"
        "  -r            Diagnose direct reclaim contention & tasks blocked in reclaim.\n"
        "  -a            Display comprehensive full memory diagnostic report.\n",
        "\n",
        "EXAMPLES",
        "  Display system memory overview & zone watermarks:",
        "    crash> mem_diag",
        "    crash> mem_diag -o\n",
        "  Diagnose OOM deadlock and unkillable (-1000) processes:",
        "    crash> mem_diag -d\n",
        "  Rank top 20 processes by physical memory (RSS):",
        "    crash> mem_diag -p\n",
        "  Deep-dive into a specific process (e.g. PID 7452):",
        "    crash> mem_diag -p 7452\n",
        "  Inspect top kernel Slab caches and unreclaimable leaks:",
        "    crash> mem_diag -s\n",
        "  Check direct reclaim contention and blocked allocators:",
        "    crash> mem_diag -r\n",
        "  Run comprehensive full report:",
        "    crash> mem_diag -a\n"
    };
}

void MemDiag::init_offset(void) {
    struct_init(zone);
    field_init(zone, _watermark);
    field_init(zone, watermark_boost);
    field_init(zone, managed_pages);
    field_init(zone, spanned_pages);
    field_init(zone, present_pages);
    field_init(zone, cma_pages);
    field_init(zone, name);
    field_init(zone, vm_stat);
    field_init(zone, free_area);

    struct_init(free_area);
    field_init(free_area, nr_free);

    struct_init(pglist_data);
    field_init(pglist_data, node_zones);
    field_init(pglist_data, nr_zones);
    field_init(pglist_data, vm_stat);
    field_init(pglist_data, kswapd);

    struct_init(kmem_cache);
    field_init(kmem_cache, name);
    field_init(kmem_cache, size);
    field_init(kmem_cache, object_size);
    field_init(kmem_cache, flags);
    field_init(kmem_cache, oo);
    field_init(kmem_cache, list);
    field_init(kmem_cache, node);

    struct_init(kmem_cache_node);
    field_init(kmem_cache_node, nr_slabs);
    field_init(kmem_cache_node, total_objects);

    struct_init(signal_struct);
    field_init(signal_struct, oom_score_adj);
    field_init(signal_struct, oom_score_adj_min);

    struct_init(mm_struct);
    field_init(mm_struct, total_vm);
    field_init(mm_struct, rss_stat);

    struct_init(mm_rss_stat);
    field_init(mm_rss_stat, count);

    struct_init(task_struct);
    field_init(task_struct, pid);
    field_init(task_struct, tgid);
    field_init(task_struct, comm);
    field_init(task_struct, mm);
    field_init(task_struct, signal);
    field_init(task_struct, state);
    field_init(task_struct, __state);
}

void MemDiag::cmd_main(void) {
    int c;
    optind = 0;
    bool show_overview = false;
    bool show_deadlock = false;
    bool show_processes = false;
    bool show_slabs = false;
    bool show_reclaim = false;
    bool show_all = false;
    pid_t target_pid = -1;

    if (argcnt < 2) {
        collect_all_data();
        print_overview();
        return;
    }

    while ((c = getopt(argcnt, args, "odp::srah")) != EOF) {
        switch (c) {
            case 'o':
                show_overview = true;
                break;
            case 'd':
                show_deadlock = true;
                break;
            case 'p':
                show_processes = true;
                if (optarg) target_pid = (pid_t)std::stol(optarg, nullptr, 0);
                break;
            case 's':
                show_slabs = true;
                break;
            case 'r':
                show_reclaim = true;
                break;
            case 'a':
                show_all = true;
                break;
            case 'h':
            default:
                argerrs++;
                break;
        }
    }

    while (optind < argcnt) {
        // If an extra argument is provided, treat it as PID if show_processes is selected or general PID
        try {
            target_pid = (pid_t)std::stol(args[optind], nullptr, 0);
            show_processes = true;
        } catch (...) {}
        optind++;
    }

    if (argerrs) {
        cmd_usage(pc->curcmd, SYNOPSIS);
        return;
    }

    collect_all_data();

    if (show_all) {
        print_full_report();
    } else if (target_pid > 0) {
        print_task_detail(target_pid);
    } else if (show_deadlock) {
        print_oom_deadlock();
    } else if (show_processes) {
        print_top_processes(20);
    } else if (show_slabs) {
        print_slab_caches(15);
    } else if (show_reclaim) {
        print_reclaim_contention();
    } else if (show_overview) {
        print_overview();
    } else {
        print_overview();
    }
}

void MemDiag::collect_memory_summary() {
    summary = MemSummary();

    if (csymbol_exists("_totalram_pages")) {
        summary.total_ram_pages = read_ulong(csymbol_value("_totalram_pages"), "totalram");
    } else if (csymbol_exists("totalram_pages")) {
        summary.total_ram_pages = read_ulong(csymbol_value("totalram_pages"), "totalram");
    }

    if (!csymbol_exists("contig_page_data")) return;
    ulong pgdat_addr = csymbol_value("contig_page_data");

    int off_vmstat = field_offset(pglist_data, vm_stat);
    if (off_vmstat >= 0) {
        ulong vmstat_base = pgdat_addr + off_vmstat;
        // In Linux 6.1 node vm_stat:
        // 0: NR_INACTIVE_ANON, 1: NR_ACTIVE_ANON
        // 2: NR_INACTIVE_FILE, 3: NR_ACTIVE_FILE
        // 5: NR_SLAB_RECLAIMABLE_B, 6: NR_SLAB_UNRECLAIMABLE_B
        long inact_anon = read_long(vmstat_base + 0 * sizeof(long), "inact_anon");
        long act_anon = read_long(vmstat_base + 1 * sizeof(long), "act_anon");
        long inact_file = read_long(vmstat_base + 2 * sizeof(long), "inact_file");
        long act_file = read_long(vmstat_base + 3 * sizeof(long), "act_file");
        long slab_reclaim = read_long(vmstat_base + 5 * sizeof(long), "slab_reclaim");
        long slab_unreclaim = read_long(vmstat_base + 6 * sizeof(long), "slab_unreclaim");

        if (inact_file > 0) summary.cached_pages += inact_file;
        if (act_file > 0) summary.cached_pages += act_file;
        if (inact_anon > 0) summary.anon_pages += inact_anon;
        if (act_anon > 0) summary.anon_pages += act_anon;
        if (slab_reclaim > 0) summary.slab_reclaimable_pages = slab_reclaim;
        if (slab_unreclaim > 0) summary.slab_unreclaimable_pages = slab_unreclaim;
        summary.total_slab_pages = summary.slab_reclaimable_pages + summary.slab_unreclaimable_pages;
    }
}

void MemDiag::collect_zones() {
    zones.clear();
    if (!csymbol_exists("contig_page_data")) return;
    ulong pgdat_addr = csymbol_value("contig_page_data");

    int off_zones = field_offset(pglist_data, node_zones);
    int zone_sz = struct_size(zone);
    if (off_zones < 0 || zone_sz <= 0) return;

    for (int i = 0; i < 4; i++) {
        ulong zaddr = pgdat_addr + off_zones + i * zone_sz;
        ulong spanned = read_ulong(zaddr + field_offset(zone, spanned_pages), "spanned");
        if (spanned == 0) continue; // Unpopulated zone

        MemZoneInfo zi;
        zi.addr = zaddr;
        zi.spanned_pages = spanned;
        zi.present_pages = read_ulong(zaddr + field_offset(zone, present_pages), "present");
        zi.managed_pages = (ulong)read_long(zaddr + field_offset(zone, managed_pages), "managed_pages");
        zi.watermark_boost = read_ulong(zaddr + field_offset(zone, watermark_boost), "boost");

        ulong name_ptr = read_pointer(zaddr + field_offset(zone, name), "name");
        if (name_ptr && is_kvaddr(name_ptr)) {
            zi.name = read_cstring(name_ptr, 32, "name");
        }
        if (zi.name.empty()) zi.name = "Zone-" + std::to_string(i);

        int off_wmark = field_offset(zone, _watermark);
        if (off_wmark >= 0) {
            zi.wmark_min = read_ulong(zaddr + off_wmark + 0 * sizeof(ulong), "min");
            zi.wmark_low = read_ulong(zaddr + off_wmark + 1 * sizeof(ulong), "low");
            zi.wmark_high = read_ulong(zaddr + off_wmark + 2 * sizeof(ulong), "high");
        }

        int off_vmstat = field_offset(zone, vm_stat);
        if (off_vmstat >= 0) {
            zi.free_pages = (ulong)read_long(zaddr + off_vmstat + 0 * sizeof(long), "free_pages"); // NR_FREE_PAGES
            zi.cma_free_pages = (ulong)read_long(zaddr + off_vmstat + 10 * sizeof(long), "cma_free_pages"); // NR_FREE_CMA_PAGES
        }

        ulong non_cma_free = zi.free_pages >= zi.cma_free_pages ? (zi.free_pages - zi.cma_free_pages) : zi.free_pages;
        if (non_cma_free < zi.wmark_min) {
            zi.status = "CRIT (< MIN)";
            summary.watermark_breached = true;
            if (summary.breached_zone_name.empty()) summary.breached_zone_name = zi.name;
        } else if (non_cma_free < zi.wmark_low) {
            zi.status = "WARN (< LOW)";
        } else {
            zi.status = "HEALTHY";
        }

        // Buddy order free counts
        int off_free_area = field_offset(zone, free_area);
        int sz_free_area = struct_size(free_area);
        if (off_free_area >= 0 && sz_free_area > 0) {
            for (int ord = 0; ord < 11; ord++) {
                ulong fa_addr = zaddr + off_free_area + ord * sz_free_area;
                ulong nrf = read_ulong(fa_addr + field_offset(free_area, nr_free), "nr_free");
                zi.order_nr_free.push_back(nrf);
            }
        }

        summary.free_ram_pages += zi.free_pages;
        zones.push_back(zi);
    }

    if (summary.total_ram_pages > summary.free_ram_pages) {
        summary.used_ram_pages = summary.total_ram_pages - summary.free_ram_pages;
    }
}

void MemDiag::collect_tasks() {
    tasks.clear();

    for (ulong task_addr : for_each_process()) {
        if (!is_kvaddr(task_addr)) continue;

        struct task_context *tc = task_to_context(task_addr);
        if (!tc) continue;

        // Inspect thread group leaders
        pid_t pid = tc->pid;
        pid_t tgid = read_int(task_addr + field_offset(task_struct, tgid), "tgid");
        if (pid != tgid) continue; // Only process group leaders

        ulong mm_addr = read_pointer(task_addr + field_offset(task_struct, mm), "mm");
        if (!mm_addr || !is_kvaddr(mm_addr)) continue; // Skip kernel threads

        auto tinfo = std::make_shared<MemTaskInfo>();
        tinfo->task_addr = task_addr;
        tinfo->mm_addr = mm_addr;
        tinfo->pid = pid;
        tinfo->tgid = tgid;
        tinfo->comm = tc->comm;

        tinfo->ppid = task_to_pid(tc->ptask);

        summary.total_user_tasks++;

        // Read OOM Score Adjustment
        ulong sig_addr = read_pointer(task_addr + field_offset(task_struct, signal), "signal");
        if (sig_addr && is_kvaddr(sig_addr)) {
            tinfo->oom_score_adj = read_short(sig_addr + field_offset(signal_struct, oom_score_adj), "oom_score_adj");
            tinfo->oom_score_adj_min = read_short(sig_addr + field_offset(signal_struct, oom_score_adj_min), "oom_score_adj_min");
        }

        if (tinfo->oom_score_adj == -1000) {
            summary.unkillable_count++;
        } else {
            summary.killable_count++;
        }

        // Virtual and physical memory
        tinfo->total_vm_pages = read_ulong(mm_addr + field_offset(mm_struct, total_vm), "total_vm");

        int off_rss = field_offset(mm_struct, rss_stat);
        if (off_rss >= 0) {
            ulong rss_base = mm_addr + off_rss;
            long file_p = read_long(rss_base + 0 * sizeof(long), "file_rss");
            long anon_p = read_long(rss_base + 1 * sizeof(long), "anon_rss");
            long swap_p = read_long(rss_base + 2 * sizeof(long), "swap_rss");
            long shmem_p = read_long(rss_base + 3 * sizeof(long), "shmem_rss");

            if (file_p > 0) tinfo->file_rss_pages = file_p;
            if (anon_p > 0) tinfo->anon_rss_pages = anon_p;
            if (swap_p > 0) tinfo->swap_rss_pages = swap_p;
            if (shmem_p > 0) tinfo->shmem_rss_pages = shmem_p;
            tinfo->total_rss_pages = tinfo->file_rss_pages + tinfo->anon_rss_pages + tinfo->shmem_rss_pages;
        }

        tasks.push_back(tinfo);
    }

    // Comprehensive scan of all running tasks for direct reclaim and allocator stalls
    reclaiming_tasks.clear();
    struct task_context *all_tc = FIRST_CONTEXT();
    size_t total_tasks = RUNNING_TASKS();
    for (size_t i = 0; i < total_tasks; i++, all_tc++) {
        if (!all_tc || !all_tc->task) continue;
        ulong taddr = all_tc->task;
        bool is_rec = false;
        std::string rec_func;

        if (tt && tt->panic_task == taddr) {
            is_rec = true;
            rec_func = "out_of_memory [PANIC TRACE]";
        } else {
            ulong fl = task_flags(taddr);
            if (fl & 0x00000800) { // PF_MEMALLOC
                is_rec = true;
                rec_func = "PF_MEMALLOC (direct reclaim)";
            } else {
#if defined(ARM64)
                ulong pc = 0, fp = 0;
                if (OFFSET(task_struct_thread_context_pc) >= 0 &&
                    OFFSET(task_struct_thread_context_fp) >= 0) {
                    pc = read_pointer(taddr + OFFSET(task_struct_thread_context_pc), "context_pc");
                    fp = read_pointer(taddr + OFFSET(task_struct_thread_context_fp), "context_fp");
                }
                if (pc && is_kvaddr(pc)) {
                    std::string sym = to_symbol(pc);
                    if (sym.find("shrink_") != std::string::npos ||
                        sym.find("try_to_free_pages") != std::string::npos ||
                        sym.find("alloc_pages") != std::string::npos ||
                        sym.find("alloc_slab") != std::string::npos ||
                        sym.find("throttle_direct_reclaim") != std::string::npos) {
                        is_rec = true;
                        rec_func = sym;
                    }
                }
                if (!is_rec && fp && is_kvaddr(fp)) {
                    ulong stackbase = GET_STACKBASE(taddr);
                    ulong stacktop = GET_STACKTOP(taddr);
                    ulong curr_fp = fp;
                    int depth = 25;
                    while (curr_fp >= stackbase && curr_fp + 16 <= stacktop && depth-- > 0) {
                        ulong next_fp = read_pointer(curr_fp, "next_fp");
                        ulong lr = read_pointer(curr_fp + 8, "lr");
                        if (lr && is_kvaddr(lr)) {
                            std::string lsym = to_symbol(lr);
                            if (lsym.find("shrink_node") != std::string::npos ||
                                lsym.find("shrink_slab") != std::string::npos ||
                                lsym.find("do_try_to_free_pages") != std::string::npos ||
                                lsym.find("try_to_free_pages") != std::string::npos ||
                                lsym.find("throttle_direct_reclaim") != std::string::npos ||
                                lsym.find("out_of_memory") != std::string::npos ||
                                lsym.find("alloc_pages_slowpath") != std::string::npos ||
                                lsym.find("alloc_slab_page") != std::string::npos) {
                                is_rec = true;
                                rec_func = lsym;
                                break;
                            }
                        }
                        if (next_fp <= curr_fp || next_fp >= stacktop) break;
                        curr_fp = next_fp;
                    }
                }
#endif
            }
        }

        if (is_rec) {
            auto rinfo = std::make_shared<MemTaskInfo>();
            rinfo->task_addr = taddr;
            rinfo->pid = all_tc->pid;
            rinfo->comm = all_tc->comm;
            rinfo->is_reclaiming = true;
            rinfo->reclaim_func = rec_func;
            reclaiming_tasks.push_back(rinfo);
        }
    }
}

void MemDiag::collect_slab_caches() {
    slab_caches.clear();
    if (!csymbol_exists("slab_caches")) return;

    ulong head = csymbol_value("slab_caches");
    int off_list = field_offset(kmem_cache, list);
    if (off_list < 0) return;

    std::vector<ulong> caches = for_each_list(head, off_list);
    for (ulong c_addr : caches) {
        if (!is_kvaddr(c_addr)) continue;

        SlabCacheInfo sc;
        sc.addr = c_addr;

        ulong name_ptr = read_pointer(c_addr + field_offset(kmem_cache, name), "name");
        if (name_ptr && is_kvaddr(name_ptr)) {
            sc.name = read_cstring(name_ptr, 64, "name");
        }
        if (sc.name.empty()) sc.name = "cache@" + std::to_string(c_addr & 0xffff);

        sc.size = read_uint(c_addr + field_offset(kmem_cache, size), "size");
        sc.object_size = read_uint(c_addr + field_offset(kmem_cache, object_size), "object_size");
        sc.flags = read_uint(c_addr + field_offset(kmem_cache, flags), "flags");
        uint oo = read_uint(c_addr + field_offset(kmem_cache, oo), "oo");
        sc.order = oo >> 16;

        sc.is_reclaimable = ((sc.flags & 0x00020000U) != 0); // SLAB_RECLAIM_ACCOUNT

        ulong node_ptr = read_pointer(c_addr + field_offset(kmem_cache, node), "node");
        if (node_ptr && is_kvaddr(node_ptr)) {
            long slabs = read_long(node_ptr + field_offset(kmem_cache_node, nr_slabs), "nr_slabs");
            long objs = read_long(node_ptr + field_offset(kmem_cache_node, total_objects), "total_objects");
            if (slabs > 0) sc.nr_slabs = (ulong)slabs;
            if (objs > 0) sc.total_objects = (ulong)objs;
        }

        sc.total_bytes = sc.nr_slabs * (1ULL << sc.order) * 4096ULL;
        slab_caches.push_back(sc);
    }

    std::sort(slab_caches.begin(), slab_caches.end(), [](const SlabCacheInfo &a, const SlabCacheInfo &b) {
        return a.total_bytes > b.total_bytes;
    });
}

void MemDiag::collect_all_data() {
    collect_memory_summary();
    collect_zones();
    collect_tasks();
    collect_slab_caches();
}

void MemDiag::print_overview() {
    PRINT("Linux Kernel Memory Health & Watermark Diagnosis\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    std::string total_str = csize(summary.total_ram_pages * 4096ULL);
    std::string free_str = csize(summary.free_ram_pages * 4096ULL);
    std::string used_str = csize(summary.used_ram_pages * 4096ULL);
    snprintf(lbuf, sizeof(lbuf), "  Total RAM:    %-12s  Free RAM: %-12s  Used RAM: %-12s",
             total_str.c_str(), free_str.c_str(), used_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string cached_str = csize(summary.cached_pages * 4096ULL);
    std::string anon_str = csize(summary.anon_pages * 4096ULL);
    std::string slab_str = csize(summary.total_slab_pages * 4096ULL);
    snprintf(lbuf, sizeof(lbuf), "  Page Cache:   %-12s  Anon RSS: %-12s  Slab RAM: %-12s",
             cached_str.c_str(), anon_str.c_str(), slab_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string slab_rec_str = csize(summary.slab_reclaimable_pages * 4096ULL);
    std::string slab_unrec_str = csize(summary.slab_unreclaimable_pages * 4096ULL);
    snprintf(lbuf, sizeof(lbuf), "  Slab Reclaim: %-12s  Slab Unreclaim: %-8s  Killable: %-5d / %-5d",
             slab_rec_str.c_str(), slab_unrec_str.c_str(), summary.killable_count, summary.total_user_tasks);
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    if (summary.watermark_breached) {
        snprintf(lbuf, sizeof(lbuf), "  [!] CRITICAL: %s zone free memory fell below MIN watermark!",
                 summary.breached_zone_name.c_str());
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Kernel direct reclaim triggered; system was starved of free pages.");
    } else {
        PRINT("│ %-76s │\n", "  [+] NORMAL: Zone watermarks are healthy and above low watermark.");
    }

    if (summary.killable_count == 0 && summary.total_user_tasks > 0) {
        PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
        PRINT("│ %-76s │\n", "  [!] OOM DEADLOCK: Zero killable processes found! (All oom_score_adj == -1000)");
        PRINT("│ %-76s │\n", "      Run 'mem_diag -d' to inspect unkillable processes and deadlock reason.");
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    // Zone Table: 12 + 12 + 11 + 11 + 11 + 16 = 73 + 7 borders = 80 chars
    PRINT("Memory Zone Watermarks & Free Space\n");
    PRINT("┌────────────┬────────────┬───────────┬───────────┬───────────┬────────────────┐\n");
    PRINT("│ Zone Name  │ Free (MB)  │ Min Wmark │ Low Wmark │ High Wmark│ Watermark Stat │\n");
    PRINT("├────────────┼────────────┼───────────┼───────────┼───────────┼────────────────┤\n");

    if (zones.empty()) {
        PRINT("│ %-76s │\n", "  [No populated memory zones detected]");
        PRINT("└────────────┴────────────┴───────────┴───────────┴───────────┴────────────────┘\n");
        return;
    }

    char row[512];
    for (const auto &z : zones) {
        std::string free_disp = csize(z.free_pages * 4096ULL);
        std::string min_disp = csize(z.wmark_min * 4096ULL);
        std::string low_disp = csize(z.wmark_low * 4096ULL);
        std::string high_disp = csize(z.wmark_high * 4096ULL);

        snprintf(row, sizeof(row),
                 "│ %-10s │ %-10s │ %-9s │ %-9s │ %-9s │ %-14s │\n",
                 z.name.c_str(), free_disp.c_str(),
                 min_disp.c_str(), low_disp.c_str(), high_disp.c_str(),
                 z.status.c_str());
        PRINT("%s", row);
    }
    PRINT("└────────────┴────────────┴───────────┴───────────┴───────────┴────────────────┘\n");
    PRINT("Commands: 'mem_diag -d' (OOM Deadlock), 'mem_diag -p' (Top RSS), 'mem_diag -s' (Slab leaks)\n");
}

void MemDiag::print_oom_deadlock() {
    PRINT("Out-Of-Memory (OOM) Deadlock & Killable Task Audit\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  User Tasks: %-6d  Killable (> -1000): %-6d  Unkillable (== -1000): %-6d",
             summary.total_user_tasks, summary.killable_count, summary.unkillable_count);
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    if (summary.killable_count == 0 && summary.total_user_tasks > 0) {
        PRINT("│ %-76s │\n", "  [!] ROOT CAUSE CONFIRMED: Out of memory and no killable processes!");
        PRINT("│ %-76s │\n", "      When memory ran out, select_bad_process() found zero killable tasks");
        PRINT("│ %-76s │\n", "      because every process had oom_score_adj == -1000 (OOM_SCORE_ADJ_MIN).");
        PRINT("│ %-76s │\n", "      This forced panic('Kernel panic - not syncing: System is deadlocked').");
    } else {
        PRINT("│ %-76s │\n", "  [+] PASS: Killable user processes exist. OOM killer can select a victim.");
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    // Unkillable tasks ranked by RSS
    std::vector<std::shared_ptr<MemTaskInfo>> unkillable;
    for (const auto &t : tasks) {
        if (t->oom_score_adj == -1000) {
            unkillable.push_back(t);
        }
    }

    std::sort(unkillable.begin(), unkillable.end(), [](const std::shared_ptr<MemTaskInfo> &a, const std::shared_ptr<MemTaskInfo> &b) {
        return a->total_rss_pages > b->total_rss_pages;
    });

    PRINT("Unkillable Processes Audit (oom_score_adj == -1000) - Ranked by RSS:\n");
    PRINT("┌────────┬──────────────────────┬──────────────┬──────────────┬────────────────┐\n");
    PRINT("│ PID    │ Command              │ Physical RSS │ Virtual Size │ OOM Score Adj  │\n");
    PRINT("├────────┼──────────────────────┼──────────────┼──────────────┼────────────────┤\n");

    if (unkillable.empty()) {
        PRINT("│ %-76s │\n", "  [No unkillable processes found]");
        PRINT("└────────┴──────────────────────┴──────────────┴──────────────┴────────────────┘\n");
        return;
    }

    char row[512];
    int disp_limit = 25;
    int count = 0;
    for (const auto &t : unkillable) {
        if (count++ >= disp_limit) break;

        std::string comm_disp = t->comm;
        if (comm_disp.length() > 20) comm_disp = comm_disp.substr(0, 17) + "...";

        std::string rss_disp = csize(t->total_rss_pages * 4096ULL);
        std::string vm_disp = csize(t->total_vm_pages * 4096ULL);

        snprintf(row, sizeof(row),
                 "│ %-6d │ %-20s │ %-12s │ %-12s │ %-14d │\n",
                 t->pid, comm_disp.c_str(), rss_disp.c_str(), vm_disp.c_str(), t->oom_score_adj);
        PRINT("%s", row);
    }
    if ((int)unkillable.size() > disp_limit) {
        char more_buf[128];
        snprintf(more_buf, sizeof(more_buf), "  ... and %d more unkillable processes (all oom_score_adj == -1000)",
                 (int)unkillable.size() - disp_limit);
        PRINT("│ %-76s │\n", more_buf);
    }
    PRINT("└────────┴──────────────────────┴──────────────┴──────────────┴────────────────┘\n");
    PRINT("Note: Processes with oom_score_adj=-1000 are completely exempt from OOM kill.\n");
}

void MemDiag::print_top_processes(int top_n) {
    std::vector<std::shared_ptr<MemTaskInfo>> sorted_tasks = tasks;
    std::sort(sorted_tasks.begin(), sorted_tasks.end(), [](const std::shared_ptr<MemTaskInfo> &a, const std::shared_ptr<MemTaskInfo> &b) {
        return a->total_rss_pages > b->total_rss_pages;
    });

    PRINT("Top %d Memory-Consuming Processes (Ranked by RSS)\n", top_n);
    PRINT("┌───────┬─────────────────┬───────────┬──────────┬──────────┬─────────┬────────┐\n");
    PRINT("│ PID   │ Command         │ Total RSS │ Anon RSS │ File RSS │ Swap    │ Score  │\n");
    PRINT("├───────┼─────────────────┼───────────┼──────────┼──────────┼─────────┼────────┤\n");

    if (sorted_tasks.empty()) {
        PRINT("│ %-76s │\n", "  [No user tasks with valid memory detected]");
        PRINT("└───────┴─────────────────┴───────────┴──────────┴──────────┴─────────┴────────┘\n");
        return;
    }

    char row[512];
    int count = 0;
    for (const auto &t : sorted_tasks) {
        if (count++ >= top_n) break;

        std::string comm_disp = t->comm;
        if (comm_disp.length() > 15) comm_disp = comm_disp.substr(0, 13) + "..";

        std::string rss_disp = csize(t->total_rss_pages * 4096ULL);
        std::string anon_disp = csize(t->anon_rss_pages * 4096ULL);
        std::string file_disp = csize(t->file_rss_pages * 4096ULL);
        std::string swap_disp = csize(t->swap_rss_pages * 4096ULL);

        snprintf(row, sizeof(row),
                 "│ %-5d │ %-15s │ %-9s │ %-8s │ %-8s │ %-7s │ %-6d │\n",
                 t->pid, comm_disp.c_str(), rss_disp.c_str(),
                 anon_disp.c_str(), file_disp.c_str(), swap_disp.c_str(),
                 t->oom_score_adj);
        PRINT("%s", row);
    }
    PRINT("└───────┴─────────────────┴───────────┴──────────┴──────────┴─────────┴────────┘\n");
    PRINT("Tip: Use 'mem_diag -p <pid>' to inspect full breakdown and threads for a PID.\n");
}

void MemDiag::print_task_detail(pid_t target_pid) {
    std::shared_ptr<MemTaskInfo> match = nullptr;
    for (const auto &t : tasks) {
        if (t->pid == target_pid) {
            match = t;
            break;
        }
    }

    if (!match) {
        PRINT("Process with PID %d not found in active user tasks.\n", target_pid);
        return;
    }

    PRINT("Process Memory Deep-Dive: PID %d (%s)\n", match->pid, match->comm.c_str());
    PRINT("┌──────────────────────┬───────────────────────────────────────────────────────┐\n");

    char row[512];
    snprintf(row, sizeof(row), "│ Process PID / TGID   │ %-53s │\n",
             (std::to_string(match->pid) + " / " + std::to_string(match->tgid)).c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Command Name         │ %-53s │\n", match->comm.c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Parent PID (PPID)    │ %-53d │\n", match->ppid);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Task Struct Pointer  │ 0x%-51lx │\n", match->task_addr);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ MM Struct Pointer    │ 0x%-51lx │\n", match->mm_addr);
    PRINT("%s", row);

    std::string adj_str = std::to_string(match->oom_score_adj);
    if (match->oom_score_adj == -1000) adj_str += " (OOM_SCORE_ADJ_MIN - Unkillable)";
    snprintf(row, sizeof(row), "│ OOM Score Adjustment │ %-53s │\n", adj_str.c_str());
    PRINT("%s", row);

    std::string vm_str = csize(match->total_vm_pages * 4096ULL) + " (" + std::to_string(match->total_vm_pages) + " pages)";
    snprintf(row, sizeof(row), "│ Total Virtual Memory │ %-53s │\n", vm_str.c_str());
    PRINT("%s", row);

    std::string rss_str = csize(match->total_rss_pages * 4096ULL) + " (" + std::to_string(match->total_rss_pages) + " pages)";
    snprintf(row, sizeof(row), "│ Physical RSS Total   │ %-53s │\n", rss_str.c_str());
    PRINT("%s", row);

    std::string anon_str = csize(match->anon_rss_pages * 4096ULL);
    snprintf(row, sizeof(row), "│ Anonymous RSS        │ %-53s │\n", anon_str.c_str());
    PRINT("%s", row);

    std::string file_str = csize(match->file_rss_pages * 4096ULL);
    snprintf(row, sizeof(row), "│ File Mapped RSS      │ %-53s │\n", file_str.c_str());
    PRINT("%s", row);

    std::string swap_str = csize(match->swap_rss_pages * 4096ULL);
    snprintf(row, sizeof(row), "│ Swap Usage           │ %-53s │\n", swap_str.c_str());
    PRINT("%s", row);

    std::string shmem_str = csize(match->shmem_rss_pages * 4096ULL);
    snprintf(row, sizeof(row), "│ Shared Memory (Shmem)│ %-53s │\n", shmem_str.c_str());
    PRINT("%s", row);

    PRINT("└──────────────────────┴───────────────────────────────────────────────────────┘\n");
}

void MemDiag::print_slab_caches(int top_n) {
    PRINT("Kernel Slab Caches Memory Analysis (Top %d by Memory)\n", top_n);
    PRINT("┌──────────────────────────┬────────────┬──────────┬───────────┬───────────────┐\n");
    PRINT("│ Slab Cache Name          │ Memory     │ Slabs    │ Objects   │ Reclaim Type  │\n");
    PRINT("├──────────────────────────┼────────────┼──────────┼───────────┼───────────────┤\n");

    if (slab_caches.empty()) {
        PRINT("│ %-76s │\n", "  [No slab caches detected in slab_caches list]");
        PRINT("└──────────────────────────┴────────────┴──────────┴───────────┴───────────────┘\n");
        return;
    }

    char row[512];
    int count = 0;
    ulong unreclaimable_total = 0;
    ulong reclaimable_total = 0;

    for (const auto &sc : slab_caches) {
        if (sc.is_reclaimable) reclaimable_total += sc.total_bytes;
        else unreclaimable_total += sc.total_bytes;

        if (count++ < top_n) {
            std::string name_disp = sc.name;
            if (name_disp.length() > 24) name_disp = name_disp.substr(0, 21) + "...";

            std::string mem_disp = csize(sc.total_bytes);
            std::string type_disp = sc.is_reclaimable ? "Reclaimable" : "Unreclaimable";

            snprintf(row, sizeof(row),
                     "│ %-24s │ %-10s │ %-8lu │ %-9lu │ %-13s │\n",
                     name_disp.c_str(), mem_disp.c_str(),
                     sc.nr_slabs, sc.total_objects, type_disp.c_str());
            PRINT("%s", row);
        }
    }
    PRINT("└──────────────────────────┴────────────┴──────────┴───────────┴───────────────┘\n");
    PRINT("Slab Summary: Reclaimable: %s | Unreclaimable: %s\n",
          csize(reclaimable_total).c_str(), csize(unreclaimable_total).c_str());
    if (unreclaimable_total > reclaimable_total) {
        PRINT("Notice: Unreclaimable slab dominates kernel memory (cannot be freed by reclaim).\n");
    }
}

void MemDiag::print_reclaim_contention() {
    PRINT("Direct Reclaim Contention & Blocked Tasks\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    int reclaiming_count = (int)reclaiming_tasks.size();

    char lbuf[256];
    if (reclaiming_count > 0) {
        snprintf(lbuf, sizeof(lbuf), "  [!] %d task(s) currently blocked in memory direct reclaim!", reclaiming_count);
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Tasks are actively stalling in shrink_node / page allocation.");
    } else {
        PRINT("│ %-76s │\n", "  [PASS] No tasks currently blocked in direct page reclaim slowpaths.");
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    PRINT("Tasks in Direct Reclaim:\n");
    PRINT("┌────────┬──────────────────┬──────────┬───────────────────────────────────────┐\n");
    PRINT("│ PID    │ Command          │ State    │ Blocked Reclaim Function / Channel    │\n");
    PRINT("├────────┼──────────────────┼──────────┼───────────────────────────────────────┤\n");

    if (reclaiming_count == 0) {
        PRINT("│ %-76s │\n", "  [No tasks blocked in direct reclaim]");
        PRINT("└────────┴──────────────────┴──────────┴───────────────────────────────────────┘\n");
        return;
    }

    char row[512];
    for (const auto &t : reclaiming_tasks) {
        std::string comm_disp = t->comm;
        if (comm_disp.length() > 16) comm_disp = comm_disp.substr(0, 14) + "..";

        std::string fn_disp = t->reclaim_func;
        if (fn_disp.length() > 37) fn_disp = fn_disp.substr(0, 34) + "...";

        snprintf(row, sizeof(row),
                 "│ %-6d │ %-16s │ %-8s │ %-37s │\n",
                 t->pid, comm_disp.c_str(), "RECLAIM", fn_disp.c_str());
        PRINT("%s", row);
    }
    PRINT("└────────┴──────────────────┴──────────┴───────────────────────────────────────┘\n");
}

void MemDiag::print_full_report() {
    print_overview();
    PRINT("\n");
    print_oom_deadlock();
    PRINT("\n");
    print_top_processes(15);
    PRINT("\n");
    print_slab_caches(10);
    PRINT("\n");
    print_reclaim_contention();
}

#pragma GCC diagnostic pop
