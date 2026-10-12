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

#include "subsys.h"
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <cstring>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"
#pragma GCC diagnostic ignored "-Wwrite-strings"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(Subsys)
#endif

Subsys::Subsys() {}

void Subsys::init_command(void) {
    cmd_name = "subsys";
    help_str_list = {
        "subsys",
        "analyze Qualcomm peripheral subsystems, SSR events, and crash states",
        "[-a] [-c [name|addr]] [-s <name|addr>] [<name|addr>] [-n|-l] [-m]\n"
        "  This command inspects Qualcomm peripheral subsystems (Modem/MPSS,\n"
        "  Audio/ADSP, Compute/CDSP, Sensors/SLPI, Neural/NPU) managed by the\n"
        "  Linux remoteproc and Subsystem Restart (SSR) framework.\n\n"
        "  -a            Display overview of all peripheral subsystems. (Default)\n"
        "  -c [name|addr]Diagnose crashed subsystems, SSR restart status, and SMEM crash reasons.\n"
        "  -s <name|addr>Deep-dive inspection of a specific subsystem (ops, subdevs, coredump).\n"
        "  <name|addr>   Shortcut to deep-dive inspection (e.g. 'subsys modem', 'subsys adsp').\n"
        "  -n, -l        Display SSR notification listener matrix (clients receiving restart events).\n"
        "  -m            Display Qualcomm SMEM and SMP2P communication states.\n",
        "\n",
        "EXAMPLES",
        "  Display system-wide peripheral subsystem overview:\n"
        "    crash> subsys\n\n"
        "  Deep-dive into Modem or ADSP directly by name or address:\n"
        "    crash> subsys modem\n"
        "    crash> subsys adsp\n\n"
        "  Diagnose subsystem crashes and SSR failure reasons:\n"
        "    crash> subsys -c\n"
        "    crash> subsys -c adsp\n\n"
        "  Display all kernel drivers listening for SSR restart events:\n"
        "    crash> subsys -n\n\n"
        "  Display Qualcomm shared memory (SMEM) and SMP2P communication channels:\n"
        "    crash> subsys -m\n"
    };
}

void Subsys::init_offset(void) {
    struct_init(rproc);
    field_init(rproc, node);
    field_init(rproc, name);
    field_init(rproc, firmware);
    field_init(rproc, priv);
    field_init(rproc, ops);
    field_init(rproc, power);
    field_init(rproc, state);
    field_init(rproc, dump_conf);
    field_init(rproc, subdevs);
    field_init(rproc, crash_handler);
    field_init(rproc, crash_cnt);
    field_init(rproc, recovery_disabled);
    field_init(rproc, auto_boot);
    field_init(rproc, dump_segments);

    struct_init(rproc_ops);
    field_init(rproc_ops, start);
    field_init(rproc_ops, stop);
    field_init(rproc_ops, coredump);
    field_init(rproc_ops, panic);

    struct_init(rproc_subdev);
    field_init(rproc_subdev, node);
    field_init(rproc_subdev, prepare);
    field_init(rproc_subdev, start);
    field_init(rproc_subdev, stop);
    field_init(rproc_subdev, unprepare);

    struct_init(rproc_dump_segment);
    field_init(rproc_dump_segment, node);
    field_init(rproc_dump_segment, da);
    field_init(rproc_dump_segment, size);

    struct_init(work_struct);
    field_init(work_struct, entry);
    field_init(work_struct, func);

    struct_init(notifier_block);
    field_init(notifier_block, notifier_call);
    field_init(notifier_block, next);
    field_init(notifier_block, priority);

    struct_init(device_node);
    field_init(device_node, name);
    field_init(device_node, full_name);

    field_init(list_head, next);
    field_init(list_head, prev);
}

void Subsys::cmd_main(void) {
    int c;
    optind = 0;
    bool show_crashes = false;
    bool show_notifiers = false;
    bool show_smem = false;
    std::string target_subsys;

    if (argcnt < 2) {
        collect_all_data();
        print_overview();
        return;
    }

    while ((c = getopt(argcnt, args, "acnls:mh")) != EOF) {
        switch (c) {
            case 'a':
                // Overview
                break;
            case 'c':
                show_crashes = true;
                break;
            case 'n':
            case 'l':
                show_notifiers = true;
                break;
            case 's':
                if (optarg) target_subsys = optarg;
                break;
            case 'm':
                show_smem = true;
                break;
            case 'h':
            default:
                argerrs++;
                break;
        }
    }

    while (optind < argcnt) {
        if (target_subsys.empty()) {
            target_subsys = args[optind];
        }
        optind++;
    }

    if (argerrs) {
        cmd_usage(pc->curcmd, SYNOPSIS);
        return;
    }

    collect_all_data();

    if (show_crashes) {
        print_crashes(target_subsys);
    } else if (!target_subsys.empty()) {
        print_detail(target_subsys);
    } else if (show_notifiers) {
        print_notifiers();
    } else if (show_smem) {
        print_smem_states();
    } else {
        print_overview();
    }
}

bool Subsys::matches_target(const std::shared_ptr<SubsysInfo> &s, const std::string &target) {
    if (target.empty()) return true;

    std::string lower_target = target;
    std::transform(lower_target.begin(), lower_target.end(), lower_target.begin(), ::tolower);

    std::string lower_name = s->name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    std::string lower_alias = s->alias_type;
    std::transform(lower_alias.begin(), lower_alias.end(), lower_alias.begin(), ::tolower);

    char hex_addr[32];
    snprintf(hex_addr, sizeof(hex_addr), "%lx", s->rproc_addr);

    return (lower_name.find(lower_target) != std::string::npos ||
            lower_alias.find(lower_target) != std::string::npos ||
            lower_target == hex_addr);
}

std::string Subsys::deduce_alias_type(const std::string &name, const std::string &fw) {
    std::string lower_name = name;
    std::string lower_fw = fw;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    std::transform(lower_fw.begin(), lower_fw.end(), lower_fw.begin(), ::tolower);

    if (lower_name.find("mss") != std::string::npos || lower_name.find("modem") != std::string::npos ||
        lower_fw.find("modem") != std::string::npos || lower_fw.find("mba") != std::string::npos) {
        return "Modem / MPSS";
    }
    if (lower_name.find("adsp") != std::string::npos || lower_name.find("lpass") != std::string::npos ||
        lower_fw.find("adsp") != std::string::npos || lower_fw.find("lpass") != std::string::npos) {
        return "Audio / ADSP";
    }
    if (lower_name.find("cdsp") != std::string::npos || lower_name.find("turing") != std::string::npos ||
        lower_fw.find("cdsp") != std::string::npos) {
        return "Compute / CDSP";
    }
    if (lower_name.find("slpi") != std::string::npos || lower_name.find("dsps") != std::string::npos ||
        lower_fw.find("slpi") != std::string::npos) {
        return "Sensors / SLPI";
    }
    if (lower_name.find("npu") != std::string::npos || lower_fw.find("npu") != std::string::npos) {
        return "Neural / NPU";
    }
    if (lower_name.find("wcss") != std::string::npos || lower_name.find("wpss") != std::string::npos ||
        lower_fw.find("wcss") != std::string::npos || lower_fw.find("wlan") != std::string::npos) {
        return "Wireless / WPSS";
    }
    return "Remoteproc";
}

std::string Subsys::read_smem_crash_reason(int smem_id) {
    if (smem_id <= 0) return "";
    if (!csymbol_exists("__smem")) return "";

    ulong smem_addr = read_pointer(csymbol_value("__smem"), "__smem");
    if (!smem_addr || !is_kvaddr(smem_addr)) return "";

    uint item_count = read_uint(smem_addr + 16, "item_count");
    if (item_count > 0 && (uint)smem_id >= item_count) return "";

    // Scan global partition and host partitions in smem
    // In Linux 6.1+, global_partition is at offset 40 (struct smem_partition: virt_base at +0)
    std::vector<ulong> partition_bases;
    ulong global_virt = read_pointer(smem_addr + 40, "global_partition_virt");
    if (global_virt && is_kvaddr(global_virt)) {
        partition_bases.push_back(global_virt);
    }

    // Host partitions start at offset 72 (14 entries of 32 bytes each)
    for (int i = 0; i < 14; i++) {
        ulong part_virt = read_pointer(smem_addr + 72 + i * 32, "host_partition_virt");
        if (part_virt && is_kvaddr(part_virt)) {
            partition_bases.push_back(part_virt);
        }
    }

    for (ulong part_base : partition_bases) {
        // struct smem_partition_header: offset_free_uncached at offset 12
        uint offset_free_uncached = read_uint(part_base + 12, "offset_free_uncached");
        if (offset_free_uncached == 0 || offset_free_uncached > 0x1000000) continue;

        ulong curr = part_base + 32; // Uncached entries start after partition header (32 bytes)
        ulong end = part_base + offset_free_uncached;

        int safety_loop = 0;
        while (curr + 16 <= end && safety_loop++ < 2000) {
            ushort canary = read_ushort(curr, "canary");
            if (canary != 0xa5a5) break;

            ushort item = read_ushort(curr + 2, "item");
            uint size = read_uint(curr + 4, "size");
            ushort padding_hdr = read_ushort(curr + 10, "padding_hdr");

            if (item == (ushort)smem_id) {
                ulong data_addr = curr + 16 + padding_hdr;
                if (is_kvaddr(data_addr)) {
                    std::string reason = read_cstring(data_addr, 256, "crash_reason");
                    // Clean non-printable characters
                    std::string clean_str;
                    for (char ch : reason) {
                        if (ch >= 32 && ch <= 126) clean_str += ch;
                        else if (ch == '\n' || ch == '\r') clean_str += ' ';
                    }
                    if (!clean_str.empty()) return clean_str;
                }
            }

            ulong step = 16 + padding_hdr + size;
            step = (step + 7) & ~7ULL; // 8-byte aligned
            if (step == 0) break;
            curr += step;
        }
    }

    return "";
}

void Subsys::collect_remoteprocs() {
    if (!csymbol_exists("rproc_list")) return;

    ulong rproc_list_head = csymbol_value("rproc_list");
    int off_node = field_offset(rproc, node);
    if (off_node < 0) off_node = 0;

    std::vector<ulong> rprocs = for_each_list(rproc_list_head, off_node);
    for (ulong rproc_addr : rprocs) {
        if (!is_kvaddr(rproc_addr)) continue;

        auto info = std::make_shared<SubsysInfo>();
        info->rproc_addr = rproc_addr;

        ulong name_ptr = read_pointer(rproc_addr + field_offset(rproc, name), "rproc_name");
        if (name_ptr && is_kvaddr(name_ptr)) {
            info->name = read_cstring(name_ptr, 64, "rproc_name");
        }
        if (info->name.empty()) {
            info->name = "remoteproc-" + std::to_string(info->rproc_addr & 0xfff);
        }

        ulong fw_ptr = read_pointer(rproc_addr + field_offset(rproc, firmware), "firmware");
        if (fw_ptr && is_kvaddr(fw_ptr)) {
            info->firmware = read_cstring(fw_ptr, 64, "firmware");
        }
        if (info->firmware.empty()) info->firmware = "-";

        info->alias_type = deduce_alias_type(info->name, info->firmware);
        info->priv_addr = read_pointer(rproc_addr + field_offset(rproc, priv), "priv");
        info->ops_addr = read_pointer(rproc_addr + field_offset(rproc, ops), "ops");
        info->state_raw = read_uint(rproc_addr + field_offset(rproc, state), "state");
        info->crash_cnt = read_uint(rproc_addr + field_offset(rproc, crash_cnt), "crash_cnt");
        info->recovery_disabled = read_bool(rproc_addr + field_offset(rproc, recovery_disabled), "recovery_disabled");
        info->auto_boot = read_bool(rproc_addr + field_offset(rproc, auto_boot), "auto_boot");
        info->dump_conf = read_int(rproc_addr + field_offset(rproc, dump_conf), "dump_conf");
        info->power_count = read_int(rproc_addr + field_offset(rproc, power), "power");

        switch (info->state_raw) {
            case 0: info->state_str = "OFFLINE"; break;
            case 1: info->state_str = "SUSPENDED"; break;
            case 2: info->state_str = "ONLINE"; break;
            case 3: info->state_str = "CRASHED"; break;
            case 4: info->state_str = "DELETED"; break;
            case 5: info->state_str = "ATTACHED"; break;
            case 6: info->state_str = "DETACHED"; break;
            default: info->state_str = "UNKNOWN(" + std::to_string(info->state_raw) + ")"; break;
        }

        switch (info->dump_conf) {
            case 0: info->dump_conf_str = "DISABLED"; break;
            case 1: info->dump_conf_str = "ENABLED"; break;
            case 2: info->dump_conf_str = "INLINE"; break;
            default: info->dump_conf_str = "UNKNOWN"; break;
        }

        // Driver ops
        if (info->ops_addr && is_kvaddr(info->ops_addr)) {
            ulong op_start = read_pointer(info->ops_addr + field_offset(rproc_ops, start), "ops_start");
            ulong op_stop = read_pointer(info->ops_addr + field_offset(rproc_ops, stop), "ops_stop");
            ulong op_coredump = read_pointer(info->ops_addr + field_offset(rproc_ops, coredump), "ops_coredump");
            ulong op_panic = read_pointer(info->ops_addr + field_offset(rproc_ops, panic), "ops_panic");

            if (op_start) info->ops_start_sym = to_symbol(op_start);
            if (op_stop) info->ops_stop_sym = to_symbol(op_stop);
            if (op_coredump) info->ops_coredump_sym = to_symbol(op_coredump);
            if (op_panic) info->ops_panic_sym = to_symbol(op_panic);
        }

        // Crash Handler Work
        ulong work_addr = rproc_addr + field_offset(rproc, crash_handler);
        info->crash_handler_work = work_addr;
        ulong func_addr = read_pointer(work_addr + field_offset(work_struct, func), "func");
        if (func_addr) {
            info->crash_handler_func = func_addr;
            info->crash_handler_func_sym = to_symbol(func_addr);
        }
        ulong entry_next = read_pointer(work_addr + field_offset(work_struct, entry) + field_offset(list_head, next), "entry_next");
        if (entry_next && is_kvaddr(entry_next) && entry_next != (work_addr + field_offset(work_struct, entry))) {
            info->crash_handler_pending = true;
        }

        // Subdevices
        ulong subdevs_head = rproc_addr + field_offset(rproc, subdevs);
        int off_subdev_node = field_offset(rproc_subdev, node);
        if (off_subdev_node < 0) off_subdev_node = 0;
        std::vector<ulong> subdev_addrs = for_each_list(subdevs_head, off_subdev_node);
        for (ulong sd_addr : subdev_addrs) {
            if (!is_kvaddr(sd_addr)) continue;
            SubsysSubdev sd;
            sd.addr = sd_addr;
            sd.prepare_fn = read_pointer(sd_addr + field_offset(rproc_subdev, prepare), "prep");
            sd.start_fn = read_pointer(sd_addr + field_offset(rproc_subdev, start), "start");
            sd.stop_fn = read_pointer(sd_addr + field_offset(rproc_subdev, stop), "stop");
            sd.unprepare_fn = read_pointer(sd_addr + field_offset(rproc_subdev, unprepare), "unprep");

            if (sd.prepare_fn) sd.prepare_sym = to_symbol(sd.prepare_fn);
            if (sd.start_fn) sd.start_sym = to_symbol(sd.start_fn);
            if (sd.stop_fn) sd.stop_sym = to_symbol(sd.stop_fn);
            if (sd.unprepare_fn) sd.unprepare_sym = to_symbol(sd.unprepare_fn);

            if (sd.start_sym.find("glink") != std::string::npos) {
                sd.type = "GLINK (IPC)";
                ulong ssr_name_ptr = read_pointer(sd_addr + 48, "glink_ssr_name");
                if (ssr_name_ptr && is_kvaddr(ssr_name_ptr)) {
                    sd.ssr_name = read_cstring(ssr_name_ptr, 32, "glink_ssr_name");
                }
            } else if (sd.start_sym.find("sysmon") != std::string::npos) {
                sd.type = "SYSMON (SSCTL)";
            } else if (sd.start_sym.find("ssr") != std::string::npos) {
                sd.type = "SSR (Restart)";
                ulong ssr_info = read_pointer(sd_addr + 48, "ssr_info");
                if (ssr_info && is_kvaddr(ssr_info)) {
                    ulong name_p = read_pointer(ssr_info, "ssr_info_name");
                    if (name_p && is_kvaddr(name_p)) {
                        sd.ssr_name = read_cstring(name_p, 32, "ssr_name");
                    }
                }
            } else if (sd.start_sym.find("smd") != std::string::npos) {
                sd.type = "SMD (Shared Mem)";
            } else {
                sd.type = sd.start_sym.empty() ? "Subdevice" : sd.start_sym;
            }

            info->subdevices.push_back(sd);
        }

        // Dump Segments
        ulong dump_head = rproc_addr + field_offset(rproc, dump_segments);
        int off_dump_node = field_offset(rproc_dump_segment, node);
        if (off_dump_node < 0) off_dump_node = 0;
        std::vector<ulong> dump_segs = for_each_list(dump_head, off_dump_node);
        info->dump_segment_count = dump_segs.size();
        for (ulong seg_addr : dump_segs) {
            if (!is_kvaddr(seg_addr)) continue;
            ulong da = read_ulong(seg_addr + field_offset(rproc_dump_segment, da), "da");
            size_t sz = read_ulong(seg_addr + field_offset(rproc_dump_segment, size), "size");
            info->total_dump_size += sz;
            if (info->dump_segments.size() < 16) {
                info->dump_segments.push_back({da, sz});
            }
        }

        // SMEM Crash Reason if subsystem has crashed
        int smem_id = 0;
        if (info->alias_type == "Modem / MPSS") smem_id = 421;
        else if (info->alias_type == "Audio / ADSP") smem_id = 423;
        else if (info->alias_type == "Compute / CDSP") smem_id = 601;
        else if (info->alias_type == "Sensors / SLPI") smem_id = 626;
        else if (info->alias_type == "Wireless / WPSS") smem_id = 424;

        if (smem_id > 0) {
            info->crash_reason = read_smem_crash_reason(smem_id);
        }

        subsys_list.push_back(info);
    }
}

void Subsys::collect_legacy_subsys() {
    if (!csymbol_exists("subsys_list")) return;

    ulong subsys_list_head = csymbol_value("subsys_list");
    ulong curr = read_pointer(subsys_list_head, "subsys_list_head");
    int count = 0;

    while (curr && curr != subsys_list_head && is_kvaddr(curr) && count++ < 50) {
        // In legacy subsys_device, list is usually embedded
        // Read name pointer if available
        ulong name_ptr = read_pointer(curr, "legacy_subsys_name");
        if (name_ptr && is_kvaddr(name_ptr)) {
            std::string name = read_cstring(name_ptr, 64, "legacy_name");
            if (!name.empty() && name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") == std::string::npos) {
                auto info = std::make_shared<SubsysInfo>();
                info->name = name;
                info->alias_type = deduce_alias_type(name, "");
                info->state_str = "ONLINE";
                info->firmware = "-";
                info->dump_conf_str = "ENABLED";
                subsys_list.push_back(info);
            }
        }
        curr = read_pointer(curr, "subsys_list_next");
    }
}

void Subsys::collect_ssr_notifiers() {
    if (!csymbol_exists("qcom_ssr_subsystem_list")) return;

    ulong head = csymbol_value("qcom_ssr_subsystem_list");
    ulong curr = read_pointer(head, "ssr_subsys_head");
    int count = 0;

    while (curr && curr != head && is_kvaddr(curr) && count++ < 50) {
        // In qcom_ssr_subsystem: list is at offset 0x438 (1080)
        ulong entry = curr - 0x438;
        ulong name_ptr = read_pointer(entry, "ssr_name");

        std::string ssr_name;
        if (name_ptr && is_kvaddr(name_ptr)) {
            ssr_name = read_cstring(name_ptr, 32, "ssr_name");
        }

        if (!ssr_name.empty()) {
            auto ssr = std::make_shared<SSRSubsystem>();
            ssr->addr = entry;
            ssr->name = ssr_name;

            // srcu_notifier_head: notifier_block* head is at offset 0x218 (536)
            ulong nb_head = read_pointer(entry + 0x218, "nb_head");
            ulong nb = nb_head;
            int nb_cnt = 0;

            while (nb && is_kvaddr(nb) && nb_cnt++ < 60) {
                ulong call_fn = read_pointer(nb + field_offset(notifier_block, notifier_call), "notifier_call");
                ulong next_nb = read_pointer(nb + field_offset(notifier_block, next), "next_nb");
                int prio = read_int(nb + field_offset(notifier_block, priority), "prio");

                if (call_fn && is_kvaddr(call_fn)) {
                    SSRClientNotifier client;
                    client.nb_addr = nb;
                    client.callback_addr = call_fn;
                    client.callback_sym = to_symbol(call_fn);
                    client.priority = prio;
                    ssr->clients.push_back(client);
                }

                if (next_nb == nb) break;
                nb = next_nb;
            }

            ssr_subsystems.push_back(ssr);
        }

        ulong next_curr = read_pointer(curr, "ssr_list_next");
        if (next_curr == curr) break;
        curr = next_curr;
    }
}

void Subsys::collect_smem_states() {
    if (!csymbol_exists("smem_states")) return;

    ulong head = csymbol_value("smem_states");
    ulong curr = read_pointer(head, "smem_states_head");
    int count = 0;

    while (curr && curr != head && is_kvaddr(curr) && count++ < 100) {
        ulong entry = curr - 8; // struct qcom_smem_state: list is at offset 8
        ulong of_node = read_pointer(entry + 24, "of_node");
        ulong update_bits = read_pointer(entry + 40, "update_bits");

        auto state_info = std::make_shared<SmemStateInfo>();
        state_info->addr = entry;
        state_info->ops_update_bits = update_bits;
        if (update_bits) state_info->update_bits_sym = to_symbol(update_bits);

        if (of_node && is_kvaddr(of_node)) {
            ulong name_ptr = read_pointer(of_node + field_offset(device_node, name), "of_name");
            if (name_ptr && is_kvaddr(name_ptr)) {
                state_info->node_name = read_cstring(name_ptr, 64, "of_name");
            }
        }
        if (state_info->node_name.empty()) {
            state_info->node_name = "smem_state@" + std::to_string(entry & 0xffff);
        }

        smem_states_list.push_back(state_info);
        ulong next_curr = read_pointer(curr, "smem_next");
        if (next_curr == curr) break;
        curr = next_curr;
    }
}

void Subsys::collect_all_data() {
    subsys_list.clear();
    ssr_subsystems.clear();
    smem_states_list.clear();
    is_shutdown_in_progress = false;

    if (csymbol_exists("qcom_device_shutdown_in_progress")) {
        is_shutdown_in_progress = (read_int(csymbol_value("qcom_device_shutdown_in_progress"), "shutdown_in_prog") != 0);
    }

    collect_remoteprocs();
    if (subsys_list.empty()) {
        collect_legacy_subsys();
    }
    collect_ssr_notifiers();
    collect_smem_states();
}

void Subsys::print_overview() {
    int total_subsys = subsys_list.size();
    int online_count = 0;
    int crashed_count = 0;
    int restarting_count = 0;
    int offline_count = 0;
    unsigned int total_crashes = 0;

    for (const auto &s : subsys_list) {
        if (s->state_str == "ONLINE") online_count++;
        else if (s->state_str == "CRASHED") crashed_count++;
        else if (s->crash_handler_pending) restarting_count++;
        else offline_count++;

        total_crashes += s->crash_cnt;
    }

    PRINT("Qualcomm Peripheral Subsystems & SSR Health Overview\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    char lbuf[256];
    snprintf(lbuf, sizeof(lbuf), "  Subsystems:   %-6d    Online:  %-6d    Crashed: %-6d",
             total_subsys, online_count, crashed_count);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  SSR Restarts: %-6u    Offline: %-6d    Pending: %-6d",
             total_crashes, offline_count, restarting_count);
    PRINT("│ %-76s │\n", lbuf);

    if (is_shutdown_in_progress) {
        PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
        PRINT("│ %-76s │\n", "  [!] CRITICAL: Qualcomm Device Shutdown / SSR is actively in progress!");
    }

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    if (crashed_count > 0) {
        snprintf(lbuf, sizeof(lbuf), "  [!] CRITICAL: %d subsystem(s) are currently in CRASHED state!", crashed_count);
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Run 'subsys -c' to inspect crash causes and failure signatures.");
    } else if (restarting_count > 0) {
        PRINT("│ %-76s │\n", "  [*] NOTICE: Subsystem restart work queue is actively running.");
        PRINT("│ %-76s │\n", "      Run 'subsys -c' to inspect recovery work handler.");
    } else if (total_crashes > 0) {
        snprintf(lbuf, sizeof(lbuf), "  [*] NOTICE: Subsystems recovered from %u previous crash events.", total_crashes);
        PRINT("│ %-76s │\n", lbuf);
        PRINT("│ %-76s │\n", "      Run 'subsys -c' to inspect historical crash details.");
    } else {
        PRINT("│ %-76s │\n", "  [+] NORMAL: All registered peripheral subsystems are healthy and running.");
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    // Table: 26 + 16 + 10 + 9 + 13 = 74 columns + 6 borders = 80 chars
    PRINT("Peripheral Subsystems Status Table\n");
    PRINT("┌──────────────────────────┬────────────────┬──────────┬─────────┬─────────────┐\n");
    PRINT("│ Subsystem Name           │ Role / Alias   │ State    │ Crashes │ Firmware    │\n");
    PRINT("├──────────────────────────┼────────────────┼──────────┼─────────┼─────────────┤\n");

    if (subsys_list.empty()) {
        PRINT("│ %-76s │\n", "  [No remoteproc / subsystem devices detected]");
        PRINT("└──────────────────────────┴────────────────┴──────────┴─────────┴─────────────┘\n");
        return;
    }

    char row[512];
    for (const auto &s : subsys_list) {
        std::string name_disp = s->name;
        if (name_disp.length() > 24) name_disp = name_disp.substr(0, 21) + "...";

        std::string role_disp = s->alias_type;
        if (role_disp.length() > 14) role_disp = role_disp.substr(0, 12) + "..";

        std::string fw_disp = s->firmware;
        if (fw_disp.length() > 11) fw_disp = fw_disp.substr(0, 9) + "..";

        std::string state_disp = s->state_str;
        if (s->crash_handler_pending) {
            state_disp = "RESTART";
        }
        if (state_disp.length() > 8) state_disp = state_disp.substr(0, 8);

        snprintf(row, sizeof(row),
                 "│ %-24s │ %-14s │ %-8s │ %5u  │ %-11s │\n",
                 name_disp.c_str(), role_disp.c_str(),
                 state_disp.c_str(), s->crash_cnt, fw_disp.c_str());
        PRINT("%s", row);
    }
    PRINT("└──────────────────────────┴────────────────┴──────────┴─────────┴─────────────┘\n");
    PRINT("Commands: 'subsys -c' (Crash & SSR details), 'subsys -s <name>' (Deep-dive), 'subsys -n' (SSR notifiers)\n");
}

void Subsys::print_crashes(const std::string &target) {
    PRINT("Qualcomm Subsystem Crash & SSR Failure Diagnosis\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");

    bool has_crashes = false;
    bool has_smem_reasons = false;
    int match_count = 0;

    for (const auto &s : subsys_list) {
        if (!target.empty() && !matches_target(s, target)) continue;
        match_count++;

        if (s->state_str == "CRASHED" || s->crash_cnt > 0 || s->crash_handler_pending) {
            has_crashes = true;
        }
        if (!s->crash_reason.empty()) {
            has_smem_reasons = true;
        }
    }

    if (!target.empty() && match_count == 0) {
        std::string err_msg = "  [!] Specified subsystem '" + target + "' not found.";
        PRINT("│ %-76s │\n", err_msg.c_str());
        PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
        return;
    }

    if (!has_crashes && !has_smem_reasons) {
        PRINT("│ %-76s │\n", "  [PASS] All peripheral subsystems are healthy. Zero crash events recorded.");
        PRINT("│ %-76s │\n", "  No SSR restarts or pending recovery work queues detected.");
        PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
        return;
    }

    char lbuf[256];
    if (has_crashes) {
        snprintf(lbuf, sizeof(lbuf), "  [!] DIAGNOSIS: Detected subsystem crashes or active SSR restart events.");
    } else {
        snprintf(lbuf, sizeof(lbuf), "  [*] NOTICE: Zero SSR reboots. Subsystem stop reasons recorded in SMEM.");
    }
    PRINT("│ %-76s │\n", lbuf);
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    for (const auto &s : subsys_list) {
        if (!target.empty() && !matches_target(s, target)) continue;
        if (s->state_str != "CRASHED" && s->crash_cnt == 0 && !s->crash_handler_pending && s->crash_reason.empty()) continue;

        PRINT("Subsystem: %s (%s)\n", s->name.c_str(), s->alias_type.c_str());
        PRINT("┌──────────────────────┬───────────────────────────────────────────────────────┐\n");

        char row[512];
        snprintf(row, sizeof(row), "│ Remoteproc Address   │ 0x%-51lx │\n", s->rproc_addr);
        PRINT("%s", row);

        std::string st_str = s->state_str;
        if (s->state_str == "CRASHED") st_str += " (SUBSYSTEM DOWN)";
        snprintf(row, sizeof(row), "│ Current State        │ %-53s │\n", st_str.c_str());
        PRINT("%s", row);

        snprintf(row, sizeof(row), "│ Crash Count          │ %-53u │\n", s->crash_cnt);
        PRINT("%s", row);

        snprintf(row, sizeof(row), "│ Auto-Recovery (SSR)  │ %-53s │\n", s->recovery_disabled ? "DISABLED (System panic on crash)" : "ENABLED");
        PRINT("%s", row);

        snprintf(row, sizeof(row), "│ Coredump Config      │ %-53s │\n", s->dump_conf_str.c_str());
        PRINT("%s", row);

        std::string handler_st = s->crash_handler_pending ? "ACTIVE / PENDING" : "IDLE";
        if (!s->crash_handler_func_sym.empty()) handler_st += " (" + s->crash_handler_func_sym + ")";
        snprintf(row, sizeof(row), "│ Crash Handler Work   │ %-53s │\n", handler_st.c_str());
        PRINT("%s", row);

        std::string reason_disp = s->crash_reason.empty() ? "(No SMEM reason recorded or watchdog bite without msg)" : s->crash_reason;
        if (reason_disp.length() > 53) reason_disp = reason_disp.substr(0, 50) + "...";
        snprintf(row, sizeof(row), "│ SMEM Crash Reason    │ %-53s │\n", reason_disp.c_str());
        PRINT("%s", row);

        PRINT("└──────────────────────┴───────────────────────────────────────────────────────┘\n\n");
    }
}

void Subsys::print_detail(const std::string &target) {
    std::shared_ptr<SubsysInfo> match = nullptr;

    for (const auto &s : subsys_list) {
        if (matches_target(s, target)) {
            match = s;
            break;
        }
    }

    if (!match) {
        PRINT("Subsystem '%s' not found. Available subsystems:\n", target.c_str());
        for (const auto &s : subsys_list) {
            PRINT("  - %s (%s) [0x%lx]\n", s->name.c_str(), s->alias_type.c_str(), s->rproc_addr);
        }
        return;
    }

    PRINT("Peripheral Subsystem Deep-Dive: %s\n", match->name.c_str());
    PRINT("┌──────────────────────┬───────────────────────────────────────────────────────┐\n");

    char row[512];
    snprintf(row, sizeof(row), "│ Subsystem Name       │ %-53s │\n", match->name.c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Role / Alias         │ %-53s │\n", match->alias_type.c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Firmware Image       │ %-53s │\n", match->firmware.c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Remoteproc Address   │ 0x%-51lx │\n", match->rproc_addr);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Private Data (priv)  │ 0x%-51lx │\n", match->priv_addr);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Driver Ops (ops)     │ 0x%-51lx │\n", match->ops_addr);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ State                │ %-53s │\n", match->state_str.c_str());
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Crash Count          │ %-53u │\n", match->crash_cnt);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Power Count          │ %-53d │\n", match->power_count);
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Auto-Boot Setting    │ %-53s │\n", match->auto_boot ? "ENABLED" : "DISABLED");
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Recovery Disabled    │ %-53s │\n", match->recovery_disabled ? "YES (Panic on failure)" : "NO (SSR enabled)");
    PRINT("%s", row);

    snprintf(row, sizeof(row), "│ Coredump Config      │ %-53s │\n", match->dump_conf_str.c_str());
    PRINT("%s", row);

    std::string sz_str = std::to_string(match->dump_segment_count) + " segments (" + csize(match->total_dump_size) + ")";
    snprintf(row, sizeof(row), "│ Dump Memory Segments │ %-53s │\n", sz_str.c_str());
    PRINT("%s", row);

    if (!match->ops_start_sym.empty()) {
        snprintf(row, sizeof(row), "│ Ops -> Start Handler │ %-53s │\n", match->ops_start_sym.c_str());
        PRINT("%s", row);
    }
    if (!match->ops_stop_sym.empty()) {
        snprintf(row, sizeof(row), "│ Ops -> Stop Handler  │ %-53s │\n", match->ops_stop_sym.c_str());
        PRINT("%s", row);
    }
    if (!match->ops_coredump_sym.empty()) {
        snprintf(row, sizeof(row), "│ Ops -> Dump Handler  │ %-53s │\n", match->ops_coredump_sym.c_str());
        PRINT("%s", row);
    }
    if (!match->ops_panic_sym.empty()) {
        snprintf(row, sizeof(row), "│ Ops -> Panic Handler │ %-53s │\n", match->ops_panic_sym.c_str());
        PRINT("%s", row);
    }

    std::string reason_disp = match->crash_reason.empty() ? "None (Subsystem healthy / no crash reason)" : match->crash_reason;
    if (reason_disp.length() > 53) reason_disp = reason_disp.substr(0, 50) + "...";
    snprintf(row, sizeof(row), "│ Crash Reason         │ %-53s │\n", reason_disp.c_str());
    PRINT("%s", row);

    PRINT("└──────────────────────┴───────────────────────────────────────────────────────┘\n\n");

    // Subdevices
    PRINT("Attached Subdevices (%zu registered):\n", match->subdevices.size());
    PRINT("┌──────────────────┬─────────────────┬─────────────────────────────────────────┐\n");
    PRINT("│ Subdevice Type   │ SSR Identifier  │ Callback Function (Start/Prepare)       │\n");
    PRINT("├──────────────────┼─────────────────┼─────────────────────────────────────────┤\n");

    if (match->subdevices.empty()) {
        PRINT("│ [No subdevices attached]                                                     │\n");
        PRINT("└──────────────────┴─────────────────┴─────────────────────────────────────────┘\n");
    } else {
        for (const auto &sd : match->subdevices) {
            std::string type_disp = sd.type;
            if (type_disp.length() > 16) type_disp = type_disp.substr(0, 14) + "..";

            std::string id_disp = sd.ssr_name.empty() ? "-" : sd.ssr_name;
            if (id_disp.length() > 15) id_disp = id_disp.substr(0, 13) + "..";

            std::string fn_disp = sd.start_sym.empty() ? sd.prepare_sym : sd.start_sym;
            if (fn_disp.empty()) fn_disp = "-";
            if (fn_disp.length() > 39) fn_disp = fn_disp.substr(0, 36) + "...";

            snprintf(row, sizeof(row), "│ %-16s │ %-15s │ %-39s │\n",
                     type_disp.c_str(), id_disp.c_str(), fn_disp.c_str());
            PRINT("%s", row);
        }
        PRINT("└──────────────────┴─────────────────┴─────────────────────────────────────────┘\n\n");
    }

    // Dump segments detail (first 8)
    if (!match->dump_segments.empty()) {
        PRINT("Memory Dump Segments Sample (Top %zu):\n", match->dump_segments.size());
        PRINT("┌──────┬──────────────────────┬──────────────────────┬─────────────────────────┐\n");
        PRINT("│ Index│ Device Address (DA)  │ Size (Bytes)         │ Formatted Size          │\n");
        PRINT("├──────┼──────────────────────┼──────────────────────┼─────────────────────────┤\n");
        int idx = 0;
        for (const auto &ds : match->dump_segments) {
            snprintf(row, sizeof(row), "│%4d  │ 0x%-18lx │ %-20zu │ %-23s │\n",
                     idx++, ds.da, ds.size, csize(ds.size).c_str());
            PRINT("%s", row);
        }
        PRINT("└──────┴──────────────────────┴──────────────────────┴─────────────────────────┘\n");
    }
}

void Subsys::print_notifiers() {
    PRINT("Subsystem Restart (SSR) Notification Matrix\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");
    PRINT("│ Registered SSR channels and kernel driver listeners dispatched during reboot │\n");
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    PRINT("┌────────────────┬────────────┬────────────────────────────────────────────────┐\n");
    PRINT("│ Subsystem      │ Priority   │ Notifier Callback Handler                      │\n");
    PRINT("├────────────────┼────────────┼────────────────────────────────────────────────┤\n");

    if (ssr_subsystems.empty()) {
        PRINT("│ [No SSR subsystems registered in qcom_ssr_subsystem_list]                    │\n");
        PRINT("└────────────────┴────────────┴────────────────────────────────────────────────┘\n");
        return;
    }

    char row[512];
    for (const auto &ssr : ssr_subsystems) {
        if (ssr->clients.empty()) {
            snprintf(row, sizeof(row), "│ %-14s │ %-10s │ %-46s │\n",
                     ssr->name.c_str(), "-", "[No registered clients]");
            PRINT("%s", row);
            continue;
        }

        bool first = true;
        for (const auto &cli : ssr->clients) {
            std::string name_disp = first ? ssr->name : "";
            first = false;

            std::string cb_disp = cli.callback_sym;
            if (cb_disp.length() > 46) cb_disp = cb_disp.substr(0, 43) + "...";

            snprintf(row, sizeof(row), "│ %-14s │ %-10d │ %-46s │\n",
                     name_disp.c_str(), cli.priority, cb_disp.c_str());
            PRINT("%s", row);
        }
        if (&ssr != &ssr_subsystems.back()) {
            PRINT("├────────────────┼────────────┼────────────────────────────────────────────────┤\n");
        }
    }
    PRINT("└────────────────┴────────────┴────────────────────────────────────────────────┘\n");
    PRINT("Tip: In SSR hang issues, trace if any listener above is stuck during notification.\n");
}

void Subsys::print_smem_states() {
    PRINT("Qualcomm Shared Memory (SMEM) & SMP2P State Channels\n");
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");
    PRINT("│ Cross-processor signaling and interrupt channels between APPS and Peripherals│\n");
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n\n");

    PRINT("┌────────────────────┬────────────────────────────────┬────────────────────────┐\n");
    PRINT("│ State Address      │ SMP2P / SMEM Node Name         │ Update Callback        │\n");
    PRINT("├────────────────────┼────────────────────────────────┼────────────────────────┤\n");

    if (smem_states_list.empty()) {
        PRINT("│ %-76s │\n", "  [No smem_states detected in smem_states list]");
        PRINT("└────────────────────┴────────────────────────────────┴────────────────────────┘\n");
        return;
    }

    char row[512];
    for (const auto &st : smem_states_list) {
        std::string name_disp = st->node_name;
        if (name_disp.length() > 30) name_disp = name_disp.substr(0, 27) + "...";

        std::string cb_disp = st->update_bits_sym.empty() ? "-" : st->update_bits_sym;
        if (cb_disp.length() > 22) cb_disp = cb_disp.substr(0, 19) + "...";

        snprintf(row, sizeof(row), "│ 0x%-16lx │ %-30s │ %-22s │\n",
                 st->addr, name_disp.c_str(), cb_disp.c_str());
        PRINT("%s", row);
    }
    PRINT("└────────────────────┴────────────────────────────────┴────────────────────────┘\n");
}

#pragma GCC diagnostic pop
