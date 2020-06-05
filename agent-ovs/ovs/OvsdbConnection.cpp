/* -*- C++ -*-; c-basic-offset: 4; indent-tabs-mode: nil */
/*
 * Implementation of ovsdb messages for engine
 *
 * Copyright (c) 2019 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

/* This must be included before anything else */
#if HAVE_CONFIG_H
#  include <config.h>
#endif

extern "C" {
#include <lib/dirs.h>
}

#include "ovs/include/OvsdbConnection.h"
#include "OvsdbMonitorMessage.h"
#include <opflexagent/logging.h>

namespace opflexagent {

mutex OvsdbConnection::ovsdbMtx;

void OvsdbConnection::send_req_cb(uv_async_t* handle) {
    unique_lock<mutex> lock(OvsdbConnection::ovsdbMtx);
    auto* reqCbd = (req_cb_data*)handle->data;
    const shared_ptr<OvsdbMessage>& req = reqCbd->req;
    yajr::rpc::MethodName method(req->getMethod().c_str());
    opflex::jsonrpc::PayloadWrapper wrapper(req.get());
    yajr::rpc::OutboundRequest outr =
        yajr::rpc::OutboundRequest(wrapper, &method, req->getReqId(), reqCbd->peer);
    outr.send();
    delete(reqCbd);
}

void OvsdbConnection::on_writeq_async(uv_async_t* handle) {
    auto* conn = (OvsdbConnection*)handle->data;
    conn->processWriteQueue();
}

void OvsdbConnection::sendTransaction(const list<OvsdbTransactMessage>& requests, Transaction* trans) {
    uint64_t reqId = 0;
    {
        unique_lock<mutex> lock(transactionMutex);
        reqId = getNextId();
        transactions[reqId] = trans;
    }
    auto* reqCbd = new req_cb_data();
    reqCbd->req = std::make_shared<TransactReq>(requests, reqId);
    reqCbd->peer = getPeer();
    send_req_async.data = (void*)reqCbd;
    uv_async_send(&send_req_async);
}

void OvsdbConnection::start() {
    LOG(DEBUG) << "Starting .....";
    unique_lock<mutex> lock(OvsdbConnection::ovsdbMtx);
    client_loop = threadManager.initTask("OvsdbConnection");
    yajr::initLoop(client_loop);
    uv_async_init(client_loop,&connect_async, connect_cb);
    uv_async_init(client_loop, &send_req_async, send_req_cb);
    writeq_async.data = this;
    uv_async_init(client_loop, &writeq_async, on_writeq_async);

    threadManager.startTask("OvsdbConnection");
}

void OvsdbConnection::connect_cb(uv_async_t* handle) {
    unique_lock<mutex> lock(OvsdbConnection::ovsdbMtx);
    auto* ocp = (OvsdbConnection*)handle->data;
    if (ocp->ovsdbUseLocalTcpPort) {
        ocp->peer = yajr::Peer::create("127.0.0.1",
                                       "6640",
                                       on_state_change,
                                       ocp, loop_selector, false);
        ocp->remote_peer = "127.0.0.1:6640";
    } else {
        std::string swPath;
        swPath.append(ovs_rundir()).append("/db.sock");
        ocp->peer = yajr::Peer::create(swPath, on_state_change,
                                       ocp, loop_selector, false);
        ocp->remote_peer = swPath;
    }
    assert(ocp->peer);
}

void OvsdbConnection::stop() {
    uv_close((uv_handle_t*)&connect_async, nullptr);
    uv_close((uv_handle_t*)&send_req_async, nullptr);
    uv_close((uv_handle_t*)&writeq_async, nullptr);
    if (peer) {
        peer->destroy();
    }
    yajr::finiLoop(client_loop);
    threadManager.stopTask("OvsdbConnection");
}

 void OvsdbConnection::on_state_change(yajr::Peer * p, void * data,
                     yajr::StateChange::To stateChange,
                     int error) {
    auto* conn = (OvsdbConnection*)data;
    switch (stateChange) {
        case yajr::StateChange::CONNECT: {
            conn->setConnected(true);
            p->startKeepAlive(0, 5000, 60000);
            // OVSDB monitor call
            conn->syncMsgsRemaining = 6;
            list<string> bridgeColumns = {}; //{"ports", "netflow", "ipfix", "mirrors"};
            auto message = new OvsdbMonitorMessage(OvsdbTable::BRIDGE, bridgeColumns, conn->getNextId());
            conn->sendMessage(message, false);
            list<string> portColumns = {}; // {"interfaces"};
            message = new OvsdbMonitorMessage(OvsdbTable::PORT, portColumns, conn->getNextId());
            conn->sendMessage(message, false);
            list<string> interfaceColumns = {}; //{"type", "options"};
            message = new OvsdbMonitorMessage(OvsdbTable::INTERFACE, interfaceColumns, conn->getNextId());
            conn->sendMessage(message, false);
            list<string> mirrorColumns = {};//{"select_src_port", "select_dst_port", "output_port"};
            message = new OvsdbMonitorMessage(OvsdbTable::MIRROR, mirrorColumns, conn->getNextId());
            conn->sendMessage(message, false);
            list<string> netflowColumns = {};//{"targets", "active_timeout", "add_id_to_interface"};
            message = new OvsdbMonitorMessage(OvsdbTable::NETFLOW, netflowColumns, conn->getNextId());
            conn->sendMessage(message, false);
            list<string> ipfixColumns = {};//{"targets", "sampling", "other_config"};
            message = new OvsdbMonitorMessage(OvsdbTable::IPFIX, ipfixColumns, conn->getNextId());
            conn->sendMessage(message, false);
        }
            break;
        case yajr::StateChange::DISCONNECT:
            conn->setConnected(false);
            LOG(INFO) << "Disconnected";
            break;
        case yajr::StateChange::TRANSPORT_FAILURE:
            conn->setConnected(false);
            LOG(ERROR) << "SSL Connection error";
            break;
        case yajr::StateChange::FAILURE:
            conn->setConnected(false);
            LOG(ERROR) << "Connection error: " << uv_strerror(error);
            break;
        case yajr::StateChange::DELETE:
            conn->setConnected(false);
            LOG(INFO) << "Connection closed";
            break;
    }
}

uv_loop_t* OvsdbConnection::loop_selector(void* data) {
    auto* conn = (OvsdbConnection*)data;
    return conn->client_loop;
}

void OvsdbConnection::connect() {
    if (!connected) {
        connect_async.data = this;
        uv_async_send(&connect_async);
    }
}

void OvsdbConnection::disconnect() {
    // TODO
}

void OvsdbConnection::handleTransaction(uint64_t reqId, const Document& payload) {
    unique_lock<mutex> lock(transactionMutex);
    auto iter = transactions.find(reqId);
    if (iter != transactions.end()) {
        iter->second->handleTransaction(reqId, payload);
        transactions.erase(iter);
    }
}

void OvsdbConnection::handleTransactionError(uint64_t reqId, const Document& payload) {
    unique_lock<mutex> lock(transactionMutex);
    auto iter = transactions.find(reqId);
    if (iter != transactions.end()) {
        transactions.erase(iter);
    }

    if (payload.HasMember("error")) {
        StringBuffer buffer;
        Writer<StringBuffer> writer(buffer);
        payload.Accept(writer);
        LOG(WARNING) << "Received error response for reqId " << reqId << " - " << buffer.GetString();
    } else {
        LOG(WARNING) << "Received error response with no error element";
    }
}

void populateValues(const Value& value, map<string, string>& values) {
    assert(value.IsArray());
    if (value.GetArray().Size() == 2) {
        if (value[0].IsString()) {
            std::string arrayType = value[0].GetString();
            if (arrayType == "uuid" && value[1].IsString()) {
                values[value[1].GetString()];
            } else if (arrayType == "set" && value[1].IsArray()) {
                for (Value::ConstValueIterator memberItr = value[1].GetArray().Begin();
                     memberItr != value[1].GetArray().End(); ++memberItr) {
                    if (memberItr->IsArray()) {
                        if (memberItr->GetArray().Size() == 2) {
                            if (memberItr->GetArray()[1].IsString()) {
                                values[memberItr->GetArray()[1].GetString()];
                            } else {
                                LOG(WARNING) << "member type = " << memberItr->GetArray()[1].GetType();
                            }
                        }
                    }
                }
            } else if (arrayType == "map") {
                for (Value::ConstValueIterator memberItr = value[1].GetArray().Begin();
                     memberItr != value[1].GetArray().End(); ++memberItr) {
                    if (memberItr->IsArray()) {
                        for (Value::ConstValueIterator mapMemberItr = value[1].GetArray().Begin();
                             mapMemberItr != value[1].GetArray().End(); ++mapMemberItr) {
                            if (mapMemberItr->GetArray().Size() == 2) {
                                if (mapMemberItr->GetArray()[0].IsString() &&
                                    mapMemberItr->GetArray()[1].IsString()) {
                                    values[memberItr->GetArray()[0].GetString()] = memberItr->GetArray()[1].GetString();
                                } else {
                                    LOG(WARNING) << "map key type = " << mapMemberItr->GetArray()[0].GetType();
                                    LOG(WARNING) << "map value type = " << mapMemberItr->GetArray()[0].GetType();
                                }
                            }
                        }
                    }
                }
            } else {
                LOG(WARNING) << "Unexpected array type of " << arrayType;
            }
        }
    }
}

void processRowUpdate(const Value& value, OvsdbRowDetails& rowDetails) {
    for (Value::ConstMemberIterator itr = value.MemberBegin();
         itr != value.MemberEnd(); ++itr) {
        if (itr->name.IsString() && itr->value.IsObject()) {
            for (Value::ConstMemberIterator propItr = itr->value.MemberBegin();
                 propItr != itr->value.MemberEnd(); ++propItr) {
                if (propItr->name.IsString()) {
                    const std::string propName = propItr->name.GetString();
                    if (propItr->value.IsString()) {
                        std::string stringValue = propItr->value.GetString();
                        rowDetails[propName] = TupleData("", stringValue);
                    } else if (propItr->value.IsArray()) {
                        map<string, string> items;
                        populateValues(propItr->value, items);
                    } else if (propItr->value.IsInt()) {
                        int intValue = propItr->value.GetInt();
                        rowDetails[propName] = TupleData("", intValue);
                    } else if (propItr->value.IsBool()) {
                        bool boolValue = propItr->value.GetBool();
                        rowDetails[propName] = TupleData("", boolValue);
                    }
                }
            }
        }
    }
}

void OvsdbConnection::handleMonitor(uint64_t reqId, const Document& payload) {
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    payload.Accept(writer);
    LOG(DEBUG) << "received monitor response with payload " << buffer.GetString();

    if (payload.IsObject()) {
        OvsdbTableDetails tableState;
        if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::BRIDGE))) {
            const Value& bridgeValue = payload[OvsdbMessage::toString(OvsdbTable::BRIDGE)];
            if (bridgeValue.IsObject()) {
                for (Value::ConstMemberIterator itr = bridgeValue.MemberBegin();
                     itr != bridgeValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        OvsdbRowDetails rowDetails;
                        std::string uuid = itr->name.GetString();
                        rowDetails["uuid"] = TupleData("", uuid);
                        std::string bridgeName;
                        processRowUpdate(itr->value, rowDetails);
                        if (rowDetails.find("name") != rowDetails.end()) {
                            bridgeName = rowDetails["name"].getStringValue();
                            // use bridge name as key as that's the most common lookup
                            tableState[bridgeName] = rowDetails;
                        } else {
                             LOG(WARNING) << "Dropping bridge with no name";
                        }
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::BRIDGE, tableState);
            }
        } else if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::IPFIX))) {
            const Value& ipfixValue = payload[OvsdbMessage::toString(OvsdbTable::IPFIX)];
            if (ipfixValue.IsObject()) {
                for (Value::ConstMemberIterator itr = ipfixValue.MemberBegin();
                     itr != ipfixValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        std::string uuid = itr->name.GetString();
                        OvsdbRowDetails rowDetails;
                        rowDetails["uuid"] = TupleData("", uuid);
                        processRowUpdate(itr->value, rowDetails);
                        tableState[uuid] = rowDetails;
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::IPFIX, tableState);
            }
        } else if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::NETFLOW))) {
            const Value& netflowValue = payload[OvsdbMessage::toString(OvsdbTable::NETFLOW)];
            if (netflowValue.IsObject()) {
                for (Value::ConstMemberIterator itr = netflowValue.MemberBegin();
                     itr != netflowValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        std::string uuid = itr->name.GetString();
                        OvsdbRowDetails rowDetails;
                        rowDetails["uuid"] = TupleData("", uuid);
                        processRowUpdate(itr->value, rowDetails);
                        tableState[uuid] = rowDetails;
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::NETFLOW, tableState);
            }
        } else if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::MIRROR))) {
            const Value& mirrorValue = payload[OvsdbMessage::toString(OvsdbTable::MIRROR)];
            if (mirrorValue.IsObject()) {
                for (Value::ConstMemberIterator itr = mirrorValue.MemberBegin();
                     itr != mirrorValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        std::string uuid = itr->name.GetString();
                        OvsdbRowDetails rowDetails;
                        rowDetails["uuid"] = TupleData("", uuid);
                        processRowUpdate(itr->value, rowDetails);
                        tableState[uuid] = rowDetails;
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::MIRROR, tableState);
            }
        } else if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::PORT))) {
            const Value& portValue = payload[OvsdbMessage::toString(OvsdbTable::PORT)];
            if (portValue.IsObject()) {
                for (Value::ConstMemberIterator itr = portValue.MemberBegin();
                     itr != portValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        OvsdbRowDetails rowDetails;
                        std::string uuid = itr->name.GetString();
                        rowDetails["uuid"] = TupleData("", uuid);
                        processRowUpdate(itr->value, rowDetails);
                        tableState[uuid] = rowDetails;
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::PORT, tableState);
            }
        } else if (payload.HasMember(OvsdbMessage::toString(OvsdbTable::INTERFACE))) {
            const Value& interfaceValue = payload[OvsdbMessage::toString(OvsdbTable::INTERFACE)];
            if (interfaceValue.IsObject()) {
                for (Value::ConstMemberIterator itr = interfaceValue.MemberBegin();
                     itr != interfaceValue.MemberEnd(); ++itr) {
                    if (itr->name.IsString() && itr->value.IsObject()) {
                        OvsdbRowDetails rowDetails;
                        std::string uuid = itr->name.GetString();
                        rowDetails["uuid"] = TupleData("", uuid);
                        processRowUpdate(itr->value, rowDetails);
                        tableState[uuid] = rowDetails;
                    }
                }
                ovsdbState.fullUpdate(OvsdbTable::INTERFACE, tableState);
            }
        } else if (!payload.ObjectEmpty()) {
            LOG(WARNING) << "Unhandled monitor";
        }
    }
    decrSyncMsgsRemaining();
}

void OvsdbConnection::handleMonitorError(uint64_t reqId, const Document& payload) {
    if (payload.HasMember("error")) {
        StringBuffer buffer;
        Writer<StringBuffer> writer(buffer);
        payload.Accept(writer);
        LOG(WARNING) << "Received error response for reqId " << reqId << " - " << buffer.GetString();
    } else {
        LOG(WARNING) << "Received error response with no error element";
    }
}

void OvsdbConnection::handleUpdate(const Document& payload) {
    StringBuffer buffer;
    Writer<StringBuffer> writer(buffer);
    payload.Accept(writer);
    LOG(DEBUG) << "Received update - " << buffer.GetString();
}

void OvsdbConnection::messagesReady() {
    uv_async_send(&writeq_async);
}

}
