/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#ifndef SRSUE_UE_STACK_NR_H
#define SRSUE_UE_STACK_NR_H

#include <functional>
#include <pthread.h>
#include <stdarg.h>
#include <string>

#include "mac_nr/mac_nr.h"
#include "rrc/rrc_nr.h"
#include "srsran/radio/radio.h"
#include "srsran/upper/pdcp.h"
#include "srsran/upper/rlc.h"
#include "upper/nas.h"
#include "upper/usim.h"

#include "srsran/common/buffer_pool.h"
#include "srsran/common/mac_pcap.h"
#include "srsran/common/multiqueue.h"
#include "srsran/common/thread_pool.h"
#include "srsran/interfaces/ue_nr_interfaces.h"

#include "srsue/hdr/ue_metrics_interface.h"
#include "ue_stack_base.h"

namespace srsue {

/** \brief L2/L3 stack class for 5G/NR UEs.
 *
 *  This class wraps all L2/L3 blocks and provides a single interface towards the PHY.
 */

class ue_stack_nr final : public ue_stack_base,
                          public stack_interface_phy_nr,
                          public stack_interface_gw,
                          public stack_interface_rrc,
                          public srsran::thread
{
public:
  ue_stack_nr();
  ~ue_stack_nr();

  std::string get_type() final;

  int  init(const stack_args_t& args_);
  int  init(const stack_args_t& args_, phy_interface_stack_nr* phy_, gw_interface_stack* gw_);
  bool switch_on() final;
  bool switch_off() final;
  void stop();

  // GW srsue stack_interface_gw dummy interface
  bool is_registered() { return true; };
  bool start_service_request() { return true; };

  bool get_metrics(stack_metrics_t* metrics);
  bool is_rrc_connected();

  // RRC interface for PHY
  void in_sync() final;
  void out_of_sync() final;
  void run_tti(uint32_t tti) final;

  // MAC interface for PHY
  sched_rnti_t get_dl_sched_rnti_nr(const uint32_t tti) final { return mac->get_dl_sched_rnti_nr(tti); }
  sched_rnti_t get_ul_sched_rnti_nr(const uint32_t tti) final { return mac->get_ul_sched_rnti_nr(tti); }
  int          sf_indication(const uint32_t tti)
  {
    run_tti(tti);
    return SRSRAN_SUCCESS;
  }
  void tb_decoded(const uint32_t cc_idx, mac_nr_grant_dl_t& grant) final { mac->tb_decoded(cc_idx, grant); }
  void new_grant_ul(const uint32_t cc_idx, const mac_nr_grant_ul_t& grant, tb_action_ul_t* action) final
  {
    mac->new_grant_ul(cc_idx, grant, action);
  }
  void prach_sent(uint32_t tti, uint32_t s_id, uint32_t t_id, uint32_t f_id, uint32_t ul_carrier_id)
  {
    mac->prach_sent(tti, s_id, t_id, f_id, ul_carrier_id);
  }

  // Interface for GW
  void write_sdu(uint32_t lcid, srsran::unique_byte_buffer_t sdu) final;
  bool is_lcid_enabled(uint32_t lcid) final { return pdcp->is_lcid_enabled(lcid); }

  // Interface for RRC
  srsran::tti_point get_current_tti() { return srsran::tti_point{0}; };

private:
  void run_thread() final;
  void run_tti_impl(uint32_t tti);
  void stop_impl();

  bool                running = false;
  srsue::stack_args_t args    = {};

  // task scheduler
  srsran::task_scheduler                task_sched;
  srsran::task_multiqueue::queue_handle sync_task_queue, ue_task_queue, gw_task_queue;

  // UE stack logging
  srslog::basic_logger& mac_logger;
  srslog::basic_logger& rlc_logger;
  srslog::basic_logger& pdcp_logger;

  // stack components
  std::unique_ptr<mac_nr>       mac;
  std::unique_ptr<rrc_nr>       rrc;
  std::unique_ptr<srsran::rlc>  rlc;
  std::unique_ptr<srsran::pdcp> pdcp;

  // RAT-specific interfaces
  phy_interface_stack_nr* phy = nullptr;
  gw_interface_stack*     gw  = nullptr;

  // Thread
  static const int STACK_MAIN_THREAD_PRIO = 4;
};

} // namespace srsue

#endif // SRSUE_UE_STACK_NR_H
