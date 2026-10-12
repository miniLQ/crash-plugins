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

#include "binder_diag.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>
#include <queue>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wwrite-strings"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(BinderDiag)
#endif

BinderDiag::BinderDiag() {}

void BinderDiag::init_command(void) {
    cmd_name = "binder_diag";
    help_str_list = {
        "binder_diag",
        "deep diagnosis for Binder IPC transaction chains, deadlocks, and starvation",
        "[-c] [-d] [-b] [-s] [-q] [-p <pid>] [-o]\n"
        "  This command inspects Android Binder IPC state across all processes,\n"
        "  reconstructs multi-hop transaction wait chains, detects circular deadlocks,\n"
        "  identifies root-cause blocker processes, and diagnoses thread pool starvation.\n\n"
        "  -c, -t        Display synchronous transaction call chains (A -> B -> C) & root cause.\n"
        "  -d            Detect circular deadlocks (ABBA or multi-hop loops) in transactions.\n"
        "  -b            Rank top blocker processes (causing the most downstream waits).\n"
        "  -s            Diagnose Binder thread pool starvation and queue congestion.\n"
        "  -q, -m        Analyze pending todo queues and oneway (async) buffer spam.\n"
        "  -p <pid>      Deep-dive inspection of a specific process's Binder transactions.\n"
        "  -o            Display high-level system overview and health check. (Default)\n",
        "\n",
        "EXAMPLES",
        "  Display system-wide Binder IPC overview and health check:\n"
        "    crash> binder_diag\n\n"
        "  Reconstruct all transaction call chains and locate the root cause:\n"
        "    crash> binder_diag -c\n\n"
        "  Check for circular deadlocks across processes:\n"
        "    crash> binder_diag -d\n\n"
        "  Rank the top blocker processes causing system hang:\n"
        "    crash> binder_diag -b\n\n"
        "  Diagnose thread pool starvation (e.g. system_server or surfaceflinger):\n"
        "    crash> binder_diag -s\n\n"
        "  Inspect transactions and threads for PID 1067:\n"
        "    crash> binder_diag -p 1067\n"
    };
}

void BinderDiag::init_offset(void) {
    struct_init(binder_proc);
    field_init(binder_proc, proc_node);
    field_init(binder_proc, context);
    field_init(binder_proc, max_threads);
    field_init(binder_proc, threads);
    field_init(binder_proc, nodes);
    field_init(binder_proc, refs_by_desc);
    field_init(binder_proc, alloc);
    field_init(binder_proc, todo);
    field_init(binder_proc, pid);
    field_init(binder_proc, tsk);
    field_init(binder_proc, waiting_threads);
    field_init(binder_proc, is_dead);
    field_init(binder_proc, is_frozen);
    field_init(binder_proc, sync_recv);
    field_init(binder_proc, async_recv);
    field_init(binder_proc, requested_threads);
    field_init(binder_proc, requested_threads_started);

    struct_init(binder_thread);
    field_init(binder_thread, proc);
    field_init(binder_thread, pid);
    field_init(binder_thread, looper);
    field_init(binder_thread, looper_need_return);
    field_init(binder_thread, transaction_stack);
    field_init(binder_thread, is_dead);
    field_init(binder_thread, rb_node);
    field_init(binder_thread, tmp_ref);
    field_init(binder_thread, waiting_thread_node);

    struct_init(binder_transaction);
    field_init(binder_transaction, work);
    field_init(binder_transaction, from);
    field_init(binder_transaction, from_parent);
    field_init(binder_transaction, to_parent);
    field_init(binder_transaction, to_thread);
    field_init(binder_transaction, to_proc);
    field_init(binder_transaction, buffer);
    field_init(binder_transaction, debug_id);
    field_init(binder_transaction, code);
    field_init(binder_transaction, flags);
    field_init(binder_transaction, need_reply);
    field_init(binder_transaction, priority);

    struct_init(binder_node);
    field_init(binder_node, debug_id);
    field_init(binder_node, work);
    field_init(binder_node, rb_node);
    field_init(binder_node, async_todo);
    field_init(binder_node, refs);
    field_init(binder_node, proc);
    field_init(binder_node, ptr);
    field_init(binder_node, cookie);

    struct_init(binder_alloc);
    field_init(binder_alloc, buffer);
    field_init(binder_alloc, buffer_size);
    field_init(binder_alloc, free_async_space);

    struct_init(binder_work);
    field_init(binder_work, entry);

    field_init(rb_root, rb_node);
    field_init(list_head, next);
    field_init(binder_context, name);
    field_init(task_struct, comm);
    field_init(task_struct, thread);
    field_init(thread_struct, cpu_context);
}

void BinderDiag::cmd_main(void) {
    int c;
    optind = 0;
    bool show_chains = false;
    bool show_deadlocks = false;
    bool show_blockers = false;
    bool show_starvation = false;
    bool show_queue = false;
    bool show_overview = false;
    int target_pid = 0;

    if (argcnt < 2) {
        collect_all_binder_data();
        print_overview();
        return;
    }

    while ((c = getopt(argcnt, args, "ctdbsqomp:h")) != EOF) {
        switch (c) {
            case 'c':
            case 't':
                show_chains = true;
                break;
            case 'd':
                show_deadlocks = true;
                break;
            case 'b':
                show_blockers = true;
                break;
            case 's':
                show_starvation = true;
                break;
            case 'q':
            case 'm':
                show_queue = true;
                break;
            case 'o':
                show_overview = true;
                break;
            case 'p':
                if (optarg) target_pid = atoi(optarg);
                break;
            case 'h':
            default:
                cmd_usage(pc->curcmd, SYNOPSIS);
                return;
        }
    }

    collect_all_binder_data();

    if (target_pid > 0) {
        print_proc_detail(target_pid);
    } else if (show_deadlocks) {
        print_deadlocks();
    } else if (show_chains) {
        print_call_chains();
    } else if (show_blockers) {
        print_top_blockers();
    } else if (show_starvation) {
        print_thread_pool_starvation();
    } else if (show_queue) {
        print_queue_and_oneway_spam();
    } else if (show_overview) {
        print_overview();
    } else {
        print_overview();
    }
}

void BinderDiag::get_thread_state_and_wchan(int tid, std::string &state_str, std::string &wchan) {
    state_str = "[unknown]";
    wchan = "";
    struct task_context *tc = pid_to_context(tid);
    if (!tc || !tc->task) return;

    char sbuf[32];
    task_state_string(tc->task, sbuf, 0);
    state_str = sbuf;

    ulong fp = 0, pc = 0;
#if defined(ARM64)
    int off_thread = field_offset(task_struct, thread);
    int off_ctx = field_offset(thread_struct, cpu_context);
    if (off_ctx < 0) off_ctx = 0;
    if (off_thread >= 0) {
        ulong ctx_addr = tc->task + off_thread + off_ctx;
        fp = read_pointer(ctx_addr + 80, "context_fp");
        pc = read_pointer(ctx_addr + 96, "context_pc");
    }
#endif

    if (fp) {
        ulong stackbase = GET_STACKBASE(tc->task);
        ulong stacktop = GET_STACKTOP(tc->task);
        if (fp >= stackbase && fp + 16 <= stacktop) {
            ulong curr_fp = fp;
            int max_depth = 20;
            while (curr_fp >= stackbase && curr_fp + 16 <= stacktop && max_depth-- > 0) {
                ulong lr = read_pointer(curr_fp + 8, "lr");
                if (lr && is_kvaddr(lr)) {
                    std::string sym = to_symbol(lr);
                    size_t pos = sym.find('+');
                    std::string fn = (pos != std::string::npos) ? sym.substr(0, pos) : sym;
                    static const std::unordered_set<std::string> primitives = {
                        "__switch_to", "cpu_switch_to", "context_switch",
                        "__schedule", "schedule", "schedule_timeout",
                        "schedule_timeout_interruptible", "schedule_timeout_uninterruptible",
                        "schedule_preempt_disabled"
                    };
                    if (primitives.find(fn) == primitives.end()) {
                        wchan = sym;
                        break;
                    }
                }
                ulong next_fp = read_pointer(curr_fp, "next_fp");
                if (next_fp <= curr_fp || next_fp >= stacktop) break;
                curr_fp = next_fp;
            }
        }
    }
    if (wchan.empty() && pc && is_kvaddr(pc)) {
        wchan = to_symbol(pc);
    }
}

void BinderDiag::collect_all_binder_data() {
    procs_by_addr.clear();
    procs_by_pid.clear();
    threads_by_addr.clear();
    threads_by_tid.clear();
    active_transactions.clear();

    ulong binder_procs = csymbol_value("binder_procs");
    if (!is_kvaddr(binder_procs)) {
        return;
    }

    const int offset = field_offset(binder_proc, proc_node);
    const std::vector<ulong> proc_list = for_each_hlist(binder_procs, offset);

    for (const auto &proc_addr : proc_list) {
        if (!is_kvaddr(proc_addr)) continue;

        void *binder_proc_buf = read_struct(proc_addr, "binder_proc");
        if (!binder_proc_buf) continue;

        auto proc = std::make_shared<DiagProc>();
        proc->proc_addr = proc_addr;
        proc->pid = INT(binder_proc_buf + field_offset(binder_proc, pid));
        proc->tsk = ULONG(binder_proc_buf + field_offset(binder_proc, tsk));
        proc->is_dead = BOOL(binder_proc_buf + field_offset(binder_proc, is_dead));
        proc->is_frozen = BOOL(binder_proc_buf + field_offset(binder_proc, is_frozen));
        proc->max_threads = INT(binder_proc_buf + field_offset(binder_proc, max_threads));
        proc->requested_threads = INT(binder_proc_buf + field_offset(binder_proc, requested_threads));
        proc->requested_threads_started = INT(binder_proc_buf + field_offset(binder_proc, requested_threads_started));

        ulong context_addr = ULONG(binder_proc_buf + field_offset(binder_proc, context));
        ulong threads_rb_node = ULONG(binder_proc_buf + field_offset(binder_proc, threads) + field_offset(rb_root, rb_node));
        ulong waiting_threads_next = ULONG(binder_proc_buf + field_offset(binder_proc, waiting_threads) + field_offset(list_head, next));
        ulong list_head_todo = ULONG(binder_proc_buf + field_offset(binder_proc, todo));
        ulong alloc_addr = proc_addr + field_offset(binder_proc, alloc);

        FREEBUF(binder_proc_buf);

        // Read context name
        if (is_kvaddr(context_addr)) {
            ulong name_addr = read_structure_field(context_addr, "binder_context", "name");
            if (is_kvaddr(name_addr)) {
                proc->context_name = read_cstring(name_addr, 16, "binder_context_name");
            }
        }
        if (proc->context_name.empty()) proc->context_name = "binder";

        // Read process comm
        if (is_kvaddr(proc->tsk)) {
            proc->comm = read_cstring(proc->tsk + field_offset(task_struct, comm), 16, "task_struct_comm");
        }
        if (proc->comm.empty()) {
            struct task_context *tc = pid_to_context(proc->pid);
            if (tc) proc->comm = tc->comm;
            else proc->comm = "[pid:" + std::to_string(proc->pid) + "]";
        }

        // Read alloc info
        if (is_kvaddr(alloc_addr)) {
            void *alloc_buf = read_struct(alloc_addr, "binder_alloc");
            if (alloc_buf) {
                proc->buffer_size = ULONG(alloc_buf + field_offset(binder_alloc, buffer_size));
                proc->free_async_space = ULONG(alloc_buf + field_offset(binder_alloc, free_async_space));
                FREEBUF(alloc_buf);
            }
        }

        // Collect threads
        collect_proc_threads(proc, threads_rb_node, waiting_threads_next);

        // Collect todo items
        collect_proc_todo(proc, list_head_todo);

        procs_by_addr[proc->proc_addr] = proc;
        procs_by_pid[proc->pid] = proc;
    }

    // Now collect active transactions from threads
    for (const auto &kv : threads_by_addr) {
        collect_thread_transactions(kv.second);
    }
}

void BinderDiag::collect_proc_threads(std::shared_ptr<DiagProc> proc, ulong threads_rb_node, ulong wait_threads_next) {
    std::vector<ulong> thread_list = for_each_rbtree(threads_rb_node, field_offset(binder_thread, rb_node));
    std::vector<ulong> wait_thread_list = for_each_list(wait_threads_next, field_offset(binder_thread, waiting_thread_node));

    proc->total_threads = thread_list.size();
    proc->ready_threads = wait_thread_list.size();
    proc->thread_addrs = thread_list;

    for (const auto &thread_addr : thread_list) {
        if (!is_kvaddr(thread_addr)) continue;

        void *thread_buf = read_struct(thread_addr, "binder_thread");
        if (!thread_buf) continue;

        auto t = std::make_shared<DiagThread>();
        t->thread_addr = thread_addr;
        t->proc_addr = proc->proc_addr;
        t->pid = INT(thread_buf + field_offset(binder_thread, pid));
        t->proc_pid = proc->pid;
        t->proc_name = proc->comm;
        t->looper = INT(thread_buf + field_offset(binder_thread, looper));
        t->is_dead = BOOL(thread_buf + field_offset(binder_thread, is_dead));
        t->transaction_stack = ULONG(thread_buf + field_offset(binder_thread, transaction_stack));
        FREEBUF(thread_buf);

        if (t->pid <= 0) continue;

        struct task_context *tc = pid_to_context(t->pid);
        if (tc) t->comm = tc->comm;
        else t->comm = proc->comm;

        get_thread_state_and_wchan(t->pid, t->state_str, t->wchan);

        threads_by_addr[t->thread_addr] = t;
        threads_by_tid[t->pid] = t;
    }
}

void BinderDiag::collect_proc_todo(std::shared_ptr<DiagProc> proc, ulong list_head_todo) {
    const int work_offset = field_offset(binder_work, entry);
    std::vector<ulong> work_list = for_each_list(list_head_todo, work_offset);
    proc->todo_count = work_list.size();

    for (const auto &work_addr : work_list) {
        struct binder_work w;
        if (!read_struct(work_addr, &w, sizeof(w), "binder_work")) continue;

        if (w.type == BINDER_WORK_TRANSACTION) {
            const ulong trans_addr = work_addr - field_offset(binder_transaction, work);
            if (!is_kvaddr(trans_addr)) continue;

            void *trans_buf = read_struct(trans_addr, "binder_transaction");
            if (!trans_buf) continue;

            auto trans = std::make_shared<DiagTransaction>();
            trans->trans_addr = trans_addr;
            trans->is_pending_in_todo = true;
            trans->debug_id = UINT(trans_buf + field_offset(binder_transaction, debug_id));
            trans->code = UINT(trans_buf + field_offset(binder_transaction, code));
            trans->flags = UINT(trans_buf + field_offset(binder_transaction, flags));
            trans->need_reply = UINT(trans_buf + field_offset(binder_transaction, need_reply));
            trans->is_oneway = (trans->flags & 0x10) != 0;
            trans->from_thread_addr = ULONG(trans_buf + field_offset(binder_transaction, from));
            trans->to_proc_addr = ULONG(trans_buf + field_offset(binder_transaction, to_proc));
            trans->to_thread_addr = ULONG(trans_buf + field_offset(binder_transaction, to_thread));
            trans->buffer_addr = ULONG(trans_buf + field_offset(binder_transaction, buffer));
            FREEBUF(trans_buf);

            trans->to_pid = proc->pid;
            trans->to_proc_name = proc->comm;

            if (is_kvaddr(trans->from_thread_addr)) {
                auto from_t = threads_by_addr[trans->from_thread_addr];
                if (from_t) {
                    trans->from_tid = from_t->pid;
                    trans->from_pid = from_t->proc_pid;
                    trans->from_proc_name = from_t->proc_name;
                    trans->from_thread_name = from_t->comm;
                }
            }

            if (trans->need_reply) {
                active_transactions.push_back(trans);
            }
        }
    }
}

void BinderDiag::collect_thread_transactions(std::shared_ptr<DiagThread> thread) {
    ulong t = thread->transaction_stack;
    int depth = 0;
    const int MAX_DEPTH = 30;

    while (t && is_kvaddr(t) && depth++ < MAX_DEPTH) {
        void *trans_buf = read_struct(t, "binder_transaction");
        if (!trans_buf) break;

        ulong t_from = ULONG(trans_buf + field_offset(binder_transaction, from));
        ulong t_from_parent = ULONG(trans_buf + field_offset(binder_transaction, from_parent));
        ulong t_to_parent = ULONG(trans_buf + field_offset(binder_transaction, to_parent));
        ulong t_to_thread = ULONG(trans_buf + field_offset(binder_transaction, to_thread));
        ulong t_to_proc = ULONG(trans_buf + field_offset(binder_transaction, to_proc));
        ulong t_buffer = ULONG(trans_buf + field_offset(binder_transaction, buffer));
        unsigned int debug_id = UINT(trans_buf + field_offset(binder_transaction, debug_id));
        unsigned int code = UINT(trans_buf + field_offset(binder_transaction, code));
        unsigned int flags = UINT(trans_buf + field_offset(binder_transaction, flags));
        unsigned int need_reply = UINT(trans_buf + field_offset(binder_transaction, need_reply));
        FREEBUF(trans_buf);

        if (t_from == thread->thread_addr) {
            // Outgoing transaction from this thread
            if (need_reply) {
                thread->is_waiting = true;
                auto trans = std::make_shared<DiagTransaction>();
                trans->trans_addr = t;
                trans->debug_id = debug_id;
                trans->code = code;
                trans->flags = flags;
                trans->need_reply = true;
                trans->is_oneway = (flags & 0x10) != 0;
                trans->from_thread_addr = thread->thread_addr;
                trans->from_tid = thread->pid;
                trans->from_pid = thread->proc_pid;
                trans->from_proc_name = thread->proc_name;
                trans->from_thread_name = thread->comm;
                trans->to_proc_addr = t_to_proc;
                trans->to_thread_addr = t_to_thread;
                trans->buffer_addr = t_buffer;

                if (is_kvaddr(t_to_proc)) {
                    auto to_p = procs_by_addr[t_to_proc];
                    if (to_p) {
                        trans->to_pid = to_p->pid;
                        trans->to_proc_name = to_p->comm;
                    }
                }

                if (is_kvaddr(t_to_thread)) {
                    auto to_t = threads_by_addr[t_to_thread];
                    if (to_t) {
                        trans->to_tid = to_t->pid;
                        trans->to_thread_name = to_t->comm;
                        if (trans->to_pid == 0) trans->to_pid = to_t->proc_pid;
                        if (trans->to_proc_name.empty()) trans->to_proc_name = to_t->proc_name;
                    }
                }

                // Avoid duplicate if already collected from todo
                bool dup = false;
                for (const auto &ex : active_transactions) {
                    if (ex->debug_id == trans->debug_id) { dup = true; break; }
                }
                if (!dup) {
                    active_transactions.push_back(trans);
                }
            }
            t = t_from_parent;
        } else if (t_to_thread == thread->thread_addr) {
            // Incoming transaction being processed by this thread
            auto proc = procs_by_addr[thread->proc_addr];
            if (proc) proc->active_threads++;
            t = t_to_parent;
        } else {
            break;
        }
    }
}

void BinderDiag::print_overview() {
    int total_procs = procs_by_pid.size();
    int total_threads = threads_by_tid.size();
    int ready_threads = 0;
    int starved_procs = 0;
    int congested_procs = 0;

    for (const auto &kv : procs_by_pid) {
        ready_threads += kv.second->ready_threads;
        if (kv.second->ready_threads == 0 && kv.second->total_threads >= kv.second->max_threads && kv.second->todo_count > 0) {
            starved_procs++;
        }
        if (kv.second->todo_count > 5) {
            congested_procs++;
        }
    }

    std::map<int, std::set<int>> adj;
    std::map<std::pair<int, int>, std::shared_ptr<DiagTransaction>> edge_trans;
    for (const auto &t : active_transactions) {
        if (t->from_pid > 0 && t->to_pid > 0 && t->from_pid != t->to_pid) {
            adj[t->from_pid].insert(t->to_pid);
            edge_trans[{t->from_pid, t->to_pid}] = t;
        }
    }
    auto deadlocks = find_deadlock_cycles(adj, edge_trans);

    PRINT("Android Binder IPC System Overview & Diagnostics\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  Binder Processes:       %-10d Total Binder Threads: %-10d", total_procs, total_threads);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Ready/Waiting Threads:  %-10d Active Sync Calls:     %-10zu", ready_threads, active_transactions.size());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Starved Processes:      %-10d Congested Queues (>5): %-10d", starved_procs, congested_procs);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Deadlock Loops Detected:%-10zu", deadlocks.size());
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Health Assessment & Alerts:");

    if (!deadlocks.empty()) {
        snprintf(lbuf, sizeof(lbuf), "  [!] CRITICAL: %zu circular deadlock(s) detected! System is frozen.", deadlocks.size());
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Run 'binder_diag -d' to view the full deadlock dependency cycles.");
    } else if (starved_procs > 0) {
        snprintf(lbuf, sizeof(lbuf), "  [!] WARNING: %d process(es) have completely exhausted their thread pools!", starved_procs);
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Run 'binder_diag -s' to view starved processes and thread counts.");
    } else if (active_transactions.empty()) {
        PRINT("│ %-76s │\n", "  [+] NORMAL: No synchronous Binder transactions in flight. System is idle.");
    } else {
        snprintf(lbuf, sizeof(lbuf), "  [*] NOTICE: %zu active sync call(s) in progress. No circular deadlocks.", active_transactions.size());
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Run 'binder_diag -c' to inspect transaction wait chains.");
    }

    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void BinderDiag::print_call_chains() {
    if (active_transactions.empty()) {
        PRINT("No active synchronous Binder transactions found in the system.\n");
        return;
    }

    // Map: caller_tid -> transaction
    std::map<int, std::shared_ptr<DiagTransaction>> caller_map;
    std::set<int> callees;
    for (const auto &t : active_transactions) {
        caller_map[t->from_tid] = t;
        if (t->to_tid > 0) callees.insert(t->to_tid);
    }

    // Roots are caller threads that are not themselves targets of another transaction
    std::vector<int> roots;
    for (const auto &kv : caller_map) {
        if (callees.find(kv.first) == callees.end()) {
            roots.push_back(kv.first);
        }
    }
    if (roots.empty()) {
        for (const auto &kv : caller_map) roots.push_back(kv.first);
    }

    PRINT("Active Binder Transaction Call Chains & Root Cause Analysis\n");
    PRINT("==============================================================================\n");

    int chain_idx = 1;
    for (int root_tid : roots) {
        std::set<int> visited;
        int curr_tid = root_tid;
        int depth = 0;

        PRINT("[Chain #%d]\n", chain_idx++);
        std::shared_ptr<DiagTransaction> last_trans = nullptr;

        while (curr_tid > 0 && visited.find(curr_tid) == visited.end()) {
            visited.insert(curr_tid);
            auto it = caller_map.find(curr_tid);
            if (it == caller_map.end()) {
                break;
            }
            auto trans = it->second;
            last_trans = trans;

            std::string indent(depth * 2, ' ');
            auto caller_t = threads_by_tid[trans->from_tid];
            std::string caller_st = caller_t ? caller_t->state_str : "?";

            if (depth == 0) {
                PRINT("%s[%d:%d] %s (%s) [State: %s]\n",
                      indent.c_str(), trans->from_pid, trans->from_tid,
                      trans->from_proc_name.c_str(), trans->from_thread_name.c_str(),
                      caller_st.c_str());
            }

            std::string target_desc;
            if (trans->to_tid > 0) {
                auto target_t = threads_by_tid[trans->to_tid];
                std::string target_st = target_t ? target_t->state_str : "?";
                target_desc = "[" + std::to_string(trans->to_pid) + ":" + std::to_string(trans->to_tid) + "] " +
                              trans->to_proc_name + " (" + trans->to_thread_name + ") [State: " + target_st + "]";
            } else {
                target_desc = "[" + std::to_string(trans->to_pid) + ":todo] " + trans->to_proc_name + " (Pending in Todo Queue)";
            }

            PRINT("%s  └─► %s (trans: %u, code: %u)\n",
                  indent.c_str(), target_desc.c_str(), trans->debug_id, trans->code);

            curr_tid = trans->to_tid;
            depth++;
        }

        // Evaluate root cause at the leaf
        if (last_trans) {
            std::string indent(depth * 2, ' ');
            if (last_trans->to_tid > 0) {
                auto leaf_t = threads_by_tid[last_trans->to_tid];
                if (leaf_t) {
                    if (strstr(leaf_t->state_str.c_str(), "UN") != NULL) {
                        PRINT("%s  ▲ [!] ROOT CAUSE: Leaf thread is blocked in D-state (wchan: %s)\n",
                              indent.c_str(), leaf_t->wchan.c_str());
                    } else if (strstr(leaf_t->state_str.c_str(), "RU") != NULL) {
                        PRINT("%s  ▲ [*] Leaf thread is currently RUNNING on CPU\n", indent.c_str());
                    } else {
                        PRINT("%s  ▲ [*] Leaf thread is waiting in kernel (wchan: %s)\n",
                              indent.c_str(), leaf_t->wchan.c_str());
                    }
                }
            } else {
                auto target_p = procs_by_pid[last_trans->to_pid];
                if (target_p && target_p->ready_threads == 0 && target_p->total_threads >= target_p->max_threads) {
                    PRINT("%s  ▲ [!] ROOT CAUSE: Thread pool STARVED in %s! (0 ready threads, %d/%d active)\n",
                          indent.c_str(), target_p->comm.c_str(), target_p->total_threads, target_p->max_threads);
                } else {
                    PRINT("%s  ▲ [*] Transaction is waiting in process todo queue for available thread\n", indent.c_str());
                }
            }
        }
        PRINT("------------------------------------------------------------------------------\n");
    }
}

std::vector<std::vector<int>> BinderDiag::find_deadlock_cycles(
    const std::map<int, std::set<int>> &adj,
    std::map<std::pair<int, int>, std::shared_ptr<DiagTransaction>> &edge_trans) {

    std::vector<std::vector<int>> cycles;
    std::set<int> visited;
    std::vector<int> path;
    std::set<int> on_path;

    std::function<void(int)> dfs = [&](int u) {
        visited.insert(u);
        path.push_back(u);
        on_path.insert(u);

        auto it = adj.find(u);
        if (it != adj.end()) {
            for (int v : it->second) {
                if (on_path.find(v) != on_path.end()) {
                    // Cycle detected!
                    std::vector<int> cycle;
                    auto pos = std::find(path.begin(), path.end(), v);
                    while (pos != path.end()) {
                        cycle.push_back(*pos);
                        pos++;
                    }
                    cycle.push_back(v);
                    cycles.push_back(cycle);
                } else if (visited.find(v) == visited.end()) {
                    dfs(v);
                }
            }
        }

        on_path.erase(u);
        path.pop_back();
    };

    for (const auto &kv : adj) {
        if (visited.find(kv.first) == visited.end()) {
            dfs(kv.first);
        }
    }

    return cycles;
}

void BinderDiag::print_deadlocks() {
    std::map<int, std::set<int>> adj;
    std::map<std::pair<int, int>, std::shared_ptr<DiagTransaction>> edge_trans;

    for (const auto &t : active_transactions) {
        if (t->from_pid > 0 && t->to_pid > 0 && t->from_pid != t->to_pid) {
            adj[t->from_pid].insert(t->to_pid);
            edge_trans[{t->from_pid, t->to_pid}] = t;
        }
    }

    auto cycles = find_deadlock_cycles(adj, edge_trans);

    PRINT("Binder IPC Deadlock Cycle Detection\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    if (cycles.empty()) {
        PRINT("│ %-76s │\n", "  [+] NORMAL: No circular deadlocks detected in current Binder IPC graph.");
        PRINT("│ %-76s │\n", "      All synchronous calls are organized in acyclic dependency trees.");
        PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
        return;
    }

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  [!] CRITICAL: %zu CIRCULAR DEADLOCK(S) DETECTED!", cycles.size());
    PRINT("│ %-76s │\n", lbuf);
    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");

    int c_idx = 1;
    for (const auto &cycle : cycles) {
        snprintf(lbuf, sizeof(lbuf), "  Deadlock Cycle #%d (Length: %zu steps):", c_idx++, cycle.size() - 1);
        PRINT("│ %-76s │\n", lbuf);

        for (size_t i = 0; i + 1 < cycle.size(); ++i) {
            int u = cycle[i];
            int v = cycle[i + 1];
            auto trans = edge_trans[{u, v}];
            std::string u_name = trans ? trans->from_proc_name : ("PID " + std::to_string(u));
            std::string v_name = trans ? trans->to_proc_name : ("PID " + std::to_string(v));
            unsigned int debug_id = trans ? trans->debug_id : 0;
            unsigned int code = trans ? trans->code : 0;

            snprintf(lbuf, sizeof(lbuf), "    - [%d] %-16s --(trans: %-5u, code: %-3u)--> [%d] %-16s",
                     u, u_name.c_str(), debug_id, code, v, v_name.c_str());
            PRINT("│ %-76s │\n", lbuf);
        }
        PRINT("│ %-76s │\n", " ");
    }

    PRINT("│ %-76s │\n", "  Root Cause Summary: The above processes are mutually waiting on each other,");
    PRINT("│ %-76s │\n", "  resulting in a permanent system stall / Watchdog bite.");
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void BinderDiag::print_top_blockers() {
    std::map<int, BlockerStats> stats_map;

    for (const auto &t : active_transactions) {
        if (t->to_pid <= 0) continue;
        auto &st = stats_map[t->to_pid];
        st.pid = t->to_pid;
        st.proc_name = t->to_proc_name;
        st.total_transactions++;
        if (t->from_pid > 0) st.blocked_pids.insert(t->from_pid);
        if (t->from_tid > 0) st.blocked_tids.insert(t->from_tid);
    }

    std::vector<BlockerStats> stats_list;
    for (const auto &kv : stats_map) {
        stats_list.push_back(kv.second);
    }

    std::sort(stats_list.begin(), stats_list.end(),
              [](const BlockerStats &a, const BlockerStats &b) {
                  if (a.blocked_tids.size() != b.blocked_tids.size())
                      return a.blocked_tids.size() > b.blocked_tids.size();
                  return a.total_transactions > b.total_transactions;
              });

    PRINT("Top Binder Blocker Processes Ranking\n");
    PRINT("┌─────┬──────┬────────────────────────┬────────────────┬────────────────┬──────────────┐\n");
    PRINT("│ Rank│  PID │ Process Name           │ Blocked Procs  │ Blocked Threads│ Total Trans  │\n");
    PRINT("├─────┼──────┼────────────────────────┼────────────────┼────────────────┼──────────────┤\n");

    if (stats_list.empty()) {
        PRINT("│     │      │ [No active blocking transactions]               │                │              │\n");
        PRINT("└─────┴──────┴────────────────────────┴────────────────┴────────────────┴──────────────┘\n");
        return;
    }

    int rank = 1;
    char row[512];
    for (const auto &st : stats_list) {
        std::string name_str = st.proc_name.empty() ? "[unknown]" : st.proc_name;
        if (name_str.length() > 22) name_str = name_str.substr(0, 19) + "...";

        snprintf(row, sizeof(row),
                 "│%4d │%5d │ %-22s │%15zu │%15zu │%13d │\n",
                 rank, st.pid, name_str.c_str(),
                 st.blocked_pids.size(), st.blocked_tids.size(), st.total_transactions);
        PRINT("%s", row);
        rank++;
    }

    PRINT("└─────┴──────┴────────────────────────┴────────────────┴────────────────┴──────────────┘\n");
    if (!stats_list.empty()) {
        PRINT("Primary Blocker: PID %d (%s) blocking %zu thread(s) across %zu process(es)\n",
              stats_list[0].pid, stats_list[0].proc_name.c_str(),
              stats_list[0].blocked_tids.size(), stats_list[0].blocked_pids.size());
    }
}

void BinderDiag::print_thread_pool_starvation() {
    std::vector<std::shared_ptr<DiagProc>> procs;
    for (const auto &kv : procs_by_pid) {
        procs.push_back(kv.second);
    }

    std::sort(procs.begin(), procs.end(),
              [](const std::shared_ptr<DiagProc> &a, const std::shared_ptr<DiagProc> &b) {
                  if (a->todo_count != b->todo_count) return a->todo_count > b->todo_count;
                  return a->total_threads > b->total_threads;
              });

    PRINT("Binder Thread Pool & Queue Health Diagnostics\n");
    PRINT("┌──────┬────────────────────────┬─────┬───────┬──────┬──────┬──────┬────────────┬──────────┐\n");
    PRINT("│  PID │ Process Name           │ Max │ Started│ Ready│ Busy │ Todo │ Free Async │ Status   │\n");
    PRINT("├──────┼────────────────────────┼─────┼───────┼──────┼──────┼──────┼────────────┼──────────┤\n");

    char row[512];
    int starved_count = 0;

    for (const auto &p : procs) {
        // Only display processes that have threads or todo items
        if (p->total_threads == 0 && p->todo_count == 0) continue;

        std::string name_str = p->comm.empty() ? "[unknown]" : p->comm;
        if (name_str.length() > 22) name_str = name_str.substr(0, 19) + "...";

        std::string status = "OK";
        if (p->ready_threads == 0 && p->total_threads >= p->max_threads && p->todo_count > 0) {
            status = "STARVED";
            starved_count++;
        } else if (p->todo_count > 5) {
            status = "CONGESTED";
        } else if (p->buffer_size > 0 && p->free_async_space < (p->buffer_size / 10)) {
            status = "LOW_BUF";
        }

        char async_str[32];
        if (p->buffer_size > 0) {
            snprintf(async_str, sizeof(async_str), "%lu KB", p->free_async_space / 1024);
        } else {
            snprintf(async_str, sizeof(async_str), "N/A");
        }

        snprintf(row, sizeof(row),
                 "│%5d │ %-22s │%4d │%6d │%5d │%5d │%5d │%11s │ %-8s │\n",
                 p->pid, name_str.c_str(), p->max_threads,
                 p->requested_threads_started, p->ready_threads,
                 p->active_threads, p->todo_count, async_str, status.c_str());
        PRINT("%s", row);
    }

    PRINT("└──────┴────────────────────────┴─────┴───────┴──────┴──────┴──────┴────────────┴──────────┘\n");
    if (starved_count > 0) {
        PRINT("\033[31m[!] ALERT: %d process(es) suffer from Thread Pool Starvation!\033[0m\n", starved_count);
        PRINT("    These processes cannot handle incoming RPC calls because all worker threads are occupied.\n");
    }
}

void BinderDiag::print_queue_and_oneway_spam() {
    PRINT("Pending Todo Queue & Oneway (Async) Spam Analysis\n");
    PRINT("┌──────┬────────────────────────┬────────────┬─────────────┬───────────────────────────┐\n");
    PRINT("│  PID │ Target Process         │ Todo Count │ Buffer Size │ Free Async Space          │\n");
    PRINT("├──────┼────────────────────────┼────────────┼─────────────┼───────────────────────────┤\n");

    char row[512];
    int total_pending = 0;
    for (const auto &kv : procs_by_pid) {
        auto p = kv.second;
        if (p->todo_count == 0 && p->buffer_size == 0) continue;
        total_pending += p->todo_count;

        std::string name_str = p->comm.empty() ? "[unknown]" : p->comm;
        if (name_str.length() > 22) name_str = name_str.substr(0, 19) + "...";

        char buf_str[32], async_str[32];
        snprintf(buf_str, sizeof(buf_str), "%lu KB", p->buffer_size / 1024);
        snprintf(async_str, sizeof(async_str), "%lu KB", p->free_async_space / 1024);

        snprintf(row, sizeof(row),
                 "│%5d │ %-22s │%11d │%12s │%26s │\n",
                 p->pid, name_str.c_str(), p->todo_count, buf_str, async_str);
        PRINT("%s", row);
    }

    PRINT("└──────┴────────────────────────┴────────────┴─────────────┴───────────────────────────┘\n");
    PRINT("Total pending transactions in todo queues: %d\n", total_pending);
}

void BinderDiag::print_proc_detail(int target_pid) {
    auto it = procs_by_pid.find(target_pid);
    if (it == procs_by_pid.end()) {
        PRINT("Process PID %d is not registered as an active Binder process.\n", target_pid);
        return;
    }
    auto p = it->second;

    PRINT("Binder Process Deep-Dive: %s (PID %d)\n", p->comm.c_str(), p->pid);
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  PID:                    %-16d Context:       %-16s", p->pid, p->context_name.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Max Threads:            %-16d Total Threads: %-16d", p->max_threads, p->total_threads);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Ready/Waiting Threads:  %-16d Active Busy:   %-16d", p->ready_threads, p->active_threads);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Pending Todo Count:     %-16d Dead / Frozen: %d / %d", p->todo_count, p->is_dead, p->is_frozen);
    PRINT("│ %-76s │\n", lbuf);

    char buf_str[32], async_str[32];
    snprintf(buf_str, sizeof(buf_str), "%lu KB", p->buffer_size / 1024);
    snprintf(async_str, sizeof(async_str), "%lu KB", p->free_async_space / 1024);
    snprintf(lbuf, sizeof(lbuf), "  Buffer Size:            %-16s Free Async:    %-16s", buf_str, async_str);
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Incoming Transactions (Who is waiting for this process):");

    int inc_count = 0;
    for (const auto &t : active_transactions) {
        if (t->to_pid == p->pid) {
            inc_count++;
            snprintf(lbuf, sizeof(lbuf), "    - From [%d:%d] %-14s (trans: %-5u, code: %-3u, oneway: %d)",
                     t->from_pid, t->from_tid, t->from_proc_name.c_str(),
                     t->debug_id, t->code, t->is_oneway);
            PRINT("│ %-76s │\n", lbuf);
        }
    }
    if (inc_count == 0) {
        PRINT("│ %-76s │\n", "    [No incoming transactions]");
    }

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Outgoing Transactions (Who this process is waiting for):");

    int out_count = 0;
    for (const auto &t : active_transactions) {
        if (t->from_pid == p->pid) {
            out_count++;
            snprintf(lbuf, sizeof(lbuf), "    - Thread %-5d --> [%d:%d] %-14s (trans: %-5u, code: %-3u)",
                     t->from_tid, t->to_pid, t->to_tid, t->to_proc_name.c_str(),
                     t->debug_id, t->code);
            PRINT("│ %-76s │\n", lbuf);
        }
    }
    if (out_count == 0) {
        PRINT("│ %-76s │\n", "    [No outgoing transactions]");
    }

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Binder Threads List & States:");

    for (ulong t_addr : p->thread_addrs) {
        auto t = threads_by_addr[t_addr];
        if (!t) continue;
        std::string wait_info = t->is_waiting ? " [WAITING]" : "";
        snprintf(lbuf, sizeof(lbuf), "    - TID %-6d %-16s State: [%-5s] wchan: %-18s%s",
                 t->pid, t->comm.c_str(), t->state_str.c_str(), t->wchan.c_str(), wait_info.c_str());
        PRINT("│ %-76s │\n", lbuf);
    }

    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

#pragma GCC diagnostic pop
