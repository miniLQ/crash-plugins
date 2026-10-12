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

#ifndef BINDER_DIAG_DEFS_H_
#define BINDER_DIAG_DEFS_H_

#include "plugin.h"
#include "binder/binder.h"
#include <vector>
#include <string>
#include <map>
#include <set>
#include <memory>
#include <unordered_map>
#include <unordered_set>

struct DiagThread {
    ulong thread_addr = 0;
    ulong proc_addr = 0;
    int pid = 0;             // TID
    int proc_pid = 0;        // Process PID
    std::string comm;        // Thread name
    std::string proc_name;   // Process name
    int looper = 0;
    bool is_dead = false;
    ulong transaction_stack = 0;
    std::string state_str;   // Task state e.g. D(UN), S(IN)
    std::string wchan;       // Wait channel function
    bool is_waiting = false; // Is this thread blocked on an outgoing transaction?
};

struct DiagProc {
    ulong proc_addr = 0;
    int pid = 0;
    ulong tsk = 0;
    std::string comm;
    std::string context_name;
    bool is_dead = false;
    bool is_frozen = false;
    int max_threads = 0;
    int requested_threads = 0;
    int requested_threads_started = 0;
    int total_threads = 0;
    int ready_threads = 0;   // In waiting_threads list
    int active_threads = 0;  // Currently in transactions
    int todo_count = 0;      // Pending works in proc->todo
    int async_todo_count = 0;
    ulong buffer_size = 0;
    ulong free_async_space = 0;
    std::vector<ulong> thread_addrs;
};

struct DiagTransaction {
    ulong trans_addr = 0;
    unsigned int debug_id = 0;
    unsigned int code = 0;
    unsigned int flags = 0;
    bool need_reply = false;
    bool is_oneway = false;

    // Caller
    ulong from_thread_addr = 0;
    int from_pid = 0;
    int from_tid = 0;
    std::string from_proc_name;
    std::string from_thread_name;

    // Callee
    ulong to_proc_addr = 0;
    ulong to_thread_addr = 0;
    int to_pid = 0;
    int to_tid = 0;
    std::string to_proc_name;
    std::string to_thread_name;

    bool is_pending_in_todo = false;
    ulong buffer_addr = 0;
    size_t data_size = 0;
};

struct BlockerStats {
    int pid = 0;
    std::string proc_name;
    std::set<int> blocked_pids;
    std::set<int> blocked_tids;
    int total_transactions = 0;
    std::string leaf_status;
};

class BinderDiag : public ParserPlugin {
private:
    std::map<ulong, std::shared_ptr<DiagProc>> procs_by_addr;
    std::map<int, std::shared_ptr<DiagProc>> procs_by_pid;
    std::map<ulong, std::shared_ptr<DiagThread>> threads_by_addr;
    std::map<int, std::shared_ptr<DiagThread>> threads_by_tid;
    std::vector<std::shared_ptr<DiagTransaction>> active_transactions;

    void collect_all_binder_data();
    void collect_proc_threads(std::shared_ptr<DiagProc> proc, ulong threads_rb_node, ulong wait_threads_next);
    void collect_proc_todo(std::shared_ptr<DiagProc> proc, ulong list_head_todo);
    void collect_thread_transactions(std::shared_ptr<DiagThread> thread);

    void get_thread_state_and_wchan(int tid, std::string &state_str, std::string &wchan);

    // Diagnostic features
    void print_overview();
    void print_call_chains();
    void print_deadlocks();
    void print_top_blockers();
    void print_thread_pool_starvation();
    void print_queue_and_oneway_spam();
    void print_proc_detail(int target_pid);

    std::vector<std::vector<int>> find_deadlock_cycles(
        const std::map<int, std::set<int>> &adj,
        std::map<std::pair<int, int>, std::shared_ptr<DiagTransaction>> &edge_trans);

public:
    BinderDiag();
    void cmd_main(void) override;
    void init_offset(void) override;
    void init_command(void) override;
    DEFINE_PLUGIN_INSTANCE(BinderDiag)
};

#endif // BINDER_DIAG_DEFS_H_
