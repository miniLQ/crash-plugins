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

#include "hungtask.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wwrite-strings"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(HungTask)
#endif

HungTask::HungTask() {}

void HungTask::init_command(void) {
    cmd_name = "hungtask";
    help_str_list = {
        "hungtask",
        "analyze hung tasks, D-state tasks, and system deadlocks",
        "[-a] [-l] [-t <pid|task>] [-d] [-s] [-m]\n"
        "  This command inspects all tasks in TASK_UNINTERRUPTIBLE (D state),\n"
        "  identifies root-cause bottlenecks and wait channels, diagnoses lock\n"
        "  chains and memory deadlocks, and displays task backtraces.\n\n"
        "  -a            Display overview of all D-state (uninterruptible) tasks. (Default)\n"
        "  -l, -w        Cluster and rank bottlenecks by Wait Channel (wchan).\n"
        "  -t <pid|task> Deep-dive analysis and full kernel backtrace for a specific task.\n"
        "  -d            Detect deadlocks: memory reclaim deadlock, lock cycles, and kswapd status.\n"
        "  -s            Display system-wide task states summary and hung_task sysctl configuration.\n"
        "  -m            Filter and list tasks blocked specifically in Memory Allocation / Reclaim.\n",
        "\n",
        "EXAMPLES",
        "  Display all D-state tasks:\n"
        "    crash> hungtask -a\n\n"
        "  Rank wait channels to locate the primary bottleneck:\n"
        "    crash> hungtask -l\n\n"
        "  Diagnose memory deadlocks and lock contention:\n"
        "    crash> hungtask -d\n\n"
        "  Deep-dive task 7452 and view its kernel call stack:\n"
        "    crash> hungtask -t 7452\n\n"
        "  Display system task states summary:\n"
        "    crash> hungtask -s\n"
    };
}

void HungTask::init_offset(void) {
    field_init(task_struct, thread);
    field_init(task_struct, flags);
    field_init(task_struct, prio);
    field_init(task_struct, mm);
    field_init(task_struct, nvcsw);
    field_init(task_struct, nivcsw);
    field_init(thread_struct, cpu_context);
}

void HungTask::cmd_main(void) {
    int c;
    optind = 0;
    bool show_all = false;
    bool show_cluster = false;
    bool show_deadlock = false;
    bool show_states = false;
    bool show_mem_only = false;
    std::string target_task_str;

    if (argcnt < 2) {
        print_hung_task_overview(false);
        return;
    }

    while ((c = getopt(argcnt, args, "alwdst:mh")) != EOF) {
        switch (c) {
            case 'a':
                show_all = true;
                break;
            case 'l':
            case 'w':
                show_cluster = true;
                break;
            case 'd':
                show_deadlock = true;
                break;
            case 's':
                show_states = true;
                break;
            case 't':
                target_task_str = optarg ? optarg : "";
                break;
            case 'm':
                show_mem_only = true;
                break;
            case 'h':
            default:
                cmd_usage(pc->curcmd, SYNOPSIS);
                return;
        }
    }

    if (!target_task_str.empty()) {
        print_task_detail(target_task_str);
    } else if (show_deadlock) {
        print_deadlock_analysis();
    } else if (show_cluster) {
        print_wchan_clustering();
    } else if (show_states) {
        print_system_states_summary();
    } else if (show_mem_only) {
        print_hung_task_overview(true);
    } else if (show_all) {
        print_hung_task_overview(false);
    } else {
        print_hung_task_overview(false);
    }
}

std::string HungTask::get_func_name(const std::string &sym) {
    size_t pos = sym.find('+');
    if (pos != std::string::npos) {
        return sym.substr(0, pos);
    }
    pos = sym.find('/');
    if (pos != std::string::npos) {
        return sym.substr(0, pos);
    }
    return sym;
}

bool HungTask::is_sched_primitive(const std::string &fn) {
    static const std::unordered_set<std::string> primitives = {
        "__switch_to", "cpu_switch_to", "context_switch",
        "__schedule", "schedule", "schedule_timeout",
        "schedule_timeout_interruptible", "schedule_timeout_uninterruptible",
        "schedule_timeout_killable", "schedule_hrtimeout",
        "schedule_preempt_disabled", "preempt_schedule",
        "schedule_idle", "pick_next_task", "asm_common_interrupt",
        "ret_from_fork", "kthread"
    };
    return primitives.find(fn) != primitives.end();
}

std::string HungTask::categorize_wait(const std::string &wchan, const std::string &caller) {
    std::string combined = wchan + " " + caller;
    std::transform(combined.begin(), combined.end(), combined.begin(), ::tolower);

    if (combined.find("reclaim") != std::string::npos ||
        combined.find("alloc_pages") != std::string::npos ||
        combined.find("throttle") != std::string::npos ||
        combined.find("shrink_") != std::string::npos ||
        combined.find("kswapd") != std::string::npos ||
        combined.find("try_to_free_pages") != std::string::npos ||
        combined.find("balance_pgdat") != std::string::npos ||
        combined.find("page_fault") != std::string::npos ||
        combined.find("handle_mm_fault") != std::string::npos ||
        combined.find("do_anonymous_page") != std::string::npos) {
        return "[Memory Reclaim / Alloc]";
    }
    if (combined.find("mutex") != std::string::npos) {
        return "[Mutex Lock]";
    }
    if (combined.find("down_read") != std::string::npos ||
        combined.find("down_write") != std::string::npos ||
        combined.find("rwsem") != std::string::npos ||
        combined.find("mmap_lock") != std::string::npos) {
        return "[RW Semaphore]";
    }
    if (combined.find("io_schedule") != std::string::npos ||
        combined.find("blk_") != std::string::npos ||
        combined.find("sync_") != std::string::npos ||
        combined.find("wait_on_page") != std::string::npos ||
        combined.find("folio_wait") != std::string::npos ||
        combined.find("bdi_") != std::string::npos ||
        combined.find("get_request") != std::string::npos ||
        combined.find("ufshcd_") != std::string::npos ||
        combined.find("sdhci_") != std::string::npos ||
        combined.find("bit_wait_io") != std::string::npos) {
        return "[D-State I/O Wait]";
    }
    if (combined.find("completion") != std::string::npos) {
        return "[Completion]";
    }
    if (combined.find("binder") != std::string::npos) {
        return "[Binder IPC]";
    }
    if (combined.find("futex") != std::string::npos) {
        return "[Futex]";
    }
    if (combined.find("flush_work") != std::string::npos ||
        combined.find("flush_workqueue") != std::string::npos ||
        combined.find("wait_on_work") != std::string::npos) {
        return "[Workqueue Flush]";
    }
    if (combined.find("sleep") != std::string::npos ||
        combined.find("nanosleep") != std::string::npos) {
        return "[Sleep / Timer]";
    }
    if (combined.find("ret_from_fork") != std::string::npos ||
        combined.find("kthread") != std::string::npos) {
        return "[Thread Init / Spawn]";
    }
    return "[Other Wait]";
}

std::string HungTask::parse_task_flags(ulong flags) {
    std::string res;
    if (flags & 0x00000001) res += "PF_VCPU ";
    if (flags & 0x00000002) res += "PF_IDLE ";
    if (flags & 0x00000004) res += "PF_EXITING ";
    if (flags & 0x00000010) res += "PF_IO_WORKER ";
    if (flags & 0x00000020) res += "PF_WQ_WORKER ";
    if (flags & 0x00000040) res += "PF_FORKNOEXEC ";
    if (flags & 0x00000800) res += "PF_MEMALLOC ";
    if (flags & 0x00002000) res += "PF_USED_MATH ";
    if (flags & 0x00010000) res += "PF_FROZEN ";
    if (flags & 0x00020000) res += "PF_FREEZER_SKIP ";
    if (flags & 0x00200000) res += "PF_KTHREAD ";
    if (flags & 0x00400000) res += "PF_RANDOMIZE ";
    if (flags & 0x00800000) res += "PF_SWAPWRITE ";
    if (flags & 0x02000000) res += "PF_NOFREEZE ";
    if (res.empty()) res = "0x" + std::to_string(flags);
    else if (res.back() == ' ') res.pop_back();
    return res;
}

void HungTask::unwind_task_stack(struct task_context *tc,
                                 std::vector<StackFrame> &frames,
                                 std::string &wchan,
                                 std::string &caller,
                                 std::string &category) {
    frames.clear();
    wchan = "[unknown]";
    caller = "[unknown]";
    category = "[Other Wait]";

    if (!tc || !tc->task) return;

    ulong fp = 0, pc = 0;

#if defined(ARM64)
    if (OFFSET(task_struct_thread_context_fp) >= 0 &&
        OFFSET(task_struct_thread_context_pc) >= 0) {
        fp = read_pointer(tc->task + OFFSET(task_struct_thread_context_fp), "context_fp");
        pc = read_pointer(tc->task + OFFSET(task_struct_thread_context_pc), "context_pc");
    } else {
        int off_thread = field_offset(task_struct, thread);
        int off_ctx = field_offset(thread_struct, cpu_context);
        if (off_ctx < 0) off_ctx = 0;
        if (off_thread >= 0) {
            ulong ctx_addr = tc->task + off_thread + off_ctx;
            fp = read_pointer(ctx_addr + 80, "context_fp");
            pc = read_pointer(ctx_addr + 96, "context_pc");
        }
    }
#endif

    ulong stackbase = GET_STACKBASE(tc->task);
    ulong stacktop = GET_STACKTOP(tc->task);

    if (pc && is_kvaddr(pc)) {
        frames.push_back({fp, pc, to_symbol(pc)});
    }

    if (fp >= stackbase && fp + 16 <= stacktop) {
        ulong curr_fp = fp;
        int max_depth = 40;
        while (curr_fp >= stackbase && curr_fp + 16 <= stacktop && max_depth-- > 0) {
            ulong next_fp = read_pointer(curr_fp, "next_fp");
            ulong lr = read_pointer(curr_fp + 8, "lr");
            if (lr && is_kvaddr(lr)) {
                frames.push_back({curr_fp, lr, to_symbol(lr)});
            }
            if (next_fp <= curr_fp || next_fp >= stacktop) {
                break;
            }
            curr_fp = next_fp;
        }
    }

    // Determine wchan and caller from frames
    int wchan_idx = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        std::string fn = get_func_name(frames[i].sym);
        if (!is_sched_primitive(fn) && fn != "[unknown]") {
            wchan_idx = i;
            break;
        }
    }

    if (wchan_idx >= 0) {
        wchan = frames[wchan_idx].sym;
        if (wchan_idx + 1 < (int)frames.size()) {
            caller = frames[wchan_idx + 1].sym;
        }
        category = categorize_wait(frames[wchan_idx].sym, caller);
    } else if (!frames.empty()) {
        wchan = frames.back().sym;
        category = categorize_wait(wchan, "");
    }
}

std::vector<std::shared_ptr<HungTaskInfo>> HungTask::collect_hung_tasks(bool mem_only) {
    std::vector<std::shared_ptr<HungTaskInfo>> hung_tasks;
    struct task_context *tc = FIRST_CONTEXT();
    size_t total_tasks = RUNNING_TASKS();

    for (size_t i = 0; i < total_tasks; i++, tc++) {
        if (!tc || !tc->task) continue;

        char sbuf[32];
        task_state_string(tc->task, sbuf, 0);

        // Check for TASK_UNINTERRUPTIBLE (D state)
        if (strstr(sbuf, "UN") == NULL) {
            continue;
        }

        auto info = std::make_shared<HungTaskInfo>();
        info->tc = tc;
        info->task_addr = tc->task;
        info->pid = tc->pid;
        info->ppid = task_to_pid(tc->ptask);
        info->tgid = task_tgid(tc->task);
        info->cpu = tc->processor;
        info->comm = tc->comm;
        info->state_short = "D(UN)";

        char full_state_buf[64];
        info->state_full = task_state_string(tc->task, full_state_buf, 1);

        info->flags = task_flags(tc->task);
        info->flags_str = parse_task_flags(info->flags);

        int off_prio = field_offset(task_struct, prio);
        if (off_prio >= 0) {
            info->prio = read_int(tc->task + off_prio, "prio");
        }

        int off_nvcsw = field_offset(task_struct, nvcsw);
        if (off_nvcsw >= 0) {
            info->nvcsw = read_ulong(tc->task + off_nvcsw, "nvcsw");
        }

        int off_nivcsw = field_offset(task_struct, nivcsw);
        if (off_nivcsw >= 0) {
            info->nivcsw = read_ulong(tc->task + off_nivcsw, "nivcsw");
        }

        info->mm = task_mm(tc->task, 0);
        if (info->mm) {
            struct task_mem_usage tm;
            BZERO(&tm, sizeof(struct task_mem_usage));
            get_task_mem_usage(tc->task, &tm);
            info->total_vm_kb = (tm.total_vm * PAGESIZE()) / 1024;
            info->rss_kb = (tm.rss * PAGESIZE()) / 1024;
        }

        unwind_task_stack(tc, info->callstack, info->wchan, info->caller, info->category);
        info->wchan_fn = get_func_name(info->wchan);

        if (mem_only && info->category != "[Memory Reclaim / Alloc]") {
            continue;
        }

        hung_tasks.push_back(info);
    }

    return hung_tasks;
}

void HungTask::print_hung_task_overview(bool mem_only) {
    auto tasks = collect_hung_tasks(mem_only);
    size_t total_tasks = RUNNING_TASKS();

    if (mem_only) {
        PRINT("Memory Reclaim Stalled Tasks Overview\n");
    } else {
        PRINT("Hung & D-State Tasks Overview (TASK_UNINTERRUPTIBLE)\n");
    }

    PRINT("┌──────┬──────┬────┬───────┬──────────────────┬──────────────────┬─────────────────────────────┬──────────────────────────┐\n");
    PRINT("│  PID │ PPID │ CPU│ State │ Task Pointer     │ Comm             │ Wait Channel (wchan)        │ Category                 │\n");
    PRINT("├──────┼──────┼────┼───────┼──────────────────┼──────────────────┼─────────────────────────────┼──────────────────────────┤\n");

    int mem_reclaim_count = 0;
    int mutex_count = 0;
    int rwsem_count = 0;
    int io_count = 0;

    for (const auto &info : tasks) {
        if (info->category == "[Memory Reclaim / Alloc]") mem_reclaim_count++;
        else if (info->category == "[Mutex Lock]") mutex_count++;
        else if (info->category == "[RW Semaphore]") rwsem_count++;
        else if (info->category == "[D-State I/O Wait]") io_count++;

        std::string comm_str = info->comm.empty() ? "[unknown]" : info->comm;
        if (comm_str.length() > 16) comm_str = comm_str.substr(0, 13) + "...";

        std::string wchan_str = info->wchan;
        if (wchan_str.length() > 27) wchan_str = wchan_str.substr(0, 24) + "...";

        char row[512];
        snprintf(row, sizeof(row),
                 "│%5d │%5d │%3d │ %-5s │ 0x%-16lx │ %-16s │ %-27s │ %-24s │\n",
                 info->pid, info->ppid, info->cpu, info->state_short.c_str(),
                 info->task_addr, comm_str.c_str(), wchan_str.c_str(),
                 info->category.c_str());
        PRINT("%s", row);
    }

    PRINT("└──────┴──────┴────┴───────┴──────────────────┴──────────────────┴─────────────────────────────┴──────────────────────────┘\n");

    double pct = (total_tasks > 0) ? ((double)tasks.size() * 100.0 / (double)total_tasks) : 0.0;
    PRINT("Total Tasks: %zu | D-State (Hung) Tasks: %zu (%.2f%%)\n",
          total_tasks, tasks.size(), pct);
    PRINT("Category Breakdown: Memory Reclaim: %d | Mutex: %d | RWSEM: %d | I/O Wait: %d\n",
          mem_reclaim_count, mutex_count, rwsem_count, io_count);

    if (mem_reclaim_count >= 5) {
        PRINT("\033[31m[!] ALERT: High concentration of tasks (%d) stuck in Memory Reclaim / Direct Reclaim!\033[0m\n",
              mem_reclaim_count);
    }
}

void HungTask::print_wchan_clustering() {
    auto tasks = collect_hung_tasks(false);
    if (tasks.empty()) {
        PRINT("No D-state tasks found in the system.\n");
        return;
    }

    std::map<std::string, WchanCluster> cluster_map;
    for (const auto &info : tasks) {
        std::string key = info->wchan_fn;
        if (key.empty()) key = "[unknown]";
        auto &c = cluster_map[key];
        c.wchan_fn = key;
        c.category = info->category;
        c.count++;
        c.tasks.push_back({info->pid, info->comm});
    }

    std::vector<WchanCluster> cluster_list;
    for (const auto &kv : cluster_map) {
        cluster_list.push_back(kv.second);
    }

    std::sort(cluster_list.begin(), cluster_list.end(),
              [](const WchanCluster &a, const WchanCluster &b) {
                  return a.count > b.count;
              });

    PRINT("Wait Channel Bottleneck Clustering & Root Cause Ranking\n");
    PRINT("┌─────┬───────┬────────┬─────────────────────────────┬──────────────────────────┬───────────────────────────────┐\n");
    PRINT("│ Rank│ Tasks │  Ratio │ Wait Channel (wchan)        │ Category                 │ Example PIDs (Comm)           │\n");
    PRINT("├─────┼───────┼────────┼─────────────────────────────┼──────────────────────────┼───────────────────────────────┤\n");

    int rank = 1;
    char row[512];
    for (const auto &c : cluster_list) {
        double ratio = (tasks.size() > 0) ? ((double)c.count * 100.0 / (double)tasks.size()) : 0.0;
        std::string wchan_str = c.wchan_fn;
        if (wchan_str.length() > 27) wchan_str = wchan_str.substr(0, 24) + "...";

        std::string ex_str;
        for (size_t i = 0; i < c.tasks.size() && i < 2; ++i) {
            if (i > 0) ex_str += ", ";
            ex_str += std::to_string(c.tasks[i].first) + "(" + c.tasks[i].second + ")";
        }
        if (c.tasks.size() > 2) ex_str += "...";
        if (ex_str.length() > 29) ex_str = ex_str.substr(0, 26) + "...";

        snprintf(row, sizeof(row),
                 "│%4d │%6d │%6.2f%% │ %-27s │ %-24s │ %-29s │\n",
                 rank, c.count, ratio, wchan_str.c_str(), c.category.c_str(),
                 ex_str.c_str());
        PRINT("%s", row);
        rank++;
    }

    PRINT("└─────┴───────┴────────┴─────────────────────────────┴──────────────────────────┴───────────────────────────────┘\n");
    if (!cluster_list.empty()) {
        PRINT("Primary System Bottleneck: '%s' with %d tasks (%.2f%% of all D-state tasks)\n",
              cluster_list[0].wchan_fn.c_str(), cluster_list[0].count,
              (double)cluster_list[0].count * 100.0 / (double)tasks.size());
    }
}

void HungTask::print_task_detail(const std::string &target_str) {
    struct task_context *tc = nullptr;

    // Try parsing as hex address
    if (target_str.find("0x") == 0 || target_str.find("0X") == 0 ||
        (target_str.length() >= 12 && target_str.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)) {
        try {
            ulong addr = std::stoul(target_str, nullptr, 16);
            if (is_kvaddr(addr)) {
                tc = task_to_context(addr);
            }
        } catch (...) {}
    }

    // Try parsing as PID
    if (!tc) {
        try {
            int pid = std::stoi(target_str);
            tc = pid_to_context(pid);
        } catch (...) {}
    }

    // Try searching by comm
    if (!tc) {
        tc = find_proc(target_str);
    }

    if (!tc || !tc->task) {
        PRINT("Task '%s' not found in kernel context table.\n", target_str.c_str());
        return;
    }

    auto info = std::make_shared<HungTaskInfo>();
    info->tc = tc;
    info->task_addr = tc->task;
    info->pid = tc->pid;
    info->ppid = task_to_pid(tc->ptask);
    info->tgid = task_tgid(tc->task);
    info->cpu = tc->processor;
    info->comm = tc->comm;

    char full_state_buf[64];
    info->state_full = task_state_string(tc->task, full_state_buf, 1);

    info->flags = task_flags(tc->task);
    info->flags_str = parse_task_flags(info->flags);

    int off_prio = field_offset(task_struct, prio);
    if (off_prio >= 0) {
        info->prio = read_int(tc->task + off_prio, "prio");
    }

    int off_nvcsw = field_offset(task_struct, nvcsw);
    if (off_nvcsw >= 0) {
        info->nvcsw = read_ulong(tc->task + off_nvcsw, "nvcsw");
    }

    int off_nivcsw = field_offset(task_struct, nivcsw);
    if (off_nivcsw >= 0) {
        info->nivcsw = read_ulong(tc->task + off_nivcsw, "nivcsw");
    }

    info->mm = task_mm(tc->task, 0);
    if (info->mm) {
        struct task_mem_usage tm;
        BZERO(&tm, sizeof(struct task_mem_usage));
        get_task_mem_usage(tc->task, &tm);
        info->total_vm_kb = (tm.total_vm * PAGESIZE()) / 1024;
        info->rss_kb = (tm.rss * PAGESIZE()) / 1024;
    }

    unwind_task_stack(tc, info->callstack, info->wchan, info->caller, info->category);

    char lbuf[256];
    PRINT("Hung Task Deep-Dive: PID %d (%s)\n", info->pid, info->comm.c_str());
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    snprintf(lbuf, sizeof(lbuf), "  PID:                    %-16d TGID:          %-16d", info->pid, info->tgid);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  PPID:                   %-16d Last CPU:      %-16d", info->ppid, info->cpu);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Task Pointer:           0x%-14lx Parent Task:   0x%-14lx", info->task_addr, tc->ptask);
    PRINT("│ %-76s │\n", lbuf);

    std::string type_str = is_kernel_thread(tc->task) ? "Kernel Thread" : "User Process";
    snprintf(lbuf, sizeof(lbuf), "  Command:                %-16s Type:          %-16s", info->comm.c_str(), type_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string s_full = info->state_full;
    if (s_full.length() > 50) s_full = s_full.substr(0, 47) + "...";
    snprintf(lbuf, sizeof(lbuf), "  State:                  %s", s_full.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string flg_str = info->flags_str;
    if (flg_str.length() > 50) flg_str = flg_str.substr(0, 47) + "...";
    snprintf(lbuf, sizeof(lbuf), "  Flags:                  %s", flg_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Priority:               %-16d Voluntary CSW: %-16lu", info->prio, info->nvcsw);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Involuntary CSW:        %-16lu mm_struct:     0x%-14lx", info->nivcsw, info->mm);
    PRINT("│ %-76s │\n", lbuf);

    if (info->mm) {
        char vm_str[32], rss_str[32];
        snprintf(vm_str, sizeof(vm_str), "%lu KB", info->total_vm_kb);
        snprintf(rss_str, sizeof(rss_str), "%lu KB", info->rss_kb);
        snprintf(lbuf, sizeof(lbuf), "  Virtual Memory:         %-16s RSS:           %-16s", vm_str, rss_str);
        PRINT("│ %-76s │\n", lbuf);
    }

    snprintf(lbuf, sizeof(lbuf), "  Wait Channel (wchan):   %s", info->wchan.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Wait Category:          %s", info->category.c_str());
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Kernel Call Stack (Unwound from cpu_context):");

    if (info->callstack.empty()) {
        PRINT("│ %-76s │\n", "    [Stack unwinding unavailable or task is active on CPU]");
    } else {
        for (size_t i = 0; i < info->callstack.size(); ++i) {
            std::string frame_str = "    #" + std::to_string(i) + "  " + info->callstack[i].sym;
            if (frame_str.length() > 74) frame_str = frame_str.substr(0, 71) + "...";
            snprintf(lbuf, sizeof(lbuf), "%s", frame_str.c_str());
            PRINT("│ %-76s │\n", lbuf);
        }
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void HungTask::print_deadlock_analysis() {
    auto tasks = collect_hung_tasks(false);

    int mem_reclaim_stalls = 0;
    int memalloc_tasks = 0;
    int mutex_stalls = 0;
    int rwsem_stalls = 0;
    int io_stalls = 0;
    int binder_stalls = 0;

    std::map<std::string, int> wchan_counts;

    for (const auto &info : tasks) {
        wchan_counts[info->wchan_fn]++;
        if (info->category == "[Memory Reclaim / Alloc]") mem_reclaim_stalls++;
        else if (info->category == "[Mutex Lock]") mutex_stalls++;
        else if (info->category == "[RW Semaphore]") rwsem_stalls++;
        else if (info->category == "[D-State I/O Wait]") io_stalls++;
        else if (info->category == "[Binder IPC]") binder_stalls++;

        if (info->flags & 0x00000800) { // PF_MEMALLOC
            memalloc_tasks++;
        }
    }

    PRINT("Deadlock & System Blockage Diagnostics\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");
    PRINT("│ %-76s │\n", "[1] Memory Pressure & Deadlock Assessment:");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  - Direct Reclaim Stalled Tasks:  %d tasks currently blocked in reclaim", mem_reclaim_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks with PF_MEMALLOC set:    %d tasks", memalloc_tasks);
    PRINT("│ %-76s │\n", lbuf);

    // Inspect kswapd threads
    for (int i = 0; i < 4; ++i) {
        std::string kname = "kswapd" + std::to_string(i);
        struct task_context *ktc = find_proc(kname);
        if (ktc) {
            char sbuf[32];
            task_state_string(ktc->task, sbuf, 0);
            std::vector<StackFrame> frames;
            std::string kwchan, kcaller, kcat;
            unwind_task_stack(ktc, frames, kwchan, kcaller, kcat);

            std::string alert = (strstr(sbuf, "UN") != NULL) ? " [ALERT: BLOCKED!]" : "";
            snprintf(lbuf, sizeof(lbuf), "  - %-10s : PID %-5ld State: [%-2s] Task: 0x%-14lx%s",
                     kname.c_str(), ktc->pid, sbuf, ktc->task, alert.c_str());
            PRINT("│ %-76s │\n", lbuf);
        }
    }

    PRINT("│ %-76s │\n", " ");
    PRINT("│ %-76s │\n", "[2] Lock Contention & IPC Breakdown:");
    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on Mutexes:      %d", mutex_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on RW Semaphores:%d", rwsem_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on I/O:          %d", io_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on Binder IPC:   %d", binder_stalls);
    PRINT("│ %-76s │\n", lbuf);

    PRINT("│ %-76s │\n", " ");
    PRINT("│ %-76s │\n", "[3] Top Blockage Functions (Wait Channels):");

    std::vector<std::pair<std::string, int>> sorted_wchans(wchan_counts.begin(), wchan_counts.end());
    std::sort(sorted_wchans.begin(), sorted_wchans.end(),
              [](const std::pair<std::string, int> &a, const std::pair<std::string, int> &b) {
                  return a.second > b.second;
              });

    for (size_t i = 0; i < sorted_wchans.size() && i < 4; ++i) {
        snprintf(lbuf, sizeof(lbuf), "  - %-30s : %d tasks waiting",
                 sorted_wchans[i].first.c_str(), sorted_wchans[i].second);
        PRINT("│ %-76s │\n", lbuf);
    }

    PRINT("│ %-76s │\n", " ");
    PRINT("│ %-76s │\n", "[4] Diagnostic Evaluation:");

    if (mem_reclaim_stalls >= 5) {
        PRINT("│ %-76s │\n", "  [!] CRITICAL: Memory Deadlock / Severe Allocation Exhaustion detected!");
        PRINT("│ %-76s │\n", "      Multiple tasks are stalled in direct page reclaim without progress.");
    } else if (mutex_stalls >= 5 || rwsem_stalls >= 5) {
        PRINT("│ %-76s │\n", "  [!] ALERT: High lock contention detected. Tasks are serialized on locks.");
    } else if (tasks.empty()) {
        PRINT("│ %-76s │\n", "  [+] NORMAL: No D-state tasks detected. System scheduling is healthy.");
    } else {
        PRINT("│ %-76s │\n", "  [*] NOTICE: Low D-state task count. Tasks are waiting on standard I/O/IPC.");
    }

    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void HungTask::print_system_states_summary() {
    int r_cnt = 0, s_cnt = 0, d_cnt = 0, id_cnt = 0, t_cnt = 0, z_cnt = 0, other_cnt = 0;
    struct task_context *tc = FIRST_CONTEXT();
    size_t total_tasks = RUNNING_TASKS();

    for (size_t i = 0; i < total_tasks; i++, tc++) {
        if (!tc || !tc->task) continue;
        char sbuf[32];
        task_state_string(tc->task, sbuf, 0);
        if (strstr(sbuf, "RU")) r_cnt++;
        else if (strstr(sbuf, "UN")) d_cnt++;
        else if (strstr(sbuf, "ID")) id_cnt++;
        else if (strstr(sbuf, "IN")) s_cnt++;
        else if (strstr(sbuf, "ST") || strstr(sbuf, "TR")) t_cnt++;
        else if (strstr(sbuf, "ZO") || strstr(sbuf, "DE")) z_cnt++;
        else other_cnt++;
    }

    PRINT("System Task States Summary\n");
    PRINT("┌───────────────────────────┬────────────┬─────────────┐\n");
    PRINT("│ Task State                │      Count │  Percentage │\n");
    PRINT("├───────────────────────────┼────────────┼─────────────┤\n");

    auto print_row = [total_tasks](const char *state_name, int cnt) {
        double pct = (total_tasks > 0) ? ((double)cnt * 100.0 / (double)total_tasks) : 0.0;
        char buf[128];
        snprintf(buf, sizeof(buf), "│ %-25s │%11d │%11.2f%% │\n", state_name, cnt, pct);
        PRINT("%s", buf);
    };

    print_row("Running (R)", r_cnt);
    print_row("Interruptible Sleep (S)", s_cnt);
    print_row("Uninterruptible Sleep (D)", d_cnt);
    print_row("Idle Kernel Sleep (ID)", id_cnt);
    print_row("Stopped / Traced (T/t)", t_cnt);
    print_row("Zombie / Dead (Z/X)", z_cnt);
    if (other_cnt > 0) {
        print_row("Other States", other_cnt);
    }
    PRINT("├───────────────────────────┼────────────┼─────────────┤\n");
    print_row("Total Tasks", total_tasks);
    PRINT("└───────────────────────────┴────────────┴─────────────┘\n");

    PRINT("Kernel Hung Task Detector Configuration:\n");
    if (csymbol_exists("sysctl_hung_task_timeout_secs")) {
        ulong timeout = read_ulong(csymbol_value("sysctl_hung_task_timeout_secs"), "timeout");
        PRINT("  sysctl_hung_task_timeout_secs : %lu s\n", timeout);
    } else {
        PRINT("  sysctl_hung_task_timeout_secs : [default: 120s]\n");
    }

    if (csymbol_exists("sysctl_hung_task_panic")) {
        int panic_val = read_int(csymbol_value("sysctl_hung_task_panic"), "panic");
        PRINT("  sysctl_hung_task_panic        : %d (%s)\n",
              panic_val, panic_val ? "Panic enabled on hung task" : "Disabled");
    }

    if (csymbol_exists("sysctl_hung_task_warnings")) {
        int warn_val = read_int(csymbol_value("sysctl_hung_task_warnings"), "warnings");
        PRINT("  sysctl_hung_task_warnings     : %d\n", warn_val);
    }

    if (csymbol_exists("sysctl_hung_task_check_count")) {
        int chk_val = read_int(csymbol_value("sysctl_hung_task_check_count"), "check_count");
        PRINT("  sysctl_hung_task_check_count  : %d\n", chk_val);
    }
}

#pragma GCC diagnostic pop
