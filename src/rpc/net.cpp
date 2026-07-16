// Copyright (c) 2009-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rpc/server.h>

#include <atomic>
#include <future>
#include <addrman.h>
#include <addrman_impl.h>
#include <banman.h>
#include <chainparams.h>
#include <clientversion.h>
#include <common/args.h>
#include <core_io.h>
#include <hash.h>
#include <net_permissions.h>
#include <net_processing.h>
#include <net/api_protocol.h>
#include <net/inference_engine.h>
#include <net_types.h>
#include <netbase.h>
#include <node/context.h>
#ifdef ENABLE_EMBEDDED_ASMAP
#include <node/data/ip_asn.dat.h>
#endif
#include <node/protocol_version.h>
#include <node/warnings.h>
#include <policy/settings.h>
#include <protocol.h>
#include <rpc/blockchain.h>
#include <rpc/protocol.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <rpc/net_rpc.h>
#include <sync.h>
#include <univalue.h>
#include <util/asmap.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <validation.h>

#include <escrow/escrow.h>
#include <billing/billing_receipt.h>
#include <rpc/escrow_rpc.h>
#include <model/registry.h>

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#ifndef SOCKET
#define SOCKET int
#endif
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#endif

using node::NodeContext;
using util::Join;

const std::vector<std::string> CONNECTION_TYPE_DOC{
 "outbound-full-relay (default automatic connections)",
 "block-relay-only (does not relay transactions or addresses)",
 "inbound (initiated by the peer)",
 "manual (added via addnode RPC or -addnode/-connect configuration options)",
 "addr-fetch (short-lived automatic connection for soliciting addresses)",
 "feeler (short-lived automatic connection for testing addresses)",
 "private-broadcast (short-lived automatic connection for broadcasting privacy-sensitive transactions)"
};

const std::vector<std::string> TRANSPORT_TYPE_DOC{
 "detecting (peer could be v1 or v2)",
 "v1 (plaintext transport protocol)",
 "v2 (BIP324 encrypted transport protocol)"
};

static RPCMethod getconnectioncount()
{
 return RPCMethod{
 "getconnectioncount",
 "Returns the number of connections to other nodes.\n",
 {},
 RPCResult{
 RPCResult::Type::NUM, "", "The connection count"
 },
 RPCExamples{
 HelpExampleCli("getconnectioncount", "")
 + HelpExampleRpc("getconnectioncount", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const CConnman& connman = EnsureConnman(node);

 return connman.GetNodeCount(ConnectionDirection::Both);
},
 };
}

static RPCMethod ping()
{
 return RPCMethod{
 "ping",
 "Requests that a ping be sent to all other nodes, to measure ping time.\n"
 "Results are provided in getpeerinfo.\n"
 "Ping command is handled in queue with all other commands, so it measures processing backlog, not just network ping.\n",
 {},
 RPCResult{RPCResult::Type::NONE, "", ""},
 RPCExamples{
 HelpExampleCli("ping", "")
 + HelpExampleRpc("ping", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 PeerManager& peerman = EnsurePeerman(node);

 // Request that each node send a ping during next message processing pass
 peerman.SendPings();
 return UniValue::VNULL;
},
 };
}

/** Returns, given services flags, a list of humanly readable (known) network services */
static UniValue GetServicesNames(ServiceFlags services)
{
 UniValue servicesNames(UniValue::VARR);

 for (const auto& flag : serviceFlagsToStr(services)) {
 servicesNames.push_back(flag);
 }

 return servicesNames;
}

static RPCMethod getpeerinfo()
{
 return RPCMethod{
 "getpeerinfo",
 "Returns data about each connected network peer as a json array of objects.",
 {},
 RPCResult{
 RPCResult::Type::ARR, "", "",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {
 {RPCResult::Type::NUM, "id", "Peer index"},
 {RPCResult::Type::STR, "addr", "(host:port) The IP address/hostname optionally followed by :port of the peer"},
 {RPCResult::Type::STR, "addrbind", /*optional=*/true, "(ip:port) Bind address of the connection to the peer"},
 {RPCResult::Type::STR, "addrlocal", /*optional=*/true, "(ip:port) Local address as reported by the peer"},
 {RPCResult::Type::STR, "network", "Network (" + Join(GetNetworkNames(/*append_unroutable=*/true), ", ") + ")"},
 {RPCResult::Type::NUM, "mapped_as", /*optional=*/true, "Mapped AS (Autonomous System) number at the end of the BGP route to the peer, used for diversifying\n"
 "peer selection (only displayed if the -asmap config option is set)"},
 {RPCResult::Type::STR_HEX, "services", "The services offered"},
 {RPCResult::Type::ARR, "servicesnames", "the services offered, in human-readable form",
 {
 {RPCResult::Type::STR, "SERVICE_NAME", "the service name if it is recognised"}
 }},
 {RPCResult::Type::BOOL, "relaytxes", "Whether we relay transactions to this peer"},
 {RPCResult::Type::NUM, "last_inv_sequence", "Mempool sequence number of this peer's last INV"},
 {RPCResult::Type::NUM, "inv_to_send", "How many txs we have queued to announce to this peer"},
 {RPCResult::Type::NUM_TIME, "lastsend", "The " + UNIX_EPOCH_TIME + " of the last send"},
 {RPCResult::Type::NUM_TIME, "lastrecv", "The " + UNIX_EPOCH_TIME + " of the last receive"},
 {RPCResult::Type::NUM_TIME, "last_transaction", "The " + UNIX_EPOCH_TIME + " of the last valid transaction received from this peer"},
 {RPCResult::Type::NUM_TIME, "last_block", "The " + UNIX_EPOCH_TIME + " of the last block received from this peer"},
 {RPCResult::Type::NUM, "bytessent", "The total bytes sent"},
 {RPCResult::Type::NUM, "bytesrecv", "The total bytes received"},
 {RPCResult::Type::NUM_TIME, "conntime", "The " + UNIX_EPOCH_TIME + " of the connection"},
 {RPCResult::Type::NUM, "timeoffset", "The time offset in seconds"},
 {RPCResult::Type::NUM, "pingtime", /*optional=*/true, "The last ping time in seconds, if any"},
 {RPCResult::Type::NUM, "minping", /*optional=*/true, "The minimum observed ping time in seconds, if any"},
 {RPCResult::Type::NUM, "pingwait", /*optional=*/true, "The duration in seconds of an outstanding ping (if non-zero)"},
 {RPCResult::Type::NUM, "version", "The peer version, such as 70001"},
 {RPCResult::Type::STR, "subver", "The string version"},
 {RPCResult::Type::BOOL, "inbound", "Inbound (true) or Outbound (false)"},
 {RPCResult::Type::BOOL, "bip152_hb_to", "Whether we selected peer as (compact blocks) high-bandwidth peer"},
 {RPCResult::Type::BOOL, "bip152_hb_from", "Whether peer selected us as (compact blocks) high-bandwidth peer"},
 {RPCResult::Type::NUM, "presynced_headers", "The current height of header pre-synchronization with this peer, or -1 if no low-work sync is in progress"},
 {RPCResult::Type::NUM, "synced_headers", "The last header we have in common with this peer"},
 {RPCResult::Type::NUM, "synced_blocks", "The last block we have in common with this peer"},
 {RPCResult::Type::ARR, "inflight", "",
 {
 {RPCResult::Type::NUM, "n", "The heights of blocks we're currently asking from this peer"},
 }},
 {RPCResult::Type::BOOL, "addr_relay_enabled", "Whether we participate in address relay with this peer"},
 {RPCResult::Type::NUM, "addr_processed", "The total number of addresses processed, excluding those dropped due to rate limiting"},
 {RPCResult::Type::NUM, "addr_rate_limited", "The total number of addresses dropped due to rate limiting"},
 {RPCResult::Type::ARR, "permissions", "Any special permissions that have been granted to this peer",
 {
 {RPCResult::Type::STR, "permission_type", Join(NET_PERMISSIONS_DOC, ",\n") + ".\n"},
 }},
 {RPCResult::Type::NUM, "minfeefilter", "The minimum fee rate for transactions this peer accepts"},
 {RPCResult::Type::OBJ_DYN, "bytessent_per_msg", "",
 {
 {RPCResult::Type::NUM, "msg", "The total bytes sent aggregated by message type\n"
 "When a message type is not listed in this json object, the bytes sent are 0.\n"
 "Only known message types can appear as keys in the object."}
 }},
 {RPCResult::Type::OBJ_DYN, "bytesrecv_per_msg", "",
 {
 {RPCResult::Type::NUM, "msg", "The total bytes received aggregated by message type\n"
 "When a message type is not listed in this json object, the bytes received are 0.\n"
 "Only known message types can appear as keys in the object and all bytes received\n"
 "of unknown message types are listed under '"+NET_MESSAGE_TYPE_OTHER+"'."}
 }},
 {RPCResult::Type::STR, "connection_type", "Type of connection: \n" + Join(CONNECTION_TYPE_DOC, ",\n") + ".\n"
 "Please note this output is unlikely to be stable in upcoming releases as we iterate to\n"
 "best capture connection behaviors."},
 {RPCResult::Type::STR, "transport_protocol_type", "Type of transport protocol: \n" + Join(TRANSPORT_TYPE_DOC, ",\n") + ".\n"},
 {RPCResult::Type::STR, "session_id", "The session ID for this connection, or \"\" if there is none (\"v2\" transport protocol only).\n"},
 }},
 }},
 },
 RPCExamples{
 HelpExampleCli("getpeerinfo", "")
 + HelpExampleRpc("getpeerinfo", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const CConnman& connman = EnsureConnman(node);
 const PeerManager& peerman = EnsurePeerman(node);

 std::vector<CNodeStats> vstats;
 connman.GetNodeStats(vstats);

 UniValue ret(UniValue::VARR);

 for (const CNodeStats& stats : vstats) {
 UniValue obj(UniValue::VOBJ);
 CNodeStateStats statestats;
 bool fStateStats = peerman.GetNodeStateStats(stats.nodeid, statestats);
 // GetNodeStateStats() requires the existence of a CNodeState and a Peer object
 // to succeed for this peer. These are created at connection initialisation and
 // exist for the duration of the connection - except if there is a race where the
 // peer got disconnected in between the GetNodeStats() and the GetNodeStateStats()
 // calls. In this case, the peer doesn't need to be reported here.
 if (!fStateStats) {
 continue;
 }
 obj.pushKV("id", stats.nodeid);
 obj.pushKV("addr", stats.m_addr_name);
 if (stats.addrBind.IsValid()) {
 obj.pushKV("addrbind", stats.addrBind.ToStringAddrPort());
 }
 if (!(stats.addrLocal.empty())) {
 obj.pushKV("addrlocal", stats.addrLocal);
 }
 obj.pushKV("network", GetNetworkName(stats.m_network));
 if (stats.m_mapped_as != 0) {
 obj.pushKV("mapped_as", stats.m_mapped_as);
 }
 ServiceFlags services{statestats.their_services};
 obj.pushKV("services", strprintf("%016x", services));
 obj.pushKV("servicesnames", GetServicesNames(services));
 obj.pushKV("relaytxes", statestats.m_relay_txs);
 obj.pushKV("last_inv_sequence", statestats.m_last_inv_seq);
 obj.pushKV("inv_to_send", statestats.m_inv_to_send);
 obj.pushKV("lastsend", TicksSinceEpoch<std::chrono::seconds>(stats.m_last_send));
 obj.pushKV("lastrecv", TicksSinceEpoch<std::chrono::seconds>(stats.m_last_recv));
 obj.pushKV("last_transaction", count_seconds(stats.m_last_tx_time));
 obj.pushKV("last_block", count_seconds(stats.m_last_block_time));
 obj.pushKV("bytessent", stats.nSendBytes);
 obj.pushKV("bytesrecv", stats.nRecvBytes);
 obj.pushKV("conntime", TicksSinceEpoch<std::chrono::seconds>(stats.m_connected));
 obj.pushKV("timeoffset", Ticks<std::chrono::seconds>(statestats.time_offset));
 if (stats.m_last_ping_time > 0us) {
 obj.pushKV("pingtime", Ticks<SecondsDouble>(stats.m_last_ping_time));
 }
 if (stats.m_min_ping_time < decltype(CNode::m_min_ping_time.load())::max()) {
 obj.pushKV("minping", Ticks<SecondsDouble>(stats.m_min_ping_time));
 }
 if (statestats.m_ping_wait > 0s) {
 obj.pushKV("pingwait", Ticks<SecondsDouble>(statestats.m_ping_wait));
 }
 obj.pushKV("version", stats.nVersion);
 // Use the sanitized form of subver here, to avoid tricksy remote peers from
 // corrupting or modifying the JSON output by putting special characters in
 // their ver message.
 obj.pushKV("subver", stats.cleanSubVer);
 obj.pushKV("inbound", stats.fInbound);
 obj.pushKV("bip152_hb_to", stats.m_bip152_highbandwidth_to);
 obj.pushKV("bip152_hb_from", stats.m_bip152_highbandwidth_from);
 obj.pushKV("presynced_headers", statestats.presync_height);
 obj.pushKV("synced_headers", statestats.nSyncHeight);
 obj.pushKV("synced_blocks", statestats.nCommonHeight);
 UniValue heights(UniValue::VARR);
 for (const int height : statestats.vHeightInFlight) {
 heights.push_back(height);
 }
 obj.pushKV("inflight", std::move(heights));
 obj.pushKV("addr_relay_enabled", statestats.m_addr_relay_enabled);
 obj.pushKV("addr_processed", statestats.m_addr_processed);
 obj.pushKV("addr_rate_limited", statestats.m_addr_rate_limited);
 UniValue permissions(UniValue::VARR);
 for (const auto& permission : NetPermissions::ToStrings(stats.m_permission_flags)) {
 permissions.push_back(permission);
 }
 obj.pushKV("permissions", std::move(permissions));
 obj.pushKV("minfeefilter", ValueFromAmount(statestats.m_fee_filter_received));

 UniValue sendPerMsgType(UniValue::VOBJ);
 for (const auto& i : stats.mapSendBytesPerMsgType) {
 if (i.second > 0)
 sendPerMsgType.pushKV(i.first, i.second);
 }
 obj.pushKV("bytessent_per_msg", std::move(sendPerMsgType));

 UniValue recvPerMsgType(UniValue::VOBJ);
 for (const auto& i : stats.mapRecvBytesPerMsgType) {
 if (i.second > 0)
 recvPerMsgType.pushKV(i.first, i.second);
 }
 obj.pushKV("bytesrecv_per_msg", std::move(recvPerMsgType));
 obj.pushKV("connection_type", ConnectionTypeAsString(stats.m_conn_type));
 obj.pushKV("transport_protocol_type", TransportTypeAsString(stats.m_transport_type));
 obj.pushKV("session_id", stats.m_session_id);

 ret.push_back(std::move(obj));
 }

 return ret;
},
 };
}

static RPCMethod addnode()
{
 return RPCMethod{
 "addnode",
 "Attempts to add or remove a node from the addnode list.\n"
 "Or try a connection to a node once.\n"
 "Nodes added using addnode (or -connect) are protected from DoS disconnection and are not required to be\n"
 "full nodes/support SegWit as other outbound peers are (though such peers will not be synced from).\n" +
 strprintf("Addnode connections are limited to %u at a time", MAX_ADDNODE_CONNECTIONS) +
 " and are counted separately from the -maxconnections limit.\n",
 {
 {"node", RPCArg::Type::STR, RPCArg::Optional::NO, "The IP address/hostname optionally followed by :port of the peer to connect to"},
 {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "'add' to add a node to the list, 'remove' to remove a node from the list, 'onetry' to try a connection to the node once"},
 {"v2transport", RPCArg::Type::BOOL, RPCArg::DefaultHint{"set by -v2transport"}, "Attempt to connect using BIP324 v2 transport protocol (ignored for 'remove' command)"},
 },
 RPCResult{RPCResult::Type::NONE, "", ""},
 RPCExamples{
 HelpExampleCli("addnode", "\"192.168.0.6:9333\" \"onetry\" true")
 + HelpExampleRpc("addnode", "\"192.168.0.6:9333\", \"onetry\" true")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 const auto command{self.Arg<std::string_view>("command")};
 if (command != "onetry" && command != "add" && command != "remove") {
 throw std::runtime_error(
 self.ToString());
 }

 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);

 const auto node_arg{self.Arg<std::string_view>("node")};
 bool node_v2transport = connman.GetLocalServices() & NODE_P2P_V2;
 bool use_v2transport = self.MaybeArg<bool>("v2transport").value_or(node_v2transport);

 if (use_v2transport && !node_v2transport) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "Error: v2transport requested but not enabled (see -v2transport)");
 }

 if (command == "onetry")
 {
 CAddress addr;
 connman.OpenNetworkConnection(addr, /*fCountFailure=*/false, /*grant_outbound=*/{}, std::string{node_arg}.c_str(), ConnectionType::MANUAL, use_v2transport);
 return UniValue::VNULL;
 }

 if (command == "add")
 {
 if (!connman.AddNode({std::string{node_arg}, use_v2transport})) {
 throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: Node already added");
 }
 }
 else if (command == "remove")
 {
 if (!connman.RemoveAddedNode(node_arg)) {
 throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node could not be removed. It has not been added previously.");
 }
 }

 return UniValue::VNULL;
},
 };
}

static RPCMethod addconnection()
{
 return RPCMethod{
 "addconnection",
 "Open an outbound connection to a specified node. This RPC is for testing only.\n",
 {
 {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The IP address and port to attempt connecting to."},
 {"connection_type", RPCArg::Type::STR, RPCArg::Optional::NO, "Type of connection to open (\"outbound-full-relay\", \"block-relay-only\", \"addr-fetch\" or \"feeler\")."},
 {"v2transport", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Attempt to connect using BIP324 v2 transport protocol"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 { RPCResult::Type::STR, "address", "Address of newly added connection." },
 { RPCResult::Type::STR, "connection_type", "Type of connection opened." },
 }},
 RPCExamples{
 HelpExampleCli("addconnection", "\"192.168.0.6:9333\" \"outbound-full-relay\" true")
 + HelpExampleRpc("addconnection", "\"192.168.0.6:9333\" \"outbound-full-relay\" true")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 if (Params().GetChainType() != ChainType::REGTEST) {
 throw std::runtime_error("addconnection is for regression testing (-regtest mode) only.");
 }

 const std::string address = request.params[0].get_str();
 auto conn_type_in{util::TrimStringView(self.Arg<std::string_view>("connection_type"))};
 ConnectionType conn_type{};
 if (conn_type_in == "outbound-full-relay") {
 conn_type = ConnectionType::OUTBOUND_FULL_RELAY;
 } else if (conn_type_in == "block-relay-only") {
 conn_type = ConnectionType::BLOCK_RELAY;
 } else if (conn_type_in == "addr-fetch") {
 conn_type = ConnectionType::ADDR_FETCH;
 } else if (conn_type_in == "feeler") {
 conn_type = ConnectionType::FEELER;
 } else {
 throw JSONRPCError(RPC_INVALID_PARAMETER, self.ToString());
 }
 bool use_v2transport{self.Arg<bool>("v2transport")};

 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);

 if (use_v2transport && !(connman.GetLocalServices() & NODE_P2P_V2)) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "Error: Adding v2transport connections requires -v2transport init flag to be set.");
 }

 const bool success = connman.AddConnection(address, conn_type, use_v2transport);
 if (!success) {
 throw JSONRPCError(RPC_CLIENT_NODE_CAPACITY_REACHED, "Error: Already at capacity for specified connection type.");
 }

 UniValue info(UniValue::VOBJ);
 info.pushKV("address", address);
 info.pushKV("connection_type", conn_type_in);

 return info;
},
 };
}

static RPCMethod disconnectnode()
{
 return RPCMethod{
 "disconnectnode",
 "Immediately disconnects from the specified peer node.\n"
 "\nStrictly one out of 'address' and 'nodeid' can be provided to identify the node.\n"
 "\nTo disconnect by nodeid, either set 'address' to the empty string, or call using the named 'nodeid' argument only.\n",
 {
 {"address", RPCArg::Type::STR, RPCArg::DefaultHint{"fallback to nodeid"}, "The IP address/port of the node"},
 {"nodeid", RPCArg::Type::NUM, RPCArg::DefaultHint{"fallback to address"}, "The node ID (see getpeerinfo for node IDs)"},
 },
 RPCResult{RPCResult::Type::NONE, "", ""},
 RPCExamples{
 HelpExampleCli("disconnectnode", "\"192.168.0.6:9333\"")
 + HelpExampleCli("disconnectnode", "\"\" 1")
 + HelpExampleRpc("disconnectnode", "\"192.168.0.6:9333\"")
 + HelpExampleRpc("disconnectnode", "\"\", 1")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);

 bool success;
 auto address{self.MaybeArg<std::string_view>("address")};
 auto node_id{self.MaybeArg<int64_t>("nodeid")};

 if (address && !node_id) {
 /* handle disconnect-by-address */
 success = connman.DisconnectNode(*address);
 } else if (node_id && (!address || address->empty())) {
 /* handle disconnect-by-id */
 success = connman.DisconnectNode(*node_id);
 } else {
 throw JSONRPCError(RPC_INVALID_PARAMS, "Only one of address and nodeid should be provided.");
 }

 if (!success) {
 throw JSONRPCError(RPC_CLIENT_NODE_NOT_CONNECTED, "Node not found in connected nodes");
 }

 return UniValue::VNULL;
},
 };
}

static RPCMethod getaddednodeinfo()
{
 return RPCMethod{
 "getaddednodeinfo",
 "Returns information about the given added node, or all added nodes\n"
 "(note that onetry addnodes are not listed here)\n",
 {
 {"node", RPCArg::Type::STR, RPCArg::DefaultHint{"all nodes"}, "If provided, return information about this specific node, otherwise all nodes are returned."},
 },
 RPCResult{
 RPCResult::Type::ARR, "", "",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "addednode", "The node IP address or name (as provided to addnode)"},
 {RPCResult::Type::BOOL, "connected", "If connected"},
 {RPCResult::Type::ARR, "addresses", "Only when connected = true",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "address", "The TKNC server IP and port we're connected to"},
 {RPCResult::Type::STR, "connected", "connection, inbound or outbound"},
 }},
 }},
 }},
 }
 },
 RPCExamples{
 HelpExampleCli("getaddednodeinfo", "\"192.168.0.201\"")
 + HelpExampleRpc("getaddednodeinfo", "\"192.168.0.201\"")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const CConnman& connman = EnsureConnman(node);

 std::vector<AddedNodeInfo> vInfo = connman.GetAddedNodeInfo(/*include_connected=*/true);

 if (auto node{self.MaybeArg<std::string_view>("node")}) {
 bool found = false;
 for (const AddedNodeInfo& info : vInfo) {
 if (info.m_params.m_added_node == *node) {
 vInfo.assign(1, info);
 found = true;
 break;
 }
 }
 if (!found) {
 throw JSONRPCError(RPC_CLIENT_NODE_NOT_ADDED, "Error: Node has not been added.");
 }
 }

 UniValue ret(UniValue::VARR);

 for (const AddedNodeInfo& info : vInfo) {
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("addednode", info.m_params.m_added_node);
 obj.pushKV("connected", info.fConnected);
 UniValue addresses(UniValue::VARR);
 if (info.fConnected) {
 UniValue address(UniValue::VOBJ);
 address.pushKV("address", info.resolvedAddress.ToStringAddrPort());
 address.pushKV("connected", info.fInbound ? "inbound" : "outbound");
 addresses.push_back(std::move(address));
 }
 obj.pushKV("addresses", std::move(addresses));
 ret.push_back(std::move(obj));
 }

 return ret;
},
 };
}

static RPCMethod getnettotals()
{
 return RPCMethod{"getnettotals",
 "Returns information about network traffic, including bytes in, bytes out,\n"
 "and current system time.",
 {},
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::NUM, "totalbytesrecv", "Total bytes received"},
 {RPCResult::Type::NUM, "totalbytessent", "Total bytes sent"},
 {RPCResult::Type::NUM_TIME, "timemillis", "Current system " + UNIX_EPOCH_TIME + " in milliseconds"},
 {RPCResult::Type::OBJ, "uploadtarget", "",
 {
 {RPCResult::Type::NUM, "timeframe", "Length of the measuring timeframe in seconds"},
 {RPCResult::Type::NUM, "target", "Target in bytes"},
 {RPCResult::Type::BOOL, "target_reached", "True if target is reached"},
 {RPCResult::Type::BOOL, "serve_historical_blocks", "True if serving historical blocks"},
 {RPCResult::Type::NUM, "bytes_left_in_cycle", "Bytes left in current time cycle"},
 {RPCResult::Type::NUM, "time_left_in_cycle", "Seconds left in current time cycle"},
 }},
 }
 },
 RPCExamples{
 HelpExampleCli("getnettotals", "")
 + HelpExampleRpc("getnettotals", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const CConnman& connman = EnsureConnman(node);

 UniValue obj(UniValue::VOBJ);
 obj.pushKV("totalbytesrecv", connman.GetTotalBytesRecv());
 obj.pushKV("totalbytessent", connman.GetTotalBytesSent());
 obj.pushKV("timemillis", TicksSinceEpoch<std::chrono::milliseconds>(SystemClock::now()));

 UniValue outboundLimit(UniValue::VOBJ);
 outboundLimit.pushKV("timeframe", count_seconds(connman.GetMaxOutboundTimeframe()));
 outboundLimit.pushKV("target", connman.GetMaxOutboundTarget());
 outboundLimit.pushKV("target_reached", connman.OutboundTargetReached(false));
 outboundLimit.pushKV("serve_historical_blocks", !connman.OutboundTargetReached(true));
 outboundLimit.pushKV("bytes_left_in_cycle", connman.GetOutboundTargetBytesLeft());
 outboundLimit.pushKV("time_left_in_cycle", count_seconds(connman.GetMaxOutboundTimeLeftInCycle()));
 obj.pushKV("uploadtarget", std::move(outboundLimit));
 return obj;
},
 };
}

static UniValue GetNetworksInfo()
{
 UniValue networks(UniValue::VARR);
 for (int n = 0; n < NET_MAX; ++n) {
 enum Network network = static_cast<enum Network>(n);
 if (network == NET_UNROUTABLE || network == NET_INTERNAL) continue;
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("name", GetNetworkName(network));
 obj.pushKV("limited", !g_reachable_nets.Contains(network));
 obj.pushKV("reachable", g_reachable_nets.Contains(network));
 if (const auto proxy = GetProxy(network)) {
 obj.pushKV("proxy", proxy->ToString());
 obj.pushKV("proxy_randomize_credentials", proxy->m_tor_stream_isolation);
 } else {
 obj.pushKV("proxy", std::string());
 obj.pushKV("proxy_randomize_credentials", false);
 }
 networks.push_back(std::move(obj));
 }
 return networks;
}

static RPCMethod getnetworkinfo()
{
 return RPCMethod{"getnetworkinfo",
 "Returns an object containing various state info regarding P2P networking.\n",
 {},
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::NUM, "version", "the server version"},
 {RPCResult::Type::STR, "subversion", "the server subversion string"},
 {RPCResult::Type::NUM, "protocolversion", "the protocol version"},
 {RPCResult::Type::STR_HEX, "localservices", "the services we offer to the network"},
 {RPCResult::Type::ARR, "localservicesnames", "the services we offer to the network, in human-readable form",
 {
 {RPCResult::Type::STR, "SERVICE_NAME", "the service name"},
 }},
 {RPCResult::Type::BOOL, "localrelay", "true if transaction relay is requested from peers"},
 {RPCResult::Type::NUM, "timeoffset", "the time offset"},
 {RPCResult::Type::NUM, "connections", "the total number of connections"},
 {RPCResult::Type::NUM, "connections_in", "the number of inbound connections"},
 {RPCResult::Type::NUM, "connections_out", "the number of outbound connections"},
 {RPCResult::Type::BOOL, "networkactive", "whether p2p networking is enabled"},
 {RPCResult::Type::ARR, "networks", "information per network",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "name", "network (" + Join(GetNetworkNames(), ", ") + ")"},
 {RPCResult::Type::BOOL, "limited", "is the network limited using -onlynet?"},
 {RPCResult::Type::BOOL, "reachable", "is the network reachable?"},
 {RPCResult::Type::STR, "proxy", "(\"host:port\") the proxy that is used for this network, or empty if none"},
 {RPCResult::Type::BOOL, "proxy_randomize_credentials", "Whether randomized credentials are used"},
 }},
 }},
 {RPCResult::Type::NUM, "relayfee", "minimum relay fee rate for transactions in " + CURRENCY_UNIT + "/kvB"},
 {RPCResult::Type::NUM, "incrementalfee", "minimum fee rate increment for mempool limiting or replacement in " + CURRENCY_UNIT + "/kvB"},
 {RPCResult::Type::ARR, "localaddresses", "list of local addresses",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "address", "network address"},
 {RPCResult::Type::NUM, "port", "network port"},
 {RPCResult::Type::NUM, "score", "relative score"},
 }},
 }},
 (IsDeprecatedRPCEnabled("warnings") ?
 RPCResult{RPCResult::Type::STR, "warnings", "any network and blockchain warnings (DEPRECATED)"} :
 RPCResult{RPCResult::Type::ARR, "warnings", "any network and blockchain warnings (run with `-deprecatedrpc=warnings` to return the latest warning as a single string)",
 {
 {RPCResult::Type::STR, "", "warning"},
 }
 }
 ),
 }
 },
 RPCExamples{
 HelpExampleCli("getnetworkinfo", "")
 + HelpExampleRpc("getnetworkinfo", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 LOCK(cs_main);
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("version", CLIENT_VERSION);
 obj.pushKV("subversion", strSubVersion);
 obj.pushKV("protocolversion",PROTOCOL_VERSION);
 NodeContext& node = EnsureAnyNodeContext(request.context);
 if (node.connman) {
 ServiceFlags services = node.connman->GetLocalServices();
 obj.pushKV("localservices", strprintf("%016x", services));
 obj.pushKV("localservicesnames", GetServicesNames(services));
 }
 if (node.peerman) {
 auto peerman_info{node.peerman->GetInfo()};
 obj.pushKV("localrelay", !peerman_info.ignores_incoming_txs);
 obj.pushKV("timeoffset", Ticks<std::chrono::seconds>(peerman_info.median_outbound_time_offset));
 }
 if (node.connman) {
 obj.pushKV("networkactive", node.connman->GetNetworkActive());
 obj.pushKV("connections", node.connman->GetNodeCount(ConnectionDirection::Both));
 obj.pushKV("connections_in", node.connman->GetNodeCount(ConnectionDirection::In));
 obj.pushKV("connections_out", node.connman->GetNodeCount(ConnectionDirection::Out));
 }
 obj.pushKV("networks", GetNetworksInfo());
 if (node.mempool) {
 // Those fields can be deprecated, to be replaced by the getmempoolinfo fields
 obj.pushKV("relayfee", ValueFromAmount(node.mempool->m_opts.min_relay_feerate.GetFeePerK()));
 obj.pushKV("incrementalfee", ValueFromAmount(node.mempool->m_opts.incremental_relay_feerate.GetFeePerK()));
 }
 UniValue localAddresses(UniValue::VARR);
 {
 LOCK(g_maplocalhost_mutex);
 for (const std::pair<const CNetAddr, LocalServiceInfo> &item : mapLocalHost)
 {
 UniValue rec(UniValue::VOBJ);
 rec.pushKV("address", item.first.ToStringAddr());
 rec.pushKV("port", item.second.nPort);
 rec.pushKV("score", item.second.nScore);
 localAddresses.push_back(std::move(rec));
 }
 }
 obj.pushKV("localaddresses", std::move(localAddresses));
 obj.pushKV("warnings", node::GetWarningsForRpc(*CHECK_NONFATAL(node.warnings), IsDeprecatedRPCEnabled("warnings")));
 return obj;
},
 };
}

static RPCMethod setban()
{
 return RPCMethod{
 "setban",
 "Attempts to add or remove an IP/Subnet from the banned list.\n",
 {
 {"subnet", RPCArg::Type::STR, RPCArg::Optional::NO, "The IP/Subnet (see getpeerinfo for nodes IP) with an optional netmask (default is /32 = single IP)"},
 {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "'add' to add an IP/Subnet to the list, 'remove' to remove an IP/Subnet from the list"},
 {"bantime", RPCArg::Type::NUM, RPCArg::Default{0}, "time in seconds how long (or until when if [absolute] is set) the IP is banned (0 or empty means using the default time of 24h which can also be overwritten by the -bantime startup argument)"},
 {"absolute", RPCArg::Type::BOOL, RPCArg::Default{false}, "If set, the bantime must be an absolute timestamp expressed in " + UNIX_EPOCH_TIME},
 },
 RPCResult{RPCResult::Type::NONE, "", ""},
 RPCExamples{
 HelpExampleCli("setban", "\"192.168.0.6\" \"add\" 86400")
 + HelpExampleCli("setban", "\"192.168.0.0/24\" \"add\"")
 + HelpExampleRpc("setban", "\"192.168.0.6\", \"add\", 86400")
 },
 [](const RPCMethod& help, const JSONRPCRequest& request) -> UniValue
{
 auto command{help.Arg<std::string_view>("command")};
 if (command != "add" && command != "remove") {
 throw std::runtime_error(help.ToString());
 }
 NodeContext& node = EnsureAnyNodeContext(request.context);
 BanMan& banman = EnsureBanman(node);

 CSubNet subNet;
 CNetAddr netAddr;
 std::string subnet_arg{help.Arg<std::string_view>("subnet")};
 const bool isSubnet{subnet_arg.find('/') != subnet_arg.npos};

 if (!isSubnet) {
 const std::optional<CNetAddr> addr{LookupHost(subnet_arg, false)};
 if (addr.has_value()) {
 netAddr = static_cast<CNetAddr>(MaybeFlipIPv6toCJDNS(CService{addr.value(), /*port=*/0}));
 }
 } else {
 subNet = LookupSubNet(subnet_arg);
 }

 if (! (isSubnet ? subNet.IsValid() : netAddr.IsValid()) ) {
 throw JSONRPCError(RPC_CLIENT_INVALID_IP_OR_SUBNET, "Error: Invalid IP/Subnet");
 }

 if (command == "add") {
 if (isSubnet ? banman.IsBanned(subNet) : banman.IsBanned(netAddr)) {
 throw JSONRPCError(RPC_CLIENT_NODE_ALREADY_ADDED, "Error: IP/Subnet already banned");
 }

 int64_t banTime = 0; //use standard bantime if not specified
 if (!request.params[2].isNull())
 banTime = request.params[2].getInt<int64_t>();

 const bool absolute{request.params[3].isNull() ? false : request.params[3].get_bool()};

 if (absolute && banTime < GetTime()) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "Error: Absolute timestamp is in the past");
 }

 if (isSubnet) {
 banman.Ban(subNet, banTime, absolute);
 if (node.connman) {
 node.connman->DisconnectNode(subNet);
 }
 } else {
 banman.Ban(netAddr, banTime, absolute);
 if (node.connman) {
 node.connman->DisconnectNode(netAddr);
 }
 }
 } else if(command == "remove") {
 if (!( isSubnet ? banman.Unban(subNet) : banman.Unban(netAddr) )) {
 throw JSONRPCError(RPC_CLIENT_INVALID_IP_OR_SUBNET, "Error: Unban failed. Requested address/subnet was not previously manually banned.");
 }
 }
 return UniValue::VNULL;
},
 };
}

static RPCMethod listbanned()
{
 return RPCMethod{
 "listbanned",
 "List all manually banned IPs/Subnets.\n",
 {},
 RPCResult{RPCResult::Type::ARR, "", "",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "address", "The IP/Subnet of the banned node"},
 {RPCResult::Type::NUM_TIME, "ban_created", "The " + UNIX_EPOCH_TIME + " the ban was created"},
 {RPCResult::Type::NUM_TIME, "banned_until", "The " + UNIX_EPOCH_TIME + " the ban expires"},
 {RPCResult::Type::NUM_TIME, "ban_duration", "The ban duration, in seconds"},
 {RPCResult::Type::NUM_TIME, "time_remaining", "The time remaining until the ban expires, in seconds"},
 }},
 }},
 RPCExamples{
 HelpExampleCli("listbanned", "")
 + HelpExampleRpc("listbanned", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 BanMan& banman = EnsureAnyBanman(request.context);

 banmap_t banMap;
 banman.GetBanned(banMap);
 const int64_t current_time{GetTime()};

 UniValue bannedAddresses(UniValue::VARR);
 for (const auto& entry : banMap)
 {
 const CBanEntry& banEntry = entry.second;
 UniValue rec(UniValue::VOBJ);
 rec.pushKV("address", entry.first.ToString());
 rec.pushKV("ban_created", banEntry.nCreateTime);
 rec.pushKV("banned_until", banEntry.nBanUntil);
 rec.pushKV("ban_duration", (banEntry.nBanUntil - banEntry.nCreateTime));
 rec.pushKV("time_remaining", (banEntry.nBanUntil - current_time));

 bannedAddresses.push_back(std::move(rec));
 }

 return bannedAddresses;
},
 };
}

static RPCMethod clearbanned()
{
 return RPCMethod{
 "clearbanned",
 "Clear all banned IPs.\n",
 {},
 RPCResult{RPCResult::Type::NONE, "", ""},
 RPCExamples{
 HelpExampleCli("clearbanned", "")
 + HelpExampleRpc("clearbanned", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 BanMan& banman = EnsureAnyBanman(request.context);

 banman.ClearBanned();

 return UniValue::VNULL;
},
 };
}

static RPCMethod setnetworkactive()
{
 return RPCMethod{
 "setnetworkactive",
 "Disable/enable all p2p network activity.\n",
 {
 {"state", RPCArg::Type::BOOL, RPCArg::Optional::NO, "true to enable networking, false to disable"},
 },
 RPCResult{RPCResult::Type::BOOL, "", "The value that was passed in"},
 RPCExamples{""},
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);

 connman.SetNetworkActive(request.params[0].get_bool());

 return connman.GetNetworkActive();
},
 };
}

static RPCMethod getnodeaddresses()
{
 return RPCMethod{"getnodeaddresses",
 "Return known addresses, after filtering for quality and recency.\n"
 "These can potentially be used to find new peers in the network.\n"
 "The total number of addresses known to the node may be higher.",
 {
 {"count", RPCArg::Type::NUM, RPCArg::Default{1}, "The maximum number of addresses to return. Specify 0 to return all known addresses."},
 {"network", RPCArg::Type::STR, RPCArg::DefaultHint{"all networks"}, "Return only addresses of the specified network. Can be one of: " + Join(GetNetworkNames(), ", ") + "."},
 },
 RPCResult{
 RPCResult::Type::ARR, "", "",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::NUM_TIME, "time", "The " + UNIX_EPOCH_TIME + " when the node was last seen"},
 {RPCResult::Type::NUM, "services", "The services offered by the node"},
 {RPCResult::Type::STR, "address", "The address of the node"},
 {RPCResult::Type::NUM, "port", "The port number of the node"},
 {RPCResult::Type::STR, "network", "The network (" + Join(GetNetworkNames(), ", ") + ") the node connected through"},
 }},
 }
 },
 RPCExamples{
 HelpExampleCli("getnodeaddresses", "8")
 + HelpExampleCli("getnodeaddresses", "4 \"i2p\"")
 + HelpExampleCli("-named getnodeaddresses", "network=onion count=12")
 + HelpExampleRpc("getnodeaddresses", "8")
 + HelpExampleRpc("getnodeaddresses", "4, \"i2p\"")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const CConnman& connman = EnsureConnman(node);

 const int count{request.params[0].isNull() ? 1 : request.params[0].getInt<int>()};
 if (count < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "Address count out of range");

 const std::optional<Network> network{request.params[1].isNull() ? std::nullopt : std::optional<Network>{ParseNetwork(request.params[1].get_str())}};
 if (network == NET_UNROUTABLE) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Network not recognized: %s", request.params[1].get_str()));
 }

 // returns a shuffled list of CAddress
 const std::vector<CAddress> vAddr{connman.GetAddressesUnsafe(count, /*max_pct=*/0, network)};
 UniValue ret(UniValue::VARR);

 for (const CAddress& addr : vAddr) {
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("time", TicksSinceEpoch<std::chrono::seconds>(addr.nTime));
 obj.pushKV("services", static_cast<std::underlying_type_t<decltype(addr.nServices)>>(addr.nServices));
 obj.pushKV("address", addr.ToStringAddr());
 obj.pushKV("port", addr.GetPort());
 obj.pushKV("network", GetNetworkName(addr.GetNetClass()));
 ret.push_back(std::move(obj));
 }
 return ret;
},
 };
}

static RPCMethod addpeeraddress()
{
 return RPCMethod{"addpeeraddress",
 "Add the address of a potential peer to an address manager table. This RPC is for testing only.",
 {
 {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The IP address of the peer"},
 {"port", RPCArg::Type::NUM, RPCArg::Optional::NO, "The port of the peer"},
 {"tried", RPCArg::Type::BOOL, RPCArg::Default{false}, "If true, attempt to add the peer to the tried addresses table"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::BOOL, "success", "whether the peer address was successfully added to the address manager table"},
 {RPCResult::Type::STR, "error", /*optional=*/true, "error description, if the address could not be added"},
 },
 },
 RPCExamples{
 HelpExampleCli("addpeeraddress", "\"1.2.3.4\" 9333 true")
 + HelpExampleRpc("addpeeraddress", "\"1.2.3.4\", 9333, true")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
 AddrMan& addrman = EnsureAnyAddrman(request.context);

 const std::string& addr_string{request.params[0].get_str()};
 const auto port{request.params[1].getInt<uint16_t>()};
 const bool tried{request.params[2].isNull() ? false : request.params[2].get_bool()};

 UniValue obj(UniValue::VOBJ);
 std::optional<CNetAddr> net_addr{LookupHost(addr_string, false)};
 if (!net_addr.has_value()) {
 throw JSONRPCError(RPC_CLIENT_INVALID_IP_OR_SUBNET, "Invalid IP address");
 }

 bool success{false};

 CService service{net_addr.value(), port};
 CAddress address{MaybeFlipIPv6toCJDNS(service), ServiceFlags{NODE_NETWORK | NODE_WITNESS}};
 address.nTime = Now<NodeSeconds>();
 // The source address is set equal to the address. This is equivalent to the peer
 // announcing itself.
 if (addrman.Add({address}, address)) {
 success = true;
 if (tried) {
 // Attempt to move the address to the tried addresses table.
 if (!addrman.Good(address)) {
 success = false;
 obj.pushKV("error", "failed-adding-to-tried");
 }
 }
 } else {
 obj.pushKV("error", "failed-adding-to-new");
 }

 obj.pushKV("success", success);
 return obj;
},
 };
}

static RPCMethod p2pinference()
{
 return RPCMethod{
 "p2pinference",
 "Send an LLM inference request to a connected peer over P2P network.\n"
 "The peer must have a model loaded and be running the inference service.\n"
 "If api_key is provided, escrow billing is enforced: balance is checked\n"
 "before inference and auto-deducted after successful inference.\n",
 {
 {"model", RPCArg::Type::STR, RPCArg::Optional::NO, "Model name to use for inference"},
 {"prompt", RPCArg::Type::STR, RPCArg::Optional::NO, "The prompt text to send to the model"},
 {"peer_id", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Specific peer ID to send to (optional, uses first connected peer if omitted)"},
 {"api_key", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "API Key for escrow billing (optional, if omitted no billing is performed)"},
 {"max_tokens", RPCArg::Type::NUM, RPCArg::Optional::OMITTED, "Maximum tokens to generate. -1=unlimited (generate until EOS), default=-1"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "content", "The inference response text"},
 {RPCResult::Type::NUM, "tokens_used", "Number of tokens used"},
 {RPCResult::Type::NUM, "cost", "Cost in TKNC tokens"},
 {RPCResult::Type::NUM, "peer_id", "The peer that processed the request"},
 {RPCResult::Type::OBJ, "escrow", "Escrow billing status (only present if api_key was provided)",
 {
 {RPCResult::Type::STR, "escrow_id", "Escrow identifier"},
 {RPCResult::Type::NUM, "remaining_tokens", "Remaining token quota after deduction"},
 {RPCResult::Type::NUM, "remaining_limit", "Remaining spending limit after deduction"},
 {RPCResult::Type::STR, "state", "Updated escrow state"},
 }},
 }},
 RPCExamples{
 HelpExampleCli("p2pinference", "\"qwen2.5-0.5b-instruct\" \"What is blockchain?\"") +
 HelpExampleRpc("p2pinference", "\"qwen2.5-0.5b-instruct\" \"What is blockchain?\"") +
 HelpExampleCli("p2pinference", "\"qwen2.5-0.5b-instruct\" \"What is blockchain?\" 0 \"tknc_abc123...\"")},
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
 const auto model{self.Arg<std::string_view>("model")};
 const auto prompt{self.Arg<std::string_view>("prompt")};

 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);
 PeerManager* peerman = node.peerman.get();

 if (!peerman) {
 throw JSONRPCError(RPC_MISC_ERROR, "PeerManager not available");
 }

 // Parse optional api_key (4th parameter, index 3)
 std::string api_key;
 bool has_api_key = false;
 if (request.params.size() > 3 && !request.params[3].isNull()) {
 api_key = request.params[3].get_str();
 has_api_key = !api_key.empty();
 }

 // Parse optional max_tokens (5th parameter, index 4). -1 = unlimited
 int max_tokens = -1; // default: unlimited (generate until EOS)
 if (request.params.size() > 4 && !request.params[4].isNull()) {
 max_tokens = request.params[4].getInt<int64_t>();
 }

 // === Escrow pre-check: if api_key provided, validate spending limit before inference ===
 // SECURITY: Use CanAffordInference which checks wallet balance AND pending costs from
 // in-flight requests. The old check only validated escrow state, not actual wallet balance,
 // allowing free inference when wallet was empty but escrow was still "active".
 std::optional<SpendingLimit> escrow_opt;
 if (has_api_key) {
 // Auto-create escrow if it doesn't exist (same as gateway/proxy paths)
 EnsureEscrowForAPIKey(api_key);

 std::string balance_error;
 if (!CanAffordInference(api_key, balance_error)) {
 throw JSONRPCError(RPC_MISC_ERROR,
 "Inference refused: " + balance_error);
 }

 // Keep the escrow reference for post-inference billing
 escrow_opt = FindSpendingLimitByAPIKey(api_key);
 if (!escrow_opt.has_value()) {
 throw JSONRPCError(RPC_INVALID_PARAMETER,
 "No escrow found for the provided api_key after auto-creation.");
 }

 // Model hash validation against ModelRegistry
 const SpendingLimit& escrow = escrow_opt.value();
 if (!escrow.model_hash.empty()) {
 ModelRegistry& registry = GetModelRegistry();
 if (registry.GetModelCount() > 0) {
 if (!registry.VerifyModelHash(escrow.model_name, escrow.model_hash)) {
 throw JSONRPCError(RPC_MISC_ERROR,
 "Model hash mismatch: escrow expects " + escrow.model_hash +
 " for model " + escrow.model_name +
 ". The registered model hash does not match.");
 }
 }
 }
 }

 NodeId target_peer = -1;
 bool explicit_peer = false;

 // Determine target peer BEFORE checking local miner
 if (request.params.size() > 2 && !request.params[2].isNull()) {
 target_peer = request.params[2].getInt<int64_t>();
 explicit_peer = true;
 } else {
 connman.ForEachNode([&target_peer](CNode* pnode) {
 if (target_peer < 0) {
 target_peer = pnode->GetId();
 }
 });
 }

 // === Only try local miner when no explicit remote peer requested ===
 if (!explicit_peer) {
 InferenceResult rpc_local = InferenceEngine::RequestLocalMiner(
 api_key, std::string(model), std::string(prompt), max_tokens);

 if (rpc_local.success) {
 UniValue ret{UniValue::VOBJ};
 ret.pushKV("content", rpc_local.content);
 ret.pushKV("tokens_used", rpc_local.tokens_used);
 ret.pushKV("cost", rpc_local.cost);
 ret.pushKV("peer_id", (int64_t)-1); // -1 = local

 // === Escrow billing: auto-deduct after local inference success ===
 if (has_api_key && rpc_local.tokens_used > 0) {
 BillingReceipt receipt;
 if (CheckAndDeductEscrow(api_key, rpc_local.tokens_used, receipt)) {
 // Re-read escrow to get updated state
 auto updated = FindSpendingLimitByAPIKey(api_key);
 UniValue escrow_obj{UniValue::VOBJ};
 if (updated.has_value()) {
 escrow_obj.pushKV("escrow_id", updated->escrow_id);
 escrow_obj.pushKV("remaining_tokens", updated->GetRemainingTokens());
 escrow_obj.pushKV("remaining_limit", ValueFromAmount(updated->spending_limit));
 escrow_obj.pushKV("state", SpendingLimitStateToString(updated->state));
 }
 ret.pushKV("escrow", escrow_obj);
 }
 }

 return ret;
 }
 // (Chinese comment removed)
 }

 if (target_peer < 0) {
 throw JSONRPCError(RPC_MISC_ERROR, "No connected peer found for inference");
 }

 APIRequest api_req;
 api_req.api_key = api_key;
 api_req.model = std::string(model);
 api_req.model_hash = "";
 api_req.max_tokens = max_tokens;
 {
 static std::atomic<uint64_t> s_req_counter{0};
 uint64_t counter_val = s_req_counter.fetch_add(1);
 api_req.request_id = static_cast<uint64_t>(GetTime()) * 1000000 + counter_val;
 api_req.nonce = static_cast<uint64_t>(GetTime()) * 1000000 + counter_val + 1;
 }

 ChatMessage msg;
 msg.role = "user";
 msg.content = std::string(prompt);
 api_req.messages.push_back(msg);

 std::future<APIResponse> future = peerman->SendInferenceRequest(target_peer, api_req);

 auto status = future.wait_for(std::chrono::seconds(86400));
 if (status == std::future_status::timeout) {
 throw JSONRPCError(RPC_MISC_ERROR, "P2P inference request timed out after 86400 seconds");
 }

 APIResponse response = future.get();

 int64_t effective_tokens = response.tokens_used;
 if (effective_tokens == 0 && !response.content.empty()) {
 effective_tokens = std::max((int64_t)1, (int64_t)(response.content.length() / 4));
 }

 UniValue ret{UniValue::VOBJ};
 ret.pushKV("content", response.content);
 ret.pushKV("tokens_used", effective_tokens);
 ret.pushKV("cost", response.cost);
 ret.pushKV("peer_id", target_peer);

 // === Escrow billing: auto-deduct after P2P inference success ===
 if (has_api_key && effective_tokens > 0) {
 BillingReceipt receipt;
 if (CheckAndDeductEscrow(api_key, effective_tokens, receipt)) {
 auto updated = FindSpendingLimitByAPIKey(api_key);
 UniValue escrow_obj{UniValue::VOBJ};
 if (updated.has_value()) {
 escrow_obj.pushKV("escrow_id", updated->escrow_id);
 escrow_obj.pushKV("remaining_tokens", updated->GetRemainingTokens());
 escrow_obj.pushKV("remaining_limit", ValueFromAmount(updated->spending_limit));
 escrow_obj.pushKV("state", SpendingLimitStateToString(updated->state));
 }
 ret.pushKV("escrow", escrow_obj);
 }
 }

 return ret;
 },
 };
}

static RPCMethod sendmsgtopeer()
{
 return RPCMethod{
 "sendmsgtopeer",
 "Send a p2p message to a peer specified by id.\n"
 "The message type and body must be provided, the message header will be generated.\n"
 "This RPC is for testing only.",
 {
 {"peer_id", RPCArg::Type::NUM, RPCArg::Optional::NO, "The peer to send the message to."},
 {"msg_type", RPCArg::Type::STR, RPCArg::Optional::NO, strprintf("The message type (maximum length %i)", CMessageHeader::MESSAGE_TYPE_SIZE)},
 {"msg", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The serialized message body to send, in hex, without a message header"},
 },
 RPCResult{RPCResult::Type::OBJ, "", "", std::vector<RPCResult>{}},
 RPCExamples{
 HelpExampleCli("sendmsgtopeer", "0 \"addr\" \"ffffff\"") + HelpExampleRpc("sendmsgtopeer", "0 \"addr\" \"ffffff\"")},
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
 const NodeId peer_id{request.params[0].getInt<int64_t>()};
 const auto msg_type{self.Arg<std::string_view>("msg_type")};
 if (msg_type.size() > CMessageHeader::MESSAGE_TYPE_SIZE) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Error: msg_type too long, max length is %i", CMessageHeader::MESSAGE_TYPE_SIZE));
 }
 auto msg{TryParseHex<unsigned char>(self.Arg<std::string_view>("msg"))};
 if (!msg.has_value()) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "Error parsing input for msg");
 }

 NodeContext& node = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node);

 CSerializedNetMsg msg_ser;
 msg_ser.data = msg.value();
 msg_ser.m_type = msg_type;

 bool success = connman.ForNode(peer_id, [&](CNode* node) {
 connman.PushMessage(node, std::move(msg_ser));
 return true;
 });

 if (!success) {
 throw JSONRPCError(RPC_MISC_ERROR, "Error: Could not send message to peer");
 }

 UniValue ret{UniValue::VOBJ};
 return ret;
 },
 };
}

static RPCMethod getaddrmaninfo()
{
 return RPCMethod{
 "getaddrmaninfo",
 "Provides information about the node's address manager by returning the number of "
 "addresses in the `new` and `tried` tables and their sum for all networks.\n",
 {},
 RPCResult{
 RPCResult::Type::OBJ_DYN, "", "json object with network type as keys", {
 {RPCResult::Type::OBJ, "network", "the network (" + Join(GetNetworkNames(), ", ") + ", all_networks)", {
 {RPCResult::Type::NUM, "new", "number of addresses in the new table, which represent potential peers the node has discovered but hasn't yet successfully connected to."},
 {RPCResult::Type::NUM, "tried", "number of addresses in the tried table, which represent peers the node has successfully connected to in the past."},
 {RPCResult::Type::NUM, "total", "total number of addresses in both new/tried tables"},
 }},
 }},
 RPCExamples{HelpExampleCli("getaddrmaninfo", "") + HelpExampleRpc("getaddrmaninfo", "")},
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
 AddrMan& addrman = EnsureAnyAddrman(request.context);

 UniValue ret(UniValue::VOBJ);
 for (int n = 0; n < NET_MAX; ++n) {
 enum Network network = static_cast<enum Network>(n);
 if (network == NET_UNROUTABLE || network == NET_INTERNAL) continue;
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("new", addrman.Size(network, true));
 obj.pushKV("tried", addrman.Size(network, false));
 obj.pushKV("total", addrman.Size(network));
 ret.pushKV(GetNetworkName(network), std::move(obj));
 }
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("new", addrman.Size(std::nullopt, true));
 obj.pushKV("tried", addrman.Size(std::nullopt, false));
 obj.pushKV("total", addrman.Size());
 ret.pushKV("all_networks", std::move(obj));
 return ret;
 },
 };
}

static RPCMethod exportasmap()
{
 return RPCMethod{
 "exportasmap",
 "Export the embedded ASMap data to a file. Any existing file at the path will be overwritten.\n",
 {
 {"path", RPCArg::Type::STR, RPCArg::Optional::NO, "Path to the output file. If relative, will be prefixed by datadir."},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "path", "the absolute path that the ASMap data was written to"},
 {RPCResult::Type::NUM, "bytes_written", "the number of bytes written to the file"},
 {RPCResult::Type::STR_HEX, "file_hash", "the SHA256 hash of the exported ASMap data"},
 }
 },
 RPCExamples{
 HelpExampleCli("exportasmap", "\"asmap.dat\"") + HelpExampleRpc("exportasmap", "\"asmap.dat\"")},
 [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
#ifndef ENABLE_EMBEDDED_ASMAP
 throw JSONRPCError(RPC_MISC_ERROR, "No embedded ASMap data available");
#else
 if (node::data::ip_asn.empty() || !CheckStandardAsmap(node::data::ip_asn)) {
 throw JSONRPCError(RPC_MISC_ERROR, "Embedded ASMap data appears to be corrupted");
 }

 const ArgsManager& args{EnsureAnyArgsman(request.context)};
 const fs::path export_path{fsbridge::AbsPathJoin(args.GetDataDirNet(), fs::u8path(self.Arg<std::string_view>("path")))};

 AutoFile file{fsbridge::fopen(export_path, "wb")};
 if (file.IsNull()) {
 throw JSONRPCError(RPC_MISC_ERROR, strprintf("Failed to open asmap file: %s", fs::PathToString(export_path)));
 }

 file << node::data::ip_asn;

 if (file.fclose() != 0) {
 throw JSONRPCError(RPC_MISC_ERROR, strprintf("Failed to close asmap file: %s", fs::PathToString(export_path)));
 }

 HashWriter hasher;
 hasher.write(node::data::ip_asn);

 UniValue result(UniValue::VOBJ);
 result.pushKV("path", export_path.utf8string());
 result.pushKV("bytes_written", (uint64_t)node::data::ip_asn.size());
 result.pushKV("file_hash", HexStr(hasher.GetSHA256()));
 return result;
#endif
 },
 };
}

UniValue AddrmanEntryToJSON(const AddrInfo& info, const CConnman& connman)
{
 UniValue ret(UniValue::VOBJ);
 ret.pushKV("address", info.ToStringAddr());
 const uint32_t mapped_as{connman.GetMappedAS(info)};
 if (mapped_as) {
 ret.pushKV("mapped_as", mapped_as);
 }
 ret.pushKV("port", info.GetPort());
 ret.pushKV("services", static_cast<std::underlying_type_t<decltype(info.nServices)>>(info.nServices));
 ret.pushKV("time", TicksSinceEpoch<std::chrono::seconds>(info.nTime));
 ret.pushKV("network", GetNetworkName(info.GetNetClass()));
 ret.pushKV("source", info.source.ToStringAddr());
 ret.pushKV("source_network", GetNetworkName(info.source.GetNetClass()));
 const uint32_t source_mapped_as{connman.GetMappedAS(info.source)};
 if (source_mapped_as) {
 ret.pushKV("source_mapped_as", source_mapped_as);
 }
 return ret;
}

UniValue AddrmanTableToJSON(const std::vector<std::pair<AddrInfo, AddressPosition>>& tableInfos, const CConnman& connman)
{
 UniValue table(UniValue::VOBJ);
 for (const auto& e : tableInfos) {
 AddrInfo info = e.first;
 AddressPosition location = e.second;
 std::ostringstream key;
 key << location.bucket << "/" << location.position;
 // Address manager tables have unique entries so there is no advantage
 // in using UniValue::pushKV, which checks if the key already exists
 // in O(N). UniValue::pushKVEnd is used instead which currently is O(1).
 table.pushKVEnd(key.str(), AddrmanEntryToJSON(info, connman));
 }
 return table;
}

static RPCMethod getrawaddrman()
{
 return RPCMethod{"getrawaddrman",
 "EXPERIMENTAL warning: this call may be changed in future releases.\n"
 "\nReturns information on all address manager entries for the new and tried tables.\n",
 {},
 RPCResult{
 RPCResult::Type::OBJ_DYN, "", "", {
 {RPCResult::Type::OBJ_DYN, "table", "buckets with addresses in the address manager table ( new, tried )", {
 {RPCResult::Type::OBJ, "bucket/position", "the location in the address manager table (<bucket>/<position>)", {
 {RPCResult::Type::STR, "address", "The address of the node"},
 {RPCResult::Type::NUM, "mapped_as", /*optional=*/true, "Mapped AS (Autonomous System) number at the end of the BGP route to the peer, used for diversifying peer selection (only displayed if the -asmap config option is set)"},
 {RPCResult::Type::NUM, "port", "The port number of the node"},
 {RPCResult::Type::STR, "network", "The network (" + Join(GetNetworkNames(), ", ") + ") of the address"},
 {RPCResult::Type::NUM, "services", "The services offered by the node"},
 {RPCResult::Type::NUM_TIME, "time", "The " + UNIX_EPOCH_TIME + " when the node was last seen"},
 {RPCResult::Type::STR, "source", "The address that relayed the address to us"},
 {RPCResult::Type::STR, "source_network", "The network (" + Join(GetNetworkNames(), ", ") + ") of the source address"},
 {RPCResult::Type::NUM, "source_mapped_as", /*optional=*/true, "Mapped AS (Autonomous System) number at the end of the BGP route to the source, used for diversifying peer selection (only displayed if the -asmap config option is set)"}
 }}
 }}
 }
 },
 RPCExamples{
 HelpExampleCli("getrawaddrman", "")
 + HelpExampleRpc("getrawaddrman", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue {
 AddrMan& addrman = EnsureAnyAddrman(request.context);
 NodeContext& node_context = EnsureAnyNodeContext(request.context);
 CConnman& connman = EnsureConnman(node_context);

 UniValue ret(UniValue::VOBJ);
 ret.pushKV("new", AddrmanTableToJSON(addrman.GetEntries(false), connman));
 ret.pushKV("tried", AddrmanTableToJSON(addrman.GetEntries(true), connman));
 return ret;
 },
 };
}

static RPCMethod getminerpeers()
{
 return RPCMethod{
 "getminerpeers",
 "Returns P2P-discovered miner peers with walleteer_id mapping.\n"
 "Each node with a local miner broadcasts MINER_INFO via P2P.\n"
 "This RPC returns the accumulated mapping for deterministic routing.",
 {},
 RPCResult{RPCResult::Type::ARR, "", "",
 {
 {RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::NUM, "peer_id", "P2P peer ID (use as target for p2pinference)"},
 {RPCResult::Type::STR, "wallet_address", "Miner's wallet address (unique identifier)"},
 {RPCResult::Type::STR, "model_name", "LLM model name"},
 {RPCResult::Type::STR, "gpu_name", "GPU device name"},
 {RPCResult::Type::NUM, "api_port", "Miner API port"},
 {RPCResult::Type::NUM_TIME, "last_seen", "Unix timestamp of last advertisement"},
 }},
 }},
 RPCExamples{
 HelpExampleCli("getminerpeers", "")
 + HelpExampleRpc("getminerpeers", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
 {
 NodeContext& node = EnsureAnyNodeContext(request.context);
 const PeerManager& peerman = EnsurePeerman(node);

 auto miner_peers = peerman.GetMinerPeers();
 UniValue arr(UniValue::VARR);
 for (const auto& entry : miner_peers) {
 UniValue obj(UniValue::VOBJ);
 obj.pushKV("peer_id", (int64_t)entry.first);
 obj.pushKV("wallet_address", entry.second.wallet_address);
 obj.pushKV("model_name", entry.second.model_name);
 obj.pushKV("gpu_name", entry.second.gpu_name);
 obj.pushKV("api_port", entry.second.api_port);
 obj.pushKV("last_seen", entry.second.last_seen);
 arr.push_back(obj);
 }
 return arr;
 },
 };
}

// Global public IP override (set via setpublicip RPC)
static std::string g_public_ip_override;
static std::mutex g_public_ip_mutex;

std::string GetPublicIPOverride()
{
 std::lock_guard<std::mutex> lock(g_public_ip_mutex);
 return g_public_ip_override;
}

std::string GetEffectivePublicIP()
{
 std::string override = GetPublicIPOverride();
 if (!override.empty()) return override;
 // TODO: Add UPnP/STUN auto-detection in future
 return "";
}

static RPCMethod setpublicip()
{
 return RPCMethod{"setpublicip",
 "Set the public IP address for this node.\n"
 "Used when the node cannot auto-detect its public IP (e.g. behind NAT without UPnP).\n"
 "Also supports setting a domain name for dynamic DNS scenarios.\n",
 {
 {"ip_or_domain", RPCArg::Type::STR, RPCArg::Optional::NO, "Public IP address or domain name"},
 },
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "public_ip", "The public IP/domain that was set"},
 {RPCResult::Type::STR, "status", "Status message"},
 }
 },
 RPCExamples{
 HelpExampleCli("setpublicip", "\"203.0.113.5\"")
 + HelpExampleCli("setpublicip", "\"my-node.example.com\"")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
 {
 std::string ip_or_domain = request.params[0].get_str();

 if (ip_or_domain.empty()) {
 throw JSONRPCError(RPC_INVALID_PARAMETER, "IP or domain must not be empty");
 }

 // Validate format: must not be a private IP
 if (ip_or_domain == "127.0.0.1" || ip_or_domain == "::1" ||
 ip_or_domain.find("192.168.") == 0 ||
 ip_or_domain.find("10.") == 0 ||
 ip_or_domain.find("172.16.") == 0) {
 throw JSONRPCError(RPC_INVALID_PARAMETER,
 "Cannot set a private/loopback IP as public IP. Use a real public address.");
 }

 {
 std::lock_guard<std::mutex> lock(g_public_ip_mutex);
 g_public_ip_override = ip_or_domain;
 }

 UniValue result(UniValue::VOBJ);
 result.pushKV("public_ip", ip_or_domain);
 result.pushKV("status", "Public IP set successfully. Node will advertise this address to peers.");

 LogInfo("RPC: setpublicip - Public IP set to %s", ip_or_domain);

 return result;
 },
 };
}

static RPCMethod getpublicip()
{
 return RPCMethod{"getpublicip",
 "Get the currently configured public IP address.\n",
 {},
 RPCResult{
 RPCResult::Type::OBJ, "", "",
 {
 {RPCResult::Type::STR, "public_ip", "The configured public IP/domain (empty if not set)"},
 {RPCResult::Type::STR, "effective_ip", "The effective public IP (override or auto-detected)"},
 }
 },
 RPCExamples{
 HelpExampleCli("getpublicip", "")
 },
 [](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
 {
 std::string public_ip = GetPublicIPOverride();
 std::string effective_ip = GetEffectivePublicIP();

 UniValue result(UniValue::VOBJ);
 result.pushKV("public_ip", public_ip);
 result.pushKV("effective_ip", effective_ip);
 return result;
 },
 };
}

void RegisterNetRPCCommands(CRPCTable& t)
{
 static const CRPCCommand commands[]{
 {"network", &getconnectioncount},
 {"network", &ping},
 {"network", &getpeerinfo},
 {"network", &addnode},
 {"network", &disconnectnode},
 {"network", &getaddednodeinfo},
 {"network", &getnettotals},
 {"network", &getnetworkinfo},
 {"network", &setban},
 {"network", &listbanned},
 {"network", &clearbanned},
 {"network", &setnetworkactive},
 {"network", &getnodeaddresses},
 {"network", &getaddrmaninfo},
 {"network", &exportasmap},
 {"network", &p2pinference},
 {"network", &getminerpeers},
 {"network", &setpublicip},
 {"network", &getpublicip},
 {"hidden", &addconnection},
 {"hidden", &addpeeraddress},
 {"hidden", &sendmsgtopeer},
 {"hidden", &getrawaddrman},
 };
 for (const auto& c : commands) {
 t.appendCommand(c.name, &c);
 }
}
