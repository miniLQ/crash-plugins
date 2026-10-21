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
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(HungTask)
#endif

HungTask::HungTask() {
    do_init_offset = true;
}

void HungTask::init_command(void) {
    cmd_name = "hungtask";
    help_str_list = {
        "hungtask",                                               /* command name */
        "analyze D-state tasks, wait channel clustering and stalls", /* short description */
        "[-a] [-l|-w] [-d] [-s] [-m] [-t <pid|comm>]\n"
        "  This command displays D-state (TASK_UNINTERRUPTIBLE) tasks and blockage diagnostics.\n"
        "\n"
        "    -a          display all D-state tasks overview\n"
        "    -l, -w      cluster tasks by wait channel (wchan) bottleneck\n"
        "    -d          display blockage and high lock contention diagnostics\n"
        "    -s          display system-wide task state summary & hung_task sysctl\n"
        "    -m          filter and display tasks stalled in direct memory reclaim\n"
        "    -t <target> inspect single task detail (by PID or comm)\n",
        "\n",
        "EXAMPLES",
        "  Display all D-state tasks:",
        "    %s> hungtask -a\n",
        "  Display wait channel bottleneck clustering:",
        "    %s> hungtask -l\n",
        "  Analyze system-wide blockage and contention:",
        "    %s> hungtask -d\n",
        "  Filter tasks stalled in memory reclaim:",
        "    %s> hungtask -m\n",
        "  Inspect detailed call stack for PID 2541:",
        "    %s> hungtask -t 2541\n",
    };
}

void HungTask::init_offset(void) {
    struct_init(task_struct);
    field_init(task_struct, pid);
    field_init(task_struct, tgid);
    field_init(task_struct, comm);
    field_init(task_struct, flags);
    field_init(task_struct, prio);
    field_init(task_struct, static_prio);
    field_init(task_struct, normal_prio);
    field_init(task_struct, nvcsw);
    field_init(task_struct, nivcsw);
    field_init(task_struct, mm);
    field_init(task_struct, active_mm);
    field_init(task_struct, thread);

    struct_init(thread_struct);
    field_init(thread_struct, cpu_context);

    struct_init(mm_struct);
    field_init(mm_struct, total_vm);
    field_init(mm_struct, rss_stat);
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
        cmd_usage(pc->curcmd, SYNOPSIS);
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

    int action_count = (show_all ? 1 : 0) + (show_cluster ? 1 : 0) +
                       (show_deadlock ? 1 : 0) + (show_states ? 1 : 0) +
                       (show_mem_only ? 1 : 0) + (!target_task_str.empty() ? 1 : 0);
    if (action_count > 1) {
        LOGE("Mutually exclusive options specified\n");
        cmd_usage(pc->curcmd, SYNOPSIS);
        return;
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
        cmd_usage(pc->curcmd, SYNOPSIS);
    }
}

std::string HungTask::get_func_name(const std::string &sym) {
    size_t pos = sym.find('+');
    if (pos != std::string::npos) {
        return sym.substr(0, pos);
    }
    return sym;
}

bool HungTask::is_sched_primitive(const std::string &fn) {
    static const std::unordered_set<std::string> primitives = {
        "schedule",
        "__schedule",
        "schedule_timeout",
        "schedule_timeout_uninterruptible",
        "schedule_timeout_interruptible",
        "io_schedule",
        "io_schedule_timeout",
        "wait_for_completion",
        "wait_for_completion_timeout",
        "wait_for_completion_interruptible",
        "wait_for_completion_interruptible_timeout",
        "do_wait",
        "out_of_line_wait_on_bit",
        "__wait_on_buffer",
        "bit_wait",
        "bit_wait_io",
        "bit_wait_timeout",
        "preempt_schedule_common",
        "preempt_schedule",
        "__switch_to",
        "context_switch"
    };
    return primitives.find(fn) != primitives.end();
}

std::string HungTask::categorize_wait(const std::string &wchan, const std::string &caller) {
    std::string wfn = get_func_name(wchan);
    std::string cfn = get_func_name(caller);
    std::string combined = wfn + " " + cfn;

    // Memory reclaim & allocation wait
    if (combined.find("reclaim") != std::string::npos ||
        combined.find("alloc_pages") != std::string::npos ||
        combined.find("shrink_") != std::string::npos ||
        combined.find("kswapd") != std::string::npos ||
        combined.find("try_to_free_pages") != std::string::npos ||
        combined.find("balance_pgdat") != std::string::npos ||
        combined.find("handle_mm_fault") != std::string::npos) {
        return "[Memory Reclaim / Alloc]";
    }

    // Mutex contention
    if (combined.find("mutex_lock") != std::string::npos ||
        combined.find("__mutex_") != std::string::npos ||
        wfn == "mutex_lock" || cfn == "mutex_lock") {
        return "[Mutex Lock]";
    }

    // Reader-Writer Semaphore contention
    if (combined.find("down_read") != std::string::npos ||
        combined.find("down_write") != std::string::npos ||
        combined.find("rwsem_") != std::string::npos ||
        combined.find("mmap_read_lock") != std::string::npos ||
        combined.find("mmap_write_lock") != std::string::npos) {
        return "[RW Semaphore]";
    }

    // Block I/O wait
    if (combined.find("io_schedule") != std::string::npos ||
        combined.find("blk_") != std::string::npos ||
        combined.find("wait_on_page") != std::string::npos ||
        combined.find("folio_wait") != std::string::npos ||
        combined.find("bdi_") != std::string::npos ||
        combined.find("ufshcd_") != std::string::npos ||
        combined.find("sdhci_") != std::string::npos ||
        combined.find("bit_wait_io") != std::string::npos) {
        return "[D-State I/O Wait]";
    }

    // Completion wait
    if (combined.find("completion") != std::string::npos ||
        combined.find("wait_for_completion") != std::string::npos) {
        return "[Completion]";
    }

    // Binder IPC wait
    if (combined.find("binder_thread_read") != std::string::npos ||
        combined.find("binder_wait_for_work") != std::string::npos ||
        combined.find("binder_ioctl") != std::string::npos ||
        combined.find("binder") != std::string::npos) {
        return "[Binder IPC]";
    }

    // Futex wait
    if (combined.find("futex") != std::string::npos ||
        combined.find("do_futex") != std::string::npos) {
        return "[Futex]";
    }

    // Workqueue flush
    if (combined.find("flush_work") != std::string::npos ||
        combined.find("flush_workqueue") != std::string::npos ||
        combined.find("wait_on_work") != std::string::npos) {
        return "[Workqueue Flush]";
    }

    // Thread spawn / initialization
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
    if (res.empty()) {
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "0x%lx", flags);
        res = hbuf;
    } else if (res.back() == ' ') {
        res.pop_back();
    }
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

    frames = unwind_task_frames(tc, 40);

    // Determine wchan and caller from frames
    int wchan_idx = -1;
    for (size_t i = 0; i < frames.size(); ++i) {
        std::string fn = get_func_name(frames[i].sym);
        if (!is_sched_primitive(fn) && fn != "[unknown]") {
            wchan_idx = (int)i;
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

        ulong state = task_state(tc->task);
        char sbuf[32];
        task_state_string(tc->task, sbuf, 0);

        // Check for TASK_UNINTERRUPTIBLE (D state, bit 0x2 in Linux kernel, or "UN" string)
        if ((state & 0x2) == 0 && strcmp(sbuf, "UN") != 0) {
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

        int off_mm = field_offset(task_struct, mm);
        if (off_mm >= 0) {
            info->mm = read_pointer(tc->task + off_mm, "mm");
            if (info->mm && is_kvaddr(info->mm)) {
                int off_total_vm = field_offset(mm_struct, total_vm);
                if (off_total_vm >= 0) {
                    ulong tvm = read_ulong(info->mm + off_total_vm, "total_vm");
                    info->total_vm_kb = (tvm * (ulong)PAGESIZE()) / 1024;
                }
            }
        }

        unwind_task_stack(tc, info->callstack, info->wchan, info->caller, info->category);
        info->wchan_fn = get_func_name(info->wchan);

        if (mem_only) {
            if (info->category == "[Memory Reclaim / Alloc]" || (info->flags & 0x00000800)) {
                hung_tasks.push_back(info);
            }
        } else {
            hung_tasks.push_back(info);
        }
    }

    return hung_tasks;
}

void HungTask::print_hung_task_overview(bool mem_only) {
    auto tasks = collect_hung_tasks(mem_only);

    if (tasks.empty()) {
        if (mem_only) {
            PRINT("No tasks currently stalled in memory reclaim (D-state).\n");
        } else {
            PRINT("No D-state (TASK_UNINTERRUPTIBLE) tasks detected on system.\n");
        }
        return;
    }

    if (mem_only) {
        PRINT("Memory Reclaim Contention / Direct Stalled Tasks (D-state)\n");
    } else {
        PRINT("D-State (TASK_UNINTERRUPTIBLE) Tasks Overview\n");
    }

    PRINT("┌──────┬──────┬────────────────────────┬────┬────────────────────────────┬────────────────────────────┐\n");
    PRINT("│  PID │ TGID │ Comm                   │ CPU│ Wait Category              │ Wait Channel (wchan)       │\n");
    PRINT("├──────┼──────┼────────────────────────┼────┼────────────────────────────┼────────────────────────────┤\n");

    char buf[512];
    for (const auto &info : tasks) {
        std::string comm_str = info->comm.empty() ? "[unknown]" : info->comm;
        if (comm_str.length() > 22) comm_str = comm_str.substr(0, 19) + "...";

        std::string cat_str = info->category;
        if (cat_str.length() > 26) cat_str = cat_str.substr(0, 23) + "...";

        std::string wchan_disp = info->wchan;
        if (wchan_disp.length() > 26) wchan_disp = wchan_disp.substr(0, 23) + "...";

        snprintf(buf, sizeof(buf),
                 "│%5d │%5d │ %-22s │%3d │ %-26s │ %-26s │\n",
                 info->pid, info->tgid, comm_str.c_str(), info->cpu,
                 cat_str.c_str(), wchan_disp.c_str());
        PRINT("%s", buf);
    }

    PRINT("└──────┴──────┴────────────────────────┴────┴────────────────────────────┴────────────────────────────┘\n");
    PRINT("Total D-State Tasks: %zu\n", tasks.size());
}

void HungTask::print_wchan_clustering() {
    auto tasks = collect_hung_tasks(false);
    if (tasks.empty()) {
        PRINT("No D-state tasks found to cluster.\n");
        return;
    }

    std::map<std::string, WchanCluster> clusters;
    for (const auto &info : tasks) {
        auto &c = clusters[info->wchan_fn];
        c.wchan_fn = info->wchan_fn;
        c.category = info->category;
        c.count++;
        c.tasks.push_back({info->pid, info->comm});
    }

    std::vector<WchanCluster> sorted_clusters;
    for (const auto &kv : clusters) {
        sorted_clusters.push_back(kv.second);
    }
    std::sort(sorted_clusters.begin(), sorted_clusters.end(),
              [](const WchanCluster &a, const WchanCluster &b) {
                  return a.count > b.count;
              });

    PRINT("Wait Channel (wchan) Bottleneck Clustering\n");
    PRINT("┌──────┬──────────────────────────────┬────────────────────────────┬─────────────────────────────┐\n");
    PRINT("│Tasks │ Bottleneck Function          │ Wait Category              │ Sample Blocked Tasks (PIDs) │\n");
    PRINT("├──────┼──────────────────────────────┼────────────────────────────┼─────────────────────────────┤\n");

    char buf[512];
    for (const auto &c : sorted_clusters) {
        std::string fn_disp = c.wchan_fn;
        if (fn_disp.length() > 28) fn_disp = fn_disp.substr(0, 25) + "...";

        std::string cat_disp = c.category;
        if (cat_disp.length() > 26) cat_disp = cat_disp.substr(0, 23) + "...";

        std::string sample_tasks;
        for (size_t i = 0; i < c.tasks.size() && i < 3; ++i) {
            if (i > 0) sample_tasks += ", ";
            sample_tasks += c.tasks[i].second + "(" + std::to_string(c.tasks[i].first) + ")";
        }
        if (c.tasks.size() > 3) {
            sample_tasks += "... (+" + std::to_string(c.tasks.size() - 3) + ")";
        }
        if (sample_tasks.length() > 27) sample_tasks = sample_tasks.substr(0, 24) + "...";

        snprintf(buf, sizeof(buf),
                 "│%5d │ %-28s │ %-26s │ %-27s │\n",
                 c.count, fn_disp.c_str(), cat_disp.c_str(), sample_tasks.c_str());
        PRINT("%s", buf);
    }

    PRINT("└──────┴──────────────────────────────┴────────────────────────────┴─────────────────────────────┘\n");
    PRINT("Total Unique Wait Bottlenecks: %zu\n", sorted_clusters.size());
}

void HungTask::print_task_detail(const std::string &target_str) {
    struct task_context *tc = nullptr;
    try {
        int pid = std::stoi(target_str);
        tc = pid_to_context(pid);
    } catch (...) {
        tc = find_proc(target_str);
    }

    if (!tc || !tc->task) {
        PRINT("Task not found: %s\n", target_str.c_str());
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
    if (off_prio >= 0) info->prio = read_int(tc->task + off_prio, "prio");

    int off_nvcsw = field_offset(task_struct, nvcsw);
    if (off_nvcsw >= 0) info->nvcsw = read_ulong(tc->task + off_nvcsw, "nvcsw");

    int off_nivcsw = field_offset(task_struct, nivcsw);
    if (off_nivcsw >= 0) info->nivcsw = read_ulong(tc->task + off_nivcsw, "nivcsw");

    unwind_task_stack(tc, info->callstack, info->wchan, info->caller, info->category);

    char lbuf[256];
    PRINT("Task Inspection: PID %d (%s)\n", info->pid, info->comm.c_str());
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");
    snprintf(lbuf, sizeof(lbuf), "  Process:               %s (PID: %d, TGID: %d, PPID: %d)",
             info->comm.c_str(), info->pid, info->tgid, info->ppid);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  task_struct:           0x%lx (CPU: %d)", info->task_addr, info->cpu);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  State:                 %s", info->state_full.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Priority:              %d", info->prio);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Context Switches:      nvcsw=%lu, nivcsw=%lu", info->nvcsw, info->nivcsw);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Task Flags:            %s", info->flags_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Wait Category:         %s", info->category.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Wait Channel:          %s", info->wchan.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Blocked Caller:        %s", info->caller.c_str());
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    snprintf(lbuf, sizeof(lbuf), "  Kernel Call Stack (Unwound Depth: %zu):", info->callstack.size());
    PRINT("│ %-76s │\n", lbuf);

    if (info->callstack.empty()) {
        PRINT("│ %-76s │\n", "    [!] Stack unwinding unavailable or task has empty context frame.");
    } else {
        for (size_t i = 0; i < info->callstack.size(); ++i) {
            const auto &frame = info->callstack[i];
            char sline[256];
            snprintf(sline, sizeof(sline), "    #%-2zu [0x%016lx] %s", i, frame.pc, frame.sym.c_str());
            PRINT("│ %-76s │\n", sline);
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

    PRINT("System-wide Blockage & Contention Diagnostic Analysis\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "[1] Total D-State Tasks Analyzed: %zu", tasks.size());
    PRINT("│ %-76s │\n", lbuf);

    PRINT("│ %-76s │\n", " ");
    PRINT("│ %-76s │\n", "[2] Wait Primitive Breakdown:");

    snprintf(lbuf, sizeof(lbuf), "  - Tasks in Direct Reclaim / Alloc: %d (PF_MEMALLOC: %d)",
             mem_reclaim_stalls, memalloc_tasks);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on Mutexes:        %d", mutex_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on RW Semaphores:  %d", rwsem_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on I/O:            %d", io_stalls);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  - Tasks waiting on Binder IPC:     %d", binder_stalls);
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
    PRINT("│ %-76s │\n", "[4] Diagnostic Evaluation (Candidate Root Causes):");

    if (mem_reclaim_stalls >= 5) {
        PRINT("│ %-76s │\n", "  [!] ALERT: Severe Memory Allocation Contention detected.");
        PRINT("│ %-76s │\n", "      Multiple tasks are stalled in direct page reclaim without progress.");
    } else if (mutex_stalls >= 5 || rwsem_stalls >= 5) {
        PRINT("│ %-76s │\n", "  [*] NOTICE: High lock contention detected. Tasks are serialized on locks.");
    } else if (tasks.empty()) {
        PRINT("│ %-76s │\n", "  [+] NORMAL: No D-state tasks detected. System scheduling is healthy.");
    } else {
        PRINT("│ %-76s │\n", "  [*] NOTICE: Low D-state task count. Tasks are waiting on standard I/O/IPC.");
    }

    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void HungTask::print_system_states_summary() {
    struct task_context *tc = FIRST_CONTEXT();
    size_t total_tasks = RUNNING_TASKS();

    std::map<std::string, int> state_distribution;

    for (size_t i = 0; i < total_tasks; i++, tc++) {
        if (!tc || !tc->task) continue;

        char sbuf[32];
        std::string s = task_state_string(tc->task, sbuf, 0);
        state_distribution[s]++;
    }

    PRINT("System-wide Task Execution States Summary\n");
    PRINT("┌───────────────────────────┬────────────┬─────────────┐\n");
    PRINT("│ State Description         │ Code       │  Task Count │\n");
    PRINT("├───────────────────────────┼────────────┼─────────────┤\n");

    for (const auto &kv : state_distribution) {
        std::string desc = "Unknown State";
        if (kv.first == "RU") desc = "TASK_RUNNING (Active / Ready)";
        else if (kv.first == "IN") desc = "TASK_INTERRUPTIBLE (Sleep)";
        else if (kv.first == "UN") desc = "TASK_UNINTERRUPTIBLE (D-state)";
        else if (kv.first == "ZO") desc = "EXIT_ZOMBIE (Terminated)";
        else if (kv.first == "ST") desc = "TASK_STOPPED";
        else if (kv.first == "TR") desc = "TASK_TRACED (Debugged)";
        else if (kv.first == "SW") desc = "TASK_SWAPPING";
        else if (kv.first == "PA") desc = "TASK_PARKED";

        char buf[256];
        snprintf(buf, sizeof(buf), "│ %-25s │ %-10s │ %11d │\n",
                 desc.c_str(), kv.first.c_str(), kv.second);
        PRINT("%s", buf);
    }

    PRINT("└───────────────────────────┴────────────┴─────────────┘\n");

    PRINT("Kernel Hung Task Detector Configuration:\n");
    if (csymbol_exists("sysctl_hung_task_timeout_secs")) {
        ulong timeout = read_ulong(csymbol_value("sysctl_hung_task_timeout_secs"), "timeout");
        PRINT("  sysctl_hung_task_timeout_secs : %lu s\n", timeout);
    } else {
        PRINT("  sysctl_hung_task_timeout_secs : unknown (detector disabled or symbol not found)\n");
    }

    if (csymbol_exists("sysctl_hung_task_panic")) {
        int panic_val = read_int(csymbol_value("sysctl_hung_task_panic"), "panic");
        PRINT("  sysctl_hung_task_panic        : %d (%s)\n",
              panic_val, panic_val ? "Panic enabled on hung task" : "Disabled");
    }
}

#pragma GCC diagnostic pop
