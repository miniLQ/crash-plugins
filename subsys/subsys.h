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

#ifndef SUBSYS_DEFS_H_
#define SUBSYS_DEFS_H_

#include "plugin.h"
#include <vector>
#include <string>
#include <map>
#include <memory>

struct SubsysSubdev {
    ulong addr = 0;
    std::string type;
    std::string ssr_name;
    ulong prepare_fn = 0;
    ulong start_fn = 0;
    ulong stop_fn = 0;
    ulong unprepare_fn = 0;
    std::string prepare_sym;
    std::string start_sym;
    std::string stop_sym;
    std::string unprepare_sym;
};

struct SubsysDumpSegment {
    ulong da = 0;
    size_t size = 0;
};

struct SubsysInfo {
    ulong rproc_addr = 0;
    std::string name;
    std::string alias_type;
    std::string firmware;
    unsigned int state_raw = 0;
    std::string state_str;
    unsigned int crash_cnt = 0;
    bool recovery_disabled = false;
    bool auto_boot = false;
    int dump_conf = 0;
    std::string dump_conf_str;
    int power_count = 0;
    ulong priv_addr = 0;
    ulong ops_addr = 0;
    std::string ops_start_sym;
    std::string ops_stop_sym;
    std::string ops_coredump_sym;
    std::string ops_panic_sym;

    // Crash Handler
    ulong crash_handler_work = 0;
    ulong crash_handler_func = 0;
    std::string crash_handler_func_sym;
    bool crash_handler_pending = false;

    // Crash reason if any (from SMEM or sysmon)
    std::string crash_reason;

    // Subdevices
    std::vector<SubsysSubdev> subdevices;

    // Dump segments
    size_t dump_segment_count = 0;
    size_t total_dump_size = 0;
    std::vector<SubsysDumpSegment> dump_segments;
};

struct SSRClientNotifier {
    ulong nb_addr = 0;
    ulong callback_addr = 0;
    std::string callback_sym;
    int priority = 0;
};

struct SSRSubsystem {
    ulong addr = 0;
    std::string name;
    std::vector<SSRClientNotifier> clients;
};

struct SmemStateInfo {
    ulong addr = 0;
    std::string node_name;
    int refcount = 0;
    ulong ops_update_bits = 0;
    std::string update_bits_sym;
};

class Subsys : public ParserPlugin {
private:
    std::vector<std::shared_ptr<SubsysInfo>> subsys_list;
    std::vector<std::shared_ptr<SSRSubsystem>> ssr_subsystems;
    std::vector<std::shared_ptr<SmemStateInfo>> smem_states_list;
    bool is_shutdown_in_progress = false;

    void collect_all_data();
    void collect_remoteprocs();
    void collect_legacy_subsys();
    void collect_ssr_notifiers();
    void collect_smem_states();

    std::string deduce_alias_type(const std::string &name, const std::string &fw);
    std::string read_smem_crash_reason(int smem_id);
    bool matches_target(const std::shared_ptr<SubsysInfo> &s, const std::string &target);

    void print_overview();
    void print_crashes(const std::string &target = "");
    void print_detail(const std::string &target);
    void print_notifiers();
    void print_smem_states();

public:
    Subsys();
    void cmd_main(void) override;
    void init_offset(void) override;
    void init_command(void) override;
    DEFINE_PLUGIN_INSTANCE(Subsys)
};

#endif // SUBSYS_DEFS_H_
