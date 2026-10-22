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

#ifndef HUNGTASK_DEFS_H_
#define HUNGTASK_DEFS_H_

#include "plugin.h"
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <unordered_set>

struct HungTaskInfo {
    struct task_context *tc = nullptr;
    ulong task_addr = 0;
    int pid = 0;
    int ppid = 0;
    int tgid = 0;
    int cpu = 0;
    std::string comm;
    std::string state_short;
    std::string state_full;
    ulong flags = 0;
    std::string flags_str;
    int prio = 0;
    ulong nvcsw = 0;
    ulong nivcsw = 0;
    ulong mm = 0;
    ulong rss_kb = 0;
    ulong total_vm_kb = 0;
    std::string wchan;
    std::string wchan_fn;
    std::string caller;
    std::string category;
    std::vector<StackFrame> callstack;
};

struct WchanCluster {
    std::string wchan_fn;
    std::string category;
    int count = 0;
    std::vector<std::pair<int, std::string>> tasks; // (pid, comm)
};

class HungTask : public ParserPlugin {
private:
    std::vector<std::shared_ptr<HungTaskInfo>> collect_hung_tasks(bool mem_only = false);
    void unwind_task_stack(struct task_context *tc,
                           std::vector<StackFrame> &frames,
                           std::string &wchan,
                           std::string &caller,
                           std::string &category);
    std::string get_func_name(const std::string &sym);
    bool is_sched_primitive(const std::string &fn);
    std::string categorize_wait(const std::string &wchan, const std::string &caller);
    std::string parse_task_flags(ulong flags);

    void print_hung_task_overview(bool mem_only = false);
    void print_wchan_clustering();
    void print_task_detail(const std::string &target_str);
    void print_deadlock_analysis();
    void print_system_states_summary();

public:
    HungTask();
    void cmd_main(void) override;
    void init_offset(void) override;
    void init_command(void) override;
    DEFINE_PLUGIN_INSTANCE(HungTask)
};

#endif // HUNGTASK_DEFS_H_
