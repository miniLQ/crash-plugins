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

#include "irq.h"
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <numeric>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpointer-arith"

#ifndef BUILD_TARGET_TOGETHER
DEFINE_PLUGIN_COMMAND(Irq)
#endif

Irq::Irq() {
    do_init_offset = true;
}

void Irq::init_command(void) {
    cmd_name = "irqs";
    help_str_list = {
        "irqs",                                                   /* command name */
        "display interrupt (IRQ) and softirq analysis",           /* short description */
        "[-a] [-c] [-s] [-S] [-i <irq>]\n"
        "  This command displays interrupt (IRQ) and SoftIRQ information.\n"
        "\n"
        "    -a          display all registered IRQs overview (default)\n"
        "    -c          display per-CPU interrupt distribution\n"
        "    -s          display interrupt storm and top frequency ranking\n"
        "    -S          display SoftIRQ handlers, counters, and pending status\n"
        "    -i <irq>    display detailed information for a specific IRQ\n",
        "\n",
        "EXAMPLES",
        "  Display all registered IRQs:",
        "    %s> irqs -a",
        "    Registered IRQs Overview",
        "    ┌─────┬────────┬──────────────┬────────────────────────────┬────────────────────────────┬──────────┬──────────┬─────────────┬───────────┐",
        "    │ IRQ#│ HWIRQ  │ Chip         │ Name / Device              │ Handler                    │    Total │ Depth    │ Trigger     │ Affinity  │",
        "    ├─────┼────────┼──────────────┼────────────────────────────┼────────────────────────────┼──────────┼──────────┼─────────────┼───────────┤",
        "    │  17 │     17 │ GICv3        │ arch_timer                 │ arch_timer_handler_virt    │  4529012 │ 0 (EN)   │ Level-Low   │ all (0-7) │",
        "    │  22 │     22 │ GICv3        │ kgsl-3d0                   │ kgsl_irq_handler           │   182390 │ 0 (EN)   │ Level-High  │ 4-7       │",
        "    └─────┴────────┴──────────────┴────────────────────────────┴────────────────────────────┴──────────┴──────────┴─────────────┴───────────┘",
        "\n",
        "  Display per-CPU interrupt distribution:",
        "    %s> irqs -c",
        "\n",
        "  Analyze interrupt storm and top frequency IRQs:",
        "    %s> irqs -s",
        "\n",
        "  Display SoftIRQ status and pending flags:",
        "    %s> irqs -S",
        "\n",
        "  Display details for IRQ 17:",
        "    %s> irqs -i 17\n",
    };
}

void Irq::init_offset(void) {
    struct_init(irq_desc);
    field_init(irq_desc, irq_data);
    field_init(irq_desc, kstat_irqs);
    field_init(irq_desc, action);
    field_init(irq_desc, depth);
    field_init(irq_desc, irq_count);
    field_init(irq_desc, irqs_unhandled);
    field_init(irq_desc, name);
    field_init(irq_desc, handle_irq);
    field_init(irq_desc, percpu_affinity);
    field_init(irq_desc, affinity_hint);

    struct_init(irq_data);
    field_init(irq_data, irq);
    field_init(irq_data, hwirq);
    field_init(irq_data, chip);
    field_init(irq_data, domain);
    field_init(irq_data, common);
    field_init(irq_data, state_use_accessors);

    struct_init(irq_chip);
    field_init(irq_chip, name);

    struct_init(irqaction);
    field_init(irqaction, handler);
    field_init(irqaction, dev_id);
    field_init(irqaction, next);
    field_init(irqaction, thread_fn);
    field_init(irqaction, flags);
    field_init(irqaction, name);

    struct_init(irq_common_data);
    field_init(irq_common_data, affinity);
    field_init(irq_common_data, state_use_accessors);

    struct_init(irq_domain);
    field_init(irq_domain, name);

    struct_init(softirq_action);
    field_init(softirq_action, action);

    struct_init(kernel_stat);
    field_init(kernel_stat, softirqs);
}

void Irq::cmd_main(void) {
    int c;
    optind = 0;
    int target_irq = -1;
    bool show_cpu = false;
    bool show_storm = false;
    bool show_softirq = false;
    bool show_detail = false;

    if (argcnt < 2) {
        print_all_irqs();
        return;
    }

    while ((c = getopt(argcnt, args, "acsi:S")) != EOF) {
        switch (c) {
            case 'a':
                // default action
                break;
            case 'c':
                show_cpu = true;
                break;
            case 's':
                show_storm = true;
                break;
            case 'S':
                show_softirq = true;
                break;
            case 'i':
                try {
                    target_irq = std::stoi(optarg);
                    show_detail = true;
                } catch (...) {
                    LOGE("Invalid IRQ number: %s\n", optarg);
                    return;
                }
                break;
            default:
                argerrs++;
                break;
        }
    }

    if (argerrs) {
        cmd_usage(pc->curcmd, SYNOPSIS);
        return;
    }

    if (show_detail && target_irq >= 0) {
        print_irq_detail(target_irq);
    } else if (show_cpu) {
        print_cpu_distribution();
    } else if (show_storm) {
        print_irq_storm();
    } else if (show_softirq) {
        print_softirqs();
    } else {
        print_all_irqs();
    }
}

int Irq::get_max_nr_irqs() {
    if (csymbol_exists("nr_irqs")) {
        int val = read_int(csymbol_value("nr_irqs"), "nr_irqs");
        if (val > 0) return val;
    }
    if (machdep->nr_irqs > 0) {
        return machdep->nr_irqs;
    }
    return 1024;
}

const std::map<int, ulong>& Irq::gather_irq_desc_map() {
    if (!cached_irq_map.empty()) {
        return cached_irq_map;
    }

    // 1. Linux 6.5+: sparse_irqs is a Maple Tree
    if ((kt->flags2 & IRQ_DESC_TREE_MAPLE) || csymbol_exists("sparse_irqs")) {
        ulong tree_addr = csymbol_exists("sparse_irqs") ? csymbol_value("sparse_irqs") : 
                          (csymbol_exists("irq_desc_tree") ? csymbol_value("irq_desc_tree") : 0);
        if (tree_addr) {
            size_t count = do_maple_tree(tree_addr, MAPLE_TREE_COUNT, NULL);
            if (count > 0) {
                struct list_pair *pairs = (struct list_pair *)GETBUF(count * sizeof(struct list_pair));
                do_maple_tree(tree_addr, MAPLE_TREE_GATHER, pairs);
                int off_irq_data = field_offset(irq_desc, irq_data);
                int off_irq = field_offset(irq_data, irq);
                for (size_t i = 0; i < count; ++i) {
                    ulong desc = (ulong)pairs[i].value;
                    if (is_kvaddr(desc)) {
                        int irq_num = (int)pairs[i].index;
                        if (off_irq_data >= 0 && off_irq >= 0) {
                            irq_num = read_int(desc + off_irq_data + off_irq, "irq_num");
                        }
                        cached_irq_map[irq_num] = desc;
                    }
                }
                FREEBUF(pairs);
                if (!cached_irq_map.empty()) return cached_irq_map;
            }
        }
    }

    // 2. Linux < 6.5: irq_desc_tree is XArray or Radix Tree
    if (csymbol_exists("irq_desc_tree")) {
        ulong tree_addr = csymbol_value("irq_desc_tree");
        if (kt->flags2 & IRQ_DESC_TREE_XARRAY) {
            size_t count = do_xarray(tree_addr, XARRAY_COUNT, NULL);
            if (count > 0) {
                struct list_pair *pairs = (struct list_pair *)GETBUF(count * sizeof(struct list_pair));
                do_xarray(tree_addr, XARRAY_GATHER, pairs);
                for (size_t i = 0; i < count; ++i) {
                    if (is_kvaddr((ulong)pairs[i].value)) {
                        cached_irq_map[(int)pairs[i].index] = (ulong)pairs[i].value;
                    }
                }
                FREEBUF(pairs);
                if (!cached_irq_map.empty()) return cached_irq_map;
            }
        } else if (kt->flags2 & IRQ_DESC_TREE_RADIX) {
            size_t count = do_radix_tree(tree_addr, RADIX_TREE_COUNT, NULL);
            if (count > 0) {
                struct list_pair *pairs = (struct list_pair *)GETBUF(count * sizeof(struct list_pair));
                do_radix_tree(tree_addr, RADIX_TREE_GATHER, pairs);
                for (size_t i = 0; i < count; ++i) {
                    if (is_kvaddr((ulong)pairs[i].value)) {
                        cached_irq_map[(int)pairs[i].index] = (ulong)pairs[i].value;
                    }
                }
                FREEBUF(pairs);
                if (!cached_irq_map.empty()) return cached_irq_map;
            }
        } else {
            // Fallback: try XArray then Radix tree
            size_t count = do_xarray(tree_addr, XARRAY_COUNT, NULL);
            if (count > 0) {
                struct list_pair *pairs = (struct list_pair *)GETBUF(count * sizeof(struct list_pair));
                do_xarray(tree_addr, XARRAY_GATHER, pairs);
                for (size_t i = 0; i < count; ++i) {
                    if (is_kvaddr((ulong)pairs[i].value)) {
                        cached_irq_map[(int)pairs[i].index] = (ulong)pairs[i].value;
                    }
                }
                FREEBUF(pairs);
                if (!cached_irq_map.empty()) return cached_irq_map;
            }

            count = do_radix_tree(tree_addr, RADIX_TREE_COUNT, NULL);
            if (count > 0) {
                struct list_pair *pairs = (struct list_pair *)GETBUF(count * sizeof(struct list_pair));
                do_radix_tree(tree_addr, RADIX_TREE_GATHER, pairs);
                for (size_t i = 0; i < count; ++i) {
                    if (is_kvaddr((ulong)pairs[i].value)) {
                        cached_irq_map[(int)pairs[i].index] = (ulong)pairs[i].value;
                    }
                }
                FREEBUF(pairs);
                if (!cached_irq_map.empty()) return cached_irq_map;
            }
        }
    }

    // 3. Static irq_desc array (legacy/embedded)
    if (cached_irq_map.empty() && csymbol_exists("irq_desc")) {
        ulong desc_array = csymbol_value("irq_desc");
        int sz = struct_size(irq_desc);
        if (sz > 0) {
            int max_irqs = get_max_nr_irqs();
            for (int i = 0; i < max_irqs; ++i) {
                ulong desc = desc_array + i * sz;
                if (is_kvaddr(desc)) {
                    cached_irq_map[i] = desc;
                }
            }
        }
    }

    return cached_irq_map;
}

std::string Irq::get_affinity_string(ulong affinity_addr) {
    if (!affinity_addr || !is_kvaddr(affinity_addr)) return "all";
    int num_cpus = kt->cpus;
    if (num_cpus <= 0 || num_cpus > 1024) return "unknown";

    std::vector<bool> cpu_bits(num_cpus, false);
    int ulongs = (num_cpus + 63) / 64;
    for (int i = 0; i < ulongs; ++i) {
        ulong val = read_ulong(affinity_addr + i * sizeof(ulong), "affinity_bits");
        for (int bit = 0; bit < 64 && (i * 64 + bit) < num_cpus; ++bit) {
            if ((val >> bit) & 1ULL) {
                cpu_bits[i * 64 + bit] = true;
            }
        }
    }

    std::string res;
    int start = -1;
    int all_count = 0;
    for (int i = 0; i < num_cpus; ++i) {
        if (cpu_bits[i]) all_count++;
        if (cpu_bits[i] && start == -1) {
            start = i;
        } else if (!cpu_bits[i] && start != -1) {
            if (!res.empty()) res += ",";
            if (start == i - 1) res += std::to_string(start);
            else res += std::to_string(start) + "-" + std::to_string(i - 1);
            start = -1;
        }
    }
    if (start != -1) {
        if (!res.empty()) res += ",";
        if (start == num_cpus - 1) res += std::to_string(start);
        else res += std::to_string(start) + "-" + std::to_string(num_cpus - 1);
    }
    if (all_count == num_cpus) return "all (0-" + std::to_string(num_cpus - 1) + ")";
    return res.empty() ? "none" : res;
}

std::string Irq::parse_trigger_type(uint state) {
    uint trig = state & 0xf;
    switch (trig) {
        case 1: return "Edge-Rising";
        case 2: return "Edge-Falling";
        case 3: return "Edge-Both";
        case 4: return "Level-High";
        case 8: return "Level-Low";
        default: return "None";
    }
}

std::string Irq::parse_action_flags(uint flags) {
    std::string res;
    if (flags & 0x00000080) res += "SHARED ";
    if (flags & 0x00000400) res += "PERCPU ";
    if (flags & 0x00002000) res += "ONESHOT ";
    if (flags & 0x00004000) res += "NO_SUSPEND ";
    if (flags & 0x00020000) res += "TIMER ";
    if (flags & 0x00000001) res += "EDGE_RISING ";
    if (flags & 0x00000002) res += "EDGE_FALLING ";
    if (flags & 0x00000004) res += "LEVEL_HIGH ";
    if (flags & 0x00000008) res += "LEVEL_LOW ";
    if (res.empty()) res = "NONE";
    else if (res.back() == ' ') res.pop_back();
    return res;
}

std::shared_ptr<IrqInfo> Irq::parse_irq_desc(int irq, ulong desc_addr) {
    auto info = std::make_shared<IrqInfo>();
    info->irq = irq;
    info->desc_addr = desc_addr;
    info->per_cpu_counts.resize(kt->cpus, 0);

    int off_irq_data = field_offset(irq_desc, irq_data);
    ulong data_addr = (off_irq_data >= 0) ? (desc_addr + off_irq_data) : 0;

    if (data_addr) {
        int off_hwirq = field_offset(irq_data, hwirq);
        if (off_hwirq >= 0) {
            info->hwirq = read_ulong(data_addr + off_hwirq, "hwirq");
        }

        int off_chip = field_offset(irq_data, chip);
        if (off_chip >= 0) {
            ulong chip_addr = read_pointer(data_addr + off_chip, "chip");
            if (chip_addr && is_kvaddr(chip_addr)) {
                int off_cname = field_offset(irq_chip, name);
                if (off_cname >= 0) {
                    ulong name_ptr = read_pointer(chip_addr + off_cname, "chip_name");
                    if (name_ptr && is_kvaddr(name_ptr)) {
                        info->chip_name = read_cstring(name_ptr, 64, "chip_name");
                    }
                }
            }
        }

        int off_domain = field_offset(irq_data, domain);
        if (off_domain >= 0) {
            ulong domain_addr = read_pointer(data_addr + off_domain, "domain");
            if (domain_addr && is_kvaddr(domain_addr)) {
                int off_dname = field_offset(irq_domain, name);
                if (off_dname >= 0) {
                    ulong dname_ptr = read_pointer(domain_addr + off_dname, "domain_name");
                    if (dname_ptr && is_kvaddr(dname_ptr)) {
                        info->domain_name = read_cstring(dname_ptr, 64, "domain_name");
                    }
                }
            }
        }

        uint state = 0;
        int off_common = field_offset(irq_data, common);
        int off_state = field_offset(irq_common_data, state_use_accessors);
        if (off_common >= 0 && off_state >= 0) {
            ulong common_addr = read_pointer(data_addr + off_common, "common");
            if (common_addr && is_kvaddr(common_addr)) {
                state = read_uint(common_addr + off_state, "state_use_accessors");
            }
        } else if (field_offset(irq_data, state_use_accessors) >= 0) {
            state = read_uint(data_addr + field_offset(irq_data, state_use_accessors), "state_use_accessors");
        }
        info->trigger_type = parse_trigger_type(state);

        ulong affinity_addr = 0;
        if (off_common >= 0) {
            ulong common_addr = read_pointer(data_addr + off_common, "common");
            int off_aff = field_offset(irq_common_data, affinity);
            if (common_addr && is_kvaddr(common_addr) && off_aff >= 0) {
                affinity_addr = common_addr + off_aff;
            }
        }
        if (!affinity_addr && field_offset(irq_desc, percpu_affinity) >= 0) {
            ulong pcpu_aff = read_pointer(desc_addr + field_offset(irq_desc, percpu_affinity), "percpu_affinity");
            if (pcpu_aff && is_kvaddr(pcpu_aff)) {
                affinity_addr = pcpu_aff;
            }
        }
        if (affinity_addr) {
            info->affinity_str = get_affinity_string(affinity_addr);
        } else {
            info->affinity_str = "all";
        }
    }

    if (field_offset(irq_desc, depth) >= 0) {
        info->depth = read_uint(desc_addr + field_offset(irq_desc, depth), "depth");
    }

    if (field_offset(irq_desc, irqs_unhandled) >= 0) {
        info->irqs_unhandled = read_uint(desc_addr + field_offset(irq_desc, irqs_unhandled), "irqs_unhandled");
    }

    int off_act = field_offset(irq_desc, action);
    if (off_act >= 0) {
        ulong act_addr = read_pointer(desc_addr + off_act, "action");
        while (act_addr && is_kvaddr(act_addr)) {
            IrqActionInfo act;
            int off_aname = field_offset(irqaction, name);
            if (off_aname >= 0) {
                ulong aname_ptr = read_pointer(act_addr + off_aname, "act_name");
                if (aname_ptr && is_kvaddr(aname_ptr)) {
                    act.name = read_cstring(aname_ptr, 64, "act_name");
                }
            }
            int off_hdl = field_offset(irqaction, handler);
            if (off_hdl >= 0) {
                act.handler = read_pointer(act_addr + off_hdl, "handler");
                act.handler_name = to_symbol(act.handler);
            }
            int off_tfn = field_offset(irqaction, thread_fn);
            if (off_tfn >= 0) {
                act.thread_fn = read_pointer(act_addr + off_tfn, "thread_fn");
                if (act.thread_fn) {
                    act.thread_fn_name = to_symbol(act.thread_fn);
                }
            }
            int off_flg = field_offset(irqaction, flags);
            if (off_flg >= 0) {
                act.flags = read_uint(act_addr + off_flg, "flags");
            }
            int off_dev = field_offset(irqaction, dev_id);
            if (off_dev >= 0) {
                act.dev_id = read_pointer(act_addr + off_dev, "dev_id");
            }
            info->actions.push_back(act);

            int off_next = field_offset(irqaction, next);
            if (off_next >= 0) {
                act_addr = read_pointer(act_addr + off_next, "next");
            } else {
                break;
            }
        }
    }

    int off_kstat = field_offset(irq_desc, kstat_irqs);
    if (off_kstat >= 0) {
        ulong kstat_ptr = read_pointer(desc_addr + off_kstat, "kstat_irqs");
        if (kstat_ptr && is_kvaddr(kstat_ptr)) {
            auto cpu_addrs = for_each_percpu(kstat_ptr);
            for (size_t c = 0; c < cpu_addrs.size() && c < (size_t)kt->cpus; ++c) {
                if (cpu_addrs[c] && is_kvaddr(cpu_addrs[c])) {
                    uint cnt = read_uint(cpu_addrs[c], "kstat_irqs_val");
                    info->per_cpu_counts[c] = cnt;
                    info->total_count += cnt;
                }
            }
        }
    }

    return info;
}

void Irq::print_all_irqs() {
    auto irq_map = gather_irq_desc_map();
    if (irq_map.empty()) {
        PRINT("No IRQ descriptors found.\n");
        return;
    }

    PRINT("Registered IRQs Overview\n");
    PRINT("┌─────┬────────┬──────────────┬────────────────────────────┬────────────────────────────┬──────────┬──────────┬─────────────┬───────────┐\n");
    PRINT("│ IRQ#│ HWIRQ  │ Chip         │ Name / Device              │ Handler                    │    Total │ Depth    │ Trigger     │ Affinity  │\n");
    PRINT("├─────┼────────┼──────────────┼────────────────────────────┼────────────────────────────┼──────────┼──────────┼─────────────┼───────────┤\n");

    uint64_t grand_total_fires = 0;
    int active_irq_count = 0;

    char buf[512];
    for (const auto& kv : irq_map) {
        auto info = parse_irq_desc(kv.first, kv.second);
        if (!info) continue;

        active_irq_count++;
        grand_total_fires += info->total_count;

        std::string act_names;
        std::string handler_syms;
        if (!info->actions.empty()) {
            for (size_t i = 0; i < info->actions.size(); ++i) {
                if (i > 0) {
                    act_names += ",";
                    handler_syms += ",";
                }
                std::string aname = info->actions[i].name.empty() ? "[none]" : info->actions[i].name;
                act_names += aname;
                handler_syms += info->actions[i].handler_name;
            }
        } else {
            act_names = "[none]";
            handler_syms = "[none]";
        }

        if (act_names.length() > 26) act_names = act_names.substr(0, 23) + "...";
        if (handler_syms.length() > 26) handler_syms = handler_syms.substr(0, 23) + "...";
        std::string chip_str = info->chip_name.empty() ? "[unknown]" : info->chip_name;
        if (chip_str.length() > 12) chip_str = chip_str.substr(0, 10) + "..";

        std::string depth_str = std::to_string(info->depth);
        if (info->depth == 0) depth_str += " (EN)";
        else depth_str += " (DIS)";

        std::string aff_str = info->affinity_str;
        if (aff_str.length() > 9) aff_str = aff_str.substr(0, 8) + ".";

        snprintf(buf, sizeof(buf),
                 "│%4d │%7lu │ %-12s │ %-26s │ %-26s │%9u │ %-8s │ %-11s │ %-9s │\n",
                 info->irq, info->hwirq, chip_str.c_str(), act_names.c_str(),
                 handler_syms.c_str(), info->total_count, depth_str.c_str(),
                 info->trigger_type.c_str(), aff_str.c_str());
        PRINT("%s", buf);
    }

    PRINT("└─────┴────────┴──────────────┴────────────────────────────┴────────────────────────────┴──────────┴──────────┴─────────────┴───────────┘\n");
    PRINT("Total Registered IRQs: %d | Total Interrupt Fires: %lu\n", active_irq_count, grand_total_fires);
}

void Irq::print_cpu_distribution() {
    auto irq_map = gather_irq_desc_map();
    if (irq_map.empty()) {
        PRINT("No IRQ descriptors found.\n");
        return;
    }

    int cpus_to_show = std::min(kt->cpus, 8);

    PRINT("Per-CPU Interrupt Distribution (Displaying up to 8 CPUs)\n");
    std::ostringstream oss_top;
    oss_top << "┌─────┬────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_top << "┬──────────";
    oss_top << "┬───────────┬───────────┐\n";
    PRINT("%s", oss_top.str().c_str());

    std::ostringstream oss_hdr;
    oss_hdr << "│ IRQ#│ Name                   ";
    for (int c = 0; c < cpus_to_show; ++c) {
        char chdr[32];
        snprintf(chdr, sizeof(chdr), "│     CPU%d ", c);
        oss_hdr << chdr;
    }
    oss_hdr << "│     Total │ Affinity  │\n";
    PRINT("%s", oss_hdr.str().c_str());

    std::ostringstream oss_mid;
    oss_mid << "├─────┼────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_mid << "┼──────────";
    oss_mid << "┼───────────┼───────────┤\n";
    PRINT("%s", oss_mid.str().c_str());

    for (const auto& kv : irq_map) {
        auto info = parse_irq_desc(kv.first, kv.second);
        if (!info) continue;

        std::string act_names;
        if (!info->actions.empty()) {
            for (size_t i = 0; i < info->actions.size(); ++i) {
                if (i > 0) act_names += ",";
                std::string aname = info->actions[i].name.empty() ? "[none]" : info->actions[i].name;
                act_names += aname;
            }
        } else {
            act_names = "[none]";
        }
        if (act_names.length() > 22) act_names = act_names.substr(0, 19) + "...";

        std::ostringstream oss_row;
        char prefix[64];
        snprintf(prefix, sizeof(prefix), "│%4d │ %-22s ", info->irq, act_names.c_str());
        oss_row << prefix;

        for (int c = 0; c < cpus_to_show; ++c) {
            uint val = (c < (int)info->per_cpu_counts.size()) ? info->per_cpu_counts[c] : 0;
            char cval[32];
            snprintf(cval, sizeof(cval), "│%9u ", val);
            oss_row << cval;
        }

        std::string aff_str = info->affinity_str;
        if (aff_str.length() > 9) aff_str = aff_str.substr(0, 8) + ".";

        char suffix[64];
        snprintf(suffix, sizeof(suffix), "│%10u │ %-9s │\n", info->total_count, aff_str.c_str());
        oss_row << suffix;

        PRINT("%s", oss_row.str().c_str());
    }

    std::ostringstream oss_bot;
    oss_bot << "└─────┴────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_bot << "┴──────────";
    oss_bot << "┴───────────┴───────────┘\n";
    PRINT("%s", oss_bot.str().c_str());
}

void Irq::print_irq_storm() {
    auto irq_map = gather_irq_desc_map();
    if (irq_map.empty()) {
        PRINT("No IRQ descriptors found.\n");
        return;
    }

    std::vector<std::shared_ptr<IrqInfo>> irq_list;
    uint64_t grand_total_fires = 0;

    for (const auto& kv : irq_map) {
        auto info = parse_irq_desc(kv.first, kv.second);
        if (info) {
            grand_total_fires += info->total_count;
            irq_list.push_back(info);
        }
    }

    std::sort(irq_list.begin(), irq_list.end(),
              [](const std::shared_ptr<IrqInfo>& a, const std::shared_ptr<IrqInfo>& b) {
                  return a->total_count > b->total_count;
              });

    PRINT("Interrupt Frequency Ranking (Top 25) & Storm Analysis\n");
    PRINT("┌─────┬─────┬────────┬────────────────────────────┬────────────────────────────┬──────────┬────────┬──────────────┐\n");
    PRINT("│ Rank│ IRQ#│ HWIRQ  │ Name / Device              │ Handler                    │    Total │  Ratio │ Top CPU Fire │\n");
    PRINT("├─────┼─────┼────────┼────────────────────────────┼────────────────────────────┼──────────┼────────┼──────────────┤\n");

    int rank = 1;
    char buf[512];
    for (const auto& info : irq_list) {
        if (rank > 25) break;

        std::string act_names;
        std::string handler_syms;
        if (!info->actions.empty()) {
            for (size_t i = 0; i < info->actions.size(); ++i) {
                if (i > 0) {
                    act_names += ",";
                    handler_syms += ",";
                }
                std::string aname = info->actions[i].name.empty() ? "[none]" : info->actions[i].name;
                act_names += aname;
                handler_syms += info->actions[i].handler_name;
            }
        } else {
            act_names = "[none]";
            handler_syms = "[none]";
        }

        if (act_names.length() > 26) act_names = act_names.substr(0, 23) + "...";
        if (handler_syms.length() > 26) handler_syms = handler_syms.substr(0, 23) + "...";

        double ratio = (grand_total_fires > 0) ? ((double)info->total_count * 100.0 / (double)grand_total_fires) : 0.0;

        int top_cpu = 0;
        uint top_cpu_val = 0;
        for (size_t c = 0; c < info->per_cpu_counts.size(); ++c) {
            if (info->per_cpu_counts[c] > top_cpu_val) {
                top_cpu_val = info->per_cpu_counts[c];
                top_cpu = c;
            }
        }

        char top_cpu_str[32];
        snprintf(top_cpu_str, sizeof(top_cpu_str), "CPU%d (%u)", top_cpu, top_cpu_val);

        snprintf(buf, sizeof(buf),
                 "│%4d │%4d │%7lu │ %-26s │ %-26s │%9u │%6.2f%% │ %-12s │\n",
                 rank, info->irq, info->hwirq, act_names.c_str(), handler_syms.c_str(),
                 info->total_count, ratio, top_cpu_str);
        PRINT("%s", buf);

        rank++;
    }

    PRINT("└─────┴─────┴────────┴────────────────────────────┴────────────────────────────┴──────────┴────────┴──────────────┘\n");
    PRINT("Total Interrupt Fires Across System: %lu\n", grand_total_fires);

    // Storm heuristic check
    if (!irq_list.empty()) {
        auto top_info = irq_list[0];
        double top_ratio = (grand_total_fires > 0) ? ((double)top_info->total_count * 100.0 / (double)grand_total_fires) : 0.0;
        if (top_ratio >= 40.0 && grand_total_fires > 10000) {
            PRINT("\033[31m[!] ALERT: Potential Interrupt Storm detected! IRQ %d accounts for %.2f%% of all interrupts.\033[0m\n",
                  top_info->irq, top_ratio);
        }
    }
}

void Irq::print_irq_detail(int target_irq) {
    auto irq_map = gather_irq_desc_map();
    auto it = irq_map.find(target_irq);
    if (it == irq_map.end()) {
        PRINT("IRQ %d not found in kernel descriptors.\n", target_irq);
        return;
    }

    auto info = parse_irq_desc(target_irq, it->second);
    if (!info) {
        PRINT("Failed to parse descriptor for IRQ %d.\n", target_irq);
        return;
    }

    char lbuf[256];
    PRINT("IRQ Details: IRQ %d\n", target_irq);
    PRINT("┌──────────────────────────────────────────────────────────────────────────────┐\n");
    snprintf(lbuf, sizeof(lbuf), "  IRQ Number:             %d", info->irq);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  HWIRQ Number:           %lu", info->hwirq);
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  irq_desc Address:       0x%lx", info->desc_addr);
    PRINT("│ %-76s │\n", lbuf);

    std::string chip_str = info->chip_name.empty() ? "[unknown]" : info->chip_name;
    snprintf(lbuf, sizeof(lbuf), "  Chip Name:              %s", chip_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string dom_str = info->domain_name.empty() ? "[none]" : info->domain_name;
    snprintf(lbuf, sizeof(lbuf), "  Domain Name:            %s", dom_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Trigger Type:           %s", info->trigger_type.c_str());
    PRINT("│ %-76s │\n", lbuf);

    std::string depth_str = (info->depth == 0 ? "0 (Enabled)" : (std::to_string(info->depth) + " (Disabled)"));
    snprintf(lbuf, sizeof(lbuf), "  Disable Depth:          %s", depth_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Unhandled Interrupts:   %u", info->irqs_unhandled);
    PRINT("│ %-76s │\n", lbuf);

    std::string aff_str = info->affinity_str;
    if (aff_str.length() > 50) aff_str = aff_str.substr(0, 47) + "...";
    snprintf(lbuf, sizeof(lbuf), "  Affinity:               %s", aff_str.c_str());
    PRINT("│ %-76s │\n", lbuf);

    snprintf(lbuf, sizeof(lbuf), "  Total Interrupt Fires:  %u", info->total_count);
    PRINT("│ %-76s │\n", lbuf);

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    PRINT("│ %-76s │\n", "  Per-CPU Breakdown:");

    size_t total_cpus = info->per_cpu_counts.size();
    for (size_t start_c = 0; start_c < total_cpus; start_c += 4) {
        std::string line_content = "   ";
        for (size_t c = start_c; c < std::min(start_c + 4, total_cpus); ++c) {
            char cbuf[64];
            snprintf(cbuf, sizeof(cbuf), "CPU%-2zu: %-8u", c, info->per_cpu_counts[c]);
            line_content += cbuf;
            if (c + 1 < std::min(start_c + 4, total_cpus)) {
                line_content += "  |  ";
            }
        }
        PRINT("│ %-76s │\n", line_content.c_str());
    }

    PRINT("├──────────────────────────────────────────────────────────────────────────────┤\n");
    snprintf(lbuf, sizeof(lbuf), "  Actions Chain (%zu registered):", info->actions.size());
    PRINT("│ %-76s │\n", lbuf);

    for (size_t i = 0; i < info->actions.size(); ++i) {
        const auto& act = info->actions[i];
        std::string act_name = act.name.empty() ? "[none]" : act.name;
        if (act_name.length() > 48) act_name = act_name.substr(0, 45) + "...";
        snprintf(lbuf, sizeof(lbuf), "    [%zu] Name:            %s", i, act_name.c_str());
        PRINT("│ %-76s │\n", lbuf);

        std::string hname = act.handler_name;
        if (hname.length() > 48) hname = hname.substr(0, 45) + "...";
        snprintf(lbuf, sizeof(lbuf), "        Handler:         %s", hname.c_str());
        PRINT("│ %-76s │\n", lbuf);

        if (act.thread_fn) {
            std::string tname = act.thread_fn_name;
            if (tname.length() > 48) tname = tname.substr(0, 45) + "...";
            snprintf(lbuf, sizeof(lbuf), "        Thread Fn:       %s", tname.c_str());
            PRINT("│ %-76s │\n", lbuf);
        }

        std::string flag_str = parse_action_flags(act.flags);
        if (flag_str.length() > 48) flag_str = flag_str.substr(0, 45) + "...";
        snprintf(lbuf, sizeof(lbuf), "        Flags:           %s", flag_str.c_str());
        PRINT("│ %-76s │\n", lbuf);

        char dev_str[64];
        snprintf(dev_str, sizeof(dev_str), "0x%lx", act.dev_id);
        snprintf(lbuf, sizeof(lbuf), "        Dev ID:          %s", dev_str);
        PRINT("│ %-76s │\n", lbuf);
    }
    PRINT("└──────────────────────────────────────────────────────────────────────────────┘\n");
}

void Irq::print_softirqs() {
    const std::vector<std::string> softirq_names = {
        "HI", "TIMER", "NET_TX", "NET_RX", "BLOCK",
        "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU"
    };
    int num_softirqs = softirq_names.size();
    int num_cpus = kt->cpus;
    int cpus_to_show = std::min(num_cpus, 8);

    ulong vec_addr = csymbol_exists("softirq_vec") ? csymbol_value("softirq_vec") : 0;
    int elem_sz = struct_size(softirq_action);
    if (elem_sz <= 0) elem_sz = sizeof(ulong);

    std::vector<uint> pending_per_cpu(num_cpus, 0);
    if (csymbol_exists("irq_stat")) {
        ulong irq_stat_addr = csymbol_value("irq_stat");
        auto cpu_addrs = for_each_percpu(irq_stat_addr);
        for (int c = 0; c < num_cpus && c < (int)cpu_addrs.size(); ++c) {
            if (cpu_addrs[c]) {
                pending_per_cpu[c] = read_uint(cpu_addrs[c], "__softirq_pending");
            }
        }
    } else if (csymbol_exists("__softirq_pending")) {
        ulong pending_addr = csymbol_value("__softirq_pending");
        auto cpu_addrs = for_each_percpu(pending_addr);
        for (int c = 0; c < num_cpus && c < (int)cpu_addrs.size(); ++c) {
            if (cpu_addrs[c]) {
                pending_per_cpu[c] = read_uint(cpu_addrs[c], "__softirq_pending");
            }
        }
    }

    std::vector<std::vector<uint>> stats(num_softirqs, std::vector<uint>(num_cpus, 0));
    if (csymbol_exists("kstat_softirqs")) {
        ulong kstat_addr = csymbol_value("kstat_softirqs");
        auto cpu_addrs = for_each_percpu(kstat_addr);
        for (int c = 0; c < num_cpus && c < (int)cpu_addrs.size(); ++c) {
            if (cpu_addrs[c]) {
                for (int s = 0; s < num_softirqs; ++s) {
                    stats[s][c] = read_uint(cpu_addrs[c] + s * sizeof(uint), "kstat_softirqs_val");
                }
            }
        }
    } else if (csymbol_exists("kstat")) {
        ulong kstat_addr = csymbol_value("kstat");
        auto cpu_addrs = for_each_percpu(kstat_addr);
        int off_softirqs = -1;
        if (struct_size(kernel_stat) > 0) {
            off_softirqs = field_offset(kernel_stat, softirqs);
        }
        if (off_softirqs < 0) {
            off_softirqs = sizeof(ulong); // In 64-bit kernel, unsigned long irqs_sum is 8 bytes
        }
        for (int c = 0; c < num_cpus && c < (int)cpu_addrs.size(); ++c) {
            if (cpu_addrs[c]) {
                for (int s = 0; s < num_softirqs; ++s) {
                    stats[s][c] = read_uint(cpu_addrs[c] + off_softirqs + s * sizeof(uint), "kstat_softirqs_val");
                }
            }
        }
    }

    PRINT("SoftIRQ System Overview (Displaying up to 8 CPUs)\n");
    std::ostringstream oss_top;
    oss_top << "┌────┬────────────┬────────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_top << "┬──────────";
    oss_top << "┬───────────┬────────────┐\n";
    PRINT("%s", oss_top.str().c_str());

    std::ostringstream oss_hdr;
    oss_hdr << "│  # │ SoftIRQ    │ Action Handler             ";
    for (int c = 0; c < cpus_to_show; ++c) {
        char chdr[32];
        snprintf(chdr, sizeof(chdr), "│     CPU%d ", c);
        oss_hdr << chdr;
    }
    oss_hdr << "│     Total │ Pending    │\n";
    PRINT("%s", oss_hdr.str().c_str());

    std::ostringstream oss_mid;
    oss_mid << "├────┼────────────┼────────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_mid << "┼──────────";
    oss_mid << "┼───────────┼────────────┤\n";
    PRINT("%s", oss_mid.str().c_str());

    for (int i = 0; i < num_softirqs; ++i) {
        std::string handler_sym = "[unknown]";
        if (vec_addr) {
            ulong act_fn = read_pointer(vec_addr + i * elem_sz, "softirq_action");
            if (act_fn) {
                handler_sym = to_symbol(act_fn);
            }
        }
        if (handler_sym.length() > 26) handler_sym = handler_sym.substr(0, 23) + "...";

        uint total_cnt = 0;
        for (int c = 0; c < num_cpus; ++c) total_cnt += stats[i][c];

        std::string pending_str = "None";
        std::string pend_cpus;
        for (int c = 0; c < num_cpus; ++c) {
            if (pending_per_cpu[c] & (1U << i)) {
                if (!pend_cpus.empty()) pend_cpus += ",";
                pend_cpus += std::to_string(c);
            }
        }
        if (!pend_cpus.empty()) {
            pending_str = "CPU:" + pend_cpus + " [!]";
        }
        if (pending_str.length() > 10) pending_str = pending_str.substr(0, 9) + ".";

        std::ostringstream oss_row;
        char prefix[128];
        snprintf(prefix, sizeof(prefix), "│%3d │ %-10s │ %-26s ", i, softirq_names[i].c_str(), handler_sym.c_str());
        oss_row << prefix;

        for (int c = 0; c < cpus_to_show; ++c) {
            char cval[32];
            snprintf(cval, sizeof(cval), "│%9u ", stats[i][c]);
            oss_row << cval;
        }

        char suffix[64];
        snprintf(suffix, sizeof(suffix), "│%10u │ %-10s │\n", total_cnt, pending_str.c_str());
        oss_row << suffix;

        PRINT("%s", oss_row.str().c_str());
    }

    std::ostringstream oss_bot;
    oss_bot << "└────┴────────────┴────────────────────────────";
    for (int c = 0; c < cpus_to_show; ++c) oss_bot << "┴──────────";
    oss_bot << "┴───────────┴────────────┘\n";
    PRINT("%s", oss_bot.str().c_str());

    PRINT("ksoftirqd Thread Status:\n");
    for (int c = 0; c < num_cpus; ++c) {
        std::string ksoft_name = "ksoftirqd/" + std::to_string(c);
        struct task_context *tc = find_proc(ksoft_name);
        if (tc) {
            char task_buf[256];
            snprintf(task_buf, sizeof(task_buf), "  %-12s : PID %-6lu Task: 0x%-16lx\n",
                     ksoft_name.c_str(), tc->pid, tc->task);
            PRINT("%s", task_buf);
        }
    }
}

#pragma GCC diagnostic pop
