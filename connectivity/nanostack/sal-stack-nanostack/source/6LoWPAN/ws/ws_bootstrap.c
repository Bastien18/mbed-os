/*
 * Copyright (c) 2018-2021, Pelion and affiliates.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <string.h>
#include "nsconfig.h"
#ifdef HAVE_WS
#include "ns_types.h"
#include "ns_trace.h"
#include "nsdynmemLIB.h"
#include "net_interface.h"
#include "eventOS_event.h"
#include "randLIB.h"
#include "common_functions.h"
#include "mac_common_defines.h"
#include "sw_mac.h"
#include "ccmLIB.h"
#include "Core/include/ns_monitor.h"
#include "NWK_INTERFACE/Include/protocol.h"
#include "6LoWPAN/Bootstraps/protocol_6lowpan.h"
#include "6LoWPAN/Bootstraps/protocol_6lowpan_interface.h"
#include "ipv6_stack/protocol_ipv6.h"
#include "ipv6_stack/ipv6_routing_table.h"
#include "6LoWPAN/MAC/mac_helper.h"
#include "6LoWPAN/MAC/mac_data_poll.h"
#include "6LoWPAN/MAC/mpx_api.h"
#include "6LoWPAN/MAC/mac_ie_lib.h"
#include "MPL/mpl.h"
#include "RPL/rpl_protocol.h"
#include "RPL/rpl_control.h"
#include "RPL/rpl_data.h"
#include "RPL/rpl_policy.h"
#include "Common_Protocols/icmpv6.h"
#include "Common_Protocols/icmpv6_radv.h"
#include "Common_Protocols/ipv6_constants.h"
#include "Common_Protocols/ip.h"
#include "Service_Libs/Trickle/trickle.h"
#include "Service_Libs/fhss/channel_list.h"
#include "Service_Libs/utils/ns_time.h"
#include "6LoWPAN/ws/ws_common_defines.h"
#include "6LoWPAN/ws/ws_common_defines.h"
#include "6LoWPAN/ws/ws_config.h"
#include "6LoWPAN/ws/ws_common.h"
#include "6LoWPAN/ws/ws_bootstrap.h"
#include "6LoWPAN/ws/ws_bbr_api_internal.h"
#include "6LoWPAN/ws/ws_common_defines.h"
#include "6LoWPAN/ws/ws_llc.h"
#include "6LoWPAN/ws/ws_neighbor_class.h"
#include "6LoWPAN/ws/ws_ie_lib.h"
#include "6LoWPAN/ws/ws_stats.h"
#include "6LoWPAN/ws/ws_cfg_settings.h"
#include "6LoWPAN/ws/ws_bootstrap_6lbr.h"
#include "6LoWPAN/ws/ws_bootstrap_6lr.h"
#include "6LoWPAN/ws/ws_bootstrap_ffn.h"
#include "6LoWPAN/ws/ws_bootstrap_6ln.h"
#include "6LoWPAN/ws/ws_phy.h"
#include "6LoWPAN/lowpan_adaptation_interface.h"
#include "Service_Libs/etx/etx.h"
#include "Service_Libs/mac_neighbor_table/mac_neighbor_table.h"
#include "Service_Libs/nd_proxy/nd_proxy.h"
#include "Service_Libs/blacklist/blacklist.h"
#include "platform/topo_trace.h"
#include "dhcp_service_api.h"
#include "libDHCPv6/libDHCPv6.h"
#include "libDHCPv6/libDHCPv6_vendordata.h"
#include "DHCPv6_client/dhcpv6_client_api.h"
#include "ws_management_api.h"
#include "net_rpl.h"
#include "mac_api.h"
#include "6LoWPAN/ws/ws_pae_controller.h"
#include "6LoWPAN/ws/ws_eapol_pdu.h"
#include "6LoWPAN/ws/ws_eapol_auth_relay.h"
#include "6LoWPAN/ws/ws_eapol_relay.h"
#include "libNET/src/net_dns_internal.h"
#include "Service_Libs/random_early_detection/random_early_detection_api.h"

#define TRACE_GROUP "wsbs"

static void ws_bootstrap_event_handler(arm_event_s *event);
static int8_t ws_bootsrap_event_trig(ws_bootsrap_event_type_e event_type, int8_t interface_id, arm_library_event_priority_e priority, void *event_data);
static uint16_t ws_bootstrap_routing_cost_calculate(protocol_interface_info_entry_t *cur);
static void ws_bootstrap_mac_security_enable(protocol_interface_info_entry_t *cur);
static void ws_bootstrap_nw_key_set(protocol_interface_info_entry_t *cur, uint8_t operation, uint8_t index, uint8_t *key);
static void ws_bootstrap_nw_key_clear(protocol_interface_info_entry_t *cur, uint8_t slot);
static void ws_bootstrap_nw_key_index_set(protocol_interface_info_entry_t *cur, uint8_t index);
static void ws_bootstrap_nw_frame_counter_set(protocol_interface_info_entry_t *cur, uint32_t counter, uint8_t slot);
static void ws_bootstrap_nw_frame_counter_read(protocol_interface_info_entry_t *cur, uint32_t *counter, uint8_t slot);
static void ws_bootstrap_authentication_completed(protocol_interface_info_entry_t *cur, auth_result_e result, uint8_t *target_eui_64);
static const uint8_t *ws_bootstrap_authentication_next_target(protocol_interface_info_entry_t *cur, const uint8_t *previous_eui_64, uint16_t *pan_id);
static ws_nud_table_entry_t *ws_nud_entry_discover(protocol_interface_info_entry_t *cur, void *neighbor);
static void ws_nud_entry_remove(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *entry_ptr);
static bool ws_neighbor_entry_nud_notify(mac_neighbor_table_entry_t *entry_ptr, void *user_data);
static void ws_bootstrap_test_procedure_trigger_timer(protocol_interface_info_entry_t *cur, uint32_t seconds);

uint16_t test_pan_version = 1;

static mac_neighbor_table_entry_t *ws_bootstrap_mac_neighbor_allocate(struct protocol_interface_info_entry *interface, const uint8_t *src64)
{
    mac_neighbor_table_entry_t *neighbor = mac_neighbor_table_entry_allocate(mac_neighbor_info(interface), src64);
    if (!neighbor) {
        return NULL;
    }
    // TODO only call these for new neighbour
    mlme_device_descriptor_t device_desc;
    neighbor->lifetime = ws_cfg_neighbour_temporary_lifetime_get();
    neighbor->link_lifetime = ws_cfg_neighbour_temporary_lifetime_get();
    mac_helper_device_description_write(interface, &device_desc, neighbor->mac64, neighbor->mac16, 0, false);
    mac_helper_devicetable_set(&device_desc, interface, neighbor->index, interface->mac_parameters->mac_default_key_index, true);

    return neighbor;
}

mac_neighbor_table_entry_t *ws_bootstrap_mac_neighbor_add(struct protocol_interface_info_entry *interface, const uint8_t *src64)
{
    mac_neighbor_table_entry_t *neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(interface), src64, MAC_ADDR_MODE_64_BIT);
    if (neighbor) {
        return neighbor;
    }

    return ws_bootstrap_mac_neighbor_allocate(interface, src64);
}

void ws_bootstrap_neighbor_set_stable(struct protocol_interface_info_entry *interface, const uint8_t *src64)
{
    mac_neighbor_table_entry_t *neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(interface), src64, MAC_ADDR_MODE_64_BIT);

    if (neighbor && neighbor->link_lifetime != WS_NEIGHBOR_LINK_TIMEOUT) {
        neighbor->lifetime = WS_NEIGHBOR_LINK_TIMEOUT;
        neighbor->link_lifetime = WS_NEIGHBOR_LINK_TIMEOUT;
        tr_info("Added new neighbor %s : index:%u", trace_array(src64, 8), neighbor->index);
    }
}

void ws_bootstrap_mac_neighbor_short_time_set(struct protocol_interface_info_entry *interface, const uint8_t *src64, uint32_t valid_time)
{
    mac_neighbor_table_entry_t *neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(interface), src64, MAC_ADDR_MODE_64_BIT);

    if (neighbor && neighbor->link_lifetime <= valid_time) {
        //mlme_device_descriptor_t device_desc;
        neighbor->lifetime = valid_time;
        neighbor->link_lifetime = valid_time;
        tr_debug("Set short response neighbor %s : index:%u", trace_array(src64, 8), neighbor->index);
    }
}

static void ws_bootstrap_neighbor_delete(struct protocol_interface_info_entry *interface, mac_neighbor_table_entry_t *entry_ptr)
{
    mac_helper_devicetable_remove(interface->mac_api, entry_ptr->index, entry_ptr->mac64);
    etx_neighbor_remove(interface->id, entry_ptr->index, entry_ptr->mac64);
    ws_neighbor_class_entry_remove(&interface->ws_info->neighbor_storage, entry_ptr->index);
}

void ws_bootstrap_neighbor_list_clean(struct protocol_interface_info_entry *interface)
{

    mac_neighbor_table_neighbor_list_clean(mac_neighbor_info(interface));
}

void ws_address_reregister_trig(struct protocol_interface_info_entry *interface)
{
    if (interface->ws_info->aro_registration_timer == 0) {
        interface->ws_info->aro_registration_timer = WS_NEIGHBOR_NUD_TIMEOUT;
    }
}

static void ws_bootstrap_address_notification_cb(struct protocol_interface_info_entry *interface, const struct if_address_entry *addr, if_address_callback_t reason)
{
    /* No need for LL address registration */
    if (addr->source == ADDR_SOURCE_UNKNOWN || !interface->ws_info) {
        return;
    }

    if (reason == ADDR_CALLBACK_DAD_COMPLETE) {
        //If address is generated manually we need to force registration
        if (addr->source != ADDR_SOURCE_DHCP) {
            //Trigger Address Registration only when Bootstrap is ready
            if (interface->nwk_bootstrap_state == ER_BOOTSRAP_DONE) {
                tr_debug("Address registration %s", trace_ipv6(addr->address));
                ws_bootstrap_6lr_address_registration_update(interface, addr->address);
            }
            ws_address_reregister_trig(interface);
        }
        if (addr_ipv6_scope(addr->address, interface) > IPV6_SCOPE_LINK_LOCAL) {
            // at least ula address available inside mesh.
            interface->global_address_available = true;
        }
    } else if (reason == ADDR_CALLBACK_DELETED) {
        // What to do?
        // Go through address list and check if there is global address still available
        if (addr->source == ADDR_SOURCE_DHCP) {
            //Deprecate dhcpv address
            uint8_t address[16];
            memcpy(address, addr->address, 16);
            dhcp_client_global_address_delete(interface->id, NULL, address);
        }
        //Discover prefix policy
        addr_policy_remove_by_label(WS_NON_PREFFRED_LABEL);

        interface->global_address_available = false;
        ns_list_foreach(if_address_entry_t, addr_str, &interface->ip_addresses) {
            if (addr_ipv6_scope(addr_str->address, interface) > IPV6_SCOPE_LINK_LOCAL) {
                // at least ula address available inside mesh.
                interface->global_address_available = true;
                break;
            }
        }
    }

    // Addressing in Wi-SUN interface was changed for Border router send new event so Application can update the state
    if (interface->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER &&
            interface->nwk_bootstrap_state == ER_BOOTSRAP_DONE) {
        if (interface->bootsrap_state_machine_cnt == 0) {
            interface->bootsrap_state_machine_cnt = 10; //Re trigger state check
        }
    }
}
#ifdef HAVE_WS_VERSION_1_1

static ws_pcap_ie_t ws_neighbour_phy_cap_list_compare(ws_phy_cap_info_t *prefered_mode, ws_phy_cap_info_t *neighbour_cap_list)
{
    ws_pcap_ie_t pref_setup;

    ws_pcap_ie_t *prefered_setup = prefered_mode->pcap;
    int length_of_list = prefered_mode->length_of_list;
    while (length_of_list) {
        for (int i = 0; i < neighbour_cap_list->length_of_list; i++) {
            //Check first phy type is matching
            if (neighbour_cap_list->pcap[i].phy_type != prefered_setup->phy_type) {
                continue;
            }
            //Validate supported
            if (neighbour_cap_list->pcap[i].operating_mode & prefered_setup->operating_mode) {

                //Take only matched opeating modes
                pref_setup.operating_mode = neighbour_cap_list->pcap[i].operating_mode & prefered_setup->operating_mode;
                pref_setup.phy_type = prefered_setup->phy_type;
                return pref_setup;
            }
            break;
        }
        prefered_setup++;
        length_of_list--;
    }
    //Mark zero operating modes
    pref_setup.operating_mode = 0;
    return pref_setup;
}

static void ws_neighbour_mdr_mode_analyze(struct protocol_interface_info_entry *interface)
{
    if (!ws_version_1_1(interface)) {
        return;
    }

    if (!interface->ws_info->uptime || (interface->ws_info->uptime % 10)) {
        return;
    }

    if (!interface->ws_info->phy_cap_info.length_of_list) {
        //No Preferred Cap modes
        return;
    }

    ns_list_foreach_safe(mac_neighbor_table_entry_t, cur, &mac_neighbor_info(interface)->neighbour_list) {

        ws_neighbor_class_entry_t *ws_neighbor = ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, cur->index);

        if (!ws_neighbor || ws_neighbor->phy_mode_id || !ws_neighbor->pcap_info.length_of_list) {
            continue;
        }

        ws_pcap_ie_t preferred = ws_neighbour_phy_cap_list_compare(&interface->ws_info->phy_cap_info, ws_neighbour_cap_pointer(ws_neighbor));
        uint8_t phy_mode_id = ws_ie_lib_phy_mode_id_get_from_phy_cap(&preferred);
        if (ws_neighbor->phy_mode_id != phy_mode_id) {
            tr_debug("Updated Neigh %u MDR phy mode id %u -> %u", cur->index, ws_neighbor->phy_mode_id, phy_mode_id);
            ws_neighbor->phy_mode_id = phy_mode_id;
        }

    }
}
#else
#define ws_neighbour_mdr_mode_analyze(interface) ((void)0)
#endif


void ws_bootstrap_configure_max_retries(protocol_interface_info_entry_t *cur, uint8_t max_mac_retries)
{
    mac_helper_mac_mlme_max_retry_set(cur->id, max_mac_retries);
}

void ws_bootstrap_configure_csma_ca_backoffs(protocol_interface_info_entry_t *cur, uint8_t max_backoffs, uint8_t min_be, uint8_t max_be)
{
    mac_helper_mac_mlme_max_csma_backoffs_set(cur->id, max_backoffs);
    mac_helper_mac_mlme_be_set(cur->id, min_be, max_be);
}

void ws_bootstrap_configure_data_request_restart(protocol_interface_info_entry_t *cur, uint8_t cca_failure_restart_max, uint8_t tx_failure_restart_max, uint16_t blacklist_min_ms, uint16_t blacklist_max_ms)
{
    mlme_request_restart_config_t request_restart_config;
    request_restart_config.cca_failure_restart_max = cca_failure_restart_max;
    request_restart_config.tx_failure_restart_max = tx_failure_restart_max;
    request_restart_config.blacklist_min_ms = blacklist_min_ms;
    request_restart_config.blacklist_max_ms = blacklist_max_ms;
    mac_helper_mac_mlme_data_request_restart_set(cur->id, &request_restart_config);
}

static int ws_bootstrap_tasklet_init(protocol_interface_info_entry_t *cur)
{
    if (cur->bootStrapId < 0) {
        cur->bootStrapId = eventOS_event_handler_create(&ws_bootstrap_event_handler, WS_INIT_EVENT);
        tr_info("WS tasklet init");
    }

    if (cur->bootStrapId < 0) {
        tr_error("tasklet init failed");
        return -1;
    }


    return 0;
}

static void ws_nwk_event_post(protocol_interface_info_entry_t *cur, arm_nwk_interface_status_type_e posted_event)
{
    arm_event_s event = {
        .receiver = cur->net_start_tasklet,
        .sender = protocol_read_tasklet_id(), /**< Event sender Tasklet ID */
        .event_type = ARM_LIB_NWK_INTERFACE_EVENT,
        .event_data = posted_event,
        .event_id = (int8_t) cur->id,
        .data_ptr = NULL,
        .priority = ARM_LIB_LOW_PRIORITY_EVENT,
    };
    if (eventOS_event_send(&event) != 0) {
        tr_error("nwk_net_event_post(): event send failed");
    }
}

static int8_t ws_bootsrap_event_trig(ws_bootsrap_event_type_e event_type, int8_t interface_id, arm_library_event_priority_e priority, void *event_data)
{
    arm_event_s event = {
        .receiver = interface_id,
        .sender = 0,
        .event_type = event_type,
        .priority = priority,
        .data_ptr = event_data,
    };
    return eventOS_event_send(&event);
}

void ws_nud_table_reset(protocol_interface_info_entry_t *cur)
{
    //Empty active list
    ns_list_foreach_safe(ws_nud_table_entry_t, entry, &cur->ws_info->active_nud_process) {
        ns_list_remove(&cur->ws_info->active_nud_process, entry);
    }

    //Empty free list
    ns_list_foreach_safe(ws_nud_table_entry_t, entry, &cur->ws_info->free_nud_entries) {
        ns_list_remove(&cur->ws_info->free_nud_entries, entry);
    }
    //Add to free list to full
    for (int i = 0; i < ACTIVE_NUD_PROCESS_MAX; i++) {
        ns_list_add_to_end(&cur->ws_info->free_nud_entries, &cur->ws_info->nud_table_entrys[i]);
    }
}

static ws_nud_table_entry_t *ws_nud_entry_get_free(protocol_interface_info_entry_t *cur)
{
    ws_nud_table_entry_t *entry = ns_list_get_first(&cur->ws_info->free_nud_entries);
    if (entry) {
        entry->wait_response = false;
        entry->retry_count = 0;
        entry->nud_process = false;
        entry->timer = randLIB_get_random_in_range(1, 900);
        entry->neighbor_info = NULL;
        ns_list_remove(&cur->ws_info->free_nud_entries, entry);
        ns_list_add_to_end(&cur->ws_info->active_nud_process, entry);
    }
    return entry;
}

void ws_nud_entry_remove_active(protocol_interface_info_entry_t *cur, void *neighbor)
{
    ws_nud_table_entry_t *entry = ws_nud_entry_discover(cur, neighbor);

    if (entry) {
        mac_neighbor_table_entry_t *mac_neighbor = neighbor;
        ns_list_remove(&cur->ws_info->active_nud_process, entry);
        ns_list_add_to_end(&cur->ws_info->free_nud_entries, entry);
        if (mac_neighbor->nud_active) {
            mac_neighbor_table_neighbor_refresh(mac_neighbor_info(cur), mac_neighbor, mac_neighbor->link_lifetime);
        }

        mac_neighbor_table_neighbor_connected(mac_neighbor_info(cur), mac_neighbor);
    }
}

static ws_nud_table_entry_t *ws_nud_entry_discover(protocol_interface_info_entry_t *cur, void *neighbor)
{
    ns_list_foreach(ws_nud_table_entry_t, entry, &cur->ws_info->active_nud_process) {
        if (entry->neighbor_info == neighbor) {
            return entry;
        }
    }
    return NULL;
}

static void ws_nud_state_clean(protocol_interface_info_entry_t *cur, ws_nud_table_entry_t *entry)
{
    mac_neighbor_table_entry_t *neighbor = entry->neighbor_info;
    ns_list_remove(&cur->ws_info->active_nud_process, entry);
    ns_list_add_to_end(&cur->ws_info->free_nud_entries, entry);
    if (neighbor->nud_active) {
        neighbor->nud_active = false;
        mac_neighbor_info(cur)->active_nud_process--;
    }
}

static void ws_nud_entry_remove(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *entry_ptr)
{
    ws_nud_table_entry_t *nud_entry = ws_nud_entry_discover(cur, entry_ptr);
    if (nud_entry) {
        ws_nud_state_clean(cur, nud_entry);
    }
}

if_address_entry_t *ws_probe_aro_address(protocol_interface_info_entry_t *interface)
{
    if (interface->global_address_available) {
        ns_list_foreach(if_address_entry_t, address, &interface->ip_addresses) {
            if (addr_ipv6_scope(address->address, interface) > IPV6_SCOPE_LINK_LOCAL) {
                return address;
            }
        }
    }
    return NULL;
}

static bool ws_nud_message_build(protocol_interface_info_entry_t *cur, mac_neighbor_table_entry_t *neighbor, bool nud_process)
{
    //Send NS
    uint8_t ll_target[16];
    aro_t aro_temp;
    //SET ARO and src address pointer to NULL by default
    aro_t *aro_ptr = NULL;
    uint8_t *src_address_ptr = NULL;

    ws_common_create_ll_address(ll_target, neighbor->mac64);
    if (nud_process) {
        tr_info("NUD generate NS %u", neighbor->index);
    } else {
        tr_info("Probe generate NS %u", neighbor->index);
        if_address_entry_t *gp_address = ws_probe_aro_address(cur);
        if (gp_address) {
            src_address_ptr = gp_address->address;
            aro_temp.status = ARO_SUCCESS;
            aro_temp.present = true;
            memcpy(aro_temp.eui64, cur->mac, 8);
            //Just Short Test
            aro_temp.lifetime = 1;
            aro_ptr = &aro_temp;
        }
    }
    buffer_t *buffer = icmpv6_build_ns(cur, ll_target, src_address_ptr, true, false, aro_ptr);
    if (buffer) {
        buffer->options.traffic_class = IP_DSCP_CS6 << IP_TCLASS_DSCP_SHIFT;
        protocol_push(buffer);
        return true;
    }
    return false;
}

void ws_nud_active_timer(protocol_interface_info_entry_t *cur, uint16_t ticks)
{
    //Convert TICKS to real milliseconds
    if (ticks > 0xffff / 100) {
        ticks = 0xffff;
    } else if (ticks == 0) {
        ticks = 1;
    } else {
        ticks *= 100;
    }

    ns_list_foreach_safe(ws_nud_table_entry_t, entry, &cur->ws_info->active_nud_process) {
        if (entry->timer <= ticks) {
            //TX Process or timeout
            if (entry->wait_response) {
                //Timeout for NUD or Probe
                if (entry->nud_process) {
                    tr_debug("NUD NA timeout");
                    if (entry->retry_count < 2) {
                        entry->timer = randLIB_get_random_in_range(1, 900);
                        entry->wait_response = false;
                    } else {
                        //Clear entry from active list
                        ws_nud_state_clean(cur, entry);
                        //Remove whole entry
                        mac_neighbor_table_neighbor_remove(mac_neighbor_info(cur), entry->neighbor_info);
                    }
                } else {
                    ws_nud_state_clean(cur, entry);
                }

            } else {
                //Random TX wait period is over
                entry->wait_response = ws_nud_message_build(cur, entry->neighbor_info, entry->nud_process);
                if (!entry->wait_response) {
                    if (entry->nud_process && entry->retry_count < 2) {
                        entry->timer = randLIB_get_random_in_range(1, 900);
                    } else {
                        //Clear entry from active list
                        //Remove and try again later on
                        ws_nud_state_clean(cur, entry);
                    }
                } else {
                    entry->retry_count++;
                    entry->timer = 5001;
                }
            }
        } else {
            entry->timer -= ticks;
        }
    }
}

static fhss_ws_neighbor_timing_info_t *ws_bootstrap_get_neighbor_info(const fhss_api_t *api, uint8_t eui64[8])
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_fhss_api(api);
    if (!cur || !cur->mac_parameters || !mac_neighbor_info(cur)) {
        return NULL;
    }
    mac_neighbor_table_entry_t *mac_neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(cur), eui64, MAC_ADDR_MODE_64_BIT);
    if (mac_neighbor) {
        ws_neighbor_class_entry_t *ws_neighbor =  ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, mac_neighbor->index);
        if (!ws_neighbor) {
            return NULL;
        }
        return &ws_neighbor->fhss_data;
    }
    //Discover temporary entry
    ws_neighbor_temp_class_t *temp_entry = ws_llc_get_eapol_temp_entry(cur, eui64);
    if (!temp_entry) {
        return NULL;
    }
    return &temp_entry->neigh_info_list.fhss_data;
}

void ws_bootstrap_llc_hopping_update(struct protocol_interface_info_entry *cur, const fhss_ws_configuration_t *fhss_configuration)
{
    cur->ws_info->hopping_schdule.uc_fixed_channel = fhss_configuration->unicast_fixed_channel;
    cur->ws_info->hopping_schdule.bc_fixed_channel = fhss_configuration->broadcast_fixed_channel;
    // Read UC channel function from WS info because FHSS might be temporarily configured to fixed channel during discovery.
    cur->ws_info->hopping_schdule.uc_channel_function = cur->ws_info->cfg->fhss.fhss_uc_channel_function;
    cur->ws_info->hopping_schdule.bc_channel_function = fhss_configuration->ws_bc_channel_function;
    cur->ws_info->hopping_schdule.fhss_bc_dwell_interval = fhss_configuration->fhss_bc_dwell_interval;
    cur->ws_info->hopping_schdule.fhss_broadcast_interval = fhss_configuration->fhss_broadcast_interval;
    cur->ws_info->hopping_schdule.fhss_uc_dwell_interval = fhss_configuration->fhss_uc_dwell_interval;
    cur->ws_info->hopping_schdule.fhss_bsi = fhss_configuration->bsi;
}

static uint8_t ws_bootstrap_generate_exluded_channel_list_from_active_channels(ws_excluded_channel_data_t *excluded_data, const uint32_t *selected_channel_mask, const uint32_t *global_channel_mask, uint16_t number_of_channels)
{
    bool active_range = false;

    //Clear Old Data
    memset(excluded_data, 0, sizeof(ws_excluded_channel_data_t));

    for (uint8_t i = 0; i < number_of_channels; i++) {
        if (!(global_channel_mask[i / 32] & (1U << (i % 32)))) {
            //Global exluded channel
            if (active_range) {
                //Mark range stop here
                active_range = false;
            }
            continue;
        }

        if (selected_channel_mask[i / 32] & (1U << (i % 32))) {
            if (active_range) {
                //Mark range stop here
                active_range = false;
            }
        } else {
            //Mark excluded channel
            //Swap Order already here
            excluded_data->channel_mask[i / 32] |= 1U << (31 - (i % 32));
            excluded_data->excluded_channel_count++;

            if (excluded_data->excluded_range_length < WS_EXCLUDED_MAX_RANGE_TO_SEND) {
                if (!active_range) {
                    excluded_data->excluded_range_length++;
                    active_range = true;
                    //Set start channel
                    excluded_data->exluded_range[excluded_data->excluded_range_length - 1].range_start = i;
                } else {
                    excluded_data->exluded_range[excluded_data->excluded_range_length - 1].range_end = i;
                }
            }
        }
    }

    excluded_data->channel_mask_bytes_inline = ((number_of_channels + 7) / 8);

    uint8_t channel_plan = 0;
    if (excluded_data->excluded_range_length == 0) {
        excluded_data->excuded_channel_ctrl = WS_EXC_CHAN_CTRL_NONE;
    } else if (excluded_data->excluded_range_length <= WS_EXCLUDED_MAX_RANGE_TO_SEND) {

        uint8_t range_length = (excluded_data->excluded_range_length * 4) + 3;
        if (range_length <= ((number_of_channels + 7) / 8) + 6) {
            excluded_data->excuded_channel_ctrl = WS_EXC_CHAN_CTRL_RANGE;
        } else {
            excluded_data->excuded_channel_ctrl = WS_EXC_CHAN_CTRL_BITMASK;
            channel_plan = 1;
        }
    } else {
        excluded_data->excuded_channel_ctrl = WS_EXC_CHAN_CTRL_BITMASK;
        channel_plan = 1;
    }
    tr_debug("Excluded ctrl %u, exluded channel count %u, total domain channels %u", excluded_data->excuded_channel_ctrl, excluded_data->excluded_channel_count, number_of_channels);
    return channel_plan;
}

void ws_bootstrap_fhss_configure_channel_masks(protocol_interface_info_entry_t *cur, fhss_ws_configuration_t *fhss_configuration)
{
    fhss_configuration->channel_mask_size = cur->ws_info->hopping_schdule.number_of_channels;
    ws_common_generate_channel_list(fhss_configuration->channel_mask, cur->ws_info->hopping_schdule.number_of_channels, cur->ws_info->hopping_schdule.regulatory_domain, cur->ws_info->hopping_schdule.operating_class, cur->ws_info->hopping_schdule.channel_plan_id);
    ws_common_generate_channel_list(fhss_configuration->unicast_channel_mask, cur->ws_info->hopping_schdule.number_of_channels, cur->ws_info->hopping_schdule.regulatory_domain, cur->ws_info->hopping_schdule.operating_class, cur->ws_info->hopping_schdule.channel_plan_id);
    // using bitwise AND operation for user set channel mask to remove channels not allowed in this device
    for (uint8_t n = 0; n < 8; n++) {
        fhss_configuration->unicast_channel_mask[n] &= cur->ws_info->cfg->fhss.fhss_channel_mask[n];
    }
    //Update Exluded channels
    cur->ws_info->hopping_schdule.channel_plan = ws_bootstrap_generate_exluded_channel_list_from_active_channels(&cur->ws_info->hopping_schdule.excluded_channels, fhss_configuration->unicast_channel_mask, fhss_configuration->channel_mask, cur->ws_info->hopping_schdule.number_of_channels);
}

int8_t ws_bootstrap_fhss_initialize(protocol_interface_info_entry_t *cur)
{
    fhss_api_t *fhss_api = ns_sw_mac_get_fhss_api(cur->mac_api);
    fhss_ws_configuration_t fhss_configuration;
    memset(&fhss_configuration, 0, sizeof(fhss_ws_configuration_t));
    if (!fhss_api) {
        // When FHSS doesn't exist yet, create one
        ws_bootstrap_fhss_configure_channel_masks(cur, &fhss_configuration);
        ws_bootstrap_fhss_set_defaults(cur, &fhss_configuration);
        fhss_api = ns_fhss_ws_create(&fhss_configuration, cur->ws_info->fhss_timer_ptr);

        if (!fhss_api) {
            return -1;
        }
        ns_sw_mac_fhss_register(cur->mac_api, fhss_api);
        // Allow transmitting unicast frames only on TX slots in normal and expedited forwarding mode
        ns_fhss_ws_set_tx_allowance_level(fhss_api, WS_TX_SLOT, WS_TX_SLOT);
    } else {
        return -1;
    }

    return 0;
}

int8_t ws_bootstrap_fhss_set_defaults(protocol_interface_info_entry_t *cur, fhss_ws_configuration_t *fhss_configuration)
{
    fhss_configuration->fhss_uc_dwell_interval = cur->ws_info->cfg->fhss.fhss_uc_dwell_interval;
    fhss_configuration->ws_uc_channel_function = (fhss_ws_channel_functions)cur->ws_info->cfg->fhss.fhss_uc_channel_function;
    fhss_configuration->ws_bc_channel_function = (fhss_ws_channel_functions)cur->ws_info->cfg->fhss.fhss_bc_channel_function;
    fhss_configuration->fhss_bc_dwell_interval = cur->ws_info->cfg->fhss.fhss_bc_dwell_interval;
    fhss_configuration->fhss_broadcast_interval = cur->ws_info->cfg->fhss.fhss_bc_interval;
    if (cur->ws_info->cfg->fhss.fhss_uc_fixed_channel != 0xffff) {
        fhss_configuration->unicast_fixed_channel = cur->ws_info->cfg->fhss.fhss_uc_fixed_channel;
    }
    fhss_configuration->broadcast_fixed_channel = cur->ws_info->cfg->fhss.fhss_bc_fixed_channel;
    return 0;
}

static bool ws_bootstrap_channel_allowed(uint8_t channel, uint32_t *channel_mask)
{
    if ((1U << (channel % 32)) & (channel_mask[channel / 32])) {
        return true;
    }
    return false;
}

uint16_t ws_bootstrap_randomize_fixed_channel(uint16_t configured_fixed_channel, uint8_t number_of_channels, uint32_t *channel_mask)
{
    if (configured_fixed_channel == 0xFFFF) {
        uint16_t random_channel = randLIB_get_random_in_range(0, number_of_channels - 1);
        while (ws_bootstrap_channel_allowed(random_channel, channel_mask) == false) {
            random_channel = randLIB_get_random_in_range(0, number_of_channels - 1);
        }
        return random_channel;
    } else {
        return configured_fixed_channel;
    }
}

static int8_t ws_bootstrap_fhss_enable(protocol_interface_info_entry_t *cur)
{
    fhss_ws_configuration_t fhss_configuration = ws_common_get_current_fhss_configuration(cur);

    // Set the LLC information to follow the actual fhss settings
    ws_bootstrap_llc_hopping_update(cur, &fhss_configuration);

    // Set neighbor info callback
    if (ns_fhss_set_neighbor_info_fp(cur->ws_info->fhss_api, &ws_bootstrap_get_neighbor_info)) {
        return -1;
    }

    return 0;
}

/* Sets the parent and broadcast schedule we are following
 *
 */
void ws_bootstrap_primary_parent_set(struct protocol_interface_info_entry *cur, llc_neighbour_req_t *neighbor_info, ws_parent_synch_e synch_req)
{
    if (!neighbor_info->ws_neighbor->broadcast_timing_info_stored) {
        tr_error("No BC timing info for set new parent");
        return;
    }

    fhss_ws_configuration_t fhss_configuration = ws_common_get_current_fhss_configuration(cur);

    // Learning broadcast network configuration
    if (neighbor_info->ws_neighbor->broadcast_shedule_info_stored) {
        if (synch_req != WS_EAPOL_PARENT_SYNCH) {
            ws_bootstrap_fhss_set_defaults(cur, &fhss_configuration);
        }
        fhss_configuration.ws_bc_channel_function = (fhss_ws_channel_functions)neighbor_info->ws_neighbor->fhss_data.bc_timing_info.broadcast_channel_function;
        if (fhss_configuration.ws_bc_channel_function == WS_FIXED_CHANNEL) {
            cur->ws_info->hopping_schdule.bc_fixed_channel = neighbor_info->ws_neighbor->fhss_data.bc_timing_info.fixed_channel;
            cur->ws_info->cfg->fhss.fhss_bc_fixed_channel = neighbor_info->ws_neighbor->fhss_data.bc_timing_info.fixed_channel;
        }
        fhss_configuration.bsi = neighbor_info->ws_neighbor->fhss_data.bc_timing_info.broadcast_schedule_id;
        fhss_configuration.fhss_bc_dwell_interval = neighbor_info->ws_neighbor->fhss_data.bc_timing_info.broadcast_dwell_interval;
        fhss_configuration.fhss_broadcast_interval = neighbor_info->ws_neighbor->fhss_data.bc_timing_info.broadcast_interval;
        fhss_configuration.broadcast_fixed_channel = cur->ws_info->cfg->fhss.fhss_bc_fixed_channel;
        neighbor_info->ws_neighbor->synch_done = true;
    }

    ns_fhss_ws_configuration_set(cur->ws_info->fhss_api, &fhss_configuration);

    // We have broadcast schedule set up set the broadcast parent schedule
    ns_fhss_ws_set_parent(cur->ws_info->fhss_api, neighbor_info->neighbor->mac64, &neighbor_info->ws_neighbor->fhss_data.bc_timing_info, synch_req != WS_PARENT_SOFT_SYNCH);

    // Update LLC to follow updated fhss settings
    ws_bootstrap_llc_hopping_update(cur, &fhss_configuration);
}

void ws_bootstrap_eapol_parent_synch(struct protocol_interface_info_entry *cur, llc_neighbour_req_t *neighbor_info)
{
    if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER || cur->ws_info->configuration_learned || !neighbor_info->ws_neighbor->broadcast_shedule_info_stored || !neighbor_info->ws_neighbor->broadcast_timing_info_stored) {
        return;
    }

    if (ws_bootstrap_candidate_parent_get(cur, neighbor_info->neighbor->mac64, false) == NULL) {
        return;
    }

    //Store Brodacst Shedule
    if (!neighbor_info->ws_neighbor->synch_done) {
        ws_bootstrap_primary_parent_set(cur, neighbor_info, WS_EAPOL_PARENT_SYNCH);
    } else {
        ns_fhss_ws_set_parent(cur->ws_info->fhss_api, neighbor_info->neighbor->mac64, &neighbor_info->ws_neighbor->fhss_data.bc_timing_info, false);
    }
}

void ws_bootstrap_ll_address_validate(struct protocol_interface_info_entry *cur)
{
    // Configure EUI64 for MAC if missing
    uint8_t mac64[8];
    if (!cur->mac_api) {
        return;
    }

    cur->mac_api->mac64_get(cur->mac_api, MAC_EXTENDED_DYNAMIC, mac64);

    if (memcmp(mac64, ADDR_UNSPECIFIED, 8) == 0) {
        cur->mac_api->mac64_get(cur->mac_api, MAC_EXTENDED_READ_ONLY, mac64);
    }

    if (memcmp(mac64, ADDR_UNSPECIFIED, 8) == 0) {
        // Generate random mac because it was not available
        randLIB_get_n_bytes_random(mac64, 8);
        mac64[0] |= 2; //Set Local Bit
        mac64[0] &= ~1; //Clear multicast bit

        tr_info("Generated random MAC address");
    }
    tr_info("MAC address: %s", trace_array(mac64, 8));
    mac_helper_mac64_set(cur, mac64);

    memcpy(cur->iid_eui64, mac64, 8);
    /* Invert U/L Bit */
    cur->iid_eui64[0] ^= 2;
    memcpy(cur->iid_slaac, cur->iid_eui64, 8);

}

/* \return 0x0100 to 0xFFFF ETX value (8 bit fraction)
 * \return 0xFFFF address not associated
 * \return 0x0000 address unknown or other error
 * \return 0x0001 no ETX statistics on this interface
 */
uint16_t ws_local_etx_read(protocol_interface_info_entry_t *interface, addrtype_t addr_type, const uint8_t *mac_adddress)
{
    uint16_t etx;

    if (!mac_adddress || !interface) {
        return 0;
    }

    uint8_t attribute_index;

    mac_neighbor_table_entry_t *mac_neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(interface), mac_adddress, addr_type);
    if (!mac_neighbor) {
        return 0xffff;
    }
    attribute_index = mac_neighbor->index;
    ws_neighbor_class_entry_t *ws_neighbour = ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, attribute_index);
    etx_storage_t *etx_entry = etx_storage_entry_get(interface->id, attribute_index);

    if (!ws_neighbour || !etx_entry) {
        return 0xffff;
    }

    etx = etx_local_etx_read(interface->id, attribute_index);

    // if we have a measurement ready then we will check the RSL validity
    if (etx != 0xffff && !ws_neighbour->candidate_parent) {
        // RSL value measured is lower than acceptable ETX will be given as MAX
        return WS_ETX_MAX << 1; // We use 8 bit fraction and ETX is usually 7 bit fraction
    }

    // If we dont have valid ETX for children we assume good ETX.
    // After enough packets is sent to children real calculated ETX is given.
    // This might result in ICMP source route errors returned to Border router causing secondary route uses
    if (etx == 0xffff && ipv6_neighbour_has_registered_by_eui64(&interface->ipv6_neighbour_cache, mac_neighbor->mac64)) {
        return 0x100;
    }

    return etx;
}

uint16_t ws_etx_read(protocol_interface_info_entry_t *interface, addrtype_t addr_type, const uint8_t *addr_ptr)
{
    if (!addr_ptr || !interface) {
        return 0;
    }
    return ws_local_etx_read(interface, addr_type, addr_ptr + PAN_ID_LEN);
}

bool ws_bootstrap_nd_ns_transmit(protocol_interface_info_entry_t *cur, ipv6_neighbour_t *entry,  bool unicast, uint8_t seq)
{
    (void)cur;
    (void)seq;

    if (unicast) {
        // Unicast NS is OK
        return false;
    }
    // Fail the resolution
    tr_warn("Link address lost for %s", trace_ipv6(entry->ip_address));
    ipv6_neighbour_entry_remove(&cur->ipv6_neighbour_cache, entry);
    // True means we skip the message sending
    return true;
}
void ws_bootstrap_memory_configuration()
{
    /* Configure memory limits for garbage collection based on total memory size
     * Starting from these values
     *      5% for High mark
     *      2% for critical mark
     *      1% for Routing limit
     * Memory     High               Critical            Drop routing
     * 32K RAM    3200 bytes         1280 Bytes          1024 bytes
     * 64K RAM    3200 bytes         1280 Bytes          1024 bytes
     * 128K RAM   6400 bytes         2560 Bytes          1280 bytes
     * 320K RAM   16000 byte         6400 Bytes          3200 bytes
     * 640K RAM   32000 byte         12800 Bytes         6400 bytes
     * 1000K RAM  50000 bytes        20000 Bytes         10000 bytes
     * 4000K RAM  120000 bytes       40000 Bytes         10000 bytes
     * */
    // In small memory devices there needs to lower limit so that there some change to be usable
    // and there is no use for having very large values on high memory devices
    ns_monitor_packet_ingress_rate_limit_by_memory(1024, 10000, 1);

    ns_monitor_heap_gc_threshold_set(3200, 120000, 95, 1280, 40000, 98);
    return;
}

void ws_bootstrap_configuration_reset(protocol_interface_info_entry_t *cur)
{
    // Configure IP stack to operate as Wi-SUN node

    // Do not process beacons
    cur->mac_parameters->beacon_ind = NULL;
    cur->mac_parameters->mac_security_level = 0;

    // Set default parameters to interface
    cur->configure_flags = INTERFACE_BOOTSTRAP_DEFINED;
    cur->configure_flags |= INTERFACE_SECURITY_DEFINED;
    cur->lowpan_info = 0;

    switch (cur->bootsrap_mode) {
        //        case NET_6LOWPAN_SLEEPY_HOST:
        case ARM_NWK_BOOTSRAP_MODE_6LoWPAN_HOST:
            break;

        case ARM_NWK_BOOTSRAP_MODE_6LoWPAN_ROUTER:
        case ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER:
            cur->lowpan_info |= INTERFACE_NWK_ROUTER_DEVICE;
            break;

        default:
            tr_err("Invalid bootstrap_mode");
    }

    cur->nwk_bootstrap_state = ER_ACTIVE_SCAN;
    cur->ws_info->network_pan_id = 0xffff;
    ws_bootstrap_asynch_trickle_stop(cur);

    //cur->mac_security_key_usage_update_cb = ws_management_mac_security_key_update_cb;
    return;
}

bool ws_bootstrap_network_name_matches(const struct mcps_data_ie_list *ie_ext, const char *network_name_ptr)
{
    ws_wp_network_name_t network_name;

    if (!network_name_ptr || !ie_ext) {
        return false;
    }

    if (!ws_wp_nested_network_name_read(ie_ext->payloadIeList, ie_ext->payloadIeListLength, &network_name)) {
        tr_warn("No network name IE");
        return false;
    }

    if (network_name.network_name_length != strlen(network_name_ptr)) {
        return false;
    }

    if (strncmp(network_name_ptr, (char *)network_name.network_name, network_name.network_name_length) != 0) {
        return false;
    }

    // names have equal length and same characters
    return true;
}

static void ws_bootstrap_decode_exclude_range_to_mask_by_range(void *mask_buffer, ws_excluded_channel_range_t *range_info, uint16_t number_of_channels)
{
    uint16_t range_start, range_stop;
    uint8_t mask_index = 0;
    //uint8_t channel_index = 0;
    uint8_t *range_ptr = range_info->range_start;
    uint32_t *mask_ptr = mask_buffer;
    while (range_info->number_of_range) {
        range_start = common_read_16_bit_inverse(range_ptr);
        range_ptr += 2;
        range_stop = common_read_16_bit_inverse(range_ptr);
        range_ptr += 2;
        range_info->number_of_range--;
        for (uint16_t channel = 0; channel < number_of_channels; channel++) {

            if (channel && (channel % 32 == 0)) {
                mask_index++;
                //channel_index = 0;
            }
            if (channel >= range_start && channel <= range_stop) {
                //mask_ptr[mask_index] |= 1U << (31 - channel_index);
                mask_ptr[channel / 32] |= 1U << (31 - (channel % 32));
            } else if (channel > range_stop) {
                break;
            }
        }
    }
}

void ws_bootstrap_candidate_parent_store(parent_info_t *parent, const struct mcps_data_ind_s *data, ws_utt_ie_t *ws_utt, ws_us_ie_t *ws_us, ws_pan_information_t *pan_information)
{
    parent->ws_utt = *ws_utt;
    // Saved from unicast IE
    parent->ws_us = *ws_us;

    //copy excluded channel here if it is inline
    if (ws_us->excluded_channel_ctrl == WS_EXC_CHAN_CTRL_RANGE) {
        memset(parent->excluded_channel_data, 0, 32);
        //Decode Range to mask here
        ws_bootstrap_decode_exclude_range_to_mask_by_range(parent->excluded_channel_data, &parent->ws_us.excluded_channels.range, 256);
        parent->ws_us.excluded_channels.mask.channel_mask = parent->excluded_channel_data;
        parent->ws_us.excluded_channels.mask.mask_len_inline = 32;
        parent->ws_us.excluded_channel_ctrl = WS_EXC_CHAN_CTRL_BITMASK;
    } else if (ws_us->excluded_channel_ctrl == WS_EXC_CHAN_CTRL_BITMASK) {
        parent->ws_us.excluded_channels.mask.channel_mask = parent->excluded_channel_data;
        memcpy(parent->excluded_channel_data, ws_us->excluded_channels.mask.channel_mask, ws_us->excluded_channels.mask.mask_len_inline);
    }

    // Saved from Pan information, do not overwrite pan_version as it is not valid here
    parent->pan_information.pan_size = pan_information->pan_size;
    parent->pan_information.routing_cost = pan_information->routing_cost;
    parent->pan_information.use_parent_bs = pan_information->use_parent_bs;
    parent->pan_information.rpl_routing_method = pan_information->rpl_routing_method;
    parent->pan_information.version = pan_information->version;

    // Saved from message
    parent->timestamp = data->timestamp;
    parent->pan_id = data->SrcPANId;
    parent->link_quality = data->mpduLinkQuality;
    parent->signal_dbm = data->signal_dbm;
    memcpy(parent->addr, data->SrcAddr, 8);

    if (ws_neighbor_class_rsl_from_dbm_calculate(parent->signal_dbm) > (DEVICE_MIN_SENS + CAND_PARENT_THRESHOLD + CAND_PARENT_HYSTERISIS)) {
        parent->link_acceptable = true;
    }
    if (ws_neighbor_class_rsl_from_dbm_calculate(parent->signal_dbm) < (DEVICE_MIN_SENS + CAND_PARENT_THRESHOLD - CAND_PARENT_HYSTERISIS)) {
        parent->link_acceptable = false;
    }
    parent->age = protocol_core_monotonic_time;
}

parent_info_t *ws_bootstrap_candidate_parent_get_best(protocol_interface_info_entry_t *cur)
{
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_reserved) {
        tr_info("candidate list a:%s panid:%x cost:%d size:%d rssi:%d txFailure:%u age:%"PRIu32, trace_array(entry->addr, 8), entry->pan_id, entry->pan_information.routing_cost, entry->pan_information.pan_size, entry->signal_dbm, entry->tx_fail, protocol_core_monotonic_time - entry->age);
    }

    return ns_list_get_first(&cur->ws_info->parent_list_reserved);
}

void ws_bootstrap_candidate_table_reset(protocol_interface_info_entry_t *cur)
{
    //Empty active list
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_free) {
        ns_list_remove(&cur->ws_info->parent_list_free, entry);
    }

    //Empty free list
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_reserved) {
        ns_list_remove(&cur->ws_info->parent_list_reserved, entry);
    }
    //Add to free list to full
    for (int i = 0; i < WS_PARENT_LIST_SIZE; i++) {
        ns_list_add_to_end(&cur->ws_info->parent_list_free, &cur->ws_info->parent_info[i]);
    }
}

static parent_info_t *ws_bootstrap_candidate_parent_allocate(protocol_interface_info_entry_t *cur, const uint8_t *addr)
{
    parent_info_t *entry = ns_list_get_first(&cur->ws_info->parent_list_free);
    if (entry) {
        memcpy(entry->addr, addr, 8);
        ns_list_remove(&cur->ws_info->parent_list_free, entry);
        ns_list_add_to_end(&cur->ws_info->parent_list_reserved, entry);
    } else {
        // If there is no free entries always allocate the last one of reserved as it is the worst
        entry = ns_list_get_last(&cur->ws_info->parent_list_reserved);

    }
    if (entry) {
        entry->tx_fail = 0;
        entry->link_acceptable = false;
    }
    return entry;
}

parent_info_t *ws_bootstrap_candidate_parent_get(struct protocol_interface_info_entry *cur, const uint8_t *addr, bool create)
{
    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_reserved) {
        if (memcmp(entry->addr, addr, 8) == 0) {
            return entry;
        }
    }
    if (create) {
        return ws_bootstrap_candidate_parent_allocate(cur, addr);
    }
    return NULL;
}

static void ws_bootstrap_candidate_parent_mark_failure(protocol_interface_info_entry_t *cur, const uint8_t *addr)
{
    parent_info_t *entry = ws_bootstrap_candidate_parent_get(cur, addr, false);
    if (entry) {
        if (entry->tx_fail >= 2) {
            ns_list_remove(&cur->ws_info->parent_list_reserved, entry);
            ns_list_add_to_end(&cur->ws_info->parent_list_free, entry);
        } else {
            entry->tx_fail++;
            ws_bootstrap_candidate_parent_sort(cur, entry);
        }

    }
}

static bool ws_bootstrap_candidate_parent_compare(parent_info_t *p1, parent_info_t *p2)
{
    // Return true if P2 is better
    // signal lower than threshold for both
    // pan_cost
    // signal quality

    if (p2->tx_fail > p1->tx_fail) {
        return false;
    }

    if (p2->tx_fail < p1->tx_fail) {
        return true;
    }

    if (p1->link_acceptable && !p2->link_acceptable) {
        // Link acceptable is always better than not
        return true;
    }
    if (!p1->link_acceptable && p2->link_acceptable) {
        // Link acceptable is always better than not
        return false;
    }

    // Select the lowest PAN cost
    uint16_t p1_pan_cost = (p1->pan_information.routing_cost / PRC_WEIGHT_FACTOR) + (p1->pan_information.pan_size / PS_WEIGHT_FACTOR);
    uint16_t p2_pan_cost = (p2->pan_information.routing_cost / PRC_WEIGHT_FACTOR) + (p2->pan_information.pan_size / PS_WEIGHT_FACTOR);
    if (p1_pan_cost > p2_pan_cost) {
        return true;
    } else if (p1_pan_cost < p2_pan_cost) {
        return false;
    }

    // If pan cost is the same then we select the one we hear highest
    if (p1->signal_dbm < p2->signal_dbm) {
        return true;
    }
    return false;
}

void ws_bootstrap_candidate_list_clean(struct protocol_interface_info_entry *cur, uint8_t pan_max, uint32_t current_time, uint16_t pan_id)
{
    int pan_count = 0;

    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_reserved) {

        if ((current_time - entry->age) > WS_PARENT_LIST_MAX_AGE) {
            ns_list_remove(&cur->ws_info->parent_list_reserved, entry);
            ns_list_add_to_end(&cur->ws_info->parent_list_free, entry);
            continue;
        }
        if (entry->pan_id == pan_id) {
            // Same panid if there is more than limited amount free those
            pan_count++;
            if (pan_count > pan_max) {
                ns_list_remove(&cur->ws_info->parent_list_reserved, entry);
                ns_list_add_to_end(&cur->ws_info->parent_list_free, entry);
                continue;
            }
        }
    }
}

void ws_bootstrap_candidate_parent_sort(struct protocol_interface_info_entry *cur, parent_info_t *new_entry)
{
    //Remove from the list

    ns_list_foreach_safe(parent_info_t, entry, &cur->ws_info->parent_list_reserved) {

        if (entry == new_entry) {
            // own entry skip it
            continue;
        }

        if (ws_bootstrap_candidate_parent_compare(entry, new_entry)) {
            // New entry is better
            //tr_debug("candidate list new is better");
            ns_list_remove(&cur->ws_info->parent_list_reserved, new_entry);
            ns_list_add_before(&cur->ws_info->parent_list_reserved, entry, new_entry);
            return;
        }
    }
    // This is the last entry
    ns_list_remove(&cur->ws_info->parent_list_reserved, new_entry);
    ns_list_add_to_end(&cur->ws_info->parent_list_reserved, new_entry);
}

static bool ws_channel_plan_zero_compare(ws_channel_plan_zero_t *rx_plan, ws_hopping_schedule_t *hopping_schdule)
{
    if (rx_plan->operation_class != hopping_schdule->operating_class) {
        return false;
    } else if (rx_plan->regulator_domain != hopping_schdule->regulatory_domain) {
        return false;
    }
    return true;
}

static bool ws_channel_plan_one_compare(ws_channel_plan_one_t *rx_plan, ws_hopping_schedule_t *hopping_schdule)
{
    uint16_t num_of_channel = hopping_schdule->number_of_channels;
    if (rx_plan->ch0 != hopping_schdule->ch0_freq) {
        return false;
    } else if (rx_plan->channel_spacing != hopping_schdule->channel_spacing) {
        return false;
    } else if (rx_plan->number_of_channel != num_of_channel) {
        return false;
    }
    return true;
}

static bool ws_channel_plan_two_compare(ws_channel_plan_two_t *rx_plan, ws_hopping_schedule_t *hopping_schdule)
{
    if (rx_plan->channel_plan_id != hopping_schdule->channel_plan_id) {
        return false;
    } else if (rx_plan->regulator_domain != hopping_schdule->regulatory_domain) {
        return false;
    }
    return true;
}

bool ws_bootstrap_validate_channel_plan(ws_us_ie_t *ws_us, struct protocol_interface_info_entry *cur)
{
    if (ws_us->channel_plan == 0) {
        if (!ws_channel_plan_zero_compare(&ws_us->plan.zero, &cur->ws_info->hopping_schdule)) {
            return false;
        }
    } else if (ws_us->channel_plan == 1) {
        if (!ws_channel_plan_one_compare(&ws_us->plan.one, &cur->ws_info->hopping_schdule)) {
            return false;
        }
    } else if (ws_us->channel_plan == 2) {
        if (!ws_version_1_1(cur)) {
            return false;
        }
        if (!ws_channel_plan_two_compare(&ws_us->plan.two, &cur->ws_info->hopping_schdule)) {
            return false;
        }
    } else {
        return false;
    }

    return true;
}

bool ws_bootstrap_validate_channel_function(ws_us_ie_t *ws_us, ws_bs_ie_t *ws_bs)
{
    if (ws_us) {
        if (ws_us->channel_function != WS_FIXED_CHANNEL &&
                ws_us->channel_function != WS_TR51CF &&
                ws_us->channel_function != WS_DH1CF) {
            return false;
        }
    }

    if (ws_bs) {
        if (ws_bs->channel_function != WS_FIXED_CHANNEL &&
                ws_bs->channel_function != WS_TR51CF &&
                ws_bs->channel_function != WS_DH1CF) {
            return false;
        }
    }

    return true;
}

uint32_t ws_time_from_last_unicast_traffic(uint32_t current_time_stamp, ws_neighbor_class_entry_t *ws_neighbor)
{
    uint32_t time_from_last_unicast_shedule = current_time_stamp;

    //Time from last RX unicast in us
    time_from_last_unicast_shedule -= ws_neighbor->fhss_data.uc_timing_info.utt_rx_timestamp;
    time_from_last_unicast_shedule /= 1000000; //Convert to seconds
    return time_from_last_unicast_shedule;
}

static void ws_bootstrap_neighbor_table_clean(struct protocol_interface_info_entry *interface)
{
    uint8_t ll_target[16];

    if (mac_neighbor_info(interface)->neighbour_list_size <= mac_neighbor_info(interface)->list_total_size - ws_common_temporary_entry_size(mac_neighbor_info(interface)->list_total_size)) {
        // Enough neighbor entries
        return;
    }
    uint32_t temp_link_min_timeout;
    if (mac_neighbor_info(interface)->neighbour_list_size == mac_neighbor_info(interface)->list_total_size) {
        temp_link_min_timeout = 1; //Accept 1 second time from last
    } else {
        temp_link_min_timeout = interface->ws_info->cfg->timing.temp_link_min_timeout;
    }

    memcpy(ll_target, ADDR_LINK_LOCAL_PREFIX, 8);

    uint32_t current_time_stamp = ns_sw_mac_read_current_timestamp(interface->mac_api);

    mac_neighbor_table_entry_t *neighbor_entry_ptr = NULL;
    ns_list_foreach_safe(mac_neighbor_table_entry_t, cur, &mac_neighbor_info(interface)->neighbour_list) {
        ws_neighbor_class_entry_t *ws_neighbor = ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, cur->index);

        if (cur->link_role == PRIORITY_PARENT_NEIGHBOUR) {
            //This is our primary parent we cannot delete
            continue;
        }

        if (cur->nud_active) {
            //If NUD process is active do not trig
            // or Negative ARO is active
            continue;
        }

        if (neighbor_entry_ptr && neighbor_entry_ptr->lifetime < cur->lifetime) {
            // We have already shorter link entry found this cannot replace it
            continue;
        }

        if (cur->link_lifetime > WS_NEIGHBOUR_TEMPORARY_ENTRY_LIFETIME && cur->link_lifetime <= WS_NEIGHBOUR_TEMPORARY_NEIGH_MAX_LIFETIME) {
            //Do not permit to remove configured temp life time
            continue;
        }

        if (cur->trusted_device) {

            if (ipv6_neighbour_has_registered_by_eui64(&interface->ipv6_neighbour_cache, cur->mac64)) {
                // We have registered entry so we have been selected as parent
                continue;
            }

            memcpy(ll_target + 8, cur->mac64, 8);
            ll_target[8] ^= 2;

            if (rpl_control_is_dodag_parent(interface, ll_target)) {
                // Possible parent is limited to 3 by default?
                continue;
            }
        }

        //Read current timestamp
        uint32_t time_from_last_unicast_shedule = ws_time_from_last_unicast_traffic(current_time_stamp, ws_neighbor);
        if (time_from_last_unicast_shedule >= temp_link_min_timeout) {
            //Accept only Enough Old Device
            if (!neighbor_entry_ptr) {
                //Accept first compare
                neighbor_entry_ptr = cur;
            } else {
                uint32_t compare_neigh_time = ws_time_from_last_unicast_traffic(current_time_stamp, ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, neighbor_entry_ptr->index));
                if (compare_neigh_time < time_from_last_unicast_shedule)  {
                    //Accept older RX timeout allways
                    neighbor_entry_ptr = cur;
                }
            }
        }
    }
    if (neighbor_entry_ptr) {
        tr_info("dropped oldest neighbour %s", trace_array(neighbor_entry_ptr->mac64, 8));
        mac_neighbor_table_neighbor_remove(mac_neighbor_info(interface), neighbor_entry_ptr);
    }

}

bool ws_bootstrap_neighbor_info_request(struct protocol_interface_info_entry *interface, const uint8_t *mac_64, llc_neighbour_req_t *neighbor_buffer, bool request_new)
{
    neighbor_buffer->ws_neighbor = NULL;
    neighbor_buffer->neighbor = mac_neighbor_table_address_discover(mac_neighbor_info(interface), mac_64, ADDR_802_15_4_LONG);
    if (neighbor_buffer->neighbor) {
        neighbor_buffer->ws_neighbor = ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, neighbor_buffer->neighbor->index);
        if (!neighbor_buffer->ws_neighbor) {
            return false;
        }
        return true;
    }
    if (!request_new) {
        return false;
    }

    ws_bootstrap_neighbor_table_clean(interface);

    neighbor_buffer->neighbor = ws_bootstrap_mac_neighbor_add(interface, mac_64);

    if (!neighbor_buffer->neighbor) {
        return false;
    }

    neighbor_buffer->ws_neighbor = ws_neighbor_class_entry_get(&interface->ws_info->neighbor_storage, neighbor_buffer->neighbor->index);
    if (!neighbor_buffer->ws_neighbor) {
        mac_neighbor_table_neighbor_remove(mac_neighbor_info(interface), neighbor_buffer->neighbor);
        return false;
    }
    ws_stats_update(interface, STATS_WS_NEIGHBOUR_ADD, 1);
    return true;
}

static void ws_neighbor_entry_remove_notify(mac_neighbor_table_entry_t *entry_ptr, void *user_data)
{

    protocol_interface_info_entry_t *cur = user_data;
    lowpan_adaptation_neigh_remove_free_tx_tables(cur, entry_ptr);
    // Sleepy host
    if (cur->lowpan_info & INTERFACE_NWK_CONF_MAC_RX_OFF_IDLE) {
        mac_data_poll_protocol_poll_mode_decrement(cur);
    }

    //TODO State machine check here

    if (ipv6_neighbour_has_registered_by_eui64(&cur->ipv6_neighbour_cache, entry_ptr->mac64)) {
        // Child entry deleted
        ws_stats_update(cur, STATS_WS_CHILD_REMOVE, 1);
    }

    if (entry_ptr->ffd_device) {
        protocol_6lowpan_release_short_link_address_from_neighcache(cur, entry_ptr->mac16);
        protocol_6lowpan_release_long_link_address_from_neighcache(cur, entry_ptr->mac64);
    }

    //NUD Process Clear Here
    ws_nud_entry_remove(cur, entry_ptr);

    ws_bootstrap_neighbor_delete(cur, entry_ptr);
    ws_stats_update(cur, STATS_WS_NEIGHBOUR_REMOVE, 1);

}

static uint32_t ws_probe_init_time_get(protocol_interface_info_entry_t *cur)
{
    if (ws_cfg_network_config_get(cur) <= CONFIG_SMALL) {
        return WS_SMALL_PROBE_INIT_BASE_SECONDS;
    }

    return WS_NORMAL_PROBE_INIT_BASE_SECONDS;
}

static bool ws_neighbor_entry_nud_notify(mac_neighbor_table_entry_t *entry_ptr, void *user_data)
{
    uint32_t time_from_start = entry_ptr->link_lifetime - entry_ptr->lifetime;
    uint8_t ll_address[16];
    bool nud_proces = false;
    bool activate_nud = false;
    bool child;
    bool candidate_parent;
    protocol_interface_info_entry_t *cur = user_data;

    ws_neighbor_class_entry_t *ws_neighbor = ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, entry_ptr->index);
    etx_storage_t *etx_entry = etx_storage_entry_get(cur->id, entry_ptr->index);

    if (!entry_ptr->trusted_device || !ws_neighbor || !etx_entry || entry_ptr->link_lifetime <= WS_NEIGHBOUR_TEMPORARY_NEIGH_MAX_LIFETIME) {
        return false;
    }

    if (lowpan_adaptation_expedite_forward_state_get(cur)) {
        //Do not send any probe or NUD when Expedite forward state is enabled
        return false;
    }

    ws_common_create_ll_address(ll_address, entry_ptr->mac64);

    if (time_from_start > WS_NEIGHBOR_NUD_TIMEOUT) {

        child = ipv6_neighbour_has_registered_by_eui64(&cur->ipv6_neighbour_cache, entry_ptr->mac64);
        candidate_parent = rpl_control_is_dodag_parent_candidate(cur, ll_address, cur->ws_info->cfg->gen.rpl_parent_candidate_max);
        /* For parents ARO registration is sent in link timeout times
         * For candidate parents NUD is needed
         * For children NUD is sent only at very close to end
         */
        if (!child && !candidate_parent) {
            // No need for keep alive
            return false;
        }
        if (child && (time_from_start < WS_NEIGHBOR_NUD_TIMEOUT * 1.8)) {
            /* This is our child with valid ARO registration send NUD if we are close to delete
             *
             * if ARO was received link is considered active so this is only in case of very long ARO registration times
             *
             * 1.8 means with link timeout of 30 minutes that NUD is sent 6 minutes before timeout
             *
             */
            return false;
        }

        if (time_from_start > WS_NEIGHBOR_NUD_TIMEOUT * 1.5) {
            activate_nud = true;
        } else {
            uint16_t switch_prob = randLIB_get_random_in_range(0, WS_NUD_RANDOM_SAMPLE_LENGTH - 1);
            //Take Random from time WS_NEIGHBOR_NUD_TIMEOUT - WS_NEIGHBOR_NUD_TIMEOUT*1.5
            if (switch_prob < WS_NUD_RANDOM_COMPARE) {
                activate_nud = true;
            }
        }
        nud_proces = activate_nud;
    } else if (etx_entry->etx_samples < WS_NEIGHBOR_ETX_SAMPLE_MAX) {
        //Take Random number for trig a prope.
        //Small network
        //ETX Sample 0: random 1-4
        //ETX Sample 1: random 2-8
        //ETX Sample 2: random 4-16
        //Medium and large
        //ETX Sample 0: random 1-8
        //ETX Sample 1: random 2-16
        //ETX Sample 2: random 4-32

        ws_common_create_ll_address(ll_address, entry_ptr->mac64);
        if (!rpl_control_probe_parent_candidate(cur, ll_address)) {
            return false;
        }

        uint32_t probe_period = ws_probe_init_time_get(cur) << etx_entry->etx_samples;
        uint32_t time_block = 1 << etx_entry->etx_samples;

        if (time_from_start >= probe_period) {
            //tr_debug("Link Probe test %u Sample trig", etx_entry->etx_samples);
            activate_nud = true;
        } else if (time_from_start > time_block) {
            uint16_t switch_prob = randLIB_get_random_in_range(0, probe_period - 1);
            //Take Random from time WS_NEIGHBOR_NUD_TIMEOUT - WS_NEIGHBOR_NUD_TIMEOUT*1.5
            if (switch_prob < 2) {
                //tr_debug("Link Probe test with jitter %"PRIu32", sample %u", time_from_start, etx_entry->etx_samples);
                activate_nud = true;
            }
        }
    }

    if (!activate_nud) {
        return false;
    }

    ws_nud_table_entry_t *entry = ws_nud_entry_get_free(cur);
    if (!entry) {
        return false;
    }
    entry->neighbor_info = entry_ptr;

    entry->nud_process = nud_proces;

    return true;
}

int ws_bootstrap_init(int8_t interface_id, net_6lowpan_mode_e bootstrap_mode)
{
    int ret_val = 0;

    ws_neighbor_class_t neigh_info;
    neigh_info.neigh_info_list = NULL;
    neigh_info.list_size = 0;
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur) {
        return -1;
    }

    mac_description_storage_size_t buffer;
    if (!cur->mac_api || !cur->mac_api->mac_storage_sizes_get || cur->mac_api->mac_storage_sizes_get(cur->mac_api, &buffer) != 0) {
        return -2;
    }

    if (buffer.key_description_table_size < 4) {
        tr_err("MAC key_description_table_size too short %d<4", buffer.key_description_table_size);
        return -2;
    }

    if (ns_sw_mac_enable_frame_counter_per_key(cur->mac_api, true)) {
        return -1;
    }

    if (!etx_storage_list_allocate(cur->id, buffer.device_decription_table_size)) {
        return -1;
    }
    if (!etx_cached_etx_parameter_set(WS_ETX_MIN_WAIT_TIME, WS_ETX_MIN_SAMPLE_COUNT, WS_NEIGHBOR_FIRST_ETX_SAMPLE_MIN_COUNT)) {
        etx_storage_list_allocate(cur->id, 0);
        return -1;
    }

    if (!etx_allow_drop_for_poor_measurements(WS_ETX_BAD_INIT_LINK_LEVEL, WS_ETX_MAX_BAD_LINK_DROP)) {
        etx_storage_list_allocate(cur->id, 0);
        return -1;
    }

    etx_max_update_set(WS_ETX_MAX_UPDATE);
    etx_max_set(WS_ETX_MAX);

    if (blacklist_init() != 0) {
        tr_err("MLE blacklist init failed.");
        return -1;
    }

    switch (bootstrap_mode) {
        //        case NET_6LOWPAN_SLEEPY_HOST:
        case NET_6LOWPAN_HOST:
            cur->bootsrap_mode = ARM_NWK_BOOTSRAP_MODE_6LoWPAN_HOST;
            break;
        case NET_6LOWPAN_ROUTER:
            cur->bootsrap_mode = ARM_NWK_BOOTSRAP_MODE_6LoWPAN_ROUTER;
            break;
        case NET_6LOWPAN_BORDER_ROUTER:
            cur->bootsrap_mode = ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER;
            break;
        default:
            return -3;
    }

    if (!ws_neighbor_class_alloc(&neigh_info, buffer.device_decription_table_size)) {
        ret_val = -1;
        goto init_fail;
    }

    //Disable allways by default
    lowpan_adaptation_interface_mpx_register(interface_id, NULL, 0);

    mac_neighbor_table_delete(mac_neighbor_info(cur));
    mac_neighbor_info(cur) = mac_neighbor_table_create(buffer.device_decription_table_size, ws_neighbor_entry_remove_notify
                                                       , ws_neighbor_entry_nud_notify, cur);
    if (!mac_neighbor_info(cur)) {
        ret_val = -1;
        goto init_fail;
    }

    if (wisun_mode_host(cur)) {
        // Configure for LFN device
#if defined(HAVE_WS) && defined(HAVE_WS_HOST)
        ws_llc_create(cur, & ws_bootstrap_6ln_asynch_ind, & ws_bootstrap_6ln_asynch_confirm, &ws_bootstrap_neighbor_info_request, &ws_bootstrap_6ln_eapol_relay_state_active);
#endif
    } else if (wisun_mode_router(cur)) {
        // Configure FFN device
#if defined(HAVE_WS) && defined(HAVE_WS_ROUTER)
        ws_llc_create(cur, &ws_bootstrap_6lr_asynch_ind, &ws_bootstrap_6lr_asynch_confirm, &ws_bootstrap_neighbor_info_request, &ws_bootstrap_6lr_eapol_relay_state_active);
#endif
    } else if (wisun_mode_border_router(cur)) {
        // Configure as Border router
#if defined(HAVE_WS) && defined(HAVE_WS_BORDER_ROUTER)
        ws_llc_create(cur, &ws_bootstrap_6lbr_asynch_ind, &ws_bootstrap_6lbr_asynch_confirm, &ws_bootstrap_neighbor_info_request, &ws_bootstrap_6lbr_eapol_relay_state_active);
#endif
    }

    mpx_api_t *mpx_api = ws_llc_mpx_api_get(cur);
    if (!mpx_api) {
        ret_val =  -4;
        goto init_fail;
    }

    if (ws_common_allocate_and_init(cur) < 0) {
        ret_val =  -4;
        goto init_fail;
    }

    if (ws_cfg_settings_interface_set(cur) < 0) {
        ret_val =  -4;
        goto init_fail;
    }

    if (ws_bootstrap_tasklet_init(cur) != 0) {
        ret_val =  -4;
        goto init_fail;
    }

    //Register MPXUser to adapatation layer
    if (lowpan_adaptation_interface_mpx_register(interface_id, mpx_api, MPX_LOWPAN_ENC_USER_ID) != 0) {
        ret_val =  -4;
        goto init_fail;
    }

    //Init PAE controller and set callback
    if (ws_pae_controller_init(cur) < 0) {
        ret_val =  -4;
        goto init_fail;
    }
    if (ws_pae_controller_nw_key_cb_register(cur, &ws_bootstrap_nw_key_set, &ws_bootstrap_nw_key_clear, &ws_bootstrap_nw_key_index_set, &ws_bootstrap_nw_frame_counter_set, &ws_bootstrap_nw_frame_counter_read) < 0) {
        ret_val =  -4;
        goto init_fail;
    }
    if (ws_pae_controller_authentication_cb_register(cur, &ws_bootstrap_authentication_completed, &ws_bootstrap_authentication_next_target) < 0) {
        ret_val =  -4;
        goto init_fail;
    }
    if (ws_pae_controller_configure(cur, &cur->ws_info->cfg->sec_timer, &cur->ws_info->cfg->sec_prot, &cur->ws_info->cfg->timing) < 0) {
        ret_val =  -4;
        goto init_fail;
    }

    //Init EAPOL PDU handler and register it to MPX
    if (ws_eapol_pdu_init(cur) < 0) {
        ret_val =  -4;
        goto init_fail;
    }
    if (ws_eapol_pdu_mpx_register(cur, mpx_api, MPX_KEY_MANAGEMENT_ENC_USER_ID != 0)) {
        ret_val =  -4;
        // add deallocs
        goto init_fail;
    }

    cur->ipv6_neighbour_cache.link_mtu = cur->max_link_mtu = WS_MPX_MAX_MTU;

    switch (bootstrap_mode) {
        //        case NET_6LOWPAN_SLEEPY_HOST:
        case NET_6LOWPAN_HOST:
            cur->if_up =  ws_bootstrap_6ln_up;
            cur->if_down =  ws_bootstrap_6ln_down;
            break;
        case NET_6LOWPAN_ROUTER:
        case NET_6LOWPAN_BORDER_ROUTER:
            cur->if_up = ws_bootstrap_ffn_up;
            cur->if_down = ws_bootstrap_ffn_down;
            break;
        default:
            break;
    }

    cur->ws_info->neighbor_storage = neigh_info;
    cur->etx_read_override = ws_etx_read;

    ws_bootstrap_configuration_reset(cur);
    addr_notification_register(ws_bootstrap_address_notification_cb);
    //Enable MAC Security by pass
    mlme_set_t set_req;
    bool state = true;
    set_req.attr = macAcceptByPassUnknowDevice;
    set_req.attr_index = 0;
    set_req.value_pointer = &state;
    set_req.value_size = sizeof(bool);
    cur->mac_api->mlme_req(cur->mac_api, MLME_SET, &set_req);

    // Set the default parameters for MPL
    cur->mpl_proactive_forwarding = true;

    // Specification is ruling out the compression mode, but we are now doing it.
    cur->mpl_seed = true;
    cur->mpl_seed_id_mode = MULTICAST_MPL_SEED_ID_IPV6_SRC_FOR_DOMAIN;

    cur->mpl_control_trickle_params.TimerExpirations = 0;

    cur->mpl_domain = mpl_domain_create(cur, ADDR_ALL_MPL_FORWARDERS, NULL, MULTICAST_MPL_SEED_ID_DEFAULT, -1, 0, NULL, NULL);
    addr_add_group(cur, ADDR_REALM_LOCAL_ALL_NODES);
    addr_add_group(cur, ADDR_REALM_LOCAL_ALL_ROUTERS);

    return 0;

    //Error handling and free memory
init_fail:
    lowpan_adaptation_interface_mpx_register(interface_id, NULL, 0);
    ws_eapol_pdu_mpx_register(cur, NULL, 0);
    mac_neighbor_table_delete(mac_neighbor_info(cur));
    etx_storage_list_allocate(cur->id, 0);
    ws_neighbor_class_dealloc(&neigh_info);
    ws_llc_delete(cur);
    ws_eapol_pdu_delete(cur);
    ws_pae_controller_delete(cur);
    return ret_val;
}

int ws_bootstrap_restart(int8_t interface_id)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !cur->ws_info) {
        return -1;
    }
    ws_bootstrap_event_discovery_start(cur);
    return 0;
}

int ws_bootstrap_restart_delayed(int8_t interface_id)
{
    protocol_interface_info_entry_t *cur = protocol_stack_interface_info_get_by_id(interface_id);
    if (!cur || !cur->ws_info) {
        return -1;
    }
    ws_bootstrap_state_change(cur, ER_WAIT_RESTART);
    cur->bootsrap_state_machine_cnt = 3;
    return 0;
}

static int8_t ws_bootstrap_phy_mode_resolver(const mac_api_t *api, uint8_t phy_mode_id, phy_rf_channel_configuration_s *rf_config)
{
    rf_config->modulation = ws_phy_get_modulation_using_phy_mode_id(phy_mode_id);
    if (rf_config->modulation == M_UNDEFINED) {
        return -1;
    }
    protocol_interface_info_entry_t *interface = protocol_stack_interface_info_get_by_id(api->parent_id);
    if (!interface) {
        return -1;
    }
    uint8_t regulatory_domain = interface->ws_info->hopping_schdule.regulatory_domain;
    uint8_t base_channel_plan_id = interface->ws_info->hopping_schdule.channel_plan_id;
    if (base_channel_plan_id == 255) {
        base_channel_plan_id = ws_phy_convert_operating_class_to_channel_plan_id(interface->ws_info->hopping_schdule.operating_class, regulatory_domain);
    }
    if (!base_channel_plan_id) {
        return -1;
    }
    // Function returns base channel plan ID, if it matches the PHY mode ID. Otherwise, nearest matching channel plan ID where PHY mode ID is allowed will be returned.
    uint8_t channel_plan_id = ws_phy_get_channel_plan_id_using_phy_mode_id(phy_mode_id, regulatory_domain, base_channel_plan_id);
    if (!channel_plan_id) {
        return -1;
    }

    rf_config->channel_0_center_frequency = ws_phy_get_channel_0_frequency_using_channel_plan_id(channel_plan_id);
    rf_config->channel_spacing = ws_phy_get_channel_spacing_using_channel_plan_id(channel_plan_id);
    rf_config->number_of_channels = ws_phy_get_number_of_channels_using_channel_plan_id(channel_plan_id);
    rf_config->datarate = ws_phy_get_datarate_using_phy_mode_id(phy_mode_id);
    if (!rf_config->channel_0_center_frequency || !rf_config->channel_spacing || !rf_config->number_of_channels || !rf_config->datarate) {
        return -1;
    }
    rf_config->ofdm_option = ws_phy_get_ofdm_option_using_phy_mode_id(phy_mode_id);
    rf_config->ofdm_mcs = ws_phy_get_ofdm_mcs_using_phy_mode_id(phy_mode_id);
    rf_config->fec = ws_phy_get_fsk_fec_enabled_using_phy_mode_id(phy_mode_id);
    rf_config->modulation_index = ws_phy_get_modulation_index_using_phy_mode_id(phy_mode_id);
    return 0;
}

static int ws_bootstrap_set_rf_config(protocol_interface_info_entry_t *cur, phy_rf_channel_configuration_s rf_configs)
{
    mlme_set_t set_request;
    // Set MAC mode
    phy_802_15_4_mode_t mac_mode = IEEE_802_15_4G_2012;
    set_request.attr = mac802_15_4Mode;
    set_request.value_pointer = &mac_mode;
    set_request.value_size = sizeof(phy_802_15_4_mode_t);
    cur->mac_api->mlme_req(cur->mac_api, MLME_SET, &set_request);
    // Set RF configuration
    set_request.attr = macRfConfiguration;
    set_request.value_pointer = &rf_configs;
    set_request.value_size = sizeof(phy_rf_channel_configuration_s);
    cur->mac_api->mlme_req(cur->mac_api, MLME_SET, &set_request);
    // Set Ack wait duration
    uint8_t bits_per_symbol = 1;
    if (rf_configs.modulation == M_OFDM) {
        bits_per_symbol = 4;
    }
    uint16_t ack_wait_symbols = WS_ACK_WAIT_SYMBOLS + (WS_TACK_MAX_MS * (rf_configs.datarate / 1000) / bits_per_symbol);
    set_request.attr = macAckWaitDuration;
    set_request.value_pointer = &ack_wait_symbols;
    set_request.value_size = sizeof(ack_wait_symbols);
    cur->mac_api->mlme_req(cur->mac_api, MLME_SET, &set_request);
    // Set multi CSMA-CA configuration
    mlme_multi_csma_ca_param_t multi_csma_params = {WS_NUMBER_OF_CSMA_PERIODS, WS_CSMA_MULTI_CCA_INTERVAL};
    set_request.attr = macMultiCSMAParameters;
    set_request.value_pointer = &multi_csma_params;
    set_request.value_size = sizeof(mlme_multi_csma_ca_param_t);
    cur->mac_api->mlme_req(cur->mac_api, MLME_SET, &set_request);
    if (cur->ws_info->hopping_schdule.regulatory_domain == REG_DOMAIN_JP) {
        // For Japan regulatory domain, use static CCA threshold -80dBm as defined by Wi-SUN FAN specification
        mac_helper_start_auto_cca_threshold(cur->id, cur->ws_info->hopping_schdule.number_of_channels, -80, -80, -80);
    } else {
        // Start automatic CCA threshold
        mac_helper_start_auto_cca_threshold(cur->id, cur->ws_info->hopping_schdule.number_of_channels, CCA_DEFAULT_DBM, CCA_HIGH_LIMIT, CCA_LOW_LIMIT);
    }
    // Enable MAC mode switch when base PHY mode ID could be found, otherwise disable the feature
    uint8_t phy_mode_id = cur->ws_info->hopping_schdule.phy_mode_id;
    if (phy_mode_id == 255) {
        phy_mode_id = ws_phy_convert_operating_mode_to_phy_mode_id(cur->ws_info->hopping_schdule.operating_mode);
    }
    if (!phy_mode_id) {
        cur->mac_api->mac_mode_switch_resolver_set(cur->mac_api, NULL, phy_mode_id);
    } else {
        cur->mac_api->mac_mode_switch_resolver_set(cur->mac_api, &ws_bootstrap_phy_mode_resolver, phy_mode_id);
    }
    return 0;
}

int ws_bootstrap_neighbor_remove(protocol_interface_info_entry_t *cur, const uint8_t *ll_address)
{
    mac_neighbor_table_entry_t *mac_neighbor = mac_neighbor_entry_get_by_ll64(mac_neighbor_info(cur), ll_address, false, NULL);

    if (mac_neighbor) {
        mac_neighbor_table_neighbor_remove(mac_neighbor_info(cur), mac_neighbor);
    }
    return 0;
}

int ws_bootstrap_aro_failure(protocol_interface_info_entry_t *cur, const uint8_t *ll_address)
{
    rpl_control_neighbor_delete(cur, ll_address);
    ws_bootstrap_neighbor_remove(cur, ll_address);
    return 0;
}

static int ws_bootstrap_operating_mode_resolver(protocol_interface_info_entry_t *cur, phy_rf_channel_configuration_s *rf_config)
{
    memset(rf_config, 0, sizeof(phy_rf_channel_configuration_s));
    rf_config->fec = false;
    rf_config->modulation = M_2FSK;
    rf_config->datarate = ws_phy_get_datarate_using_operating_mode(cur->ws_info->hopping_schdule.operating_mode);
    rf_config->modulation_index = ws_phy_get_modulation_index_using_operating_mode(cur->ws_info->hopping_schdule.operating_mode);
    rf_config->channel_0_center_frequency = (uint32_t)cur->ws_info->hopping_schdule.ch0_freq * 100000;
    rf_config->channel_spacing = ws_phy_decode_channel_spacing(cur->ws_info->hopping_schdule.channel_spacing);
    rf_config->number_of_channels = cur->ws_info->hopping_schdule.number_of_channels;
    return 0;
}

int ws_bootstrap_set_domain_rf_config(protocol_interface_info_entry_t *cur)
{
    phy_rf_channel_configuration_s rf_config;
    memset(&rf_config, 0, sizeof(phy_rf_channel_configuration_s));

    uint8_t phy_mode_id = cur->ws_info->hopping_schdule.phy_mode_id;
    if (phy_mode_id == 255) {
        phy_mode_id = ws_phy_convert_operating_mode_to_phy_mode_id(cur->ws_info->hopping_schdule.operating_mode);
    }

    if (!phy_mode_id || ws_bootstrap_phy_mode_resolver(cur->mac_api, phy_mode_id, &rf_config)) {
        // Cannot resolve RF configuration using PHY mode ID, try with operating mode
        ws_bootstrap_operating_mode_resolver(cur, &rf_config);
    }

    ws_bootstrap_set_rf_config(cur, rf_config);
    return 0;
}

static void ws_bootstrap_mac_activate(protocol_interface_info_entry_t *cur, uint16_t channel, uint16_t panid, bool coordinator)
{
    mlme_start_t start_req;
    memset(&start_req, 0, sizeof(mlme_start_t));

    cur->mac_parameters->pan_id = panid;
    cur->mac_parameters->mac_channel = channel;

    start_req.PANId = panid;
    start_req.LogicalChannel = channel;
    start_req.BeaconOrder = 0x0f;
    start_req.SuperframeOrder = 0x0f;
    start_req.PANCoordinator = coordinator;

    if (cur->mac_api) {
        cur->mac_api->mlme_req(cur->mac_api, MLME_START, (void *)&start_req);
    }
}

void ws_bootstrap_fhss_activate(protocol_interface_info_entry_t *cur)
{
    tr_debug("FHSS activate");
    ws_bootstrap_fhss_enable(cur);
    ws_llc_hopping_schedule_config(cur, &cur->ws_info->hopping_schdule);
    // Only supporting fixed channel

    tr_debug("MAC init");
    mac_helper_pib_boolean_set(cur, macRxOnWhenIdle, true);
    cur->lowpan_info &=  ~INTERFACE_NWK_CONF_MAC_RX_OFF_IDLE;
    ws_bootstrap_mac_security_enable(cur);
    ws_bootstrap_mac_activate(cur, cur->ws_info->cfg->fhss.fhss_uc_fixed_channel, cur->ws_info->network_pan_id, true);
    return;
}

void ws_bootstrap_ip_stack_reset(protocol_interface_info_entry_t *cur)
{
    tr_debug("ip stack reset");
    // Delete all temporary cached information
    ipv6_neighbour_cache_flush(&cur->ipv6_neighbour_cache);
    lowpan_context_list_free(&cur->lowpan_contexts);
}

void ws_bootstrap_ip_stack_activate(protocol_interface_info_entry_t *cur)
{
    tr_debug("ip stack init");
    clear_power_state(ICMP_ACTIVE);
    cur->lowpan_info |= INTERFACE_NWK_BOOTSRAP_ACTIVE;
    ws_bootstrap_ip_stack_reset(cur);
}

static uint16_t ws_bootstrap_routing_cost_calculate(protocol_interface_info_entry_t *cur)
{
    mac_neighbor_table_entry_t *mac_neighbor = mac_neighbor_entry_get_priority(mac_neighbor_info(cur));
    if (!mac_neighbor) {
        return 0xffff;
    }
    ws_neighbor_class_entry_t *ws_neighbor =  ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, mac_neighbor->index);
    if (!ws_neighbor) {
        return 0xffff;
    }

    uint16_t etx = ws_local_etx_read(cur, ADDR_802_15_4_LONG, mac_neighbor->mac64);
    if (etx == 0) {
        etx = WS_ETX_MAX; //SET maximum value here if ETX is unknown
    } else {
        //Scale to 128 based ETX (local read return 0x100 - 0xffff
        etx = etx >> 1;
    }
    // Make the 0xffff as maximum value
    if (ws_neighbor->routing_cost + etx > 0xffff) {
        return 0xffff;
    }

    return ws_neighbor->routing_cost + etx;
}

void ws_bootstrap_network_start(protocol_interface_info_entry_t *cur)
{
    //Set Network names, Pan information configure, hopping schedule & GTKHash
    ws_llc_set_network_name(cur, (uint8_t *)cur->ws_info->cfg->gen.network_name, strlen(cur->ws_info->cfg->gen.network_name));
    ws_llc_set_pan_information_pointer(cur, &cur->ws_info->pan_information);
}

void ws_bootstrap_advertise_start(protocol_interface_info_entry_t *cur)
{
    cur->ws_info->trickle_pa_running = true;
    trickle_start(&cur->ws_info->trickle_pan_advertisement, &cur->ws_info->trickle_params_pan_discovery);
    cur->ws_info->trickle_pc_running = true;
    cur->ws_info->trickle_pc_consistency_block_period = 0;
    trickle_start(&cur->ws_info->trickle_pan_config, &cur->ws_info->trickle_params_pan_discovery);
}

// Start authentication
void ws_bootstrap_start_authentication(protocol_interface_info_entry_t *cur)
{
    // Set PAN ID and network name to controller
    ws_pae_controller_nw_info_set(cur, cur->ws_info->network_pan_id, cur->ws_info->pan_information.pan_version, cur->ws_info->cfg->gen.network_name);

    ws_pae_controller_authenticate(cur);
}

static void ws_bootstrap_mac_security_enable(protocol_interface_info_entry_t *cur)
{
    mac_helper_default_security_level_set(cur, AES_SECURITY_LEVEL_ENC_MIC64);
    mac_helper_default_security_key_id_mode_set(cur, MAC_KEY_ID_MODE_IDX);
}

static void ws_bootstrap_nw_key_set(protocol_interface_info_entry_t *cur, uint8_t slot, uint8_t index, uint8_t *key)
{
    mac_helper_security_key_to_descriptor_set(cur, key, index + 1, slot);
}

static void ws_bootstrap_nw_key_clear(protocol_interface_info_entry_t *cur, uint8_t slot)
{
    mac_helper_security_key_descriptor_clear(cur, slot);
}

static void ws_bootstrap_nw_key_index_set(protocol_interface_info_entry_t *cur, uint8_t index)
{

    if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER) {
        if (cur->mac_parameters->mac_default_key_index != 0 && cur->mac_parameters->mac_default_key_index  != index + 1) {
            tr_info("New Pending key Request %u", index + 1);
            cur->ws_info->pending_key_index_info.state = PENDING_KEY_INDEX_ADVERTISMENT;
            cur->ws_info->pending_key_index_info.index = index;
            return;
        }
    }

    mac_helper_security_auto_request_key_index_set(cur, index, index + 1);
}

static void ws_bootstrap_nw_frame_counter_set(protocol_interface_info_entry_t *cur, uint32_t counter, uint8_t slot)
{
    // Set frame counter
    mac_helper_key_link_frame_counter_set(cur->id, counter, slot);
}

static void ws_bootstrap_nw_frame_counter_read(protocol_interface_info_entry_t *cur, uint32_t *counter, uint8_t slot)
{
    // Read frame counter
    mac_helper_key_link_frame_counter_read(cur->id, counter, slot);
}

static void ws_bootstrap_authentication_completed(protocol_interface_info_entry_t *cur, auth_result_e result, uint8_t *target_eui_64)
{
    if (result == AUTH_RESULT_OK) {
        tr_info("authentication success eui64:%s", trace_array(target_eui_64, 8));
        if (target_eui_64) {
            // Authentication was made contacting the authenticator
            cur->ws_info->authentication_time = cur->ws_info->uptime;
        }
        ws_bootstrap_event_configuration_start(cur);
    } else if (result == AUTH_RESULT_ERR_TX_ERR) {
        // eapol parent selected is not working
        tr_debug("authentication TX failed");

        ws_bootstrap_candidate_parent_mark_failure(cur, target_eui_64);
        // Go back for network scanning
        ws_bootstrap_state_change(cur, ER_ACTIVE_SCAN);

        // Start PAS interval between imin - imax.
        cur->ws_info->trickle_pas_running = true;
        trickle_start(&cur->ws_info->trickle_pan_advertisement_solicit, &cur->ws_info->trickle_params_pan_discovery);

        // Parent selection is made before imin/2 so if there is parent candidates solicit is not sent
        cur->bootsrap_state_machine_cnt = randLIB_get_random_in_range(10, cur->ws_info->trickle_params_pan_discovery.Imin >> 1);
        tr_info("Making parent selection in %u s", (cur->bootsrap_state_machine_cnt / 10));
    } else {
        tr_debug("authentication failed");
        // What else to do to start over again...
        // Trickle is reseted when entering to discovery from state 2
        trickle_inconsistent_heard(&cur->ws_info->trickle_pan_advertisement_solicit, &cur->ws_info->trickle_params_pan_discovery);
        ws_bootstrap_event_discovery_start(cur);
    }
}

static const uint8_t *ws_bootstrap_authentication_next_target(protocol_interface_info_entry_t *cur, const uint8_t *previous_eui_64, uint16_t *pan_id)
{
    ws_bootstrap_candidate_parent_mark_failure(cur, previous_eui_64);

    // Gets best target
    parent_info_t *parent_info = ws_bootstrap_candidate_parent_get_best(cur);
    if (parent_info) {
        /* On failure still continues with the new parent, and on next call,
           will try to set the neighbor again */
        ws_bootstrap_neighbor_set(cur, parent_info, true);
        *pan_id = parent_info->pan_id;
        return parent_info->addr;
    }

    // If no targets found, retries the last one
    return previous_eui_64;
}


/*
 * Event transitions
 *
 * */
void ws_bootstrap_event_discovery_start(protocol_interface_info_entry_t *cur)
{
    ws_bootsrap_event_trig(WS_DISCOVERY_START, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, NULL);
}
void ws_bootstrap_event_configuration_start(protocol_interface_info_entry_t *cur)
{
    ws_bootsrap_event_trig(WS_CONFIGURATION_START, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, NULL);
}
void ws_bootstrap_event_authentication_start(protocol_interface_info_entry_t *cur)
{
    ws_bootstrap_state_change(cur, ER_PANA_AUTH);
}
void ws_bootstrap_event_operation_start(protocol_interface_info_entry_t *cur)
{
    ws_bootsrap_event_trig(WS_OPERATION_START, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, NULL);
}
void ws_bootstrap_event_routing_ready(protocol_interface_info_entry_t *cur)
{
    ws_bootsrap_event_trig(WS_ROUTING_READY, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, NULL);
}

void ws_bootstrap_event_disconnect(protocol_interface_info_entry_t *cur, ws_bootsrap_event_type_e event_type)
{
    ws_bootsrap_event_trig(event_type, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, NULL);
}
void ws_bootstrap_event_test_procedure_trigger(protocol_interface_info_entry_t *cur, ws_bootsrap_procedure_t procedure)
{
    if (cur->bootStrapId < 0) {
        return;
    }
    ws_bootsrap_event_trig(WS_TEST_PROC_TRIGGER, cur->bootStrapId, ARM_LIB_LOW_PRIORITY_EVENT, (void *) procedure);
}

void ws_bootstrap_configuration_trickle_reset(protocol_interface_info_entry_t *cur)
{
    trickle_inconsistent_heard(&cur->ws_info->trickle_pan_config, &cur->ws_info->trickle_params_pan_discovery);
}

static void ws_bootstrap_set_asynch_channel_list(protocol_interface_info_entry_t *cur, asynch_request_t *async_req)
{
    memset(&async_req->channel_list, 0, sizeof(channel_list_s));
    if (cur->ws_info->cfg->fhss.fhss_uc_channel_function == WS_FIXED_CHANNEL) {
        //SET 1 Channel only
        uint16_t channel_number = cur->ws_info->cfg->fhss.fhss_uc_fixed_channel;
        async_req->channel_list.channel_mask[channel_number / 32] = 1U << (channel_number % 32);
    } else {
        ws_common_generate_channel_list(async_req->channel_list.channel_mask, cur->ws_info->hopping_schdule.number_of_channels, cur->ws_info->hopping_schdule.regulatory_domain, cur->ws_info->hopping_schdule.operating_class, cur->ws_info->hopping_schdule.channel_plan_id);
    }

    async_req->channel_list.channel_page = CHANNEL_PAGE_10;
}

static void ws_bootstrap_pan_advert_solicit(protocol_interface_info_entry_t *cur)
{
    asynch_request_t async_req;
    memset(&async_req, 0, sizeof(asynch_request_t));
    async_req.message_type = WS_FT_PAN_ADVERT_SOL;
    //Request UTT Header and US and Net name from payload
    async_req.wh_requested_ie_list.utt_ie = true;
    async_req.wp_requested_nested_ie_list.us_ie = true;
    async_req.wp_requested_nested_ie_list.net_name_ie = true;

    ws_bootstrap_set_asynch_channel_list(cur, &async_req);


    async_req.security.SecurityLevel = 0;

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PAS, 1);
    ws_llc_asynch_request(cur, &async_req);
}

static void ws_bootstrap_pan_config_solicit(protocol_interface_info_entry_t *cur)
{
    asynch_request_t async_req;
    memset(&async_req, 0, sizeof(asynch_request_t));
    async_req.message_type = WS_FT_PAN_CONF_SOL;
    //Request UTT Header and US and Net name from payload
    async_req.wh_requested_ie_list.utt_ie = true;
    async_req.wp_requested_nested_ie_list.us_ie = true;
    async_req.wp_requested_nested_ie_list.net_name_ie = true;

    ws_bootstrap_set_asynch_channel_list(cur, &async_req);
    async_req.security.SecurityLevel = 0;

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PCS, 1);
    ws_llc_asynch_request(cur, &async_req);
}

static void ws_bootstrap_pan_advert(protocol_interface_info_entry_t *cur)
{
    asynch_request_t async_req;
    memset(&async_req, 0, sizeof(asynch_request_t));
    async_req.message_type = WS_FT_PAN_ADVERT;
    //Request UTT Header, Pan information and US and Net name from payload
    async_req.wh_requested_ie_list.utt_ie = true;
    async_req.wp_requested_nested_ie_list.us_ie = true;
    async_req.wp_requested_nested_ie_list.pan_ie = true;
    async_req.wp_requested_nested_ie_list.net_name_ie = true;
    if (ws_version_1_1(cur)) {
        async_req.wp_requested_nested_ie_list.phy_cap_ie = true;
    }

    ws_bootstrap_set_asynch_channel_list(cur, &async_req);
    async_req.security.SecurityLevel = 0;

    if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER) {
        // Border routers write the NW size
        cur->ws_info->pan_information.pan_size = ws_bbr_pan_size(cur);
        cur->ws_info->pan_information.routing_cost = 0;
    } else {
        // Nodes need to calculate routing cost
        // PAN size is saved from latest PAN advertisement
        cur->ws_info->pan_information.routing_cost = ws_bootstrap_routing_cost_calculate(cur);
    }

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PA, 1);
    ws_llc_asynch_request(cur, &async_req);
}

static void ws_bootstrap_pan_config(protocol_interface_info_entry_t *cur)
{
    asynch_request_t async_req;
    memset(&async_req, 0, sizeof(asynch_request_t));
    async_req.message_type = WS_FT_PAN_CONF;
    //Request UTT Header, Pan information and US and Net name from payload
    async_req.wh_requested_ie_list.utt_ie = true;
    async_req.wh_requested_ie_list.bt_ie = true;
    async_req.wp_requested_nested_ie_list.us_ie = true;
    async_req.wp_requested_nested_ie_list.bs_ie = true;
    async_req.wp_requested_nested_ie_list.pan_version_ie = true;
    async_req.wp_requested_nested_ie_list.gtkhash_ie = true;
    async_req.wp_requested_nested_ie_list.vp_ie = true;
    if (ws_version_1_1(cur)) {
        async_req.wp_requested_nested_ie_list.lfn_gtk_version_ie = ws_lfn_version_learned(cur);
    }

    ws_bootstrap_set_asynch_channel_list(cur, &async_req);

    async_req.security.SecurityLevel = mac_helper_default_security_level_get(cur);
    async_req.security.KeyIdMode = mac_helper_default_security_key_id_mode_get(cur);
    if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER && cur->ws_info->pending_key_index_info.state == PENDING_KEY_INDEX_ADVERTISMENT) {
        async_req.security.KeyIndex =  cur->ws_info->pending_key_index_info.index + 1;
        cur->ws_info->pending_key_index_info.state = PENDING_KEY_INDEX_ACTIVATE;
    } else {
        async_req.security.KeyIndex = mac_helper_default_key_index_get(cur);
    }

    ws_stats_update(cur, STATS_WS_ASYNCH_TX_PC, 1);
    ws_llc_asynch_request(cur, &async_req);
}

static void ws_bootstrap_event_handler(arm_event_s *event)
{
    protocol_interface_info_entry_t *cur;
    cur = protocol_stack_interface_info_get_by_bootstrap_id(event->receiver);
    if (!cur) {
        return;
    }

    if (wisun_mode_host(cur)) {
        ws_bootstrap_6ln_event_handler(cur, event);
    } else if (wisun_mode_router(cur)) {
        ws_bootstrap_6lr_event_handler(cur, event);
    } else if (wisun_mode_border_router(cur)) {
        ws_bootstrap_6lbr_event_handler(cur, event);
    }
}

int8_t ws_bootstrap_neighbor_set(protocol_interface_info_entry_t *cur, parent_info_t *parent_ptr, bool clear_list)
{
    uint16_t pan_id = cur->ws_info->network_pan_id;

    // Add EAPOL neighbor
    cur->ws_info->network_pan_id = parent_ptr->pan_id;
    cur->ws_info->pan_information.pan_size = parent_ptr->pan_information.pan_size;
    cur->ws_info->pan_information.routing_cost = parent_ptr->pan_information.routing_cost;
    cur->ws_info->pan_information.use_parent_bs = parent_ptr->pan_information.use_parent_bs;
    cur->ws_info->pan_information.pan_version = 0; // This is learned from actual configuration

    // If PAN ID changes, clear learned neighbors and activate FHSS
    if (pan_id != cur->ws_info->network_pan_id) {
        if (clear_list) {
            ws_bootstrap_neighbor_list_clean(cur);
        }
        ws_bootstrap_fhss_activate(cur);
    }

    llc_neighbour_req_t neighbor_info;
    if (!ws_bootstrap_neighbor_info_request(cur, parent_ptr->addr, &neighbor_info, true)) {
        //Remove Neighbour and set Link setup back
        ns_list_remove(&cur->ws_info->parent_list_reserved, parent_ptr);
        ns_list_add_to_end(&cur->ws_info->parent_list_free, parent_ptr);
        return -1;
    }
    ws_bootstrap_neighbor_set_stable(cur, parent_ptr->addr);
    ws_neighbor_class_neighbor_unicast_time_info_update(neighbor_info.ws_neighbor, &parent_ptr->ws_utt, parent_ptr->timestamp, parent_ptr->addr);
    ws_neighbor_class_neighbor_unicast_schedule_set(neighbor_info.ws_neighbor, &parent_ptr->ws_us, &cur->ws_info->hopping_schdule);
    return 0;
}

/*
 * State machine
 *
 * */
void ws_bootstrap_state_disconnect(protocol_interface_info_entry_t *cur, ws_bootsrap_event_type_e event_type)
{
    if (cur->nwk_bootstrap_state == ER_RPL_NETWORK_LEAVING) {
        //Already moved to leaving state.
        return;
    }
    // We are no longer connected
    cur->ws_info->connected_time = 0;

    if (cur->rpl_domain && cur->nwk_bootstrap_state == ER_BOOTSRAP_DONE) {
        //Stop Asych Timer
        ws_bootstrap_asynch_trickle_stop(cur);
        tr_debug("Start Network soft leaving");
        if (event_type == WS_FAST_DISCONNECT) {
            rpl_control_instant_poison(cur, cur->rpl_domain);
            cur->bootsrap_state_machine_cnt = 80; //Give 8 seconds time to send Poison
        } else {
            rpl_control_poison(cur->rpl_domain, 1);
            cur->bootsrap_state_machine_cnt = 6000; //Give 10 minutes time for poison if RPL is not report
        }

    } else {
        ws_bootstrap_event_discovery_start(cur);
    }
    cur->nwk_bootstrap_state = ER_RPL_NETWORK_LEAVING;
}

bool ws_bootstrap_state_discovery(struct protocol_interface_info_entry *cur)
{
    if (cur->nwk_bootstrap_state == ER_ACTIVE_SCAN) {
        return true;
    }
    return false;
}

bool ws_bootstrap_state_authenticate(struct protocol_interface_info_entry *cur)
{
    // Think about the state value
    if (cur->nwk_bootstrap_state == ER_PANA_AUTH) {
        return true;
    }
    return false;
}

bool ws_bootstrap_state_configure(struct protocol_interface_info_entry *cur)
{
    // Think about the state value
    if (cur->nwk_bootstrap_state == ER_SCAN) {
        return true;
    }
    return false;
}

bool ws_bootstrap_state_wait_rpl(struct protocol_interface_info_entry *cur)
{
    // Think about the state value
    if (cur->nwk_bootstrap_state == ER_RPL_SCAN) {
        return true;
    }
    return false;
}

bool ws_bootstrap_state_active(struct protocol_interface_info_entry *cur)
{
    if (cur->nwk_bootstrap_state == ER_BOOTSRAP_DONE) {
        return true;
    }
    return false;
}

void ws_bootstrap_state_change(protocol_interface_info_entry_t *cur, icmp_state_t nwk_bootstrap_state)
{
    cur->bootsrap_state_machine_cnt = 1;
    cur->nwk_bootstrap_state = nwk_bootstrap_state;
}

void ws_bootstrap_network_down(protocol_interface_info_entry_t *cur)
{
    ws_nwk_event_post(cur, ARM_NWK_NWK_CONNECTION_DOWN);
}

void ws_bootstrap_trickle_timer(protocol_interface_info_entry_t *cur, uint16_t ticks)
{
    if (cur->ws_info->trickle_pas_running &&
            trickle_timer(&cur->ws_info->trickle_pan_advertisement_solicit, &cur->ws_info->trickle_params_pan_discovery, ticks)) {
        // send PAN advertisement solicit
        tr_info("Send PAN advertisement Solicit");
        ws_bootstrap_pan_advert_solicit(cur);
    }
    if (cur->ws_info->trickle_pcs_running) {

        //Update MAX config sol timeout timer
        if (cur->ws_info->pan_config_sol_max_timeout > ticks) {
            cur->ws_info->pan_config_sol_max_timeout -= ticks;
        } else {
            //Config sol state timeout
            cur->ws_info->pan_config_sol_max_timeout = 0;
        }

        if (trickle_timer(&cur->ws_info->trickle_pan_config_solicit, &cur->ws_info->trickle_params_pan_discovery, ticks)) {
            if (cur->ws_info->pas_requests < PCS_MAX) {
                // send PAN Configuration solicit
                tr_info("Send PAN configuration Solicit");
                ws_bootstrap_pan_config_solicit(cur);
            }
            //Update counter every time reason that we detect PCS_MAX higher state
            cur->ws_info->pas_requests++;
        }

        if (cur->ws_info->pas_requests > PCS_MAX || cur->ws_info->pan_config_sol_max_timeout == 0) {
            // if MAX PCS sent or max waited timeout restart discovery
            // Trickle is reseted when entering to discovery from state 3
            tr_info("PAN configuration Solicit timeout");
            trickle_inconsistent_heard(&cur->ws_info->trickle_pan_advertisement_solicit, &cur->ws_info->trickle_params_pan_discovery);
            ws_bootstrap_event_discovery_start(cur);
            return;
        }
    }
    if (cur->ws_info->trickle_pa_running &&
            trickle_timer(&cur->ws_info->trickle_pan_advertisement, &cur->ws_info->trickle_params_pan_discovery, ticks)) {
        // send PAN advertisement
        tr_info("Send PAN advertisement");
        ws_bootstrap_pan_advert(cur);
    }
    if (cur->ws_info->trickle_pc_running) {

        if (cur->ws_info->trickle_pc_consistency_block_period) {
            if (ticks >= cur->ws_info->trickle_pc_consistency_block_period) {
                cur->ws_info->trickle_pc_consistency_block_period = 0;
            } else {
                cur->ws_info->trickle_pc_consistency_block_period -= ticks;
            }
        }

        if (trickle_timer(&cur->ws_info->trickle_pan_config, &cur->ws_info->trickle_params_pan_discovery, ticks)) {
            // send PAN Configuration
            tr_info("Send PAN configuration");
            ws_bootstrap_pan_config(cur);
        }
    }
}

void ws_bootstrap_asynch_trickle_stop(protocol_interface_info_entry_t *cur)
{
    cur->ws_info->trickle_pas_running = false;
    cur->ws_info->trickle_pa_running = false;
    cur->ws_info->trickle_pcs_running = false;
    cur->ws_info->trickle_pc_running = false;
    cur->ws_info->trickle_pc_consistency_block_period = 0;
}


void ws_bootstrap_seconds_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    /*Update join state statistics*/
    if (ws_bootstrap_state_discovery(cur)) {
        ws_stats_update(cur, STATS_WS_STATE_1, 1);
    } else if (ws_bootstrap_state_authenticate(cur)) {
        ws_stats_update(cur, STATS_WS_STATE_2, 1);
    } else if (ws_bootstrap_state_configure(cur)) {
        ws_stats_update(cur, STATS_WS_STATE_3, 1);
    } else if (ws_bootstrap_state_wait_rpl(cur)) {
        ws_stats_update(cur, STATS_WS_STATE_4, 1);
    } else if (ws_bootstrap_state_active(cur)) {
        ws_stats_update(cur, STATS_WS_STATE_5, 1);
        //Update neighbour MDR phy capability mode id
        ws_neighbour_mdr_mode_analyze(cur);
    }
    cur->ws_info->uptime++;

    ws_llc_timer_seconds(cur, seconds);

    ws_bootstrap_test_procedure_trigger_timer(cur, seconds);
}

int ws_bootstrap_stack_info_get(protocol_interface_info_entry_t *cur, struct ws_stack_info *info_ptr)
{

    ws_neighbor_class_entry_t *ws_neighbour = NULL;

    memset(info_ptr, 0, sizeof(struct ws_stack_info));
    mac_neighbor_table_entry_t *mac_parent = mac_neighbor_entry_get_priority(mac_neighbor_info(cur));

    if (mac_parent) {
        ws_neighbour = ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, mac_parent->index);
        ws_common_create_ll_address(info_ptr->parent, mac_parent->mac64);
    }
    if (ws_neighbour) {
        info_ptr->rsl_in = ws_neighbor_class_rsl_in_get(ws_neighbour);
        info_ptr->rsl_out = ws_neighbor_class_rsl_out_get(ws_neighbour);
        info_ptr->routing_cost = ws_neighbour->routing_cost;
    }
    info_ptr->device_min_sens = DEVICE_MIN_SENS;
    if (ws_bootstrap_state_discovery(cur)) {
        info_ptr->join_state = 1;
    } else if (ws_bootstrap_state_authenticate(cur)) {
        info_ptr->join_state = 2;
    } else if (ws_bootstrap_state_configure(cur)) {
        info_ptr->join_state = 3;
    } else if (ws_bootstrap_state_wait_rpl(cur)) {
        info_ptr->join_state = 4;
    } else if (ws_bootstrap_state_active(cur)) {
        info_ptr->join_state = 5;
    }
    info_ptr->pan_id = cur->ws_info->network_pan_id;

    return 0;
}
int ws_bootstrap_neighbor_info_get(protocol_interface_info_entry_t *cur, ws_neighbour_info_t *neighbor_ptr, uint16_t table_max)
{
    uint8_t count = 0;
    if (!neighbor_ptr) {
        // Return the aount of neighbors.
        for (int n = 0; n < mac_neighbor_info(cur)->list_total_size; n++) {
            mac_neighbor_table_entry_t *mac_entry = mac_neighbor_table_attribute_discover(mac_neighbor_info(cur), n);
            if (mac_entry && mac_entry->lifetime && mac_entry->lifetime != 0xffffffff) {
                count++;
            }
        }
        return count;
    }

    if (table_max > mac_neighbor_info(cur)->list_total_size) {
        table_max = mac_neighbor_info(cur)->list_total_size;
    }

    for (int n = 0; n < mac_neighbor_info(cur)->list_total_size; n++) {
        if (count > table_max) {
            break;
        }

        mac_neighbor_table_entry_t *mac_entry = mac_neighbor_table_attribute_discover(mac_neighbor_info(cur), n);
        ws_neighbor_class_entry_t *ws_neighbor =  ws_neighbor_class_entry_get(&cur->ws_info->neighbor_storage, n);
        if (mac_entry && ws_neighbor && mac_entry->lifetime && mac_entry->lifetime != 0xffffffff) {
            // Active neighbor entry
            uint8_t ll_address[16];
            memset(neighbor_ptr + count, 0, sizeof(ws_neighbour_info_t));
            neighbor_ptr[count].lifetime = mac_entry->lifetime;

            neighbor_ptr[count].rsl_in = ws_neighbor_class_rsl_in_get(ws_neighbor);
            neighbor_ptr[count].rsl_out = ws_neighbor_class_rsl_out_get(ws_neighbor);

            // ETX is shown calculated as 8 bit integer, but more common way is to use 7 bit such that 128 means ETX:1.0
            neighbor_ptr[count].etx = ws_local_etx_read(cur, ADDR_802_15_4_LONG, mac_entry->mac64);
            if (neighbor_ptr[count].etx != 0xffff) {
                neighbor_ptr[count].etx = neighbor_ptr[count].etx >> 1;
            }
            ws_common_create_ll_address(ll_address, mac_entry->mac64);
            memcpy(neighbor_ptr[count].link_local_address, ll_address, 16);

            if (rpl_control_is_dodag_parent_candidate(cur, ll_address, cur->ws_info->cfg->gen.rpl_parent_candidate_max)) {
                neighbor_ptr[count].type = WS_CANDIDATE_PARENT;
            }
            neighbor_ptr[count].rpl_rank = rpl_control_neighbor_info_get(cur, ll_address, neighbor_ptr[count].global_address);

            if (mac_entry->link_role == PRIORITY_PARENT_NEIGHBOUR) {
                neighbor_ptr[count].type = WS_PRIMARY_PARENT;
            }
            if (mac_entry->link_role == SECONDARY_PARENT_NEIGHBOUR) {
                neighbor_ptr[count].type = WS_SECONDARY_PARENT;
            }
            if (mac_entry->link_role == CHILD_NEIGHBOUR) {
                neighbor_ptr[count].type = WS_CHILD;
            }

            ipv6_neighbour_t *IPv6_neighbor = ipv6_neighbour_get_registered_by_eui64(&cur->ipv6_neighbour_cache, mac_entry->mac64);
            if (IPv6_neighbor) {
                //This is a child
                neighbor_ptr[count].type = WS_CHILD;
                memcpy(neighbor_ptr[count].global_address, IPv6_neighbor->ip_address, 16);
                // Child lifetimes are based on Registration times not a link time
                neighbor_ptr[count].lifetime = IPv6_neighbor->lifetime;
            }
            count++;
        }
    }

    // Go through list
    return count;
}

//Calculate max_packet queue size
static uint16_t ws_bootstrap_define_congestin_max_threshold(uint32_t heap_total_size, uint16_t packet_size, uint16_t packet_per_seconds, uint32_t max_delay, uint16_t min_packet_queue_size, uint16_t max_packet_queue_size)
{
    uint32_t max_packet_count = 0;
    if (heap_total_size) {
        //Claculate how many packet can be max queue to half of heap
        max_packet_count = (heap_total_size / 2) / packet_size;
    }

    //Calculate how many packet is possible to queue for guarantee given max delay
    uint32_t max_delayded_queue_size = max_delay * packet_per_seconds;

    if (max_packet_count > max_delayded_queue_size) {
        //Limit queue size by MAX delay
        max_packet_count = max_delayded_queue_size;
    }

    if (max_packet_count > max_packet_queue_size) {
        //Limit queue size by Max
        max_packet_count = max_packet_queue_size;
    } else if (max_packet_count < min_packet_queue_size) {
        //Limit queue size by Min
        max_packet_count = min_packet_queue_size;
    }
    return (uint16_t)max_packet_count;
}

static uint16_t ws_bootstrap_packet_per_seconds(protocol_interface_info_entry_t *cur, uint16_t packet_size)
{
    uint32_t data_rate = ws_common_datarate_get(cur);

    //calculate how many packet is possible send in paper
    data_rate /= 8 * packet_size;

    //Divide optimal  by / 5 because we split TX / RX slots and BC schedule
    //With Packet size 500 it should return
    //Return 15 for 300kBits
    //Return 7 for 150kBits
    //Return 2 for 50kBits
    return data_rate / 5;
}

void ws_bootstrap_packet_congestion_init(protocol_interface_info_entry_t *cur)
{
    random_early_detection_free(cur->random_early_detection);
    cur->random_early_detection = NULL;

    //TODO implement API for HEAP info request
    uint32_t heap_size;
    const mem_stat_t *mem_stats = ns_dyn_mem_get_mem_stat();
    if (mem_stats) {
        heap_size = mem_stats->heap_sector_size;
    } else {
        heap_size = 0;
    }

    uint16_t packet_per_seconds = ws_bootstrap_packet_per_seconds(cur, WS_CONGESTION_PACKET_SIZE);

    uint16_t min_th, max_th;

    if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER) {
        max_th = ws_bootstrap_define_congestin_max_threshold(heap_size, WS_CONGESTION_PACKET_SIZE, packet_per_seconds, WS_CONGESTION_QUEUE_DELAY, WS_CONGESTION_BR_MIN_QUEUE_SIZE, WS_CONGESTION_BR_MAX_QUEUE_SIZE);
    } else {
        max_th = ws_bootstrap_define_congestin_max_threshold(heap_size, WS_CONGESTION_PACKET_SIZE, packet_per_seconds, WS_CONGESTION_QUEUE_DELAY, WS_CONGESTION_NODE_MIN_QUEUE_SIZE, WS_CONGESTION_NODE_MAX_QUEUE_SIZE);
    }

    min_th = max_th / 2;
    tr_info("Wi-SUN packet congestion minTh %u, maxTh %u, drop probability %u weight %u, Packet/Seconds %u", min_th, max_th, WS_CONGESTION_RED_DROP_PROBABILITY, RED_AVERAGE_WEIGHT_EIGHTH, packet_per_seconds);
    cur->random_early_detection = random_early_detection_create(min_th, max_th, WS_CONGESTION_RED_DROP_PROBABILITY, RED_AVERAGE_WEIGHT_EIGHTH);

}

static bool auto_test_proc_trg_enabled = false;

int ws_bootstrap_test_procedure_trigger(protocol_interface_info_entry_t *cur, ws_bootsrap_procedure_t procedure)
{
    switch (procedure) {
        case PROCEDURE_AUTO_ON:
            tr_info("Trigger bootstrap test procedures automatically");
            auto_test_proc_trg_enabled = true;
            return 0;
        case PROCEDURE_AUTO_OFF:
            tr_info("Disable automatic bootstrap test procedure triggering");
            auto_test_proc_trg_enabled = false;
            return 0;
        default:
            break;
    }

    if (!cur) {
        return -1;
    }

    switch (procedure) {
        case PROCEDURE_DIS:
        case PROCEDURE_DAO:
        case PROCEDURE_PAS:
        case PROCEDURE_PCS:
        case PROCEDURE_EAPOL:
        case PROCEDURE_RPL:
            if (cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER) {
                tr_info("Not allowed on Border Router");
                return -1;
            }
            break;
        default:
            break;
    }

    if (cur->interface_mode != INTERFACE_UP) {
        tr_info("Interface is not up");
        return -1;
    }

    ws_bootstrap_event_test_procedure_trigger(cur, procedure);
    return 0;
}

void ws_bootstrap_test_procedure_trigger_exec(protocol_interface_info_entry_t *cur, ws_bootsrap_procedure_t procedure)
{
    switch (procedure) {
        case PROCEDURE_DIS:
            if (cur->nwk_bootstrap_state == ER_RPL_SCAN || ws_bootstrap_state_active(cur)) {
                if(cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_ROUTER || cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER){
                    tr_info("trigger DODAG information object solicit");
                    rpl_control_transmit_dis(cur->rpl_domain, cur, 0, 0, NULL, 0, ADDR_LINK_LOCAL_ALL_RPL_NODES);
                }else{
                    tr_warn("Doesn't trigger DODAG information DIS");
                }
            } else {
                tr_info("wrong state: DODAG information object solicit not triggered");
            }
            break;
        case PROCEDURE_DIO:
            if (ws_bootstrap_state_active(cur)) {
                if(cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_ROUTER || cur->bootsrap_mode == ARM_NWK_BOOTSRAP_MODE_6LoWPAN_BORDER_ROUTER){
                    tr_info("trigger DODAG information object");
                    rpl_control_transmit_dio_trigger(cur, cur->rpl_domain);
                }else{
                    tr_warn("Doesn't trigger DODAG information DIS");
                }
            } else {
                tr_info("wrong state: DODAG information object not triggered");
            }
            break;
        case PROCEDURE_DAO:
            // Can be triggered if in correct state and there is selected RPL parent
            if ((cur->nwk_bootstrap_state == ER_RPL_SCAN || ws_bootstrap_state_active(cur))
                    && rpl_control_parent_candidate_list_size(cur, true) > 0) {
                tr_info("trigger Destination advertisement object");
                rpl_control_dao_timeout(cur->rpl_domain, 2);
            } else {
                tr_info("wrong state: Destination advertisement object not triggered");
            }
            break;
        case PROCEDURE_PAS:
        case PROCEDURE_PAS_TRICKLE_INCON:
            tr_info("trigger PAN advertisement Solicit");
            if (procedure != PROCEDURE_PAS_TRICKLE_INCON) {
                tr_info("send PAN advertisement Solicit");
                ws_bootstrap_pan_advert_solicit(cur);
            }
            if (cur->ws_info->trickle_pas_running) {
                trickle_inconsistent_heard(&cur->ws_info->trickle_pan_advertisement_solicit, &cur->ws_info->trickle_params_pan_discovery);
            }
            break;
        case PROCEDURE_PA:
            if (cur->ws_info->trickle_pa_running) {
                tr_info("trigger PAN advertisement");
                ws_bootstrap_pan_advert(cur);
                trickle_inconsistent_heard(&cur->ws_info->trickle_pan_advertisement, &cur->ws_info->trickle_params_pan_discovery);
            } else {
                tr_info("wrong state: PAN advertisement not triggered");
            }
            break;
        case PROCEDURE_PCS:
        case PROCEDURE_PCS_TRICKLE_INCON:
            if (cur->ws_info->trickle_pcs_running || ws_bootstrap_state_active(cur)) {
                tr_info("trigger PAN configuration Solicit");
                if (procedure != PROCEDURE_PCS_TRICKLE_INCON) {
                    tr_info("send PAN configuration Solicit");
                    ws_bootstrap_pan_config_solicit(cur);
                }
                if (cur->ws_info->trickle_pcs_running) {
                    trickle_inconsistent_heard(&cur->ws_info->trickle_pan_config_solicit, &cur->ws_info->trickle_params_pan_discovery);
                }
            } else {
                tr_info("wrong state: PAN configuration Solicit not triggered");
            }
            break;
        case PROCEDURE_PC:
            if (cur->ws_info->trickle_pc_running) {
                tr_info("trigger PAN configuration");
                ws_bootstrap_pan_config(cur);
                trickle_inconsistent_heard(&cur->ws_info->trickle_pan_config, &cur->ws_info->trickle_params_pan_discovery);
            } else {
                tr_info("wrong state: PAN configuration not triggered");
            }
            break;
        case PROCEDURE_EAPOL:
            if (cur->nwk_bootstrap_state == ER_ACTIVE_SCAN) {
                tr_info("trigger EAPOL target selection");
                if (cur->bootsrap_state_machine_cnt > 3) {
                    cur->bootsrap_state_machine_cnt = 3;
                }
            } else {
                tr_info("wrong state: EAPOL target selection not triggered");
            }
            break;
        case PROCEDURE_RPL: {
            bool neigth_has_ext = false;
            for (int n = 0; n < mac_neighbor_info(cur)->list_total_size; n++) {
                mac_neighbor_table_entry_t *mac_entry = mac_neighbor_table_attribute_discover(mac_neighbor_info(cur), n);
                if (mac_entry) {
                    uint16_t etx = ws_local_etx_read(cur, ADDR_802_15_4_LONG, mac_entry->mac64);
                    if (etx != 0xFFFF) {
                        neigth_has_ext = true;
                    }
                }
            }
            /* If selecting RPL parent, there is some RPL candidates and neighbors with ETX try
               the RPL parent selection procedure */
            if (cur->nwk_bootstrap_state == ER_RPL_SCAN && neigth_has_ext &&
                    rpl_control_parent_candidate_list_size(cur, false) > 0) {
                tr_info("trigger RPL parent selection");
                rpl_control_parent_selection_trigger(cur->rpl_domain);
            } else {
                tr_info("wrong state: RPL parent selection not triggered");
            }
            break;
        }
        default:
            break;
    }
}

static void ws_bootstrap_test_procedure_trigger_timer(protocol_interface_info_entry_t *cur, uint32_t seconds)
{
    if (!auto_test_proc_trg_enabled) {
        cur->ws_info->test_proc_trg.auto_trg_enabled = false;
        return;
    }

    cur->ws_info->test_proc_trg.auto_trg_enabled = true;

    if (cur->nwk_bootstrap_state == ER_ACTIVE_SCAN) {
        if (cur->ws_info->trickle_pas_running) {
            if (cur->ws_info->test_proc_trg.pas_trigger_timer > seconds) {
                cur->ws_info->test_proc_trg.pas_trigger_timer -= seconds;
            } else  {
                if (cur->ws_info->test_proc_trg.pas_trigger_count > 2) {
                    ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PAS_TRICKLE_INCON);
                } else {
                    cur->ws_info->test_proc_trg.pas_trigger_count++;
                    ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PAS);
                }
                cur->ws_info->test_proc_trg.pas_trigger_timer = (cur->ws_info->trickle_params_pan_discovery.Imin / 10);
            }
            if (cur->ws_info->test_proc_trg.eapol_trigger_timer > seconds) {
                cur->ws_info->test_proc_trg.eapol_trigger_timer -= seconds;
            } else {
                ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_EAPOL);
                cur->ws_info->test_proc_trg.eapol_trigger_timer = (cur->ws_info->trickle_params_pan_discovery.Imin / 10) / 2;
            }
        }
    } else if (cur->nwk_bootstrap_state == ER_SCAN) {
        if (cur->ws_info->trickle_pcs_running) {
            if (cur->ws_info->test_proc_trg.pcs_trigger_timer > seconds) {
                cur->ws_info->test_proc_trg.pcs_trigger_timer -= seconds;
            } else  {
                if (cur->ws_info->test_proc_trg.pcs_trigger_count > 2) {
                    ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PCS_TRICKLE_INCON);
                } else {
                    cur->ws_info->test_proc_trg.pcs_trigger_count++;
                    ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_PCS);
                }
                cur->ws_info->test_proc_trg.pcs_trigger_timer = (cur->ws_info->trickle_params_pan_discovery.Imin / 10);
            }
        }
    } else if (cur->nwk_bootstrap_state == ER_RPL_SCAN) {
        if (cur->ws_info->test_proc_trg.dis_trigger_timer > seconds) {
            cur->ws_info->test_proc_trg.dis_trigger_timer -= seconds;
        } else  {
            ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_DIS);
            cur->ws_info->test_proc_trg.dis_trigger_timer_val *= 2;
            if (cur->ws_info->test_proc_trg.dis_trigger_timer_val > (WS_RPL_DIS_INITIAL_TIMEOUT / 10) * 4) {
                cur->ws_info->test_proc_trg.dis_trigger_timer_val = (WS_RPL_DIS_INITIAL_TIMEOUT / 10) * 4;
            }
            cur->ws_info->test_proc_trg.dis_trigger_timer = cur->ws_info->test_proc_trg.dis_trigger_timer_val;
        }
        if (cur->ws_info->test_proc_trg.rpl_trigger_timer > seconds) {
            cur->ws_info->test_proc_trg.rpl_trigger_timer -= seconds;
        } else  {
            ws_bootstrap_test_procedure_trigger_exec(cur, PROCEDURE_RPL);
            cur->ws_info->test_proc_trg.rpl_trigger_timer_val *= 2;
            if (cur->ws_info->test_proc_trg.rpl_trigger_timer_val > (WS_RPL_DIS_INITIAL_TIMEOUT / 10) * 2) {
                cur->ws_info->test_proc_trg.rpl_trigger_timer_val = (WS_RPL_DIS_INITIAL_TIMEOUT / 10) * 2;
            }
            cur->ws_info->test_proc_trg.rpl_trigger_timer = cur->ws_info->test_proc_trg.rpl_trigger_timer_val;
        }
    } else {
        cur->ws_info->test_proc_trg.dis_trigger_timer_val = (WS_RPL_DIS_INITIAL_TIMEOUT / 10) / 2;
        cur->ws_info->test_proc_trg.rpl_trigger_timer_val = (WS_RPL_DIS_INITIAL_TIMEOUT / 10) / 2;
        cur->ws_info->test_proc_trg.pas_trigger_count = 0;
        cur->ws_info->test_proc_trg.pcs_trigger_count = 0;
    }
}

#endif //HAVE_WS
