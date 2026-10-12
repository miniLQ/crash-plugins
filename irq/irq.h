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

#ifndef IRQ_DEFS_H_
#define IRQ_DEFS_H_

#include "plugin.h"
#include <map>
#include <vector>
#include <string>
#include <memory>

struct IrqActionInfo {
    std::string name;
    ulong handler = 0;
    std::string handler_name;
    ulong thread_fn = 0;
    std::string thread_fn_name;
    uint flags = 0;
    ulong dev_id = 0;
};

struct IrqInfo {
    int irq = -1;
    ulong hwirq = 0;
    ulong desc_addr = 0;
    std::string chip_name;
    std::string domain_name;
    uint depth = 0;
    uint irqs_unhandled = 0;
    uint total_count = 0;
    std::vector<uint> per_cpu_counts;
    std::string trigger_type;
    std::string affinity_str;
    std::vector<IrqActionInfo> actions;
};

class Irq : public ParserPlugin {
private:
    std::map<int, ulong> cached_irq_map;
    const std::map<int, ulong>& gather_irq_desc_map();
    int get_max_nr_irqs();
    std::shared_ptr<IrqInfo> parse_irq_desc(int irq, ulong desc_addr);
    std::string get_affinity_string(ulong affinity_addr);
    std::string parse_trigger_type(uint state);
    std::string parse_action_flags(uint flags);

    void print_all_irqs();
    void print_cpu_distribution();
    void print_irq_storm();
    void print_irq_detail(int target_irq);
    void print_softirqs();

public:
    Irq();
    void cmd_main(void) override;
    void init_offset(void) override;
    void init_command(void) override;
    DEFINE_PLUGIN_INSTANCE(Irq)
};

#endif // IRQ_DEFS_H_
