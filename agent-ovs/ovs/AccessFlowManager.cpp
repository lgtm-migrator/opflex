/*
 * Copyright (c) 2016 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#include "AccessFlowManager.h"
#include <opflexagent/QosManager.h>
#include "CtZoneManager.h"
#include "FlowBuilder.h"
#include "FlowUtils.h"
#include "FlowConstants.h"
#include "RangeMask.h"
#include "eth.h"
#include "ip.h"
#include <opflexagent/logging.h>

#include <boost/system/error_code.hpp>
#include <boost/algorithm/string/find_iterator.hpp>
#include <boost/algorithm/string/finder.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/algorithm/string/classification.hpp>
#include "ovs-ofputil.h"
#include <modelgbp/gbp/DirectionEnumT.hpp>
#include <modelgbp/gbp/ConnTrackEnumT.hpp>

#include <string>
#include <vector>
#include <sstream>

namespace opflexagent {

using std::string;
using std::vector;
using std::unordered_set;
using std::shared_ptr;
using opflex::modb::URI;
typedef EndpointListener::uri_set_t uri_set_t;
using boost::algorithm::make_split_iterator;
using boost::algorithm::token_finder;
using boost::algorithm::is_any_of;
using boost::copy_range;
using boost::optional;

static const char* ID_NAMESPACES[] =
    {"secGroup", "secGroupSet"};

static const int VMM_DOMAIN_DN_PARTS = 4;
static const char* ID_NMSPC_SECGROUP     = ID_NAMESPACES[0];
static const char* ID_NMSPC_SECGROUP_SET = ID_NAMESPACES[1];

void AccessFlowManager::populateTableDescriptionMap(
        SwitchManager::TableDescriptionMap &fwdTblDescr) {
    // Populate descriptions of flow tables
#define TABLE_DESC(table_id, table_name, drop_reason) \
        fwdTblDescr.insert( \
                    std::make_pair(table_id, \
                            std::make_pair(table_name, drop_reason)));
    TABLE_DESC(SERVICE_BYPASS_TABLE_ID, "SERVICE_BYPASS_TABLE",
               "Skip security-group checks for Service loopback traffic")
    TABLE_DESC(GROUP_MAP_TABLE_ID, "GROUP_MAP_TABLE", "Access port incorrect")
    TABLE_DESC(SYS_SEC_GRP_IN_TABLE_ID, "SYS_SEC_GRP_IN_TABLE_ID",
            "Ingress System Security group derivation missing/incorrect")
    TABLE_DESC(SEC_GROUP_IN_TABLE_ID, "SEC_GROUP_IN_TABLE",
            "Ingress Security group derivation missing/incorrect")
    TABLE_DESC(SYS_SEC_GRP_OUT_TABLE_ID, "SYS_SEC_GRP_OUT_TABLE_ID",
            "Egress System Security group derivation missing/incorrect")
    TABLE_DESC(SEC_GROUP_OUT_TABLE_ID, "SEC_GROUP_OUT_TABLE",
            "Egress security group missing/incorrect")
    TABLE_DESC(TAP_TABLE_ID, "TAP_TABLE",
            "Tap missing/incorrect")
    TABLE_DESC(OUT_TABLE_ID, "OUT_TABLE", "Output port missing/incorrect")
#undef TABLE_DESC
}

AccessFlowManager::AccessFlowManager(Agent& agent_,
                                     SwitchManager& switchManager_,
                                     IdGenerator& idGen_,
                                     CtZoneManager& ctZoneManager_)
    : agent(agent_), switchManager(switchManager_), idGen(idGen_),
      ctZoneManager(ctZoneManager_), taskQueue(agent.getAgentIOService()),
      conntrackEnabled(false), stopping(false), dropLogRemotePort(0) {
    // set up flow tables
    switchManager.setMaxFlowTables(NUM_FLOW_TABLES);
    SwitchManager::TableDescriptionMap fwdTblDescr;
    populateTableDescriptionMap(fwdTblDescr);
    switchManager.setForwardingTableList(fwdTblDescr);
}

static string getSecGrpSetId(const uri_set_t& secGrps) {
   std::stringstream ss;
   bool notfirst = false;
   for (const URI& uri : secGrps) {
       if (notfirst) ss << ",";
       notfirst = true;
       ss << uri.toString();
   }
   return ss.str();
}

void AccessFlowManager::enableConnTrack() {
    conntrackEnabled = true;
}

void AccessFlowManager::start() {
    switchManager.getPortMapper().registerPortStatusListener(this);
    agent.getEndpointManager().registerListener(this);
    agent.getLearningBridgeManager().registerListener(this);
    agent.getPolicyManager().registerListener(this);
    agent.getExtraConfigManager().registerListener(this);
    agent.getQosManager().registerListener(this);

    for (size_t i = 0; i < sizeof(ID_NAMESPACES)/sizeof(char*); i++) {
        idGen.initNamespace(ID_NAMESPACES[i]);
    }

    createStaticFlows();
}

void AccessFlowManager::stop() {
    stopping = true;
    switchManager.getPortMapper().unregisterPortStatusListener(this);
    agent.getEndpointManager().unregisterListener(this);
    agent.getLearningBridgeManager().unregisterListener(this);
    agent.getPolicyManager().unregisterListener(this);
    agent.getQosManager().unregisterListener(this);
}

void AccessFlowManager::endpointUpdated(const string& uuid) {
    if (stopping) return;
    taskQueue.dispatch(uuid, [=](){ handleEndpointUpdate(uuid); });
}

void AccessFlowManager::dscpQosUpdated(const string& interface, uint8_t dscp) {
    if (stopping) return;
    taskQueue.dispatch(interface, [=]() { handleDscpQosUpdate(interface, dscp); });
}

void AccessFlowManager::secGroupSetUpdated(const uri_set_t& secGrps) {
    if (stopping) return;
    const string id = getSecGrpSetId(secGrps);
    taskQueue.dispatch("set:" + id,
                       [=]() { handleSecGrpSetUpdate(secGrps, id); });
}

void AccessFlowManager::configUpdated(const opflex::modb::URI& configURI) {
    if (stopping) return;
    switchManager.enableSync();
}

void AccessFlowManager::secGroupUpdated(const opflex::modb::URI& uri) {
    if (stopping) return;
    taskQueue.dispatch("secgrp:" + uri.toString(),
                       [=]() { handleSecGrpUpdate(uri); });
}

void AccessFlowManager::portStatusUpdate(const string& portName,
                                         uint32_t portNo, bool) {
    if (stopping) return;
    agent.getAgentIOService()
        .dispatch([=]() { handlePortStatusUpdate(portName, portNo); });
}

void AccessFlowManager::setDropLog(const string& dropLogPort, const string& dropLogRemoteIp,
        const uint16_t _dropLogRemotePort) {
    dropLogIface = dropLogPort;
    boost::system::error_code ec;
    address tunDst = address::from_string(dropLogRemoteIp, ec);
    if (ec) {
        LOG(ERROR) << "Invalid drop-log tunnel destination IP: "
                   << dropLogRemoteIp << ": " << ec.message();
    } else if (tunDst.is_v6()) {
        LOG(ERROR) << "IPv6 drop-log tunnel destinations are not supported";
    } else {
        dropLogDst = tunDst;
        LOG(INFO) << "DropLog port set to " << dropLogPort
                   << " tunnel destination: " << dropLogRemoteIp
                   << ":" <<_dropLogRemotePort;
    }
    dropLogRemotePort = _dropLogRemotePort;
}

static FlowEntryPtr flowEmptySecGroup(uint32_t emptySecGrpSetId) {
    FlowBuilder noSecGrp;
    flowutils::match_group(noSecGrp,
                           PolicyManager::MAX_POLICY_RULE_PRIORITY,
                           emptySecGrpSetId, 0);
    noSecGrp.action().go(AccessFlowManager::TAP_TABLE_ID);
    return noSecGrp.build();
}

static uint64_t getPushVlanMeta(std::shared_ptr<const Endpoint>& ep) {
    return ep->isAccessAllowUntagged() ?
        flow::meta::access_out::UNTAGGED_AND_PUSH_VLAN :
        flow::meta::access_out::PUSH_VLAN;
}

static void flowBypassDhcpRequest(FlowEntryList& el, bool v4,
                                  bool skip_pop_vlan, uint32_t inport,
                                  uint32_t outport,
                                  std::shared_ptr<const Endpoint>& ep ) {
    FlowBuilder fb;
    if (ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        fb.priority(201).inPort(inport);
    } else {
        fb.priority(200).inPort(inport);
    }

    flowutils::match_dhcp_req(fb, v4);
    fb.action().reg(MFF_REG7, outport);

    if (ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        fb.vlan(ep->getAccessIfaceVlan().get());
        fb.action().metadata(flow::meta::access_out::POP_VLAN|
                             flow::meta::access_meta::EGRESS_DIR,
                             flow::meta::ACCESS_MASK);
    }

    if(!ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        fb.action().metadata(flow::meta::access_meta::EGRESS_DIR,
                             flow::meta::access_meta::MASK);
    }

    if (skip_pop_vlan) {
        fb.tci(0, 0x1fff);
        fb.action().metadata(flow::meta::access_meta::EGRESS_DIR,
                             flow::meta::access_meta::MASK);
    }

    fb.action().go(AccessFlowManager::TAP_TABLE_ID);
    fb.build(el);
}

static void flowBypassFloatingIP(FlowEntryList& el, uint32_t inport,
                                 uint32_t outport, bool in,
                                 bool skip_pop_vlan,
                                 address& floatingIp,
                                 std::shared_ptr<const Endpoint>& ep ) {
    FlowBuilder fb;
    if (ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        fb.priority(201).inPort(inport);
    } else {
        fb.priority(200).inPort(inport);
    }

    if (floatingIp.is_v4()) {
        fb.ethType(eth::type::IP);
    } else {
        fb.ethType(eth::type::IPV6);
    }

    if (in) {
        fb.ipSrc(floatingIp);
    } else {
        fb.ipDst(floatingIp);
    }

    fb.action().reg(MFF_REG7, outport);
    if (ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        if (in) {
            fb.action()
              .reg(MFF_REG5, ep->getAccessIfaceVlan().get())
              .metadata((getPushVlanMeta(ep)|flow::meta::access_meta::INGRESS_DIR),
                        flow::meta::ACCESS_MASK);
        } else {
            fb.vlan(ep->getAccessIfaceVlan().get());
            fb.action()
              .metadata((flow::meta::access_out::POP_VLAN|
                        flow::meta::access_meta::EGRESS_DIR),
                        flow::meta::ACCESS_MASK);
        }
    }

    if(!ep->getAccessIfaceVlan() && !skip_pop_vlan) {
        fb.action()
          .metadata((in?flow::meta::access_meta::INGRESS_DIR:
                    flow::meta::access_meta::EGRESS_DIR),
                    flow::meta::access_meta::MASK);

    }

    if (skip_pop_vlan) {
        if(!in) {
            fb.tci(0, 0x1fff);
        }
        fb.action()
          .metadata((in?flow::meta::access_meta::INGRESS_DIR:
                    flow::meta::access_meta::EGRESS_DIR),
                    flow::meta::access_meta::MASK);
    }

    fb.action().go(AccessFlowManager::TAP_TABLE_ID);
    fb.build(el);
}

static void flowBypassServiceIP(FlowEntryList& el,
                                uint32_t accessPort, uint32_t uplinkPort,
                                std::shared_ptr<const Endpoint>& ep ) {

    for (const string& epipStr : ep->getIPs()) {
        network::cidr_t cidr;
        if (!network::cidr_from_string(epipStr, cidr, false))
            continue;
        for (const string& svcipStr : ep->getServiceIPs()) {
            boost::system::error_code ec;
            address serviceAddr =
                address::from_string(svcipStr, ec);
            if (ec) continue;

            FlowBuilder ingress, egress;
            ingress.priority(10)
                   .ethType(eth::type::IP)
                   .inPort(uplinkPort)
                   .ipSrc(serviceAddr)
                   .ipDst(cidr.first, cidr.second)
                   .action()
                   .reg(MFF_REG7, accessPort);
            if (ep->getAccessIfaceVlan()) {
                ingress.action()
                       .reg(MFF_REG5, ep->getAccessIfaceVlan().get())
                       .metadata((flow::meta::access_out::PUSH_VLAN|
                                  flow::meta::access_meta::INGRESS_DIR),
                                 flow::meta::ACCESS_MASK);
            } else {
                ingress.action()
                       .metadata(flow::meta::access_meta::INGRESS_DIR,
                                 flow::meta::access_meta::MASK);
            }
            ingress.action().go(AccessFlowManager::TAP_TABLE_ID);
            ingress.build(el);

            egress.priority(10)
                  .ethType(eth::type::IP)
                  .inPort(accessPort)
                  .ipSrc(cidr.first, cidr.second)
                  .ipDst(serviceAddr)
                  .action()
                  .reg(MFF_REG7, uplinkPort);
            if (ep->getAccessIfaceVlan()) {
                egress.vlan(ep->getAccessIfaceVlan().get());
                egress.action()
                      .metadata((flow::meta::access_out::POP_VLAN|
                                flow::meta::access_meta::EGRESS_DIR),
                                flow::meta::ACCESS_MASK);
            } else {
                egress.tci(0, 0x1fff);
                egress.action()
                      .metadata(flow::meta::access_meta::EGRESS_DIR,
                                flow::meta::access_meta::MASK);
            }
            egress.action().go(AccessFlowManager::TAP_TABLE_ID);
            egress.build(el);
        }
    }
}

void AccessFlowManager::createStaticFlows() {
    LOG(DEBUG) << "Writing static flows";
    {
        FlowEntryList outFlows;
        FlowBuilder()
            .priority(1)
            .metadata(flow::meta::access_out::POP_VLAN,
                      flow::meta::out::MASK)
            .tci(0x1000, 0x1000)
            .action()
            .popVlan().outputReg(MFF_REG7)
            .parent().build(outFlows);
        FlowBuilder()
            .priority(1)
            .metadata(flow::meta::access_out::PUSH_VLAN,
                      flow::meta::out::MASK)
            .action()
            .pushVlan().regMove(MFF_REG5, MFF_VLAN_VID).outputReg(MFF_REG7)
            .parent().build(outFlows);
        /*
         * The packet is replicated for a specical case of
         * Openshift bootstrap that does not use vlan 4094
         * This is ugly but they do not have iproute2/tc
         * installed to do this in a cleaner way
         * This duplication only happens when endpoint
         * file has "access-interface-vlan" attr
         */
        FlowBuilder()
            .priority(1)
            .metadata(flow::meta::access_out::UNTAGGED_AND_PUSH_VLAN,
                      flow::meta::out::MASK)
            .action()
            .outputReg(MFF_REG7)
            .pushVlan().regMove(MFF_REG5, MFF_VLAN_VID).outputReg(MFF_REG7)
            .parent().build(outFlows);
        outFlows.push_back(flowutils::default_out_flow());

        switchManager.writeFlow("static", OUT_TABLE_ID, outFlows);
    }
    {
        TlvEntryList tlvFlows;
        for(int i = 0; i <= 10; i++) {
            FlowBuilder().tlv(0xffff, i, 4, i).buildTlv(tlvFlows);
        }
        FlowBuilder().tlv(0xffff, 11, 16, 11).buildTlv(tlvFlows);
        FlowBuilder().tlv(0xffff, 12, 4, 12).buildTlv(tlvFlows);
        FlowBuilder().tlv(0xffff, 13, 4, 13).buildTlv(tlvFlows);
        FlowBuilder().tlv(0xffff, 14, 8, 14).buildTlv(tlvFlows);
        switchManager.writeTlv("DropLogStatic", tlvFlows);
    }
    {
        FlowEntryList dropLogFlows;
        FlowBuilder().priority(0)
                .action().go(SERVICE_BYPASS_TABLE_ID)
                .parent().build(dropLogFlows);
        switchManager.writeFlow("static", DROP_LOG_TABLE_ID, dropLogFlows);
        /* Insert a flow at the end of every table to match dropped packets
         * and go to the drop table where it will be punted to a port when configured
         */
        for(unsigned table_id = SERVICE_BYPASS_TABLE_ID; table_id < EXP_DROP_TABLE_ID; table_id++) {
            FlowEntryList dropLogFlow;
            FlowBuilder().priority(0).cookie(flow::cookie::TABLE_DROP_FLOW)
                    .flags(OFPUTIL_FF_SEND_FLOW_REM)
                    .action().dropLog(table_id)
                    .go(EXP_DROP_TABLE_ID)
                    .parent().build(dropLogFlow);
            switchManager.writeFlow("DropLogFlow", table_id, dropLogFlow);
        }
        handleDropLogPortUpdate();
    }
    {
        FlowEntryList skipServiceFlows;
        FlowBuilder().priority(1)
            .action().go(GROUP_MAP_TABLE_ID)
            .parent().build(skipServiceFlows);
        switchManager.writeFlow("static", SERVICE_BYPASS_TABLE_ID, skipServiceFlows);
    }

    {
        FlowEntryList tapFlows;
        /*For now make the DNS punt flows static*/
        FlowBuilder().priority(2).cookie(flow::cookie::DNS_RESPONSE_V4)
            .ethType(eth::type::IP)
            .proto(ip::type::TCP)
            .tpSrc(tcp::type::DNS)
            .metadata(flow::meta::access_meta::INGRESS_DIR,
                      flow::meta::access_meta::MASK)
            .action().controller()
            .go(OUT_TABLE_ID)
            .parent().build(tapFlows);
        FlowBuilder().priority(2).cookie(flow::cookie::DNS_RESPONSE_V6)
            .ethType(eth::type::IPV6)
            .proto(ip::type::TCP)
            .tpSrc(tcp::type::DNS)
            .metadata(flow::meta::access_meta::INGRESS_DIR,
                      flow::meta::access_meta::MASK)
            .action().controller()
            .go(OUT_TABLE_ID)
            .parent().build(tapFlows);
        FlowBuilder().priority(2).cookie(flow::cookie::DNS_RESPONSE_V4)
            .ethType(eth::type::IP)
            .proto(ip::type::UDP)
            .tpSrc(udp::type::DNS)
            .metadata(flow::meta::access_meta::INGRESS_DIR,
                      flow::meta::access_meta::MASK)
            .action().controller()
            .go(OUT_TABLE_ID)
            .parent().build(tapFlows);
        FlowBuilder().priority(2).cookie(flow::cookie::DNS_RESPONSE_V6)
            .ethType(eth::type::IPV6)
            .proto(ip::type::UDP)
            .tpSrc(udp::type::DNS)
            .metadata(flow::meta::access_meta::INGRESS_DIR,
                      flow::meta::access_meta::MASK)
            .action().controller()
            .go(OUT_TABLE_ID)
            .parent().build(tapFlows);
        FlowBuilder().priority(1)
            .action().go(OUT_TABLE_ID)
            .parent().build(tapFlows);
        switchManager.writeFlow("static", TAP_TABLE_ID, tapFlows);
    }
    {
        FlowEntryList defaultSysIngressFlow;
        FlowBuilder().priority(1)
            .action().go(SEC_GROUP_IN_TABLE_ID)
            .parent().build(defaultSysIngressFlow);
        switchManager.writeFlow("static", SYS_SEC_GRP_IN_TABLE_ID, defaultSysIngressFlow);

    }
    {
        FlowEntryList defaultSysEgressFlow;
        FlowBuilder().priority(1)
            .action().go(SEC_GROUP_OUT_TABLE_ID)
            .parent().build(defaultSysEgressFlow);
        switchManager.writeFlow("static", SYS_SEC_GRP_OUT_TABLE_ID, defaultSysEgressFlow);

    }

    // everything is allowed for endpoints with no security group set
    uint32_t emptySecGrpSetId = idGen.getId(ID_NMSPC_SECGROUP_SET, "");
    switchManager.writeFlow("static", SEC_GROUP_OUT_TABLE_ID,
                            flowEmptySecGroup(emptySecGrpSetId));
    switchManager.writeFlow("static", SEC_GROUP_IN_TABLE_ID,
                            flowEmptySecGroup(emptySecGrpSetId));
}

void AccessFlowManager::handleEndpointUpdate(const string& uuid) {
    LOG(DEBUG) << "Updating endpoint " << uuid;
    shared_ptr<const Endpoint> ep =
        agent.getEndpointManager().getEndpoint(uuid);
    if (!ep) {
        switchManager.clearFlows(uuid, GROUP_MAP_TABLE_ID);
        switchManager.clearFlows(uuid, SERVICE_BYPASS_TABLE_ID);
        if (conntrackEnabled)
            ctZoneManager.erase(uuid);
        return;
    }

    uint32_t accessPort = OFPP_NONE;
    uint32_t uplinkPort = OFPP_NONE;
    const optional<string>& accessIface = ep->getAccessInterface();
    const optional<string>& uplinkIface = ep->getAccessUplinkInterface();
    if (accessIface)
        accessPort = switchManager.getPortMapper().FindPort(accessIface.get());
    if (uplinkIface) {
        uplinkPort = switchManager.getPortMapper().FindPort(uplinkIface.get());
    }

    uint32_t secGrpSetId = idGen.getId(ID_NMSPC_SECGROUP_SET,
                                       getSecGrpSetId(ep->getSecurityGroups()));
    uint16_t zoneId = -1;
    if (conntrackEnabled) {
        zoneId = ctZoneManager.getId(uuid);
        if (zoneId == static_cast<uint16_t>(-1))
            LOG(ERROR) << "Could not allocate connection tracking zone for "
                       << uuid;
    }

    MaskList trunkVlans;
    if (ep->getInterfaceName()) {
        LearningBridgeManager& lbMgr = agent.getLearningBridgeManager();
        std::unordered_set<std::string> lbiUuids;
        lbMgr.getLBIfaceByIface(ep->getInterfaceName().get(), lbiUuids);

        for (auto& lbiUuid : lbiUuids) {
            auto iface = lbMgr.getLBIface(lbiUuid);
            if (!iface) continue;

            for (auto& range : iface->getTrunkVlans()) {
                MaskList masks;
                RangeMask::getMasks(range.first, range.second, masks);
                trunkVlans.insert(trunkVlans.end(),
                                  masks.begin(), masks.end());
            }
        }
    }

    FlowEntryList el;
    FlowEntryList skipServiceFlows;

    if (accessPort != OFPP_NONE && uplinkPort != OFPP_NONE) {
        {
            FlowBuilder in;
            in.priority(100).inPort(accessPort);
            if (zoneId != static_cast<uint16_t>(-1))
                in.action()
                    .reg(MFF_REG6, zoneId);

            in.action()
                .reg(MFF_REG0, secGrpSetId)
                .reg(MFF_REG7, uplinkPort);

            if (ep->getAccessIfaceVlan()) {
                in.vlan(ep->getAccessIfaceVlan().get());
                in.action()
                  .metadata((flow::meta::access_out::POP_VLAN|
                             flow::meta::access_meta::EGRESS_DIR),
                            flow::meta::ACCESS_MASK);
            } else {
                in.tci(0, 0x1fff);
                in.action()
                  .metadata(flow::meta::access_meta::EGRESS_DIR,
                            flow::meta::access_meta::MASK);
            }

            in.action().go(SYS_SEC_GRP_OUT_TABLE_ID);
            in.build(el);

        }

        /*
         * When an endpoint that is backend for a service is
         * reaching its own service IP we skip security group
         * checks
         */
        flowBypassServiceIP(skipServiceFlows, accessPort,
                            uplinkPort, ep);

        /*
         * We allow without tags to handle Openshift bootstrap
         */
        if (ep->isAccessAllowUntagged() && ep->getAccessIfaceVlan()) {
            FlowBuilder inSkipVlan;

            inSkipVlan.priority(99).inPort(accessPort)
                      .tci(0, 0x1fff);
            if (zoneId != static_cast<uint16_t>(-1))
                inSkipVlan.action()
                    .reg(MFF_REG6, zoneId);

            inSkipVlan.action()
                .reg(MFF_REG0, secGrpSetId)
                .reg(MFF_REG7, uplinkPort)
                .metadata(flow::meta::access_meta::EGRESS_DIR,
                          flow::meta::access_meta::MASK)
                .go(SYS_SEC_GRP_OUT_TABLE_ID);
            inSkipVlan.build(el);
        }

        /*
         * Allow DHCP request to bypass the access bridge policy when
         * virtual DHCP is enabled.
         * We allow both with / without tags to handle Openshift
         * bootstrap
         */
        optional<Endpoint::DHCPv4Config> v4c = ep->getDHCPv4Config();
        if (v4c) {
            flowBypassDhcpRequest(el, true, false, accessPort,
                                  uplinkPort, ep);
            if (ep->isAccessAllowUntagged() && ep->getAccessIfaceVlan())
                flowBypassDhcpRequest(el, true, true, accessPort,
                                      uplinkPort, ep);
        }

        optional<Endpoint::DHCPv6Config> v6c = ep->getDHCPv6Config();
        if(v6c) {
            flowBypassDhcpRequest(el, false, false, accessPort,
                                  uplinkPort, ep);
            if (ep->isAccessAllowUntagged() && ep->getAccessIfaceVlan())
                flowBypassDhcpRequest(el, false, true, accessPort,
                                      uplinkPort, ep);
        }

        {
            FlowBuilder out;
            if (zoneId != static_cast<uint16_t>(-1))
                out.action()
                    .reg(MFF_REG6, zoneId);

            out.priority(100).inPort(uplinkPort)
                .action()
                .reg(MFF_REG0, secGrpSetId)
                .reg(MFF_REG7, accessPort);
            if (ep->getAccessIfaceVlan()) {
                out.action()
                   .reg(MFF_REG5, ep->getAccessIfaceVlan().get())
                   .metadata((getPushVlanMeta(ep)|flow::meta::access_meta::INGRESS_DIR),
                             flow::meta::ACCESS_MASK);
            } else {
                out.action()
                   .metadata(flow::meta::access_meta::INGRESS_DIR,
                             flow::meta::access_meta::MASK);
            }
            out.action().go(SYS_SEC_GRP_IN_TABLE_ID);
            out.build(el);
        }

        // Bypass the access bridge for ports trunked by learning
        // bridge interfaces.
        for (const Mask& m : trunkVlans) {
            uint16_t tci = 0x1000 | m.first;
            uint16_t mask = 0x1000 | (0xfff & m.second);
            FlowBuilder().priority(500).inPort(accessPort)
                .tci(tci, mask)
                .action()
                .output(uplinkPort)
                .parent().build(el);
            FlowBuilder().priority(500).inPort(uplinkPort)
                .tci(tci, mask)
                .action()
                .output(accessPort)
                .parent().build(el);
        }

        // Bypass conntrack from endpoint reaching its
        // floating ips.
        for(const Endpoint::IPAddressMapping& ipm :
                ep->getIPAddressMappings()) {
            if (!ipm.getMappedIP() || !ipm.getEgURI())
                continue;

	    boost::system::error_code ec;
            address mappedIp =
                address::from_string(ipm.getMappedIP().get(), ec);
            if (ec) continue;

            address floatingIp;
            if (ipm.getFloatingIP()) {
                floatingIp =
                    address::from_string(ipm.getFloatingIP().get(), ec);
                if (ec) continue;
                if (floatingIp.is_v4() != mappedIp.is_v4()) continue;
                if (floatingIp.is_unspecified()) continue;
            }
            flowBypassFloatingIP(el, accessPort, uplinkPort, false,
                                 false, floatingIp, ep);
            flowBypassFloatingIP(el, uplinkPort, accessPort, true,
                                 false, floatingIp, ep);
            /*
             * We allow both with / without tags to handle Openshift
             * bootstrap
             */
            if (ep->isAccessAllowUntagged() && ep->getAccessIfaceVlan()) {
                flowBypassFloatingIP(el, accessPort, uplinkPort, false,
                                     true, floatingIp, ep);
                flowBypassFloatingIP(el, uplinkPort, accessPort, true,
                                     true, floatingIp, ep);
            }
        }
    }
    switchManager.writeFlow(uuid, GROUP_MAP_TABLE_ID, el);
    switchManager.writeFlow(uuid, SERVICE_BYPASS_TABLE_ID, skipServiceFlows);
}

void AccessFlowManager::handleDscpQosUpdate(const string& interface, uint8_t dscp) {
    string objIdV4 = interface + string("ipv4");
    string objIdV6 = interface + string("ipv6");
    switchManager.clearFlows(objIdV4, 0);
    switchManager.clearFlows(objIdV6, 0);

    if (dscp == 0) {
        return ;
    }

    LOG(DEBUG) << "add-flow-dscp : " << interface;
    uint32_t ofPort = switchManager.getPortMapper().FindPort(interface);
    FlowEntryList dscpFlowV4;
    FlowBuilder()
        .table(0)
        .priority(65535)
        .ethType(eth::type::IP)
        .inPort(ofPort)
        .action()
        .setDscp(dscp)
        .resubmit(ofPort,1)
        .parent().build(dscpFlowV4);
    switchManager.writeFlow(objIdV4, 0, dscpFlowV4);

    FlowEntryList dscpFlowV6;
    FlowBuilder()
        .table(0)
        .priority(65535)
        .ethType(eth::type::IPV6)
        .inPort(ofPort)
        .action()
        .setDscp(dscp)
        .resubmit(ofPort,1)
        .parent().build(dscpFlowV6);
    switchManager.writeFlow(objIdV6, 0, dscpFlowV6);
}

void AccessFlowManager::handleDropLogPortUpdate() {
    FlowEntryList catchDropFlows;
    if(dropLogIface.empty() || !dropLogDst.is_v4()) {
        switchManager.clearFlows("static", EXP_DROP_TABLE_ID);
        LOG(WARNING) << "Ignoring dropLog port " << dropLogIface
        << " " << dropLogDst;
        return;
    }
    int dropLogPort = switchManager.getPortMapper().FindPort(dropLogIface);
    if(dropLogPort != OFPP_NONE) {
        FlowBuilder().priority(0)
                .metadata(flow::meta::DROP_LOG, flow::meta::DROP_LOG)
                .action()
                .reg(MFF_TUN_DST, dropLogDst.to_v4().to_ulong())
                .output(dropLogPort)
                .parent().build(catchDropFlows);
        switchManager.writeFlow("static", EXP_DROP_TABLE_ID, catchDropFlows);
    }
}

void AccessFlowManager::handlePortStatusUpdate(const string& portName,
                                               uint32_t) {
    unordered_set<std::string> eps;
    LOG(DEBUG) << "Port-status update for " << portName;
    agent.getEndpointManager().getEndpointsByAccessIface(portName, eps);
    agent.getEndpointManager().getEndpointsByAccessUplink(portName, eps);
    for (const std::string& ep : eps)
        endpointUpdated(ep);
    if(portName == dropLogIface) {
        handleDropLogPortUpdate();
    }
}

void AccessFlowManager::handleSecGrpUpdate(const opflex::modb::URI& uri) {
    unordered_set<uri_set_t> secGrpSets;
    agent.getEndpointManager().getSecGrpSetsForSecGrp(uri, secGrpSets);
    for (const uri_set_t& secGrpSet : secGrpSets)
        secGroupSetUpdated(secGrpSet);
}

bool AccessFlowManager::checkIfSystemSecurityGroup(const string& uri){
    const string& opflex_domain = agent.getPolicyManager().getOpflexDomain();
    std::vector<std::string> parts;
    std::string domain_name;

    std::string systemSgName = "_SystemSecurityGroup";

    split(parts, opflex_domain, boost::is_any_of("/"));
    if (parts.size() == VMM_DOMAIN_DN_PARTS){
        const string& ctrlr = parts[2];
        std::vector<std::string> ctrlr_parts;
        split(ctrlr_parts, ctrlr, boost::is_any_of("-"));
        if (ctrlr_parts.size() == 3){
            domain_name  = ctrlr_parts[2];
            // eg: SG010197146194_SystemSecurityGroup
            systemSgName = domain_name + "_SystemSecurityGroup";
        }
    }
    if (uri.find(systemSgName) != std::string::npos){
        return true;
    }
    return false;
}

void AccessFlowManager::handleSecGrpSetUpdate(const uri_set_t& secGrps,
                                              const string& secGrpsIdStr) {
    using modelgbp::gbpe::L24Classifier;
    using modelgbp::gbp::DirectionEnumT;
    using modelgbp::gbp::ConnTrackEnumT;
    using flowutils::CA_REFLEX_REV_ALLOW;
    using flowutils::CA_REFLEX_REV_TRACK;
    using flowutils::CA_REFLEX_FWD;
    using flowutils::CA_ALLOW;
    using flowutils::CA_REFLEX_FWD_TRACK;
    using flowutils::CA_REFLEX_FWD_EST;
    using flowutils::CA_REFLEX_REV_RELATED;

    LOG(DEBUG) << "Updating security group set \"" << secGrpsIdStr << "\"";

    if (agent.getEndpointManager().secGrpSetEmpty(secGrps)) {
        switchManager.clearFlows(secGrpsIdStr, SEC_GROUP_IN_TABLE_ID);
        switchManager.clearFlows(secGrpsIdStr, SEC_GROUP_OUT_TABLE_ID);
        switchManager.clearFlows(secGrpsIdStr, SYS_SEC_GRP_IN_TABLE_ID);
        switchManager.clearFlows(secGrpsIdStr, SYS_SEC_GRP_OUT_TABLE_ID);
        return;
    }

    uint32_t secGrpSetId = idGen.getId(ID_NMSPC_SECGROUP_SET, secGrpsIdStr);

    FlowEntryList secGrpIn;
    FlowEntryList secGrpOut;
    FlowEntryList sysSecGrpIn;
    FlowEntryList sysSecGrpOut;

    bool any_system_sec_rule_configured = false;

    for (const opflex::modb::URI& secGrp : secGrps) {
        PolicyManager::rule_list_t rules;
        agent.getPolicyManager().getSecGroupRules(secGrp, rules);

        bool isSystemRule = false;
        int ingress_table = SEC_GROUP_IN_TABLE_ID;
        int egress_table = SEC_GROUP_OUT_TABLE_ID;
        int after_ingress_table = TAP_TABLE_ID;
        int after_egress_table = TAP_TABLE_ID;
        
        FlowEntryList *secGrpInRef = &secGrpIn;
        FlowEntryList *secGrpOutRef = &secGrpOut;

        bool system_sec_group = false;
        std::string uri (secGrp.toString());

        if (checkIfSystemSecurityGroup(secGrp.toString())){
            system_sec_group = true;

            ingress_table = SYS_SEC_GRP_IN_TABLE_ID;
            egress_table = SYS_SEC_GRP_OUT_TABLE_ID;

            after_ingress_table = SEC_GROUP_IN_TABLE_ID;
            after_egress_table = SEC_GROUP_OUT_TABLE_ID;
        
            secGrpInRef = &sysSecGrpIn;
            secGrpOutRef = &sysSecGrpOut;
        }

        for (shared_ptr<PolicyRule>& pc : rules) {
            if (system_sec_group){
                any_system_sec_rule_configured = true;
                isSystemRule = true;
            }

            uint8_t dir = pc->getDirection();
            bool skipL34 = false;
            const shared_ptr<L24Classifier>& cls = pc->getL24Classifier();
            const URI& ruleURI = cls.get()->getURI();
            uint64_t secGrpCookie =
                idGen.getId("l24classifierRule", ruleURI.toString());
            boost::optional<const network::subnets_t&> remoteSubs;
            boost::optional<const network::service_ports_t&> namedSvcPorts;
            if (!pc->getRemoteSubnets().empty() || !pc->getNamedServicePorts().empty()) {
                remoteSubs = pc->getRemoteSubnets();
                namedSvcPorts = pc->getNamedServicePorts();
            } else {
                skipL34 = !agent.addL34FlowsWithoutSubnet();
                LOG(DEBUG) << "skipL34 flows: " << skipL34
                           << " for rule: " << ruleURI;
            }

            bool log = false;
            flowutils::ClassAction act = flowutils::CA_DENY;

            if (pc->getAllow()) {
                if (cls->getConnectionTracking(ConnTrackEnumT::CONST_NORMAL) ==
                    ConnTrackEnumT::CONST_REFLEXIVE) {
                    act = CA_REFLEX_FWD;
                } else {
                    act = CA_ALLOW;
                }
            }

            if (pc->getLog()) {
                log = pc->getLog();
            }
            /*
             * Do not program higher level protocols
             * when remote subnet is missing
             * except when agent.addL34FlowsWithoutSubnet() == true
             */
            if (skipL34) {
                if (dir == DirectionEnumT::CONST_BIDIRECTIONAL ||
                    dir == DirectionEnumT::CONST_IN) {
                    if (act == flowutils::CA_DENY) {
                         flowutils::add_l2classifier_entries(*cls, act, log,
                                                            EXP_DROP_TABLE_ID, ingress_table,
                                                            EXP_DROP_TABLE_ID,
                                                            pc->getPriority(),
                                                            OFPUTIL_FF_SEND_FLOW_REM,
                                                            secGrpCookie,
                                                            secGrpSetId, 0,
                                                            isSystemRule,
                                                            *secGrpInRef);
                    } else {
                         flowutils::add_l2classifier_entries(*cls, act, log,
                                                             after_ingress_table, ingress_table,
                                                             EXP_DROP_TABLE_ID,
                                                             pc->getPriority(),
                                                             OFPUTIL_FF_SEND_FLOW_REM,
                                                             secGrpCookie,
                                                             secGrpSetId, 0,
                                                             isSystemRule,
                                                             *secGrpInRef);
                    }
                }
                if (dir == DirectionEnumT::CONST_BIDIRECTIONAL ||
                    dir == DirectionEnumT::CONST_OUT) {
                    if (act == flowutils::CA_DENY) {
                         flowutils::add_l2classifier_entries(*cls, act, log,
                                                            EXP_DROP_TABLE_ID, egress_table,
                                                            EXP_DROP_TABLE_ID,
                                                            pc->getPriority(),
                                                            OFPUTIL_FF_SEND_FLOW_REM,
                                                            secGrpCookie,
                                                            secGrpSetId, 0,
                                                            isSystemRule,
                                                            *secGrpOutRef);
                    } else {
                         flowutils::add_l2classifier_entries(*cls, act, log,
                                                             after_egress_table, egress_table,
                                                             EXP_DROP_TABLE_ID,
                                                             pc->getPriority(),
                                                             OFPUTIL_FF_SEND_FLOW_REM,
                                                             secGrpCookie,
                                                             secGrpSetId, 0, 
                                                             isSystemRule,
                                                             *secGrpOutRef);
                      }
                }
                continue;
            }

            if (dir == DirectionEnumT::CONST_BIDIRECTIONAL ||
                dir == DirectionEnumT::CONST_IN) {
                if (act == flowutils::CA_DENY) {
                         flowutils::add_classifier_entries(*cls, act, log,
                                                          remoteSubs,
                                                          boost::none,
                                                          boost::none,
                                                          EXP_DROP_TABLE_ID, ingress_table,
                                                          EXP_DROP_TABLE_ID,
                                                          pc->getPriority(),
                                                          OFPUTIL_FF_SEND_FLOW_REM,
                                                          secGrpCookie,
                                                          secGrpSetId, 0,
                                                          isSystemRule,
                                                          *secGrpInRef);
                }  else {
                         flowutils::add_classifier_entries(*cls, act, log,
                                                           remoteSubs,
                                                           boost::none,
                                                           boost::none,
                                                           after_ingress_table, ingress_table,
                                                           EXP_DROP_TABLE_ID,
                                                           pc->getPriority(),
                                                           OFPUTIL_FF_SEND_FLOW_REM,
                                                           secGrpCookie,
                                                           secGrpSetId, 0,
                                                           isSystemRule,
                                                           *secGrpInRef);
                   }
                if (act == CA_REFLEX_FWD) {
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_FWD_TRACK, log,
                                                      remoteSubs,
                                                      boost::none,
                                                      boost::none,
                                                      GROUP_MAP_TABLE_ID, ingress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpInRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_FWD_EST, log,
                                                      remoteSubs,
                                                      boost::none,
                                                      boost::none,
                                                      after_ingress_table, ingress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpInRef);
                    // add reverse entries for reflexive classifier
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_TRACK, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      GROUP_MAP_TABLE_ID, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      0,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_ALLOW, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      after_egress_table, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_RELATED, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      after_egress_table, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                }
            }
            if (dir == DirectionEnumT::CONST_BIDIRECTIONAL ||
                dir == DirectionEnumT::CONST_OUT) {
                if (act == flowutils::CA_DENY) {
                    flowutils::add_classifier_entries(*cls, act, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      EXP_DROP_TABLE_ID, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                } else {
                      flowutils::add_classifier_entries(*cls, act, log,
                                                        boost::none,
                                                        remoteSubs,
                                                        namedSvcPorts,
                                                        after_egress_table, egress_table,
                                                        EXP_DROP_TABLE_ID,
                                                        pc->getPriority(),
                                                        OFPUTIL_FF_SEND_FLOW_REM,
                                                        secGrpCookie,
                                                        secGrpSetId, 0,
                                                        isSystemRule,
                                                        *secGrpOutRef);
                  }
                if (act == CA_REFLEX_FWD) {
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_FWD_TRACK, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      GROUP_MAP_TABLE_ID, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_FWD_EST, log,
                                                      boost::none,
                                                      remoteSubs,
                                                      namedSvcPorts,
                                                      after_egress_table, egress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpOutRef);
                    // add reverse entries for reflexive classifier
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_TRACK, log,
                                                      remoteSubs,
                                                      boost::none,
                                                      boost::none,
                                                      GROUP_MAP_TABLE_ID, ingress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      0,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpInRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_ALLOW, log,
                                                      remoteSubs,
                                                      boost::none,
                                                      boost::none,
                                                      after_ingress_table, ingress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpInRef);
                    flowutils::add_classifier_entries(*cls, CA_REFLEX_REV_RELATED, log,
                                                      remoteSubs,
                                                      boost::none,
                                                      boost::none,
                                                      after_ingress_table, ingress_table,
                                                      EXP_DROP_TABLE_ID,
                                                      pc->getPriority(),
                                                      OFPUTIL_FF_SEND_FLOW_REM,
                                                      secGrpCookie,
                                                      secGrpSetId, 0,
                                                      isSystemRule,
                                                      *secGrpInRef);
                }
            }
        }
    }

    switchManager.writeFlow(secGrpsIdStr, SEC_GROUP_IN_TABLE_ID, secGrpIn);
    switchManager.writeFlow(secGrpsIdStr, SEC_GROUP_OUT_TABLE_ID, secGrpOut);

    if (any_system_sec_rule_configured){
        /*
         * Configure drop flows to drop packets not matching any system
         * security group rules.
         * */
        FlowEntryList dropLogFlowIn;
        FlowBuilder().priority(2).cookie(flow::cookie::TABLE_DROP_FLOW)
            .flags(OFPUTIL_FF_SEND_FLOW_REM)
            .action().dropLog(SYS_SEC_GRP_IN_TABLE_ID)
            .go(EXP_DROP_TABLE_ID)
            .parent().build(dropLogFlowIn);
        switchManager.writeFlow("SystemDropLogFlow", SYS_SEC_GRP_IN_TABLE_ID, dropLogFlowIn);

        FlowEntryList dropLogFlowOut;
        FlowBuilder().priority(2).cookie(flow::cookie::TABLE_DROP_FLOW)
            .flags(OFPUTIL_FF_SEND_FLOW_REM)
            .action().dropLog(SYS_SEC_GRP_OUT_TABLE_ID)
            .go(EXP_DROP_TABLE_ID)
            .parent().build(dropLogFlowOut);
        switchManager.writeFlow("SystemDropLogFlow", SYS_SEC_GRP_OUT_TABLE_ID, dropLogFlowOut);

        /*
         * Configure system security group rules.
         * */
        switchManager.writeFlow(secGrpsIdStr, SYS_SEC_GRP_IN_TABLE_ID, sysSecGrpIn);
        switchManager.writeFlow(secGrpsIdStr, SYS_SEC_GRP_OUT_TABLE_ID, sysSecGrpOut);
    } else{

        /* Delete all flows in System security  group tables except static flows.
         * Static flows will simply forward packets to usual security group tables.
         * */
        switchManager.clearFlows(secGrpsIdStr, SYS_SEC_GRP_IN_TABLE_ID);
        switchManager.clearFlows(secGrpsIdStr, SYS_SEC_GRP_OUT_TABLE_ID);

        switchManager.clearFlows("SystemDropLogFlow", SYS_SEC_GRP_IN_TABLE_ID);
        switchManager.clearFlows("SystemDropLogFlow", SYS_SEC_GRP_OUT_TABLE_ID);
    }
}

void AccessFlowManager::lbIfaceUpdated(const std::string& uuid) {
    LOG(DEBUG) << "Updating learning bridge interface " << uuid;

    LearningBridgeManager& lbMgr = agent.getLearningBridgeManager();
    shared_ptr<const LearningBridgeIface> iface = lbMgr.getLBIface(uuid);

    if (!iface)
        return;

    if (iface->getInterfaceName()) {
        EndpointManager& epMgr = agent.getEndpointManager();
        std::unordered_set<std::string> epUuids;
        epMgr.getEndpointsByIface(iface->getInterfaceName().get(), epUuids);

        for (auto& epUuid : epUuids) {
            endpointUpdated(epUuid);
        }
    }
}

void AccessFlowManager::rdConfigUpdated(const opflex::modb::URI& rdURI) {
//Interface not used
}

void AccessFlowManager::packetDropLogConfigUpdated(const opflex::modb::URI& dropLogCfgURI) {
    if (stopping) return;
    using modelgbp::observer::DropLogConfig;
    using modelgbp::observer::DropLogModeEnumT;
    FlowEntryList dropLogFlows;
    optional<shared_ptr<DropLogConfig>> dropLogCfg =
            DropLogConfig::resolve(agent.getFramework(), dropLogCfgURI);
    if(!dropLogCfg) {
        FlowBuilder().priority(2)
                .action().go(SERVICE_BYPASS_TABLE_ID)
                .parent().build(dropLogFlows);
        switchManager.writeFlow("DropLogConfig", DROP_LOG_TABLE_ID, dropLogFlows);
        LOG(INFO) << "Defaulting to droplog disabled";
        return;
    }
    if(dropLogCfg.get()->getDropLogEnable(0) != 0) {
        if(dropLogCfg.get()->getDropLogMode(
                    DropLogModeEnumT::CONST_UNFILTERED_DROP_LOG) ==
           DropLogModeEnumT::CONST_UNFILTERED_DROP_LOG) {
            FlowBuilder().priority(2)
                    .action()
                    .metadata(flow::meta::DROP_LOG,
                              flow::meta::DROP_LOG)
                    .go(SERVICE_BYPASS_TABLE_ID)
                    .parent().build(dropLogFlows);
            LOG(INFO) << "Droplog mode set to unfiltered";
        } else {
            switchManager.clearFlows("DropLogConfig", DROP_LOG_TABLE_ID);
            LOG(INFO) << "Droplog mode set to filtered";
            return;
        }
    } else {
        FlowBuilder().priority(2)
                .action()
                .go(SERVICE_BYPASS_TABLE_ID)
                .parent().build(dropLogFlows);
        LOG(INFO) << "Droplog disabled";
    }
    switchManager.writeFlow("DropLogConfig", DROP_LOG_TABLE_ID, dropLogFlows);
}

void AccessFlowManager::packetDropFlowConfigUpdated(const opflex::modb::URI& dropFlowCfgURI) {
    if (stopping) return;
    using modelgbp::observer::DropFlowConfig;
    optional<shared_ptr<DropFlowConfig>> dropFlowCfg =
            DropFlowConfig::resolve(agent.getFramework(), dropFlowCfgURI);
    if(!dropFlowCfg) {
        switchManager.clearFlows(dropFlowCfgURI.toString(), DROP_LOG_TABLE_ID);
        return;
    }
    FlowEntryList dropLogFlows;
    FlowBuilder fb;
    fb.priority(1);
    if(dropFlowCfg.get()->isEthTypeSet()) {
        fb.ethType(dropFlowCfg.get()->getEthType(0));
    }
    if(dropFlowCfg.get()->isInnerSrcAddressSet()) {
	address addr = address::from_string(
                dropFlowCfg.get()->getInnerSrcAddress(""));
        fb.ipSrc(addr);
    }
    if(dropFlowCfg.get()->isInnerDstAddressSet()) {
        address addr = address::from_string(
                dropFlowCfg.get()->getInnerDstAddress(""));
        fb.ipDst(addr);
    }
    if(dropFlowCfg.get()->isOuterSrcAddressSet()) {
        address addr = address::from_string(
                dropFlowCfg.get()->getOuterSrcAddress(""));
        fb.outerIpSrc(addr);
    }
    if(dropFlowCfg.get()->isOuterDstAddressSet()) {
        address addr = address::from_string(
                dropFlowCfg.get()->getOuterDstAddress(""));
        fb.outerIpDst(addr);
    }
    if(dropFlowCfg.get()->isTunnelIdSet()) {
        fb.tunId(dropFlowCfg.get()->getTunnelId(0));
    }
    if(dropFlowCfg.get()->isIpProtoSet()) {
        fb.proto(dropFlowCfg.get()->getIpProto(0));
    }
    if(dropFlowCfg.get()->isSrcPortSet()) {
        fb.tpSrc(dropFlowCfg.get()->getSrcPort(0));
    }
    if(dropFlowCfg.get()->isDstPortSet()) {
        fb.tpDst(dropFlowCfg.get()->getDstPort(0));
    }
    fb.action().metadata(flow::meta::DROP_LOG, flow::meta::DROP_LOG)
            .go(SERVICE_BYPASS_TABLE_ID).parent().build(dropLogFlows);
    switchManager.writeFlow(dropFlowCfgURI.toString(), DROP_LOG_TABLE_ID,
            dropLogFlows);
}

static bool secGrpSetIdGarbageCb(EndpointManager& endpointManager,
                                 const string& str) {
    uri_set_t secGrps;

    typedef boost::algorithm::split_iterator<string::const_iterator> ssi;
    ssi it = make_split_iterator(str, token_finder(is_any_of(",")));

    for(; it != ssi(); ++it) {
        string uri(copy_range<string>(*it));
        if (!uri.empty())
            secGrps.insert(URI(uri));
    }

    if (secGrps.empty()) return true;
    return !endpointManager.secGrpSetEmpty(secGrps);
}

void AccessFlowManager::cleanup() {
    using namespace modelgbp::gbp;

    auto gcb1 = [=](const std::string& ns,
                    const std::string& str) -> bool {
        return IdGenerator::uriIdGarbageCb<SecGroup>(agent.getFramework(),
                                                     ns, str);
    };
    idGen.collectGarbage(ID_NMSPC_SECGROUP, gcb1);

    auto gcb2 = [=](const std::string&,
                    const std::string& str) -> bool {
        return secGrpSetIdGarbageCb(agent.getEndpointManager(), str);
    };
    idGen.collectGarbage(ID_NMSPC_SECGROUP_SET, gcb2);
}

} // namespace opflexagent
