/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Utility functions for flow tables
 *
 * Copyright (c) 2016 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#ifndef OPFLEXAGENT_FLOWCONSTANTS_H
#define OPFLEXAGENT_FLOWCONSTANTS_H

#include <cstdint>

namespace opflexagent {
namespace flow {

namespace cookie {
/**
 * The cookie used for flows that direct neighbor discovery
 * packets to the controller
 */
extern const uint64_t NEIGH_DISC;

/**
 * The cookie used for flows that direct DHCPv4 packets to the
 * controller
 */
extern const uint64_t DHCP_V4;

/**
 * The cookie used for flows that direct DHCPv6 packets to the
 * controller
 */
extern const uint64_t DHCP_V6;

/**
 * The cookie used for flows that direct virtual IPv4 announcement
 * packets to the controller
 */
extern const uint64_t VIRTUAL_IP_V4;

/**
 * The cookie used for flows that direct virtual IPv6 announcement
 * packets to the controller
 */
extern const uint64_t VIRTUAL_IP_V6;

/**
 * The cookie used for flows that direct ICMPv4 error messages that
 * require body translation to the controller
 */
extern const uint64_t ICMP_ERROR_V4;

/**
 * The cookie used for flows that direct ICMPv6 error messages that
 * require body translation to the controller
 */
extern const uint64_t ICMP_ERROR_V6;

/**
 * The cookie used for flows for responding to ICMPv4 echo requests
 */
extern const uint64_t ICMP_ECHO_V4;

/**
 * The cookie used for flows for responding to ICMPv6 echo requests
 */
extern const uint64_t ICMP_ECHO_V6;

/**
 * The cookie used for flows for counting per RD drops in policy table
 */
extern const uint64_t RD_POL_DROP_FLOW;

/**
 * The cookie used for flows for per table drops.
 */
extern const uint64_t TABLE_DROP_FLOW;

/**
 * The cookie used for flows to capture DNS v4 response packets
 */
extern const uint64_t DNS_RESPONSE_V4;

/**
 * The cookie used for flows to capture DNS v6 response packets
 */
extern const uint64_t DNS_RESPONSE_V6;
} // namespace cookie

namespace meta {

/**
 * "Policy applied" bit.  Indicates that policy has been applied
 * already for this flow
 */
extern const uint64_t POLICY_APPLIED;

/**
 * Indicates that a flow comes from a service interface.  Will go
 * through normal forwarding pipeline but should bypass policy.
 */
extern const uint64_t FROM_SERVICE_INTERFACE;

/**
 * Indicates that a packet has been routed and is allowed to hairpin
 */
extern const uint64_t ROUTED;

/**
 * Indicates that if this packet is dropped, then it should be logged
 */
extern const uint64_t DROP_LOG;

namespace out {

/**
 * the OUT_MASK specifies 8 bits that indicate the action to take
 * in the output table.  If nothing is set, then the action is to
 * output to the interface in REG7
 */
extern const uint64_t MASK;

/**
 * Resubmit to the first "dest" table with the source registers
 * set to the corresponding values for the EPG in REG7
 */
extern const uint64_t RESUBMIT_DST;

/**
 * Perform "outbound" NAT action and then resubmit with the source
 * EPG set to the mapped NAT EPG
 */
extern const uint64_t NAT;

/**
 * Output to the interface in REG7 but intercept ICMP error
 * replies and overwrite the encapsulated error packet source
 * address with the (rewritten) destination address of the outer
 * packet.
 */
extern const uint64_t REV_NAT;

/**
 * Output to the tunnel destination appropriate for the EPG
 */
extern const uint64_t TUNNEL;

/**
 * Output to the flood group appropriate for the EPG
 */
extern const uint64_t FLOOD;

/**
 * Output to the tunnel destination specified in the output register
 */
extern const uint64_t REMOTE_TUNNEL;

/**
 * Output to the veth_host_ac destination specified in output register
 */
extern const uint64_t HOST_ACCESS;

/**
 * Remote tunnel to a proxy
 */
extern const uint64_t REMOTE_TUNNEL_PROXY;

/**
 * Bounce to a remote tunnel on same port as input to CSR
 */
extern const uint64_t REMOTE_TUNNEL_BOUNCE_TO_CSR;

/**
 * Bounce to a remote tunnel on same port as input to node
 */
extern const uint64_t REMOTE_TUNNEL_BOUNCE_TO_NODE;
} // namespace out

namespace access_meta {

const uint64_t MASK =0x0300;
/**
 * Ingress to ep
 */
const uint64_t INGRESS_DIR = 0x100;
/**
 * Egress from ep
 */
const uint64_t EGRESS_DIR = 0x200;

} // namespace access_meta

namespace access_out {

/**
 * Pop the VLAN tag
 */
const uint64_t POP_VLAN = 0x1;

/**
 * Push the VLAN tag stored in REG5
 */
const uint64_t PUSH_VLAN = 0x2;

/**
 * Replicate the packet both untagged followed by tagged
 */
const uint64_t UNTAGGED_AND_PUSH_VLAN = 0x3;

} // namespace access_out

extern const uint64_t ACCESS_MASK;
} // namespace meta

} // namespace flow
} // namespace opflexagent

#endif /* OPFLEXAGENT_FLOWCONSTANTS_H */
