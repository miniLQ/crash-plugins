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
#include <set>

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
        "[-a] [-c [name|addr]] [-s <name|addr>] [-n|-l] [-m]\n"
        "  This command inspects Qualcomm peripheral subsystems (Modem/MPSS,\n"
        "  Audio/ADSP, Compute/CDSP, Sensors/SLPI, Neural/NPU) managed by the\n"
        "  Linux remoteproc and Subsystem Restart (SSR) framework.\n\n"
        "  -a            Display overview of all peripheral subsystems.\n"
        "  -c [name|addr]Diagnose crashed subsystems, crash counts, and SMEM crash reasons.\n"
        "  -s <name|addr>Deep-dive inspection of a specific subsystem (ops, subdevs, coredump).\n"
        "  -n, -l        Display SSR notification listener matrix (clients receiving restart events).\n"
        "  -m            Display Qualcomm SMEM and SMP2P communication states.\n",
        "\n",
        "EXAMPLES",
        "  Display system-wide peripheral subsystem overview:\n"
        "    crash> subsys -a\n\n"
        "  Deep-dive into Modem or ADSP directly by name or address:\n"
        "    crash> subsys -s modem\n"
        "    crash> subsys -s 0xffffff801a2b3c00\n\n"
        "  Diagnose subsystem crashes and SSR failure reasons:\n"
        "    crash> subsys -c\n"
        "    crash> subsys -c adsp\n\n"
        "  Display all kernel drivers listening for SSR restart events:\n"
        "    crash> subsys -n\n"
        "    crash> subsys -l\n\n"
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
    bool show_overview = false;
    bool show_crashes = false;
    bool show_notifiers = false;
    bool show_smem = false;
    std::string target_subsys;

    if (argcnt < 2) {
        cmd_usage(pc->curcmd, SYNOPSIS);
        return;
    }

    while ((c = getopt(argcnt, args, "acnls:mh")) != EOF) {
        switch (c) {
            case 'a':
                show_overview = true;
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

    // Mutual exclusivity check
    int opt_count = 0;
    if (show_overview) opt_count++;
    if (show_crashes) opt_count++;
    if (show_notifiers) opt_count++;
    if (show_smem) opt_count++;
    if (!target_subsys.empty() && !show_crashes) opt_count++;

    if (opt_count > 1) {
        LOGE("Mutually exclusive options specified\n");
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

    std::string hex_target = lower_target;
    if (hex_target.rfind("0x", 0) == 0 || hex_target.rfind("0X", 0) == 0) {
        hex_target = hex_target.substr(2);
    }

    std::string lower_name = s->name;
    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
    std::string lower_alias = s->alias_type;
    std::transform(lower_alias.begin(), lower_alias.end(), lower_alias.begin(), ::tolower);

    char hex_addr[32];
    snprintf(hex_addr, sizeof(hex_addr), "%lx", s->rproc_addr);

    return (lower_name.find(lower_target) != std::string::npos ||
            lower_alias.find(lower_target) != std::string::npos ||
            hex_target == hex_addr);
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

    uint item_count = read_uint(smem_addr + (sizeof(ulong) == 8 ? 16 : 8), "item_count");
    if (item_count > 0 && (uint)smem_id >= item_count) return "";

    std::vector<ulong> partition_bases;
    // global_partition virt_base
    ulong global_virt = read_pointer(smem_addr + (sizeof(ulong) == 8 ? 40 : 20), "global_partition_virt");
    if (global_virt && is_kvaddr(global_virt)) {
        partition_bases.push_back(global_virt);
    }

    // Host partitions: check up to 15 hosts
    int part_stride = (sizeof(ulong) == 8 ? 32 : 16);
    ulong host_part_start = smem_addr + (sizeof(ulong) == 8 ? 72 : 36);
    for (int i = 0; i < 15; i++) {
        ulong part_virt = read_pointer(host_part_start + i * part_stride, "host_partition_virt");
        if (part_virt && is_kvaddr(part_virt)) {
            partition_bases.push_back(part_virt);
        }
    }

    for (ulong part_base : partition_bases) {
        // struct smem_partition_header: offset_free_uncached at offset 12
        uint offset_free_uncached = read_uint(part_base + 12, "offset_free_uncached");
        if (offset_free_uncached == 0 || offset_free_uncached > 0x1000000) continue;

        ulong curr = part_base + 32;
        ulong end = part_base + offset_free_uncached;
        if (end <= curr) continue;

        int safety_loop = 0;
        while (curr + 16 <= end && safety_loop++ < 2000) {
            ushort canary = read_ushort(curr, "canary");
            if (canary != 0xa5a5) break;

            ushort item = read_ushort(curr + 2, "item");
            uint size = read_uint(curr + 4, "size");
            ushort padding_hdr = read_ushort(curr + 10, "padding_hdr");

            if (item == (ushort)smem_id) {
                ulong data_addr = curr + 16 + padding_hdr;
                if (data_addr + size <= end && is_kvaddr(data_addr)) {
                    std::string reason = read_cstring(data_addr, std::min((size_t)size, (size_t)256), "crash_reason");
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
            if (step < 16 || curr + step <= curr || curr + step > end) break;
            curr += step;
        }
    }

    return "";
}

void Subsys::collect_remoteprocs() {
    if (!csymbol_exists("rproc_list")) return;

    ulong rproc_list_head = csymbol_value("rproc_list");
    int off_node = field_offset(rproc, node);
    if (off_node < 0) {
        LOGE("Failed to find offset for rproc.node\n");
        return;
    }

    /* rproc_subdev field offsets (struct is actually qcom_subdev in kernel).
     * Hoisted here to avoid repeated type table lookups per rproc. */
    int off_subdev_node    = field_offset(rproc_subdev, node);
    int off_subdev_prepare = field_offset(rproc_subdev, prepare);
    int off_subdev_start   = field_offset(rproc_subdev, start);
    int off_subdev_stop    = field_offset(rproc_subdev, stop);
    int off_subdev_unprepare = field_offset(rproc_subdev, unprepare);
    if (off_subdev_node < 0) {
        LOGE("Failed to find offset for rproc_subdev.node\n");
        return;
    }

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
        if (off_subdev_node >= 0) {
            std::vector<ulong> subdev_addrs = for_each_list(subdevs_head, off_subdev_node);
            /* Compute qcom_subdev tail from declared field offsets.
             * Avoids struct_size() which logs a typeinfo error. */
            int subdev_sz = 0;
            for (int off : {off_subdev_node, off_subdev_prepare, off_subdev_start, off_subdev_stop, off_subdev_unprepare}) {
                int end = off + (int)sizeof(ulong);
                if (end > subdev_sz) subdev_sz = end;
            }
            if (subdev_sz <= 0) subdev_sz = sizeof(ulong) * 6;

            for (ulong sd_addr : subdev_addrs) {
                if (!is_kvaddr(sd_addr)) continue;
                SubsysSubdev sd;
                sd.addr = sd_addr;
                sd.prepare_fn = read_pointer(sd_addr + off_subdev_prepare, "prep");
                sd.start_fn = read_pointer(sd_addr + off_subdev_start, "start");
                sd.stop_fn = read_pointer(sd_addr + off_subdev_stop, "stop");
                sd.unprepare_fn = read_pointer(sd_addr + off_subdev_unprepare, "unprep");

                if (sd.prepare_fn) sd.prepare_sym = to_symbol(sd.prepare_fn);
                if (sd.start_fn) sd.start_sym = to_symbol(sd.start_fn);
                if (sd.stop_fn) sd.stop_sym = to_symbol(sd.stop_fn);
                if (sd.unprepare_fn) sd.unprepare_sym = to_symbol(sd.unprepare_fn);

                if (sd.start_sym.find("glink") != std::string::npos) {
                    sd.type = "GLINK (IPC)";
                    ulong ssr_name_ptr = read_pointer(sd_addr + subdev_sz, "glink_ssr_name");
                    if (ssr_name_ptr && is_kvaddr(ssr_name_ptr)) {
                        sd.ssr_name = read_cstring(ssr_name_ptr, 32, "glink_ssr_name");
                    }
                } else if (sd.start_sym.find("sysmon") != std::string::npos) {
                    sd.type = "SYSMON (SSCTL)";
                } else if (sd.start_sym.find("ssr") != std::string::npos) {
                    sd.type = "SSR (Restart)";
                    ulong ssr_info = read_pointer(sd_addr + subdev_sz, "ssr_info");
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
        }

        // Dump Segments (up to 16 sampled)
        ulong dump_head = rproc_addr + field_offset(rproc, dump_segments);
        int off_dump_node = field_offset(rproc_dump_segment, node);
        if (off_dump_node >= 0) {
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
        }

        // SMEM Crash Reason if subsystem has crashed
        int smem_id = 0;
        if (info->alias_type == "Modem / MPSS") smem_id = SMEM_SSR_REASON_MSS0;
        else if (info->alias_type == "Audio / ADSP") smem_id = SMEM_SSR_REASON_LPASS;
        else if (info->alias_type == "Compute / CDSP") smem_id = SMEM_SSR_REASON_CDSP;
        else if (info->alias_type == "Sensors / SLPI") smem_id = SMEM_SSR_REASON_SLPI;
        else if (info->alias_type == "Wireless / WPSS") smem_id = SMEM_SSR_REASON_WCNSS;

        if (smem_id > 0) {
            info->crash_reason = read_smem_crash_reason(smem_id);
        }

        subsys_list.push_back(info);
    }
}

void Subsys::collect_ssr_notifiers() {
    if (!csymbol_exists("qcom_ssr_subsystem_list")) return;

    ulong head = csymbol_value("qcom_ssr_subsystem_list");
    ulong curr = read_pointer(head, "ssr_subsys_head");
    int count = 0;
    std::set<ulong> visited_subsys;

    int off_list = (sizeof(ulong) == 8 ? 0x438 : 0x21c);
    int off_head = (sizeof(ulong) == 8 ? 0x218 : 0x10c);

    while (curr && curr != head && is_kvaddr(curr) && count++ < 50) {
        if (visited_subsys.find(curr) != visited_subsys.end()) break;
        visited_subsys.insert(curr);

        ulong entry = curr - off_list;
        ulong name_ptr = read_pointer(entry, "ssr_name");

        std::string ssr_name;
        if (name_ptr && is_kvaddr(name_ptr)) {
            ssr_name = read_cstring(name_ptr, 32, "ssr_name");
        }

        if (!ssr_name.empty()) {
            auto ssr = std::make_shared<SSRSubsystem>();
            ssr->addr = entry;
            ssr->name = ssr_name;

            ulong nb_head = read_pointer(entry + off_head, "nb_head");
            ulong nb = nb_head;
            int nb_cnt = 0;
            std::set<ulong> visited_nb;

            while (nb && is_kvaddr(nb) && nb_cnt++ < 60) {
                if (visited_nb.find(nb) != visited_nb.end()) break;
                visited_nb.insert(nb);

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

                nb = next_nb;
            }

            ssr_subsystems.push_back(ssr);
        }

        curr = read_pointer(curr, "ssr_list_next");
    }
}

void Subsys::collect_smem_states() {
    if (!csymbol_exists("smem_states")) return;

    ulong head = csymbol_value("smem_states");
    ulong curr = read_pointer(head, "smem_states_head");
    int count = 0;
    std::set<ulong> visited_states;

    int off_list = (sizeof(ulong) == 8 ? 8 : 4);
    int off_of_node = (sizeof(ulong) == 8 ? 24 : 12);
    int off_ops = (sizeof(ulong) == 8 ? 32 : 16);

    while (curr && curr != head && is_kvaddr(curr) && count++ < 100) {
        if (visited_states.find(curr) != visited_states.end()) break;
        visited_states.insert(curr);

        ulong entry = curr - off_list;
        ulong of_node = read_pointer(entry + off_of_node, "of_node");
        ulong ops_ptr = read_pointer(entry + off_ops, "ops");
        ulong update_bits = (ops_ptr && is_kvaddr(ops_ptr)) ? read_pointer(ops_ptr, "update_bits") : 0;

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
        curr = read_pointer(curr, "smem_next");
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
    PRINT("==============================================================================\n");
    PRINT("  Subsystems:   %-6d    Online:  %-6d    Crashed: %-6d\n",
          total_subsys, online_count, crashed_count);
    PRINT("  Crash Count:  %-6u    Offline: %-6d    Pending: %-6d\n",
          total_crashes, offline_count, restarting_count);

    if (is_shutdown_in_progress) {
        PRINT("------------------------------------------------------------------------------\n");
        PRINT("  [!] CRITICAL: Qualcomm Device Shutdown / SSR is actively in progress!\n");
    }

    PRINT("------------------------------------------------------------------------------\n");
    if (crashed_count > 0) {
        PRINT("  [!] %d subsystem(s) are currently in CRASHED state!\n", crashed_count);
        PRINT("      Run 'subsys -c' to inspect crash causes and failure signatures.\n");
    } else if (restarting_count > 0) {
        PRINT("  [*] Subsystem restart work queue is actively running.\n");
        PRINT("      Run 'subsys -c' to inspect recovery work handler.\n");
    } else if (total_crashes > 0) {
        PRINT("  [*] Subsystems recovered from %u previous crash events.\n", total_crashes);
        PRINT("      Run 'subsys -c' to inspect historical crash details.\n");
    } else {
        PRINT("  [+] All registered peripheral subsystems are healthy and running.\n");
    }
    PRINT("==============================================================================\n\n");

    PRINT("Peripheral Subsystems Status Table\n");
    PRINT("%-26s %-16s %-10s %-9s %-13s\n", "Subsystem Name", "Role / Alias", "State", "Crashes", "Firmware");
    PRINT("------------------------------------------------------------------------------\n");

    if (subsys_list.empty()) {
        PRINT("  [No remoteproc / subsystem devices detected]\n");
        PRINT("------------------------------------------------------------------------------\n");
        return;
    }

    for (const auto &s : subsys_list) {
        std::string name_disp = s->name;
        if (name_disp.length() > 25) name_disp = name_disp.substr(0, 22) + "...";

        std::string role_disp = s->alias_type;
        if (role_disp.length() > 15) role_disp = role_disp.substr(0, 13) + "..";

        std::string fw_disp = s->firmware;
        if (fw_disp.length() > 12) fw_disp = fw_disp.substr(0, 10) + "..";

        std::string state_disp = s->state_str;
        if (s->crash_handler_pending) {
            state_disp = "RESTART";
        }
        if (state_disp.length() > 9) state_disp = state_disp.substr(0, 9);

        PRINT("%-26s %-16s %-10s %-9u %-13s\n",
              name_disp.c_str(), role_disp.c_str(),
              state_disp.c_str(), s->crash_cnt, fw_disp.c_str());
    }
    PRINT("------------------------------------------------------------------------------\n");
    PRINT("Commands: 'subsys -c' (Crash & SSR details), 'subsys -s <name>' (Deep-dive), 'subsys -n' (SSR notifiers)\n");
}

void Subsys::print_crashes(const std::string &target) {
    PRINT("Qualcomm Subsystem Crash & SSR Failure Diagnosis\n");
    PRINT("==============================================================================\n");

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
        PRINT("  Specified subsystem '%s' not found.\n", target.c_str());
        PRINT("==============================================================================\n");
        return;
    }

    if (!has_crashes && !has_smem_reasons) {
        PRINT("  [PASS] All peripheral subsystems are healthy. Zero crash events recorded.\n");
        PRINT("  No SSR restarts or pending recovery work queues detected.\n");
        PRINT("==============================================================================\n");
        return;
    }

    if (has_crashes) {
        PRINT("  [!] Detected subsystem crashes or active SSR restart events.\n");
    } else {
        PRINT("  [*] Zero SSR reboots. Subsystem stop reasons recorded in SMEM.\n");
    }
    PRINT("==============================================================================\n\n");

    for (const auto &s : subsys_list) {
        if (!target.empty() && !matches_target(s, target)) continue;
        if (s->state_str != "CRASHED" && s->crash_cnt == 0 && !s->crash_handler_pending && s->crash_reason.empty()) continue;

        PRINT("Subsystem: %s (%s)\n", s->name.c_str(), s->alias_type.c_str());
        PRINT("------------------------------------------------------------------------------\n");
        PRINT("  Remoteproc Address : 0x%lx\n", s->rproc_addr);

        std::string st_str = s->state_str;
        if (s->state_str == "CRASHED") st_str += " (SUBSYSTEM DOWN)";
        PRINT("  Current State      : %s\n", st_str.c_str());
        PRINT("  Crash Count        : %u\n", s->crash_cnt);
        PRINT("  Auto-Recovery (SSR): %s\n", s->recovery_disabled ? "DISABLED (No restart attempt)" : "ENABLED");
        PRINT("  Coredump Config    : %s\n", s->dump_conf_str.c_str());

        std::string handler_st = s->crash_handler_pending ? "ACTIVE / PENDING" : "IDLE";
        if (!s->crash_handler_func_sym.empty()) handler_st += " (" + s->crash_handler_func_sym + ")";
        PRINT("  Crash Handler Work : %s\n", handler_st.c_str());

        std::string reason_disp = s->crash_reason.empty() ? "(No SMEM reason recorded)" : s->crash_reason;
        PRINT("  SMEM Crash Reason  : %s\n", reason_disp.c_str());
        PRINT("------------------------------------------------------------------------------\n\n");
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
    PRINT("==============================================================================\n");
    PRINT("  Name / Role        : %s (%s)\n", match->name.c_str(), match->alias_type.c_str());
    PRINT("  Remoteproc Address : 0x%lx\n", match->rproc_addr);
    PRINT("  Firmware Image     : %s\n", match->firmware.c_str());
    PRINT("  Current State      : %s (raw: %u)\n", match->state_str.c_str(), match->state_raw);
    PRINT("  Power Refcount     : %d\n", match->power_count);
    PRINT("  Auto-Boot On Load  : %s\n", match->auto_boot ? "YES" : "NO");
    PRINT("  SSR Auto-Recovery  : %s\n", match->recovery_disabled ? "DISABLED (No restart attempt)" : "ENABLED");
    PRINT("  Crash Count        : %u\n", match->crash_cnt);
    PRINT("  Coredump Setting   : %s\n", match->dump_conf_str.c_str());

    if (!match->crash_reason.empty()) {
        PRINT("  SMEM Crash Reason  : %s\n", match->crash_reason.c_str());
    }

    PRINT("------------------------------------------------------------------------------\n");
    PRINT("  Remoteproc Driver Operations (ops: 0x%lx):\n", match->ops_addr);
    PRINT("    - start()    : %s\n", match->ops_start_sym.empty() ? "-" : match->ops_start_sym.c_str());
    PRINT("    - stop()     : %s\n", match->ops_stop_sym.empty() ? "-" : match->ops_stop_sym.c_str());
    PRINT("    - coredump() : %s\n", match->ops_coredump_sym.empty() ? "-" : match->ops_coredump_sym.c_str());
    PRINT("    - panic()    : %s\n", match->ops_panic_sym.empty() ? "-" : match->ops_panic_sym.c_str());

    PRINT("------------------------------------------------------------------------------\n");
    PRINT("  Attached Subdevices (%zu registered):\n", match->subdevices.size());
    if (match->subdevices.empty()) {
        PRINT("    [No subdevices registered]\n");
    } else {
        for (const auto &sd : match->subdevices) {
            std::string extra;
            if (!sd.ssr_name.empty()) extra = " [" + sd.ssr_name + "]";
            PRINT("    - 0x%lx: %-18s%s\n", sd.addr, sd.type.c_str(), extra.c_str());
        }
    }

    PRINT("------------------------------------------------------------------------------\n");
    PRINT("  Coredump Memory Segments (%zu total, %zu MB total dump size):\n",
          match->dump_segment_count, match->total_dump_size / (1024 * 1024));
    if (match->dump_segments.empty()) {
        PRINT("    [No dump segments configured]\n");
    } else {
        for (const auto &seg : match->dump_segments) {
            PRINT("    - Device Addr: 0x%-12lx Size: %8zu KB (%zu MB)\n",
                  seg.da, seg.size / 1024, seg.size / (1024 * 1024));
        }
        if (match->dump_segment_count > match->dump_segments.size()) {
            PRINT("    ... and %zu more segments\n", match->dump_segment_count - match->dump_segments.size());
        }
    }
    PRINT("==============================================================================\n");
}

void Subsys::print_notifiers() {
    PRINT("Qualcomm Subsystem Restart (SSR) Notification Clients\n");
    PRINT("==============================================================================\n");
    PRINT("%-16s %-12s %-48s\n", "Subsystem", "Priority", "Notifier Callback Handler");
    PRINT("------------------------------------------------------------------------------\n");

    if (ssr_subsystems.empty()) {
        PRINT("  [No SSR subsystems registered in qcom_ssr_subsystem_list]\n");
        PRINT("------------------------------------------------------------------------------\n");
        return;
    }

    for (const auto &ssr : ssr_subsystems) {
        if (ssr->clients.empty()) {
            PRINT("%-16s %-12s %-48s\n", ssr->name.c_str(), "-", "[No registered clients]");
            continue;
        }

        bool first = true;
        for (const auto &cli : ssr->clients) {
            std::string name_disp = first ? ssr->name : "";
            first = false;

            std::string cb_disp = cli.callback_sym;
            if (cb_disp.length() > 48) cb_disp = cb_disp.substr(0, 45) + "...";

            PRINT("%-16s %-12d %-48s\n", name_disp.c_str(), cli.priority, cb_disp.c_str());
        }
    }
    PRINT("------------------------------------------------------------------------------\n");
    PRINT("Tip: In SSR hang issues, trace if any listener above is stuck during notification.\n");
}

void Subsys::print_smem_states() {
    PRINT("Qualcomm Shared Memory (SMEM) & SMP2P State Channels\n");
    PRINT("==============================================================================\n");
    PRINT("%-20s %-32s %-24s\n", "State Address", "SMP2P / SMEM Node Name", "Update Callback");
    PRINT("------------------------------------------------------------------------------\n");

    if (smem_states_list.empty()) {
        PRINT("  [No smem_states detected in smem_states list]\n");
        PRINT("------------------------------------------------------------------------------\n");
        return;
    }

    for (const auto &st : smem_states_list) {
        std::string name_disp = st->node_name;
        if (name_disp.length() > 30) name_disp = name_disp.substr(0, 27) + "...";

        std::string cb_disp = st->update_bits_sym.empty() ? "-" : st->update_bits_sym;
        if (cb_disp.length() > 24) cb_disp = cb_disp.substr(0, 21) + "...";

        PRINT("0x%-18lx %-32s %-24s\n", st->addr, name_disp.c_str(), cb_disp.c_str());
    }
    PRINT("------------------------------------------------------------------------------\n");
}

#pragma GCC diagnostic pop
