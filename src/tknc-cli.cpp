// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2024-present The TKN Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <tknc-build-config.h> // IWYU pragma: keep

#include <chainparamsbase.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/license_info.h>
#include <common/system.h>
#include <compat/compat.h>
#include <compat/stdin.h>
#include <interfaces/init.h>
#include <interfaces/ipc.h>
#include <interfaces/rpc.h>
#include <rpc/client.h>
#include <rpc/mining.h>
#include <rpc/protocol.h>
#include <rpc/request.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/chaintype.h>
#include <util/exception.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <util/fs_helpers.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>
#include <set>
#include <sstream>
#include <iomanip>

#ifndef WIN32
#include <unistd.h>
#include <termios.h>
#else
#include <conio.h>
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#endif

#include <event2/buffer.h>
#include <event2/keyvalq_struct.h>
#include <support/events.h>

// For BIP39 mnemonic system (real key derivation)
#include <key.h>
#include <key_io.h>
#include <random.h>

#include <compat/openssl_compat.h>
#include <crypto/sha256.h>

using util::Join;
using util::ToString;

// Use plain system_clock; mocked server time is not needed for now.
using CliClock = std::chrono::system_clock;

const TranslateFn G_TRANSLATION_FUN{nullptr};

static const char DEFAULT_RPCCONNECT[] = "127.0.0.1";
static constexpr const char* DEFAULT_RPC_REQ_ID{"1"};
static const int DEFAULT_HTTP_CLIENT_TIMEOUT=900;
static constexpr int DEFAULT_WAIT_CLIENT_TIMEOUT = 0;
static const bool DEFAULT_NAMED=false;
static const int CONTINUE_EXECUTION=-1;
static constexpr uint8_t NETINFO_MAX_LEVEL{4};
static constexpr int8_t UNKNOWN_NETWORK{-1};
// See GetNetworkName() in netbase.cpp
static constexpr std::array NETWORKS{"not_publicly_routable", "ipv4", "ipv6", "onion", "i2p", "cjdns", "internal"};
static constexpr std::array NETWORK_SHORT_NAMES{"npr", "ipv4", "ipv6", "onion", "i2p", "cjdns", "int"};
static constexpr std::array UNREACHABLE_NETWORK_IDS{/*not_publicly_routable*/0, /*internal*/6};

/** Default number of blocks to generate for RPC generatetoaddress. */
static const std::string DEFAULT_NBLOCKS = "1";

/** Default -color setting. */
static const std::string DEFAULT_COLOR_SETTING{"auto"};

static void SetupCliArgs(ArgsManager& argsman)
{
    SetupHelpOptions(argsman);

    const auto defaultBaseParams = CreateBaseChainParams(ChainType::MAIN);
    const auto testnetBaseParams = CreateBaseChainParams(ChainType::TESTNET);
    const auto testnet4BaseParams = CreateBaseChainParams(ChainType::TESTNET4);
    const auto signetBaseParams = CreateBaseChainParams(ChainType::SIGNET);
    const auto regtestBaseParams = CreateBaseChainParams(ChainType::REGTEST);

    argsman.AddArg("-version", "Print version and exit", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-conf=<file>", strprintf("Specify configuration file. Relative paths will be prefixed by datadir location. (default: %s)", TKNC_CONF_FILENAME), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-datadir=<dir>", "Specify data directory", ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-generate",
                   strprintf("Generate blocks, equivalent to RPC getnewaddress followed by RPC generatetoaddress. Optional positional integer "
                             "arguments are number of blocks to generate (default: %s) and maximum iterations to try (default: %s), equivalent to "
                             "RPC generatetoaddress nblocks and maxtries arguments. Example: tknc-cli -generate 4 1000",
                             DEFAULT_NBLOCKS, DEFAULT_MAX_TRIES),
                   ArgsManager::ALLOW_ANY, OptionsCategory::CLI_COMMANDS);
    argsman.AddArg("-addrinfo", "Get the number of addresses known to the node, per network and total.", ArgsManager::ALLOW_ANY, OptionsCategory::CLI_COMMANDS);
    argsman.AddArg("-getinfo", "Get general information from the remote server. Note that unlike server-side RPC calls, the output of -getinfo is the result of multiple non-atomic requests. Some entries in the output may represent results from different states (e.g. wallet balance may be as of a different block from the chain state reported)", ArgsManager::ALLOW_ANY, OptionsCategory::CLI_COMMANDS);
    argsman.AddArg("-netinfo", strprintf("Get network peer connection information from the remote server. An optional argument from 0 to %d can be passed for different peers listings (default: 0). If a non-zero value is passed, an additional \"outonly\" (or \"o\") argument can be passed to see outbound peers only. Pass \"help\" (or \"h\") for detailed help documentation.", NETINFO_MAX_LEVEL), ArgsManager::ALLOW_ANY, OptionsCategory::CLI_COMMANDS);

    SetupChainParamsBaseOptions(argsman);
    argsman.AddArg("-color=<when>", strprintf("Color setting for CLI output (default: %s). Valid values: always, auto (add color codes when standard output is connected to a terminal and OS is not WIN32), never. Only applies to the output of -getinfo.", DEFAULT_COLOR_SETTING), ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-named", strprintf("Pass named instead of positional arguments (default: %s)", DEFAULT_NAMED), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcid=<id>", strprintf("Set a custom JSON-RPC request ID string (default: %s)", DEFAULT_RPC_REQ_ID), ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION | ArgsManager::DISALLOW_ELISION, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcclienttimeout=<n>", strprintf("Timeout in seconds during HTTP requests, or 0 for no timeout. (default: %d)", DEFAULT_HTTP_CLIENT_TIMEOUT), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcconnect=<ip>", strprintf("Send commands to node running on <ip> (default: %s)", DEFAULT_RPCCONNECT), ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpccookiefile=<loc>", "Location of the auth cookie. Relative paths will be prefixed by the executable directory. (default: executable directory)", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcpassword=<pw>", "Password for JSON-RPC connections", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcport=<port>", strprintf("Connect to JSON-RPC on <port> (default: %u, testnet: %u, testnet4: %u, signet: %u, regtest: %u)", defaultBaseParams->RPCPort(), testnetBaseParams->RPCPort(), testnet4BaseParams->RPCPort(), signetBaseParams->RPCPort(), regtestBaseParams->RPCPort()), ArgsManager::ALLOW_ANY | ArgsManager::NETWORK_ONLY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcuser=<user>", "Username for JSON-RPC connections", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcwait", "Wait for RPC server to start", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcwaittimeout=<n>", strprintf("Timeout in seconds to wait for the RPC server to start, or 0 for no timeout. (default: %d)", DEFAULT_WAIT_CLIENT_TIMEOUT), ArgsManager::ALLOW_ANY | ArgsManager::DISALLOW_NEGATION, OptionsCategory::OPTIONS);
    argsman.AddArg("-rpcwallet=<walletname>", "Send RPC for non-default wallet on RPC server (needs to exactly match corresponding -wallet option passed to tkncd). This changes the RPC endpoint used, e.g. http://127.0.0.1:9331/wallet/<walletname>", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-stdin", "Read extra arguments from standard input, one per line until EOF/Ctrl-D (recommended for sensitive information such as passphrases). When combined with -stdinrpcpass, the first line from standard input is used for the RPC password.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-stdinrpcpass", "Read RPC password from standard input as a single line. When combined with -stdin, the first line from standard input is used for the RPC password. When combined with -stdinwalletpassphrase, -stdinrpcpass consumes the first line, and -stdinwalletpassphrase consumes the second.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-stdinwalletpassphrase", "Read wallet passphrase from standard input as a single line. When combined with -stdin, the first line from standard input is used for the wallet passphrase.", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
    argsman.AddArg("-ipcconnect=<address>", "Connect to tknc-node through IPC socket instead of TCP socket to execute requests. Valid <address> values are 'auto' to try to connect to default socket path at <datadir>/node.sock but fall back to TCP if it is not available, 'unix' to connect to the default socket and fail if it isn't available, or 'unix:<socket path>' to connect to a socket at a nonstandard path. -noipcconnect can be specified to avoid attempting to use IPC at all. Default value: auto", ArgsManager::ALLOW_ANY, OptionsCategory::IPC);
}

std::optional<std::string> RpcWalletName(const ArgsManager& args)
{
    // Check IsArgNegated to return nullopt instead of "0" if -norpcwallet is specified
    if (args.IsArgNegated("-rpcwallet")) return std::nullopt;
    return args.GetArg("-rpcwallet");
}

/** libevent event log callback */
static void libevent_log_cb(int severity, const char *msg)
{
    // Ignore everything other than errors
    if (severity >= EVENT_LOG_ERR) {
        throw std::runtime_error(strprintf("libevent error: %s", msg));
    }
}

// Exception on connection error; drives -rpcwait behavior.
struct CConnectionFailed : std::runtime_error {
    explicit inline CConnectionFailed(const std::string& msg) :
        std::runtime_error(msg)
    {}
};

// Returns EXIT_ code to stop or CONTINUE_EXECUTION to proceed.
static int AppInitRPC(int argc, char* argv[])
{
    SetupCliArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        tfm::format(std::cerr, "Error parsing command line arguments: %s\n", error);
        return EXIT_FAILURE;
    }
    if (argc < 2) {
        // No arguments → skip help text, but still need full initialization
        // (ReadConfigFiles, SelectBaseParams) before entering interactive mode.
        // Fall through to initialization below, then return EXIT_SUCCESS.
    } else if (HelpRequested(gArgs) || gArgs.GetBoolArg("-version", false)) {
        std::string strUsage = CLIENT_NAME " RPC client version " + FormatFullVersion() + "\n";

        if (gArgs.GetBoolArg("-version", false)) {
            strUsage += FormatParagraph(LicenseInfo());
        } else {
            strUsage += "\n"
                "The tknc-cli utility provides a command line interface to interact with a " CLIENT_NAME " RPC server.\n"
                "\nIt can be used to query network information, manage wallets, create or broadcast transactions, and control the " CLIENT_NAME " server.\n"
                "\nUse the \"help\" command to list all commands. Use \"help <command>\" to show help for that command.\n"
                "The -named option allows you to specify parameters using the key=value format, eliminating the need to pass unused positional parameters.\n"
                "\n"
                "Usage: tknc-cli [options] <command> [params]\n"
                "or:    tknc-cli [options] -named <command> [name=value]...\n"
                "or:    tknc-cli [options] help\n"
                "or:    tknc-cli [options] help <command>\n"
                "\n";
            strUsage += "\n" + gArgs.GetHelpMessage();
        }

        tfm::format(std::cout, "%s", strUsage);
        return EXIT_SUCCESS;
    }
    if (!CheckDataDirOption(gArgs)) {
        tfm::format(std::cerr, "Error: Specified data directory \"%s\" does not exist.\n", gArgs.GetArg("-datadir", ""));
        return EXIT_FAILURE;
    }
    if (!gArgs.ReadConfigFiles(error, true)) {
        tfm::format(std::cerr, "Error reading configuration file: %s\n", error);
        return EXIT_FAILURE;
    }
    // Check for chain settings (BaseParams() calls are only valid after this clause)
    try {
        SelectBaseParams(gArgs.GetChainType());
    } catch (const std::exception& e) {
        tfm::format(std::cerr, "Error: %s\n", e.what());
        return EXIT_FAILURE;
    } catch (...) {
        tfm::format(std::cerr, "Error: unknown exception in SelectBaseParams\n");
        return EXIT_FAILURE;
    }
    // No arguments → interactive mode (EXIT_SUCCESS routes to RunInteractiveMode)
    // Has command → continue to CommandLineRPC (CONTINUE_EXECUTION)
    return (argc < 2) ? EXIT_SUCCESS : CONTINUE_EXECUTION;
}


/** Reply structure for request_done to fill in */
struct HTTPReply
{
    HTTPReply() = default;

    int status{0};
    int error{-1};
    std::string body;
};

static std::string http_errorstring(int code)
{
    switch(code) {
    case EVREQ_HTTP_TIMEOUT:
        return "timeout reached";
    case EVREQ_HTTP_EOF:
        return "EOF reached";
    case EVREQ_HTTP_INVALID_HEADER:
        return "error while reading header, or invalid header";
    case EVREQ_HTTP_BUFFER_ERROR:
        return "error encountered while reading or writing";
    case EVREQ_HTTP_REQUEST_CANCEL:
        return "request was canceled";
    case EVREQ_HTTP_DATA_TOO_LONG:
        return "response body is larger than allowed";
    default:
        return "unknown";
    }
}

static void http_request_done(struct evhttp_request *req, void *ctx)
{
    HTTPReply *reply = static_cast<HTTPReply*>(ctx);

    if (req == nullptr) {
        reply->status = 0;
        return;
    }

    reply->status = evhttp_request_get_response_code(req);

    struct evbuffer *buf = evhttp_request_get_input_buffer(req);
    if (buf)
    {
        size_t size = evbuffer_get_length(buf);
        if (size > 0) {
            reply->body.resize(size);
            evbuffer_copyout(buf, &reply->body[0], size);
            evbuffer_drain(buf, size);
        }
    }
}

static void http_error_cb(enum evhttp_request_error err, void *ctx)
{
    HTTPReply *reply = static_cast<HTTPReply*>(ctx);
    reply->error = err;
}

static int8_t NetworkStringToId(const std::string& str)
{
    for (size_t i = 0; i < NETWORKS.size(); ++i) {
        if (str == NETWORKS[i]) return i;
    }
    return UNKNOWN_NETWORK;
}

/** Handle the conversion from a command-line to a JSON-RPC request,
 * as well as converting back to a JSON object that can be shown as result.
 */
struct BaseRequestHandler {
    virtual ~BaseRequestHandler() = default;
    virtual UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) = 0;
    virtual UniValue ProcessReply(const UniValue &batch_in) = 0;
};

/** Process addrinfo requests */
struct AddrinfoRequestHandler : BaseRequestHandler {
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        if (!args.empty()) {
            throw std::runtime_error("-addrinfo takes no arguments");
        }
        return JSONRPCRequestObj("getaddrmaninfo", NullUniValue, 1);
    }

    UniValue ProcessReply(const UniValue& reply) override
    {
        if (!reply["error"].isNull()) {
            if (reply["error"]["code"].getInt<int>() == RPC_METHOD_NOT_FOUND) {
                throw std::runtime_error("-addrinfo requires tkncd v26.0 or later which supports getaddrmaninfo RPC. Please upgrade your node or use tknc-cli from the same version.");
            }
            return reply;
        }
        // Process getaddrmaninfo reply
        const std::vector<std::string>& network_types{reply["result"].getKeys()};
        const std::vector<UniValue>& addrman_counts{reply["result"].getValues()};

        // Prepare result to return to user.
        UniValue result{UniValue::VOBJ}, addresses{UniValue::VOBJ};

        for (size_t i = 0; i < network_types.size(); ++i) {
            int addr_count = addrman_counts[i]["total"].getInt<int>();
            if (network_types[i] == "all_networks") {
                addresses.pushKV("total", addr_count);
            } else {
                addresses.pushKV(network_types[i], addr_count);
            }
        }
        result.pushKV("addresses_known", std::move(addresses));
        return JSONRPCReplyObj(std::move(result), NullUniValue, /*id=*/1, JSONRPCVersion::V2);
    }
};

/** Process getinfo requests */
struct GetinfoRequestHandler : BaseRequestHandler {
    const int ID_NETWORKINFO = 0;
    const int ID_BLOCKCHAININFO = 1;
    const int ID_WALLETINFO = 2;
    const int ID_BALANCES = 3;

    /** Create a simulated `getinfo` request. */
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        if (!args.empty()) {
            throw std::runtime_error("-getinfo takes no arguments");
        }
        UniValue result(UniValue::VARR);
        result.push_back(JSONRPCRequestObj("getnetworkinfo", NullUniValue, ID_NETWORKINFO));
        result.push_back(JSONRPCRequestObj("getblockchaininfo", NullUniValue, ID_BLOCKCHAININFO));
        result.push_back(JSONRPCRequestObj("getwalletinfo", NullUniValue, ID_WALLETINFO));
        result.push_back(JSONRPCRequestObj("getbalances", NullUniValue, ID_BALANCES));
        return result;
    }

    /** Collect values from the batch and form a simulated `getinfo` reply. */
    UniValue ProcessReply(const UniValue &batch_in) override
    {
        UniValue result(UniValue::VOBJ);
        const std::vector<UniValue> batch = JSONRPCProcessBatchReply(batch_in);
        // Errors in getnetworkinfo() and getblockchaininfo() are fatal, pass them on;
        // getwalletinfo() and getbalances() are allowed to fail if there is no wallet.
        if (!batch[ID_NETWORKINFO]["error"].isNull()) {
            return batch[ID_NETWORKINFO];
        }
        if (!batch[ID_BLOCKCHAININFO]["error"].isNull()) {
            return batch[ID_BLOCKCHAININFO];
        }
        result.pushKV("version", batch[ID_NETWORKINFO]["result"]["version"]);
        result.pushKV("blocks", batch[ID_BLOCKCHAININFO]["result"]["blocks"]);
        result.pushKV("headers", batch[ID_BLOCKCHAININFO]["result"]["headers"]);
        result.pushKV("verificationprogress", batch[ID_BLOCKCHAININFO]["result"]["verificationprogress"]);
        result.pushKV("timeoffset", batch[ID_NETWORKINFO]["result"]["timeoffset"]);

        UniValue connections(UniValue::VOBJ);
        connections.pushKV("in", batch[ID_NETWORKINFO]["result"]["connections_in"]);
        connections.pushKV("out", batch[ID_NETWORKINFO]["result"]["connections_out"]);
        connections.pushKV("total", batch[ID_NETWORKINFO]["result"]["connections"]);
        result.pushKV("connections", std::move(connections));

        result.pushKV("networks", batch[ID_NETWORKINFO]["result"]["networks"]);
        result.pushKV("difficulty", batch[ID_BLOCKCHAININFO]["result"]["difficulty"]);
        result.pushKV("chain", UniValue(batch[ID_BLOCKCHAININFO]["result"]["chain"]));
        if (!batch[ID_WALLETINFO]["result"].isNull()) {
            result.pushKV("has_wallet", true);
            result.pushKV("keypoolsize", batch[ID_WALLETINFO]["result"]["keypoolsize"]);
            result.pushKV("walletname", batch[ID_WALLETINFO]["result"]["walletname"]);
            if (!batch[ID_WALLETINFO]["result"]["unlocked_until"].isNull()) {
                result.pushKV("unlocked_until", batch[ID_WALLETINFO]["result"]["unlocked_until"]);
            }
        }
        if (!batch[ID_BALANCES]["result"].isNull()) {
            result.pushKV("balance", batch[ID_BALANCES]["result"]["mine"]["trusted"]);
        }
        result.pushKV("relayfee", batch[ID_NETWORKINFO]["result"]["relayfee"]);
        result.pushKV("warnings", batch[ID_NETWORKINFO]["result"]["warnings"]);
        return JSONRPCReplyObj(std::move(result), NullUniValue,  /*id=*/1, JSONRPCVersion::V2);
    }
};

/** Process netinfo requests */
class NetinfoRequestHandler : public BaseRequestHandler
{
private:
    std::array<std::array<uint16_t, NETWORKS.size() + 1>, 3> m_counts{{{}}}; //!< Peer counts by (in/out/total, networks/total)
    uint8_t m_block_relay_peers_count{0};
    uint8_t m_manual_peers_count{0};
    uint8_t m_details_level{0}; //!< Optional user-supplied arg to set dashboard details level
    bool DetailsRequested() const { return m_details_level; }
    bool IsAddressSelected() const { return m_details_level == 2 || m_details_level == 4; }
    bool IsVersionSelected() const { return m_details_level == 3 || m_details_level == 4; }
    bool m_outbound_only_selected{false};
    bool m_is_asmap_on{false};
    size_t m_max_addr_length{0};
    size_t m_max_addr_processed_length{5};
    size_t m_max_addr_rate_limited_length{6};
    size_t m_max_age_length{5};
    size_t m_max_id_length{2};
    size_t m_max_services_length{6};
    struct Peer {
        std::string addr;
        std::string sub_version;
        std::string conn_type;
        std::string network;
        std::string age;
        std::string services;
        std::string transport_protocol_type;
        double min_ping;
        double ping;
        int64_t addr_processed;
        int64_t addr_rate_limited;
        int64_t last_blck;
        int64_t last_recv;
        int64_t last_send;
        int64_t last_trxn;
        int id;
        int mapped_as;
        int version;
        bool is_addr_relay_enabled;
        bool is_bip152_hb_from;
        bool is_bip152_hb_to;
        bool is_outbound;
        bool is_tx_relay;
        bool operator<(const Peer& rhs) const { return std::tie(is_outbound, min_ping) < std::tie(rhs.is_outbound, rhs.min_ping); }
    };
    std::vector<Peer> m_peers;
    std::string ChainToString() const
    {
        switch (gArgs.GetChainType()) {
        case ChainType::TESTNET4:
            return " testnet4";
        case ChainType::TESTNET:
            return " testnet";
        case ChainType::SIGNET:
            return " signet";
        case ChainType::REGTEST:
            return " regtest";
        case ChainType::MAIN:
            return "";
        }
        assert(false);
    }
    std::string PingTimeToString(double seconds) const
    {
        if (seconds < 0) return "";
        const double milliseconds{round(1000 * seconds)};
        return milliseconds > 999999 ? "-" : ToString(milliseconds);
    }
    std::string ConnectionTypeForNetinfo(const std::string& conn_type) const
    {
        if (conn_type == "outbound-full-relay") return "full";
        if (conn_type == "block-relay-only") return "block";
        if (conn_type == "manual" || conn_type == "feeler") return conn_type;
        if (conn_type == "addr-fetch") return "addr";
        if (conn_type == "private-broadcast") return "priv";
        return "";
    }
    std::string FormatServices(const UniValue& services)
    {
        std::string str;
        for (size_t i = 0; i < services.size(); ++i) {
            const std::string s{services[i].get_str()};
            str += s == "NETWORK_LIMITED" ? 'l' : s == "P2P_V2" ? '2' : ToLower(s[0]);
        }
        return str;
    }
    static std::string ServicesList(const UniValue& services)
    {
        std::string str{services.size() ? services[0].get_str() : ""};
        for (size_t i{1}; i < services.size(); ++i) {
            str += ", " + services[i].get_str();
        }
        for (auto& c: str) {
            c = (c == '_' ? ' ' : ToLower(c));
        }
        return str;
    }

public:
    static constexpr int ID_PEERINFO = 0;
    static constexpr int ID_NETWORKINFO = 1;

    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        if (!args.empty()) {
            uint8_t n{0};
            if (const auto res{ToIntegral<uint8_t>(args.at(0))}) {
                n = *res;
                m_details_level = std::min(n, NETINFO_MAX_LEVEL);
            } else {
                throw std::runtime_error(strprintf("invalid -netinfo level argument: %s\nFor more information, run: tknc-cli -netinfo help", args.at(0)));
            }
            if (args.size() > 1) {
                if (std::string_view s{args.at(1)}; n && (s == "o" || s == "outonly")) {
                    m_outbound_only_selected = true;
                } else if (n) {
                    throw std::runtime_error(strprintf("invalid -netinfo outonly argument: %s\nFor more information, run: tknc-cli -netinfo help", s));
                } else {
                    throw std::runtime_error(strprintf("invalid -netinfo outonly argument: %s\nThe outonly argument is only valid for a level greater than 0 (the first argument). For more information, run: tknc-cli -netinfo help", s));
                }
            }
        }
        UniValue result(UniValue::VARR);
        result.push_back(JSONRPCRequestObj("getpeerinfo", NullUniValue, ID_PEERINFO));
        result.push_back(JSONRPCRequestObj("getnetworkinfo", NullUniValue, ID_NETWORKINFO));
        return result;
    }

    UniValue ProcessReply(const UniValue& batch_in) override
    {
        const std::vector<UniValue> batch{JSONRPCProcessBatchReply(batch_in)};
        if (!batch[ID_PEERINFO]["error"].isNull()) return batch[ID_PEERINFO];
        if (!batch[ID_NETWORKINFO]["error"].isNull()) return batch[ID_NETWORKINFO];

        const UniValue& networkinfo{batch[ID_NETWORKINFO]["result"]};
        if (networkinfo["version"].getInt<int>() < 209900) {
            throw std::runtime_error("-netinfo requires tkncd server to be running v0.21.0 and up");
        }
        const int64_t time_now{TicksSinceEpoch<std::chrono::seconds>(CliClock::now())};

        // Count peer connection totals, and if DetailsRequested(), store peer data in a vector of structs.
        for (const UniValue& peer : batch[ID_PEERINFO]["result"].getValues()) {
            const std::string network{peer["network"].get_str()};
            const int8_t network_id{NetworkStringToId(network)};
            if (network_id == UNKNOWN_NETWORK) continue;
            const bool is_outbound{!peer["inbound"].get_bool()};
            const bool is_tx_relay{peer["relaytxes"].isNull() ? true : peer["relaytxes"].get_bool()};
            const std::string conn_type{peer["connection_type"].get_str()};
            ++m_counts.at(is_outbound).at(network_id);      // in/out by network
            ++m_counts.at(is_outbound).at(NETWORKS.size()); // in/out overall
            ++m_counts.at(2).at(network_id);                // total by network
            ++m_counts.at(2).at(NETWORKS.size());           // total overall
            if (conn_type == "block-relay-only") ++m_block_relay_peers_count;
            if (conn_type == "manual") ++m_manual_peers_count;
            if (m_outbound_only_selected && !is_outbound) continue;
            if (DetailsRequested()) {
                // Push data for this peer to the peers vector.
                const int peer_id{peer["id"].getInt<int>()};
                const int mapped_as{peer["mapped_as"].isNull() ? 0 : peer["mapped_as"].getInt<int>()};
                const int version{peer["version"].getInt<int>()};
                const int64_t addr_processed{peer["addr_processed"].isNull() ? 0 : peer["addr_processed"].getInt<int64_t>()};
                const int64_t addr_rate_limited{peer["addr_rate_limited"].isNull() ? 0 : peer["addr_rate_limited"].getInt<int64_t>()};
                const int64_t conn_time{peer["conntime"].getInt<int64_t>()};
                const int64_t last_blck{peer["last_block"].getInt<int64_t>()};
                const int64_t last_recv{peer["lastrecv"].getInt<int64_t>()};
                const int64_t last_send{peer["lastsend"].getInt<int64_t>()};
                const int64_t last_trxn{peer["last_transaction"].getInt<int64_t>()};
                const double min_ping{peer["minping"].isNull() ? -1 : peer["minping"].get_real()};
                const double ping{peer["pingtime"].isNull() ? -1 : peer["pingtime"].get_real()};
                const std::string addr{peer["addr"].get_str()};
                const std::string age{conn_time == 0 ? "" : ToString((time_now - conn_time) / 60)};
                const std::string services{FormatServices(peer["servicesnames"])};
                const std::string sub_version{peer["subver"].get_str()};
                const std::string transport{peer["transport_protocol_type"].isNull() ? "v1" : peer["transport_protocol_type"].get_str()};
                const bool is_addr_relay_enabled{peer["addr_relay_enabled"].isNull() ? false : peer["addr_relay_enabled"].get_bool()};
                const bool is_bip152_hb_from{peer["bip152_hb_from"].get_bool()};
                const bool is_bip152_hb_to{peer["bip152_hb_to"].get_bool()};
                m_peers.push_back({addr, sub_version, conn_type, NETWORK_SHORT_NAMES[network_id], age, services, transport, min_ping, ping, addr_processed, addr_rate_limited, last_blck, last_recv, last_send, last_trxn, peer_id, mapped_as, version, is_addr_relay_enabled, is_bip152_hb_from, is_bip152_hb_to, is_outbound, is_tx_relay});
                m_max_addr_length = std::max(addr.length() + 1, m_max_addr_length);
                m_max_addr_processed_length = std::max(ToString(addr_processed).length(), m_max_addr_processed_length);
                m_max_addr_rate_limited_length = std::max(ToString(addr_rate_limited).length(), m_max_addr_rate_limited_length);
                m_max_age_length = std::max(age.length(), m_max_age_length);
                m_max_id_length = std::max(ToString(peer_id).length(), m_max_id_length);
                m_max_services_length = std::max(services.length(), m_max_services_length);
                m_is_asmap_on |= (mapped_as != 0);
            }
        }

        // Generate report header.
        const std::string services{DetailsRequested() ? strprintf(" - services %s", FormatServices(networkinfo["localservicesnames"])) : ""};
        std::string result{strprintf("%s client %s%s - server %i%s%s\n\n", CLIENT_NAME, FormatFullVersion(), ChainToString(), networkinfo["protocolversion"].getInt<int>(), networkinfo["subversion"].get_str(), services)};

        // Report detailed peer connections list sorted by direction and minimum ping time.
        if (DetailsRequested() && !m_peers.empty()) {
            std::sort(m_peers.begin(), m_peers.end());
            result += strprintf("<->   type   net %*s  v  mping   ping send recv  txn  blk  hb %*s%*s%*s ",
                                m_max_services_length, "serv",
                                m_max_addr_processed_length, "addrp",
                                m_max_addr_rate_limited_length, "addrl",
                                m_max_age_length, "age");
            if (m_is_asmap_on) result += " asmap ";
            result += strprintf("%*s %-*s%s\n", m_max_id_length, "id", IsAddressSelected() ? m_max_addr_length : 0, IsAddressSelected() ? "address" : "", IsVersionSelected() ? "version" : "");
            for (const Peer& peer : m_peers) {
                std::string version{ToString(peer.version) + peer.sub_version};
                result += strprintf(
                    "%3s %6s %5s %*s %2s%7s%7s%5s%5s%5s%5s  %2s %*s%*s%*s%*i %*s %-*s%s\n",
                    peer.is_outbound ? "out" : "in",
                    ConnectionTypeForNetinfo(peer.conn_type),
                    peer.network,
                    m_max_services_length, // variable spacing
                    peer.services,
                    (peer.transport_protocol_type.size() == 2 && peer.transport_protocol_type[0] == 'v') ? peer.transport_protocol_type[1] : ' ',
                    PingTimeToString(peer.min_ping),
                    PingTimeToString(peer.ping),
                    peer.last_send ? ToString(time_now - peer.last_send) : "",
                    peer.last_recv ? ToString(time_now - peer.last_recv) : "",
                    peer.last_trxn ? ToString((time_now - peer.last_trxn) / 60) : peer.is_tx_relay ? "" : "*",
                    peer.last_blck ? ToString((time_now - peer.last_blck) / 60) : "",
                    strprintf("%s%s", peer.is_bip152_hb_to ? "." : " ", peer.is_bip152_hb_from ? "*" : " "),
                    m_max_addr_processed_length, // variable spacing
                    peer.addr_processed ? ToString(peer.addr_processed) : peer.is_addr_relay_enabled ? "" : ".",
                    m_max_addr_rate_limited_length, // variable spacing
                    peer.addr_rate_limited ? ToString(peer.addr_rate_limited) : "",
                    m_max_age_length, // variable spacing
                    peer.age,
                    m_is_asmap_on ? 7 : 0, // variable spacing
                    m_is_asmap_on && peer.mapped_as ? ToString(peer.mapped_as) : "",
                    m_max_id_length, // variable spacing
                    peer.id,
                    IsAddressSelected() ? m_max_addr_length : 0, // variable spacing
                    IsAddressSelected() ? peer.addr : "",
                    IsVersionSelected() && version != "0" ? version : "");
            }
            result += strprintf("                %*s         ms     ms  sec  sec  min  min                %*s\n\n", m_max_services_length, "", m_max_age_length, "min");
        }

        // Report peer connection totals by type.
        result += "     ";
        std::vector<int8_t> reachable_networks;
        for (const UniValue& network : networkinfo["networks"].getValues()) {
            if (network["reachable"].get_bool()) {
                const std::string& network_name{network["name"].get_str()};
                const int8_t network_id{NetworkStringToId(network_name)};
                if (network_id == UNKNOWN_NETWORK) continue;
                result += strprintf("%8s", network_name); // column header
                reachable_networks.push_back(network_id);
            }
        };

        for (const size_t network_id : UNREACHABLE_NETWORK_IDS) {
            if (m_counts.at(2).at(network_id) == 0) continue;
            result += strprintf("%8s", NETWORK_SHORT_NAMES.at(network_id)); // column header
            reachable_networks.push_back(network_id);
        }

        result += "   total   block";
        if (m_manual_peers_count) result += "  manual";

        const std::array rows{"in", "out", "total"};
        for (size_t i = 0; i < rows.size(); ++i) {
            result += strprintf("\n%-5s", rows[i]); // row header
            for (int8_t n : reachable_networks) {
                result += strprintf("%8i", m_counts.at(i).at(n)); // network peers count
            }
            result += strprintf("   %5i", m_counts.at(i).at(NETWORKS.size())); // total peers count
            if (i == 1) { // the outbound row has two extra columns for block relay and manual peer counts
                result += strprintf("   %5i", m_block_relay_peers_count);
                if (m_manual_peers_count) result += strprintf("   %5i", m_manual_peers_count);
            }
        }

        // Report local services, addresses, ports, and scores.
        if (!DetailsRequested()) {
            result += strprintf("\n\nLocal services: %s", ServicesList(networkinfo["localservicesnames"]));
        }
        result += "\n\nLocal addresses";
        const std::vector<UniValue>& local_addrs{networkinfo["localaddresses"].getValues()};
        if (local_addrs.empty()) {
            result += ": n/a\n";
        } else {
            size_t max_addr_size{0};
            for (const UniValue& addr : local_addrs) {
                max_addr_size = std::max(addr["address"].get_str().length() + 1, max_addr_size);
            }
            for (const UniValue& addr : local_addrs) {
                result += strprintf("\n%-*s    port %6i    score %6i", max_addr_size, addr["address"].get_str(), addr["port"].getInt<int>(), addr["score"].getInt<int>());
            }
        }

        return JSONRPCReplyObj(UniValue{result}, NullUniValue, /*id=*/1, JSONRPCVersion::V2);
    }

    const std::string m_help_doc{
        "-netinfo (level [outonly]) | help\n\n"
        "Returns a network peer connections dashboard with information from the remote server.\n"
        "This human-readable interface will change regularly and is not intended to be a stable API.\n"
        "Under the hood, -netinfo fetches the data by calling getpeerinfo and getnetworkinfo.\n"
        + strprintf("An optional argument from 0 to %d can be passed for different peers listings; values above %d up to 255 are parsed as %d.\n", NETINFO_MAX_LEVEL, NETINFO_MAX_LEVEL, NETINFO_MAX_LEVEL) +
        "If that argument is passed, an optional additional \"outonly\" argument may be passed to obtain the listing with outbound peers only.\n"
        "Pass \"help\" or \"h\" to see this detailed help documentation.\n"
        "If more than two arguments are passed, only the first two are read and parsed.\n"
        "Suggestion: use -netinfo with the Linux watch(1) command for a live dashboard; see example below.\n\n"
        "Arguments:\n"
        + strprintf("1. level (integer 0-%d, optional)  Specify the info level of the peers dashboard (default 0):\n", NETINFO_MAX_LEVEL) +
        "                                  0 - Peer counts for each reachable network as well as for block relay peers\n"
        "                                      and manual peers, and the list of local addresses and ports\n"
        "                                  1 - Like 0 but preceded by a peers listing (without address and version columns)\n"
        "                                  2 - Like 1 but with an address column\n"
        "                                  3 - Like 1 but with a version column\n"
        "                                  4 - Like 1 but with both address and version columns\n"
        "2. outonly (\"outonly\" or \"o\", optional) Return the peers listing with outbound peers only, i.e. to save screen space\n"
        "                                        when a node has many inbound peers. Only valid if a level is passed.\n\n"
        "help (\"help\" or \"h\", optional) Print this help documentation instead of the dashboard.\n\n"
        "Result:\n\n"
        + strprintf("* The peers listing in levels 1-%d displays all of the peers sorted by direction and minimum ping time:\n\n", NETINFO_MAX_LEVEL) +
        "  Column   Description\n"
        "  ------   -----------\n"
        "  <->      Direction\n"
        "           \"in\"  - inbound connections are those initiated by the peer\n"
        "           \"out\" - outbound connections are those initiated by us\n"
        "  type     Type of peer connection\n"
        "           \"full\"   - full relay, the default\n"
        "           \"block\"  - block relay; like full relay but does not relay transactions or addresses\n"
        "           \"manual\" - peer we manually added using RPC addnode or the -addnode/-connect config options\n"
        "           \"feeler\" - short-lived connection for testing addresses\n"
        "           \"addr\"   - address fetch; short-lived connection for requesting addresses\n"
        "           \"priv\"   - private broadcast; short-lived connection for broadcasting our transactions\n"
        "  net      Network the peer connected through (\"ipv4\", \"ipv6\", \"onion\", \"i2p\", \"cjdns\", or \"npr\" (not publicly routable))\n"
        "  serv     Services offered by the peer\n"
        "           \"n\" - NETWORK: peer can serve the full block chain\n"
        "           \"b\" - BLOOM: peer can handle bloom-filtered connections (see BIP 111)\n"
        "           \"w\" - WITNESS: peer can be asked for blocks and transactions with witness data (SegWit)\n"
        "           \"c\" - COMPACT_FILTERS: peer can handle basic block filter requests (see BIPs 157 and 158)\n"
        "           \"l\" - NETWORK_LIMITED: peer limited to serving only the last 288 blocks (~2 days)\n"
        "           \"2\" - P2P_V2: peer supports version 2 P2P transport protocol, as defined in BIP 324\n"
        "           \"u\" - UNKNOWN: unrecognized bit flag\n"
        "  v        Version of transport protocol used for the connection\n"
        "  mping    Minimum observed ping time, in milliseconds (ms)\n"
        "  ping     Last observed ping time, in milliseconds (ms)\n"
        "  send     Time since last message sent to the peer, in seconds\n"
        "  recv     Time since last message received from the peer, in seconds\n"
        "  txn      Time since last novel transaction received from the peer and accepted into our mempool, in minutes\n"
        "           \"*\" - we do not relay transactions to this peer (getpeerinfo \"relaytxes\" is false)\n"
        "  blk      Time since last novel block passing initial validity checks received from the peer, in minutes\n"
        "  hb       High-bandwidth BIP152 compact block relay\n"
        "           \".\" (to)   - we selected the peer as a high-bandwidth peer\n"
        "           \"*\" (from) - the peer selected us as a high-bandwidth peer\n"
        "  addrp    Total number of addresses processed, excluding those dropped due to rate limiting\n"
        "           \".\" - we do not relay addresses to this peer (getpeerinfo \"addr_relay_enabled\" is false)\n"
        "  addrl    Total number of addresses dropped due to rate limiting\n"
        "  age      Duration of connection to the peer, in minutes\n"
        "  asmap    Mapped AS (Autonomous System) number at the end of the BGP route to the peer, used for diversifying\n"
        "           peer selection (only displayed if the -asmap config option is set)\n"
        "  id       Peer index, in increasing order of peer connections since node startup\n"
        "  address  IP address and port of the peer\n"
        "  version  Peer version and subversion concatenated, e.g. \"70016/TKNC:1.0.0/\"\n\n"
        "* The peer counts table displays the number of peers for each reachable network as well as\n"
        "  the number of block relay peers and manual peers.\n\n"
        "* The local addresses table lists each local address broadcast by the node, the port, and the score.\n\n"
        "Examples:\n\n"
        "Peer counts table of reachable networks and list of local addresses\n"
        "> tknc-cli -netinfo\n\n"
        "The same, preceded by a peers listing without address and version columns\n"
        "> tknc-cli -netinfo 1\n\n"
        "Full dashboard\n"
        + strprintf("> tknc-cli -netinfo %d\n\n", NETINFO_MAX_LEVEL) +
        "Full dashboard, but with outbound peers only\n"
        + strprintf("> tknc-cli -netinfo %d outonly\n\n", NETINFO_MAX_LEVEL) +
        "Full live dashboard, adjust --interval or --no-title as needed (Linux)\n"
        + strprintf("> watch --interval 1 --no-title tknc-cli -netinfo %d\n\n", NETINFO_MAX_LEVEL) +
        "See this help\n"
        "> tknc-cli -netinfo help\n"};
};

/** Process RPC generatetoaddress request. */
class GenerateToAddressRequestHandler : public BaseRequestHandler
{
public:
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        address_str = args.at(1);
        UniValue params{RPCConvertValues("generatetoaddress", args)};
        return JSONRPCRequestObj("generatetoaddress", params, 1);
    }

    UniValue ProcessReply(const UniValue &reply) override
    {
        UniValue result(UniValue::VOBJ);
        result.pushKV("address", address_str);
        result.pushKV("blocks", reply.get_obj()["result"]);
        return JSONRPCReplyObj(std::move(result), NullUniValue, /*id=*/1, JSONRPCVersion::V2);
    }
protected:
    std::string address_str;
};

/** Process default single requests */
struct DefaultRequestHandler : BaseRequestHandler {
    UniValue PrepareRequest(const std::string& method, const std::vector<std::string>& args) override
    {
        UniValue params;
        if(gArgs.GetBoolArg("-named", DEFAULT_NAMED)) {
            params = RPCConvertNamedValues(method, args);
        } else {
            params = RPCConvertValues(method, args);
        }
        UniValue id{UniValue::VSTR, gArgs.GetArg("-rpcid", DEFAULT_RPC_REQ_ID)};
        return JSONRPCRequestObj(method, params, id);
    }

    UniValue ProcessReply(const UniValue &reply) override
    {
        return reply.get_obj();
    }
};

static std::optional<UniValue> CallIPC(BaseRequestHandler* rh, const std::string& strMethod, const std::vector<std::string>& args, const std::string& endpoint, const std::string& username)
{
    auto ipcconnect{gArgs.GetArg("-ipcconnect", "auto")};
    if (ipcconnect == "0") return {}; // Do not attempt IPC if -ipcconnect is disabled.
    if (gArgs.IsArgSet("-rpcconnect") && !gArgs.IsArgNegated("-rpcconnect")) {
        if (ipcconnect == "auto") return {}; // Use HTTP if -ipcconnect=auto is set and -rpcconnect is enabled.
        throw std::runtime_error("-rpcconnect and -ipcconnect options cannot both be enabled");
    }

    std::unique_ptr<interfaces::Init> local_init{interfaces::MakeBasicInit("tknc-cli")};
    if (!local_init || !local_init->ipc()) {
        if (ipcconnect == "auto") return {}; // Use HTTP if -ipcconnect=auto is set and there is no IPC support.
        throw std::runtime_error("tknc-cli was not built with IPC support");
    }

    std::unique_ptr<interfaces::Init> node_init;
    try {
        node_init = local_init->ipc()->connectAddress(ipcconnect);
        if (!node_init) return {}; // Fall back to HTTP if -ipcconnect=auto connect failed.
    } catch (const std::exception& e) {
        // Catch connect error if -ipcconnect=unix was specified
        throw CConnectionFailed{strprintf("%s\n\n"
            "Probably tknc-node is not running or not listening on a unix socket. Can be started with:\n\n"
            "    tknc-node -chain=%s -ipcbind=unix", e.what(), gArgs.GetChainTypeString())};
    }

    std::unique_ptr<interfaces::Rpc> rpc{node_init->makeRpc()};
    assert(rpc);
    UniValue request{rh->PrepareRequest(strMethod, args)};
    UniValue reply{rpc->executeRpc(std::move(request), endpoint, username)};
    return rh->ProcessReply(reply);
}

static UniValue CallRPC(BaseRequestHandler* rh, const std::string& strMethod, const std::vector<std::string>& args, const std::string& endpoint, const std::string& username)
{
    std::string host;
    // Port preference: -rpcport, then -rpcconnect port, then chain default.
    uint16_t port{BaseParams().RPCPort()};
    {
        uint16_t rpcconnect_port{0};
        const std::string rpcconnect_str = gArgs.GetArg("-rpcconnect", DEFAULT_RPCCONNECT);
        if (!SplitHostPort(rpcconnect_str, rpcconnect_port, host)) {
            // Use raw arg (not parsed) to aid troubleshooting.
            throw std::runtime_error(strprintf("Invalid port provided in -rpcconnect: %s", rpcconnect_str));
        } else {
            if (rpcconnect_port != 0) {
                // Use the valid port provided in rpcconnect
                port = rpcconnect_port;
            } // else, no port was provided in rpcconnect (continue using default one)
        }

        if (std::optional<std::string> rpcport_arg = gArgs.GetArg("-rpcport")) {
            // -rpcport was specified
            const uint16_t rpcport_int{ToIntegral<uint16_t>(rpcport_arg.value()).value_or(0)};
            if (rpcport_int == 0) {
                // Use raw arg (not parsed) to aid troubleshooting.
                throw std::runtime_error(strprintf("Invalid port provided in -rpcport: %s", rpcport_arg.value()));
            }

            // Use the valid port provided
            port = rpcport_int;

            // If there was a valid port provided in rpcconnect,
            // rpcconnect_port is non-zero.
            if (rpcconnect_port != 0) {
                tfm::format(std::cerr, "Warning: Port specified in both -rpcconnect and -rpcport. Using -rpcport %u\n", port);
            }
        }
    }

    // Obtain event base
    raii_event_base base = obtain_event_base();

    // Synchronously look up hostname
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);

    // Set connection timeout
    {
        const int timeout = gArgs.GetIntArg("-rpcclienttimeout", DEFAULT_HTTP_CLIENT_TIMEOUT);
        if (timeout > 0) {
            evhttp_connection_set_timeout(evcon.get(), timeout);
        } else {
            // Indefinite request timeouts are not possible in libevent-http, so we
            // set the timeout to a very long time period instead.

            constexpr int YEAR_IN_SECONDS = 31556952; // Average length of year in Gregorian calendar
            evhttp_connection_set_timeout(evcon.get(), 5 * YEAR_IN_SECONDS);
        }
    }

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == nullptr) {
        throw std::runtime_error("create http request failed");
    }

    evhttp_request_set_error_cb(req.get(), http_error_cb);

    // Get credentials
    std::string rpc_credentials;
    std::optional<AuthCookieResult> auth_cookie_result;
    if (gArgs.GetArg("-rpcpassword", "") == "") {
        // Try fall back to cookie-based authentication if no password is provided
        auth_cookie_result = GetAuthCookie(rpc_credentials);
    } else {
        rpc_credentials = username + ":" + gArgs.GetArg("-rpcpassword", "");
    }

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    assert(output_headers);
    evhttp_add_header(output_headers, "Host", host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Content-Type", "application/json");
    evhttp_add_header(output_headers, "Authorization", (std::string("Basic ") + EncodeBase64(rpc_credentials)).c_str());

    // Attach request data
    std::string strRequest = rh->PrepareRequest(strMethod, args).write() + "\n";
    struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req.get());
    assert(output_buffer);
    evbuffer_add(output_buffer, strRequest.data(), strRequest.size());

    int r = evhttp_make_request(evcon.get(), req.release(), EVHTTP_REQ_POST, endpoint.c_str());
    if (r != 0) {
        throw CConnectionFailed("send http request failed");
    }

    event_base_dispatch(base.get());

    if (response.status == 0) {
        std::string responseErrorMessage;
        if (response.error != -1) {
            responseErrorMessage = strprintf(" (error code %d - \"%s\")", response.error, http_errorstring(response.error));
        }
        throw CConnectionFailed(strprintf("Could not connect to the server %s:%d%s\n\n"
                    "Make sure the tkncd server is running and that you are connecting to the correct RPC port.\n"
                    "Use \"tknc-cli -help\" for more info.",
                    host, port, responseErrorMessage));
    } else if (response.status == HTTP_UNAUTHORIZED) {
        std::string error{"Authorization failed: "};
        if (auth_cookie_result.has_value()) {
            switch (*auth_cookie_result) {
            case AuthCookieResult::Error:
                error += "Failed to read cookie file and no rpcpassword was specified.";
                break;
            case AuthCookieResult::Disabled:
                error += "Cookie file was disabled via -norpccookiefile and no rpcpassword was specified.";
                break;
            case AuthCookieResult::Ok:
                error += "Cookie file credentials were invalid and no rpcpassword was specified.";
                break;
            }
        } else {
            error += "Incorrect rpcuser or rpcpassword were specified.";
        }
        error += strprintf(" Configuration file: (%s)", fs::PathToString(gArgs.GetConfigFilePath()));
        throw std::runtime_error(error);
    } else if (response.status == HTTP_SERVICE_UNAVAILABLE) {
        throw std::runtime_error(strprintf("Server response: %s", response.body));
    } else if (response.status >= 400 && response.status != HTTP_BAD_REQUEST && response.status != HTTP_NOT_FOUND && response.status != HTTP_INTERNAL_SERVER_ERROR)
        throw std::runtime_error(strprintf("server returned HTTP error %d", response.status));
    else if (response.body.empty())
        throw std::runtime_error("no response from server");

    // Parse reply
    UniValue valReply(UniValue::VSTR);
    if (!valReply.read(response.body))
        throw std::runtime_error("couldn't parse reply from server");
    UniValue reply = rh->ProcessReply(valReply);
    if (reply.empty())
        throw std::runtime_error("expected reply to have result, error and id properties");

    return reply;
}

/**
 * ConnectAndCallRPC wraps CallRPC with -rpcwait and an exception handler.
 *
 * @param[in] rh         Pointer to RequestHandler.
 * @param[in] strMethod  Reference to const string method to forward to CallRPC.
 * @param[in] rpcwallet  Reference to const optional string wallet name to forward to CallRPC.
 * @returns the RPC response as a UniValue object.
 * @throws a CConnectionFailed std::runtime_error if connection failed or RPC server still in warmup.
 */
static UniValue ConnectAndCallRPC(BaseRequestHandler* rh, const std::string& strMethod, const std::vector<std::string>& args, const std::optional<std::string>& rpcwallet = {})
{
    UniValue response(UniValue::VOBJ);
    // Execute and handle connection failures with -rpcwait.
    const bool fWait = gArgs.GetBoolArg("-rpcwait", false);
    const int timeout = gArgs.GetIntArg("-rpcwaittimeout", DEFAULT_WAIT_CLIENT_TIMEOUT);
    const auto deadline{std::chrono::steady_clock::now() + 1s * timeout};

    // check if we should use a special wallet endpoint
    std::string endpoint = "/";
    if (rpcwallet) {
        char* encodedURI = evhttp_uriencode(rpcwallet->data(), rpcwallet->size(), false);
        if (encodedURI) {
            endpoint = "/wallet/" + std::string(encodedURI);
            free(encodedURI);
        } else {
            throw CConnectionFailed("uri-encode failed");
        }
    }

    std::string username{gArgs.GetArg("-rpcuser", "")};
    do {
        try {
            if (auto ipc_response{CallIPC(rh, strMethod, args, endpoint, username)}) {
                response = std::move(*ipc_response);
            } else {
                response = CallRPC(rh, strMethod, args, endpoint, username);
            }
            if (fWait) {
                const UniValue& error = response.find_value("error");
                if (!error.isNull() && error["code"].getInt<int>() == RPC_IN_WARMUP) {
                    throw CConnectionFailed("server in warmup");
                }
            }
            break; // Connection succeeded, no need to retry.
        } catch (const CConnectionFailed& e) {
            if (fWait && (timeout <= 0 || std::chrono::steady_clock::now() < deadline)) {
                UninterruptibleSleep(1s);
            } else {
                throw CConnectionFailed(strprintf("timeout on transient error: %s", e.what()));
            }
        }
    } while (fWait);
    return response;
}

/** Parse UniValue result to update the message to print to std::cout. */
static void ParseResult(const UniValue& result, std::string& strPrint)
{
    if (result.isNull()) return;
    strPrint = result.isStr() ? result.get_str() : result.write(2);
}

/** Parse UniValue error to update the message to print to std::cerr and the code to return. */
static void ParseError(const UniValue& error, std::string& strPrint, int& nRet)
{
    if (error.isObject()) {
        const UniValue& err_code = error.find_value("code");
        const UniValue& err_msg = error.find_value("message");
        if (!err_code.isNull()) {
            strPrint = "error code: " + err_code.getValStr() + "\n";
        }
        if (err_msg.isStr()) {
            strPrint += ("error message:\n" + err_msg.get_str());
        }
        if (err_code.isNum() && err_code.getInt<int>() == RPC_WALLET_NOT_SPECIFIED) {
            strPrint += " Or for the CLI, specify the \"-rpcwallet=<walletname>\" option before the command";
            strPrint += " (run \"tknc-cli -h\" for help or \"tknc-cli listwallets\" to see which wallets are currently loaded).";
        }
    } else {
        strPrint = "error: " + error.write();
    }
    nRet = abs(error["code"].getInt<int>());
}

/**
 * GetWalletBalances calls listwallets; if more than one wallet is loaded, it then
 * fetches mine.trusted balances for each loaded wallet and pushes them to `result`.
 *
 * @param result  Reference to UniValue object the wallet names and balances are pushed to.
 */
static void GetWalletBalances(UniValue& result)
{
    DefaultRequestHandler rh;
    const UniValue listwallets = ConnectAndCallRPC(&rh, "listwallets", /* args=*/{});
    if (!listwallets.find_value("error").isNull()) return;
    const UniValue& wallets = listwallets.find_value("result");
    if (wallets.size() <= 1) return;

    UniValue balances(UniValue::VOBJ);
    for (const UniValue& wallet : wallets.getValues()) {
        const std::string& wallet_name = wallet.get_str();
        const UniValue getbalances = ConnectAndCallRPC(&rh, "getbalances", /* args=*/{}, wallet_name);
        const UniValue& balance = getbalances.find_value("result")["mine"]["trusted"];
        balances.pushKV(wallet_name, balance);
    }
    result.pushKV("balances", std::move(balances));
}

/**
 * GetProgressBar constructs a progress bar with 5% intervals.
 *
 * @param[in]   progress      The proportion of the progress bar to be filled between 0 and 1.
 * @param[out]  progress_bar  String representation of the progress bar.
 */
static void GetProgressBar(double progress, std::string& progress_bar)
{
    if (progress < 0 || progress > 1) return;

    static constexpr double INCREMENT{0.05};
    static const std::string COMPLETE_BAR{"\u2592"};
    static const std::string INCOMPLETE_BAR{"\u2591"};

    for (int i = 0; i < progress / INCREMENT; ++i) {
        progress_bar += COMPLETE_BAR;
    }

    for (int i = 0; i < (1 - progress) / INCREMENT; ++i) {
        progress_bar += INCOMPLETE_BAR;
    }
}

/**
 * ParseGetInfoResult takes in -getinfo result in UniValue object and parses it
 * into a user friendly UniValue string to be printed on the console.
 * @param[out] result  Reference to UniValue result containing the -getinfo output.
 */
static void ParseGetInfoResult(UniValue& result)
{
    if (!result.find_value("error").isNull()) return;

    std::string RESET, GREEN, BLUE, YELLOW, MAGENTA, CYAN;
    bool should_colorize = false;

#ifndef WIN32
    if (isatty(fileno(stdout))) {
        // By default, only print colored text if OS is not WIN32 and stdout is connected to a terminal.
        should_colorize = true;
    }
#endif

    {
        const std::string color{gArgs.GetArg("-color", DEFAULT_COLOR_SETTING)};
        if (color == "always") {
            should_colorize = true;
        } else if (color == "never") {
            should_colorize = false;
        } else if (color != "auto") {
            throw std::runtime_error("Invalid value for -color option. Valid values: always, auto, never.");
        }
    }

    if (should_colorize) {
        RESET = "\x1B[0m";
        GREEN = "\x1B[32m";
        BLUE = "\x1B[34m";
        YELLOW = "\x1B[33m";
        MAGENTA = "\x1B[35m";
        CYAN = "\x1B[36m";
    }

    std::string result_string = strprintf("%sChain: %s%s\n", BLUE, result["chain"].getValStr(), RESET);
    result_string += strprintf("Blocks: %s\n", result["blocks"].getValStr());
    result_string += strprintf("Headers: %s\n", result["headers"].getValStr());

    const double ibd_progress{result["verificationprogress"].get_real()};
    std::string ibd_progress_bar;
    // Display the progress bar only if IBD progress is less than 99%
    if (ibd_progress < 0.99) {
      GetProgressBar(ibd_progress, ibd_progress_bar);
      // Add padding between progress bar and IBD progress
      ibd_progress_bar += " ";
    }

    result_string += strprintf("Verification progress: %s%.4f%%\n", ibd_progress_bar, ibd_progress * 100);
    result_string += strprintf("Difficulty: %s\n\n", result["difficulty"].getValStr());

    result_string += strprintf(
        "%sNetwork: in %s, out %s, total %s%s\n",
        GREEN,
        result["connections"]["in"].getValStr(),
        result["connections"]["out"].getValStr(),
        result["connections"]["total"].getValStr(),
        RESET);
    result_string += strprintf("Version: %s\n", result["version"].getValStr());
    result_string += strprintf("Time offset (s): %s\n", result["timeoffset"].getValStr());

    // proxies
    std::map<std::string, std::vector<std::string>> proxy_networks;
    std::vector<std::string> ordered_proxies;

    for (const UniValue& network : result["networks"].getValues()) {
        const std::string proxy = network["proxy"].getValStr();
        if (proxy.empty()) continue;
        // Add proxy to ordered_proxy if has not been processed
        if (!proxy_networks.contains(proxy)) ordered_proxies.push_back(proxy);

        proxy_networks[proxy].push_back(network["name"].getValStr());
    }

    std::vector<std::string> formatted_proxies;
    formatted_proxies.reserve(ordered_proxies.size());
    for (const std::string& proxy : ordered_proxies) {
        formatted_proxies.emplace_back(strprintf("%s (%s)", proxy, Join(proxy_networks.find(proxy)->second, ", ")));
    }
    result_string += strprintf("Proxies: %s\n", formatted_proxies.empty() ? "n/a" : Join(formatted_proxies, ", "));

    result_string += strprintf("Min tx relay fee rate (%s/kvB): %s\n\n", CURRENCY_UNIT, result["relayfee"].getValStr());

    if (!result["has_wallet"].isNull()) {
        const std::string walletname = result["walletname"].getValStr();
        result_string += strprintf("%sWallet: %s%s\n", MAGENTA, walletname.empty() ? "\"\"" : walletname, RESET);

        result_string += strprintf("Keypool size: %s\n", result["keypoolsize"].getValStr());
        if (!result["unlocked_until"].isNull()) {
            result_string += strprintf("Unlocked until: %s\n", result["unlocked_until"].getValStr());
        }
    }
    if (!result["balance"].isNull()) {
        result_string += strprintf("%sBalance:%s %s\n\n", CYAN, RESET, result["balance"].getValStr());
    }

    if (!result["balances"].isNull()) {
        result_string += strprintf("%sBalances%s\n", CYAN, RESET);

        size_t max_balance_length{10};

        for (const std::string& wallet : result["balances"].getKeys()) {
            max_balance_length = std::max(result["balances"][wallet].getValStr().length(), max_balance_length);
        }

        for (const std::string& wallet : result["balances"].getKeys()) {
            result_string += strprintf("%*s %s\n",
                                       max_balance_length,
                                       result["balances"][wallet].getValStr(),
                                       wallet.empty() ? "\"\"" : wallet);
        }
        result_string += "\n";
    }

    const std::string warnings{result["warnings"].getValStr()};
    result_string += strprintf("%sWarnings:%s %s", YELLOW, RESET, warnings.empty() ? "(none)" : warnings);

    result.setStr(result_string);
}

/**
 * Call RPC getnewaddress.
 * @returns getnewaddress response as a UniValue object.
 */
static UniValue GetNewAddress()
{
    DefaultRequestHandler rh;
    return ConnectAndCallRPC(&rh, "getnewaddress", /* args=*/{}, RpcWalletName(gArgs));
}

/**
 * Check bounds and set up args for RPC generatetoaddress params: nblocks, address, maxtries.
 * @param[in] address  Reference to const string address to insert into the args.
 * @param     args     Reference to vector of string args to modify.
 */
static void SetGenerateToAddressArgs(const std::string& address, std::vector<std::string>& args)
{
    if (args.size() > 2) throw std::runtime_error("too many arguments (maximum 2 for nblocks and maxtries)");
    if (args.size() == 0) {
        args.emplace_back(DEFAULT_NBLOCKS);
    } else if (args.at(0) == "0") {
        throw std::runtime_error("the first argument (number of blocks to generate, default: " + DEFAULT_NBLOCKS + ") must be an integer value greater than zero");
    }
    args.emplace(args.begin() + 1, address);
}

static int CommandLineRPC(int argc, char *argv[])
{
    std::string strPrint;
    int nRet = 0;
    try {
        // Skip switches
        while (argc > 1 && IsSwitchChar(argv[1][0])) {
            argc--;
            argv++;
        }
        std::string rpcPass;
        if (gArgs.GetBoolArg("-stdinrpcpass", false)) {
            NO_STDIN_ECHO();
            if (!StdinReady()) {
                fputs("RPC password> ", stderr);
                fflush(stderr);
            }
            if (!std::getline(std::cin, rpcPass)) {
                throw std::runtime_error("-stdinrpcpass specified but failed to read from standard input");
            }
            if (StdinTerminal()) {
                fputc('\n', stdout);
            }
            gArgs.ForceSetArg("-rpcpassword", rpcPass);
        }
        std::vector<std::string> args = std::vector<std::string>(&argv[1], &argv[argc]);
        if (gArgs.GetBoolArg("-stdinwalletpassphrase", false)) {
            NO_STDIN_ECHO();
            std::string walletPass;
            if (args.size() < 1 || !args[0].starts_with("walletpassphrase")) {
                throw std::runtime_error("-stdinwalletpassphrase is only applicable for walletpassphrase(change)");
            }
            if (!StdinReady()) {
                fputs("Wallet passphrase> ", stderr);
                fflush(stderr);
            }
            if (!std::getline(std::cin, walletPass)) {
                throw std::runtime_error("-stdinwalletpassphrase specified but failed to read from standard input");
            }
            if (StdinTerminal()) {
                fputc('\n', stdout);
            }
            args.insert(args.begin() + 1, walletPass);
        }
        if (gArgs.GetBoolArg("-stdin", false)) {
            // Read one arg per line from stdin and append
            std::string line;
            while (std::getline(std::cin, line)) {
                args.push_back(line);
            }
            if (StdinTerminal()) {
                fputc('\n', stdout);
            }
        }
        gArgs.CheckMultipleCLIArgs();
        std::unique_ptr<BaseRequestHandler> rh;
        std::string method;
        if (gArgs.GetBoolArg("-getinfo", false)) {
            rh.reset(new GetinfoRequestHandler());
        } else if (gArgs.GetBoolArg("-netinfo", false)) {
            if (!args.empty() && (args.at(0) == "h" || args.at(0) == "help")) {
                tfm::format(std::cout, "%s\n", NetinfoRequestHandler().m_help_doc);
                return 0;
            }
            rh.reset(new NetinfoRequestHandler());
        } else if (gArgs.GetBoolArg("-generate", false)) {
            const UniValue getnewaddress{GetNewAddress()};
            const UniValue& error{getnewaddress.find_value("error")};
            if (error.isNull()) {
                SetGenerateToAddressArgs(getnewaddress.find_value("result").get_str(), args);
                rh.reset(new GenerateToAddressRequestHandler());
            } else {
                ParseError(error, strPrint, nRet);
            }
        } else if (gArgs.GetBoolArg("-addrinfo", false)) {
            rh.reset(new AddrinfoRequestHandler());
        } else {
            rh.reset(new DefaultRequestHandler());
            if (args.size() < 1) {
                throw std::runtime_error("too few parameters (need at least command)");
            }
            method = args[0];
            args.erase(args.begin()); // Remove trailing method name from arguments vector
        }
        if (nRet == 0) {
            // Perform RPC call
            const std::optional<std::string> wallet_name{RpcWalletName(gArgs)};
            const UniValue reply = ConnectAndCallRPC(rh.get(), method, args, wallet_name);

            // Parse reply
            UniValue result = reply.find_value("result");
            const UniValue& error = reply.find_value("error");
            if (error.isNull()) {
                if (gArgs.GetBoolArg("-getinfo", false)) {
                    if (!wallet_name) {
                        GetWalletBalances(result); // fetch multiwallet balances and append to result
                    }
                    ParseGetInfoResult(result);
                }

                ParseResult(result, strPrint);
            } else {
                ParseError(error, strPrint, nRet);
            }
        }
    } catch (const std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRPC()");
        throw;
    }

    if (strPrint != "") {
        tfm::format(nRet == 0 ? std::cout : std::cerr, "%s\n", strPrint);
    }
    return nRet;
}

// ==================== Forward Declarations for Inference Service ====================

// Forward declarations for functions used by inference service
static void ClearScreen();
static void PressContinue();

// ==================== HTTP Client for WEB Server ====================

/**
 * CallHTTP - Make HTTP POST request to a server (WEB seed or local API Gateway).
 * Used for inference service API calls.
 * @param host  e.g. "localhost" or "66.154.101.183" or "127.0.0.1"
 * @param port  e.g. 80 or 9313
 * @param endpoint  e.g. "/api/v1/chat" or "/v1/chat/completions"
 * @param body  JSON body string
 * @param extra_headers  additional headers (e.g. "Authorization: Bearer xxx")
 * @param timeout_sec  connection/read timeout in seconds (default 30)
 * @returns response body string
 */
static std::string CallHTTP(const std::string& host, int port, const std::string& endpoint,
                            const std::string& body, const std::string& extra_headers = "",
                            int timeout_sec = 30)
{
    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);
    evhttp_connection_set_timeout(evcon.get(), timeout_sec);

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    if (req == nullptr) {
        throw std::runtime_error("create http request failed");
    }
    evhttp_request_set_error_cb(req.get(), http_error_cb);

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    assert(output_headers);
    evhttp_add_header(output_headers, "Host", host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Content-Type", "application/json");
    if (!extra_headers.empty()) {
        // Parse "Key: Value" pairs separated by \r\n
        std::istringstream stream(extra_headers);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string val = line.substr(pos + 1);
                if (!val.empty() && val[0] == ' ') val = val.substr(1);
                evhttp_add_header(output_headers, key.c_str(), val.c_str());
            }
        }
    }

    struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req.get());
    assert(output_buffer);
    evbuffer_add(output_buffer, body.data(), body.size());

    int r = evhttp_make_request(evcon.get(), req.release(), EVHTTP_REQ_POST, endpoint.c_str());
    if (r != 0) {
        throw std::runtime_error("send http request failed");
    }
    event_base_dispatch(base.get());

    if (response.status == 0) {
        throw std::runtime_error("Could not connect to server " + host + ":" + std::to_string(port));
    }
    if (response.status >= 400) {
        throw std::runtime_error("Server returned HTTP " + std::to_string(response.status) + ": " + response.body);
    }
    return response.body;
}

// ==================== Inference Service Globals ====================

static std::string g_last_api_key;
static std::string g_last_endpoint;
static std::string g_web_host = "66.154.101.183";
static int g_web_port = 80;

struct APIKeyInfo {
    std::string api_key;
    std::string miner_wallet;
    std::string model_name;
    std::string endpoint;
    double balance = 0.0;
    int64_t used;
    int64_t remaining;
int64_t tokens_per_tknc = 0;
};

static std::vector<APIKeyInfo> g_stored_api_keys;

// ==================== Interactive Menu System ====================

struct Language {
    int code;
    std::string name;
    std::string native_name;
};

static const std::vector<Language> LANGUAGES = {
    {1, "English", "English"},
    {2, "中文简体", "简体中文"},
    {3, "日本語", "日本語"},
    {4, "한국어", "한국어"},
    {5, "Español", "Español"},
    {6, "Français", "Français"},
    {7, "Deutsch", "Deutsch"},
    {8, "Русский", "Русский"},
    {9, "Português", "Português"}
};

struct I18nTexts {
    std::string select_language;
    std::string main_menu_title;
    std::string menu_new_wallet;
    std::string menu_restore_wallet;
    std::string menu_open_wallet;
    std::string menu_wallet_info;
    std::string menu_send_tknc;
    std::string menu_backup_keys;
    std::string menu_node_info;
    std::string menu_blockchain_info;
    std::string menu_network_info;
    std::string menu_block_info;
    std::string menu_transactions;
    std::string menu_mining_info;
    std::string menu_generate_address;
    std::string menu_logout;
    std::string menu_wallet_address;
    std::string menu_wallet_balance;
    std::string menu_sign_message;
    std::string menu_exit;
    std::string menu_back;       // q shortcut
    std::string menu_quit;       // e shortcut
    std::string logged_in_title;
    std::string logged_in_as;
    std::string not_logged_in_title;
    std::string public_menu_title;
    std::string wallet_menu_title;
    std::string prompt_select;
    std::string create_wallet_title;
    std::string prompt_wallet_name;
    std::string prompt_password;
    std::string prompt_password_confirm;
    std::string password_mismatch;
    std::string wallet_creating;
    std::string wallet_created_success;
    std::string wallet_address_label;
    std::string wallet_balance_label;
    std::string wallet_total_balance_label;
    std::string addr_balance_label;
    std::string select_address_prompt;
    std::string addr_switched_msg;
    std::string send_from_label;
    std::string no_utxo_msg;
    std::string restore_title;
    std::string restore_from_backup;
    std::string restore_from_descriptors;
    std::string restore_prompt_file;
    std::string restore_success;
    std::string restore_failed;
    std::string info_title;
    std::string info_wallet_name;
    std::string info_address;
    std::string info_balance;
    std::string info_no_wallet;
    std::string send_title;
    std::string send_to_address;
    std::string send_amount;
    std::string send_confirm;
    std::string send_success;
    std::string send_failed;
    std::string send_insufficient;
    std::string keys_title;
    std::string keys_unlock_prompt;
    std::string keys_descriptor_label;
    std::string keys_warning;
    std::string press_continue;
    std::string invalid_input;
    std::string back_to_menu;
    std::string yes_option;
    std::string password_too_short;
    std::string wallet_name_label;
    std::string import_warning;
    std::string format_label;
    std::string descriptor_type;
    std::string legacy_type;
    std::string available_wallets;
    std::string available_label;
    std::string error_prefix;
    std::string exit_message;
    std::string node_info_title;
    std::string blockchain_title;
    std::string network_title;
    std::string block_title;
    std::string block_height_label;
    std::string block_hash_label;
    std::string block_time_label;
    std::string block_tx_count_label;
    std::string transactions_title;
    std::string no_transactions;
    std::string tx_id_label;
    std::string tx_amount_label;
    std::string tx_confirmations_label;
    std::string tx_direction_label;
    std::string tx_address_label;
    std::string tx_time_label;
    std::string tx_send_label;
    std::string tx_receive_label;
    std::string tx_from_label;   // Sender address (for receive)
    std::string tx_to_label;     // Recipient address (for send)
    std::string mining_title;
    std::string mining_blocks_label;
    std::string mining_difficulty_label;
    std::string mining_hashrate_label;
    std::string generate_addr_title;
    std::string generate_addr_success;
    std::string prompt_block_hash;
    std::string prompt_block_height;
    std::string prompt_tx_count;
    std::string sign_title;
    std::string sign_message_prompt;
    std::string sign_result_label;
    std::string auto_login_msg;
    std::string wallet_ready_title;
    std::string wallet_ready_msg;
    // CreateNewWallet backup prompt strings
    std::string backup_important;
    std::string backup_goto_keys;
    std::string backup_descriptor_info;
    std::string backup_confirm_prompt;
    std::string backup_reminder;
    std::string wallet_name_label2;  // "Wallet name: " shown after creation
    std::string wallet_create_failed;
    std::string wallet_create_error;
    // RestoreWallet strings
    std::string prompt_backup_path;
    std::string err_file_empty;
    std::string err_file_not_found;
    std::string err_file_invalid;
    std::string prompt_new_wallet_name;
    std::string err_wallet_name_empty;
    std::string prompt_ext_descriptor;
    std::string prompt_int_descriptor;
    std::string err_no_descriptors;
    // RestoreWallet: mnemonic restore option
    std::string restore_from_mnemonic;
    std::string prompt_enter_mnemonic;
    std::string prompt_mnemonic_word;
    std::string prompt_mnemonic_passphrase;
    std::string restore_mnemonic_success;
    std::string restore_mnemonic_invalid;
    std::string restore_mnemonic_xprv_failed;
    // Additional i18n strings (previously hardcoded)
    std::string msg_no_wallet_create_first;
    std::string prompt_unlock_or_skip;
    std::string msg_using_wallet;
    std::string err_addr_empty;
    std::string err_amount_nan;
    std::string err_amount_positive;
    std::string msg_no_bech32_descriptor;
    // Real BIP39 mnemonic strings
    std::string mnemonic_title;
    std::string mnemonic_warning;
    std::string mnemonic_word_label;
    std::string mnemonic_verify_title;
    std::string mnemonic_verify_prompt;
    std::string mnemonic_verify_success;
    std::string mnemonic_verify_failed;
    std::string mnemonic_backup_confirm;
    std::string mnemonic_backup_prompt;
    std::string mnemonic_backup_confirmed;
    // P0 Security strings (previously hardcoded English)
    std::string keys_warning_full_key;
    std::string keys_full_key_header;
    std::string keys_hidden_msg;
    std::string sign_own_address_only;
    // Change password
    std::string menu_change_password;
    std::string change_pass_title;
    std::string change_pass_old;
    std::string change_pass_new;
    std::string change_pass_confirm;
    std::string change_pass_success;
    std::string change_pass_failed;
    std::string change_pass_mismatch;
    std::string change_pass_not_encrypted;
    // Address label management
    std::string menu_addr_label;
    std::string addr_label_title;
    std::string addr_label_prompt;
    std::string addr_label_name_prompt;
    std::string addr_label_success;
    std::string addr_label_failed;
    std::string addr_label_removed;
    std::string addr_label_not_found;
    // Sign message i18n (previously hardcoded English)
    std::string sign_addr_prompt;
    std::string sign_wallet_locked;
    std::string sign_must_unlock;
    std::string sign_addr_empty;
    std::string sign_addr_too_long;
    std::string sign_msg_too_long;
    std::string sign_addr_not_owned;
    std::string sign_verify_failed;
    // Security/warning strings (previously hardcoded English)
    std::string warn_no_passphrase;
    std::string warn_anyone_access;
    std::string confirm_unencrypted;
    std::string cleaning_incomplete_wallet;
    std::string wallet_functional_msg;
    std::string restore_access_denied;
    std::string wallet_encrypted_must_unlock;
    std::string import_descriptors_later;
    std::string bip39_checksum_mismatch;
    std::string mnemonic_typo_check;
    std::string too_many_failed_attempts;
    // Info display labels (previously hardcoded English)
    std::string label_chain;
    std::string label_headers;
    std::string label_version;
    std::string label_subversion;
    std::string label_connections;
    std::string label_peers;
    std::string label_progress;
    std::string label_size_on_disk;
    std::string label_protocol;
    std::string label_connections_io;
    std::string label_network_active;
    std::string label_connected_peers;
    std::string label_previous;
    std::string label_next;
    std::string label_merkle_root;
    std::string label_nonce;
    std::string label_tx_list;
    std::string label_network_hs;
    std::string label_pooled_tx;
    std::string label_current_height;
    // Connection/startup messages (previously hardcoded)
    std::string msg_connecting;
    std::string err_cannot_connect;
    std::string msg_ensure_running;
    std::string msg_press_exit;
    std::string msg_ok;
    // CLI error messages (previously hardcoded)
    std::string err_cli_error;
    std::string err_code_label;
    std::string err_details_label;
    std::string err_unknown;
    // Menu options/prompts (previously hardcoded)
    std::string block_explore_options;
    std::string block_explore_select;
    std::string select_wallet_fmt;
    // Lock/unlock prompts (previously hardcoded)
    std::string prompt_locked_unlock;
    // Display labels (previously hardcoded)
    std::string label_receiving;
    std::string label_change;
    std::string label_public;
    std::string label_private;
    std::string label_primary;
    // Yes/No for display
    std::string label_yes_short;
    std::string label_no_short;
    // Inference Service i18n
    std::string menu_inference_service;
    std::string inference_title;
    std::string inference_menu_enter_key;
    std::string inference_menu_test;
    std::string inference_back;
    std::string inference_enter_key_prompt;
    std::string inference_key_info_title;
    std::string inference_key_label;
    std::string inference_miner_label;
    std::string inference_model_label;
    std::string inference_endpoint_label;
    std::string inference_balance_label;
    std::string inference_rate_label;
    std::string inference_used_label;
    std::string inference_remaining_label;
    std::string inference_prompt_question;
    std::string inference_result_label;
    std::string inference_tokens_label;
    std::string inference_cost_label;
    std::string inference_refund_success;
    std::string inference_refund_failed;
    std::string inference_invalid_key;
    std::string inference_key_not_found;
    std::string inference_test_success;
    std::string inference_test_failed;
    std::string inference_refund_confirm;
};

static std::map<int, I18nTexts> I18N;

static void InitI18N() {
    I18nTexts en;
    en.select_language = "\n========================================\n  TokenCoin Wallet Manager v1.0\n  Select Language\n========================================\n";
    en.not_logged_in_title = "\n========================================\n  TokenCoin Wallet Manager [Public]\n========================================\n";
    en.public_menu_title = "\n========================================\n  TokenCoin Wallet Manager [Public]\n========================================\n";
    en.logged_in_title = "\n========================================\n  TokenCoin Wallet Manager\n========================================\n";
    en.wallet_menu_title = "\n========================================\n  TokenCoin Wallet Manager [Wallet: %s]\n========================================\n";
    en.menu_new_wallet = "1. Create New Wallet";
    en.menu_restore_wallet = "2. Restore Wallet";
    en.menu_open_wallet = "3. Open Wallet";
    en.menu_node_info = "4. Node Information";
    en.menu_blockchain_info = "5. Blockchain Info";
    en.menu_network_info = "6. Network Info";
    en.menu_block_info = "7. Block Explorer";
    en.menu_mining_info = "8. Mining Status";
    en.menu_exit = "0. Back to Language";
    en.menu_back = "\nESC. Back";
    en.menu_quit = "e. Exit";
    en.prompt_select = "> ";
    en.menu_wallet_address = "1. Wallet Address";
    en.menu_send_tknc = "2. Send token";
    en.menu_transactions = "3. Transaction History";
    en.menu_backup_keys = "4. Backup Keys";
    en.menu_generate_address = "5. Generate New Address";
    en.menu_addr_label = "6. Address Label";
    en.menu_change_password = "7. Change Password";
    en.menu_sign_message = "8. Sign Message";
    en.menu_logout = "10. Logout";
    en.create_wallet_title = "\n--- Create New Wallet ---\n";
    en.prompt_wallet_name = "Enter wallet name: ";
    en.prompt_password = "Set password: ";
    en.prompt_password_confirm = "Confirm password: ";
    en.password_mismatch = "ERROR: Passwords do not match!";
    en.wallet_creating = "\nCreating wallet...";
    en.wallet_created_success = "\n✅ Wallet created successfully!\n";
    en.wallet_address_label = "Address: ";
    en.wallet_balance_label = "Balance: ";
    en.wallet_total_balance_label = "Total Balance: ";
    en.addr_balance_label = "Address Balance: ";
    en.select_address_prompt = "Enter number to switch address: ";
    en.addr_switched_msg = "✅ Switched to: ";
    en.send_from_label = "From: ";
    en.no_utxo_msg = "No available UTXOs for this address.\n";
    en.restore_title = "\n--- Restore Wallet ---\n";
    en.restore_from_backup = "From backup file (.dat)";
    en.restore_from_descriptors = "From keys";
    en.restore_prompt_file = "Select [1-2]: ";
    en.restore_success = "✅ Restored successfully!\n";
    en.restore_failed = "❌ Failed: ";
    en.info_title = "\n--- Wallet Info ---\n";
    en.info_wallet_name = "Wallet: ";
    en.info_address = "Address: ";
    en.info_balance = "Balance: ";
    en.info_no_wallet = "No wallet loaded.\n";
    en.send_title = "\n--- Send token ---\n";
    en.send_to_address = "Recipient address: ";
    en.send_amount = "Amount (token): ";
    en.send_confirm = "Confirm? (y/n): ";
    en.send_success = "✅ Sent! TXID: ";
    en.send_failed = "❌ Failed: ";
    en.send_insufficient = "❌ Insufficient balance!\n";
    en.keys_title = "\n--- Backup Keys ---\n";
    en.keys_unlock_prompt = "Enter password (60s): ";
    en.keys_descriptor_label = "Keys:\n";
    en.keys_warning = "⚠️ Keep secure! Never share!\n";
    en.press_continue = "\nPress Enter to continue...";
    en.invalid_input = "Invalid input.";
    en.back_to_menu = "\nReturning to menu...";
    en.yes_option = "y";
    en.password_too_short = "ERROR: Password must be at least 8 characters!";
    en.wallet_name_label = "Wallet Name: ";
    en.import_warning = "Import warning: ";
    en.format_label = "Format: ";
    en.descriptor_type = "Key";
    en.legacy_type = "Legacy";
    en.available_wallets = "Available wallets:\n";
    en.available_label = "Available: ";
    en.error_prefix = "Error: ";
    en.exit_message = "Thank you for using token!\n";
    en.node_info_title = "\n--- Node Information ---\n";
    en.blockchain_title = "\n--- Blockchain Information ---\n";
    en.network_title = "\n--- Network Information ---\n";
    en.block_title = "\n--- Block Explorer ---\n";
    en.block_height_label = "Block Height: ";
    en.block_hash_label = "Block Hash: ";
    en.block_time_label = "Time: ";
    en.block_tx_count_label = "Transactions: ";
    en.transactions_title = "\n--- Transaction History ---\n";
    en.no_transactions = "No transactions found.\n";
    en.tx_id_label = "TXID: ";
    en.tx_amount_label = "Amount: ";
    en.tx_confirmations_label = "Confirmations: ";
    en.tx_address_label = "Address";
    en.tx_send_label = "Send";
    en.tx_receive_label = "Receive";
    en.tx_from_label = "From";
    en.tx_to_label = "To";
    en.mining_title = "\n--- Mining Status ---\n";
    en.mining_blocks_label = "Blocks: ";
    en.mining_difficulty_label = "Difficulty: ";
    en.mining_hashrate_label = "Hashrate: ";
    en.generate_addr_title = "\n--- Generate New Address ---\n";
    en.generate_addr_success = "New address generated: ";
    en.prompt_block_hash = "Enter block hash (or 'latest'): ";
    en.prompt_block_height = "Enter block height (or 'latest'): ";
    en.prompt_tx_count = "Number of transactions [10]: ";
    en.sign_title = "\n--- Sign Message ---\n";
    en.sign_message_prompt = "Message to sign: ";
    en.sign_result_label = "Signature: ";
    en.auto_login_msg = "Auto-logging into wallet...";
    en.wallet_ready_title = "\n========================================\n  Wallet Ready!\n========================================\n";
    en.wallet_ready_msg = "Wallet: %s\nAddress: %s\nBalance: %s token\n";
    // CreateNewWallet backup prompt strings
    en.backup_important = "[IMPORTANT] Backup your wallet NOW!\n";
    en.backup_goto_keys = "Go to: Wallet Menu -> 5. Backup Keys\n";
    en.backup_descriptor_info = "This saves your descriptor (xprv) which CAN restore your wallet.\n";
    en.backup_confirm_prompt = "Type 'yes' to confirm you will backup: ";
    en.backup_reminder = "\nPlease backup your wallet from the main menu.\n";
    en.wallet_name_label2 = "Wallet name: ";
    en.wallet_create_failed = "Wallet creation failed. Please check node status and try again.\n";
    en.wallet_create_error = "An unexpected error occurred. Please try again.\n";
    // RestoreWallet strings
    en.prompt_backup_path = "Backup file path: ";
    en.err_file_empty = "File path cannot be empty.\n";
    en.err_file_not_found = "File not found: ";
    en.err_file_invalid = "Not a valid file: ";
    en.prompt_new_wallet_name = "New wallet name: ";
    en.err_wallet_name_empty = "Wallet name cannot be empty.\n";
    en.prompt_ext_descriptor = "\nReceiving key: ";
    en.prompt_int_descriptor = "Change key: ";
    en.err_no_descriptors = ": No descriptors\n";
    // RestoreWallet: mnemonic restore option
    en.restore_from_mnemonic = "From mnemonic phrase";
    en.prompt_enter_mnemonic = "\nEnter your 12 recovery words (one per line, empty line when done):\n";
    en.prompt_mnemonic_word = "  Word %d: ";
    en.prompt_mnemonic_passphrase = "Passphrase (optional, press Enter to skip): ";
    en.restore_mnemonic_success = "\n✅ Wallet restored from recovery phrase!\n";
    en.restore_mnemonic_invalid = "Invalid word '%s' at position %d. BIP39 English words only.\n";
    en.restore_mnemonic_xprv_failed = "Key derivation failed. Check your recovery phrase and passphrase.\n";
    en.msg_no_wallet_create_first = "Please create or restore a wallet first.\n";
    en.prompt_unlock_or_skip = "Enter password (or press Enter to skip): ";
    en.msg_using_wallet = "\nUsing wallet: ";
    en.err_addr_empty = "Address cannot be empty.\n";
    en.err_amount_nan = "Invalid amount: not a number.\n";
    en.err_amount_positive = "Amount must be greater than 0.\n";
    en.msg_no_bech32_descriptor = "No Bech32 (token) keys found in wallet.\n";
    // Real BIP39 mnemonic strings
    en.mnemonic_title = "\n=== Write Down Your Recovery Phrase ===\n";
    en.mnemonic_warning = "Keep this information secure! Anyone with these words can access your wallet.\n";
    en.mnemonic_word_label = "  %2d. %s\n";
    en.mnemonic_verify_title = "\n=== Verify Your Recovery Phrase ===\n";
    en.mnemonic_verify_prompt = "Enter word #%d: ";
    en.mnemonic_verify_success = "Verification passed! Wallet is ready.\n";
    en.mnemonic_verify_failed = "Verification failed! Please check your backup carefully.\n";
    en.mnemonic_backup_confirm = "\nIMPORTANT: Have you written down all 12 words?\n";
    en.mnemonic_backup_prompt = "Type 'yes' to confirm: ";
    en.mnemonic_backup_confirmed = "Backup confirmed! Your wallet is now active.\n";
    // P0 Security strings
    en.keys_warning_full_key = "WARNING: Displaying full private key allows anyone seeing your screen to steal ALL funds.\n";
    en.keys_full_key_header = "\n=== FULL PRIVATE KEYS (keep secret!) ===\n";
    en.keys_hidden_msg = "(Keys hidden — use this terminal only in a secure location)\n";
    en.sign_own_address_only = "You can only sign messages with your own addresses.\n";
    // Change password
    en.change_pass_title = "\n--- Change Password ---\n";
    en.change_pass_old = "Current password: ";
    en.change_pass_new = "New password: ";
    en.change_pass_confirm = "Confirm new password: ";
    en.change_pass_success = "Password changed successfully!\n";
    en.change_pass_failed = "Failed to change password: ";
    en.change_pass_mismatch = "ERROR: New passwords do not match!\n";
    en.change_pass_not_encrypted = "This wallet is not encrypted. Cannot change password.\n";
    // Delete address
    en.addr_label_title = "\n--- Address Label ---\n";
    en.addr_label_prompt = "Enter address: ";
    en.addr_label_name_prompt = "Label name (empty to remove): ";
    en.addr_label_success = "Label set successfully.\n";
    en.addr_label_failed = "Failed to set label: ";
    en.addr_label_removed = "Label removed.\n";
    en.addr_label_not_found = "Address not found in wallet.\n";
    // Sign message i18n
    en.sign_addr_prompt = "Address: ";
    en.sign_wallet_locked = "Wallet is locked. Enter passphrase to unlock: ";
    en.sign_must_unlock = "Wallet must be unlocked to sign messages.\n";
    en.sign_addr_empty = "Address cannot be empty.\n";
    en.sign_addr_too_long = "Address too long (max 90 characters).\n";
    en.sign_msg_too_long = "Message too long (max 4096 characters).\n";
    en.sign_addr_not_owned = "Address does not belong to current wallet.\n";
    en.sign_verify_failed = "Failed to verify address ownership.\n";
    // Security/warning strings
    en.warn_no_passphrase = "WARNING: No passphrase entered. Your wallet will be created WITHOUT encryption.\n";
    en.warn_anyone_access = "Anyone with access to this computer can spend your funds.\n";
    en.confirm_unencrypted = "Type 'yes' to confirm creating an unencrypted wallet, or press Enter to set a passphrase: ";
    en.cleaning_incomplete_wallet = "Cleaning up incomplete wallet from node...\n";
    en.wallet_functional_msg = "The wallet is functional. Try 'Generate New Address' from the wallet menu.\n";
    en.restore_access_denied = "Wallet restored but access denied — incorrect passphrase.\n";
    en.wallet_encrypted_must_unlock = "This wallet is encrypted. You must enter the passphrase to access it.\n";
    en.import_descriptors_later = "You can import missing descriptors later from Backup Keys menu.\n";
    en.bip39_checksum_mismatch = "Invalid recovery phrase: BIP39 checksum mismatch.\n";
    en.mnemonic_typo_check = "Please check your words for typos. Each word must be exactly correct.\n";
    en.too_many_failed_attempts = "Too many failed attempts. Please wait ";
    // Info display labels
    en.label_chain = "Chain: ";
    en.label_headers = "Headers: ";
    en.label_version = "Version: ";
    en.label_subversion = "Subversion: ";
    en.label_connections = "Connections: ";
    en.label_peers = "Peers: ";
    en.label_progress = "Progress: ";
    en.label_size_on_disk = "Size on disk: ";
    en.label_protocol = "Protocol: ";
    en.label_connections_io = "Connections (in/out): ";
    en.label_network_active = "Network Active: ";
    en.label_connected_peers = "--- Connected Peers (%d) ---\n";
    en.label_previous = "Previous: ";
    en.label_next = "Next: ";
    en.label_merkle_root = "Merkle Root: ";
    en.label_nonce = "Nonce: ";
    en.label_tx_list = "Transactions:\n";
    en.label_network_hs = "Network H/s: ";
    en.label_pooled_tx = "Pooled TX: ";
    en.label_current_height = "Current Height: ";
    // Connection/startup messages
    en.msg_connecting = "Connecting to TokenCoin node...";
    en.err_cannot_connect = "  Cannot connect to TokenCoin node\n";
    en.msg_ensure_running = "Please ensure tkncd.exe is running.\n\n";
    en.msg_press_exit = "Press Enter to exit...\n";
    en.msg_ok = " OK!\n\n";
    // CLI error messages
    en.err_cli_error = "  token CLI Error\n";
    en.err_code_label = "Error code: ";
    en.err_details_label = "Details: ";
    en.err_unknown = "Unknown error occurred.\n";
    // Menu options/prompts
    en.block_explore_options = "1. By Hash\n2. By Height\n3. Latest Block\n\n";
    en.block_explore_select = "Select [1-3]: ";
    en.select_wallet_fmt = "Select wallet [1-%d]: ";
    // Lock/unlock prompts
    en.prompt_locked_unlock = "Wallet is locked. Enter passphrase to unlock: ";
    // Display labels
    en.label_receiving = "[Receiving] ";
    en.label_change = "[Change]    ";
    en.label_public = "[Public]  ";
    en.label_private = "[Private] ";
    en.label_primary = "(Primary)";
    // Yes/No short labels
    en.label_yes_short = "Yes";
    en.label_no_short = "No";
    // LLM Service i18n
    en.menu_inference_service = "9. LLM";
    en.inference_title = "\n========================================\n  LLM Service\n========================================\n";
    en.inference_menu_enter_key = "1. Enter API Key";
    en.inference_menu_test = "2. Test LLM";
    en.inference_back = "0. Back";
    en.inference_enter_key_prompt = "Paste your API Key: ";
    en.inference_key_info_title = "\n--- API Key Info ---\n";
    en.inference_key_label = "Key:      ";
    en.inference_miner_label = "Miner:    ";
    en.inference_model_label = "Model:    ";
    en.inference_endpoint_label = "Endpoint: ";
    en.inference_balance_label = "Balance:  ";
    en.inference_rate_label = "Rate:     ";
    en.inference_used_label = "Used:     ";
    en.inference_remaining_label = "Remaining:";
    en.inference_prompt_question = "Enter your question: ";
    en.inference_result_label = "Response: ";
    en.inference_tokens_label = "Tokens:   ";
    en.inference_cost_label = "Cost:     ";
    en.inference_refund_success = "Refund successful! TXID: ";
    en.inference_refund_failed = "Refund failed: ";
    en.inference_invalid_key = "Invalid API key format.\n";
    en.inference_key_not_found = "API key not found or expired.\n";
    en.inference_test_success = "\n--- LLM Result ---\n";
    en.inference_test_failed = "LLM failed: ";
    en.inference_refund_confirm = "Refund remaining balance for this key? (y/n): ";
    I18N[1] = en;

    I18nTexts zh;
    zh.select_language = "\n========================================\n  TokenCoin 钱包管理器 v1.0\n  选择语言\n========================================\n";
    zh.not_logged_in_title = "\n========================================\n  TokenCoin 钱包管理器 [公共]\n========================================\n";
    zh.public_menu_title = "\n========================================\n  TokenCoin 钱包管理器 [公共]\n========================================\n";
    zh.logged_in_title = "\n========================================\n  TokenCoin 钱包管理器\n========================================\n";
    zh.wallet_menu_title = "\n========================================\n  TokenCoin 钱包管理器 [钱包: %s]\n========================================\n";
    zh.menu_new_wallet = "1. 创建新钱包";
    zh.menu_restore_wallet = "2. 恢复钱包";
    zh.menu_open_wallet = "3. 打开钱包";
    zh.menu_node_info = "4. 节点信息";
    zh.menu_blockchain_info = "5. 区块链信息";
    zh.menu_network_info = "6. 网络信息";
    zh.menu_block_info = "7. 区块浏览器";
    zh.menu_mining_info = "8. 挖矿状态";
    zh.menu_exit = "0. 返回语言选择";
    zh.menu_back = "\nESC. 返回";
    zh.menu_quit = "e. 退出";
    zh.prompt_select = "> ";
    zh.menu_wallet_address = "1. 钱包地址";
    zh.menu_send_tknc = "2. 转账token";
    zh.menu_transactions = "3. 交易记录";
    zh.menu_backup_keys = "4. 备份密钥";
    zh.menu_generate_address = "5. 生成新地址";
    zh.menu_addr_label = "6. 地址标签";
    zh.menu_change_password = "7. 更改密码";
    zh.menu_sign_message = "8. 签名功能";
    zh.menu_logout = "10. 退出登录";
    zh.create_wallet_title = "\n--- 创建新钱包 ---\n";
    zh.prompt_wallet_name = "输入钱包名称: ";
    zh.prompt_password = "设置密码: ";
    zh.prompt_password_confirm = "确认密码: ";
    zh.password_mismatch = "错误: 密码不一致!";
    zh.wallet_creating = "\n正在创建...";
    zh.wallet_created_success = "\n✅ 钱包创建成功!\n";
    zh.wallet_address_label = "地址: ";
    zh.wallet_balance_label = "余额: ";
    zh.wallet_total_balance_label = "钱包总余额: ";
    zh.addr_balance_label = "地址余额: ";
    zh.select_address_prompt = "输入编号切换地址: ";
    zh.addr_switched_msg = "✅ 已切换到: ";
    zh.send_from_label = "发送地址: ";
    zh.no_utxo_msg = "该地址没有可用余额。\n";
    zh.restore_title = "\n--- 恢复钱包 ---\n";
    zh.restore_from_backup = "从备份文件恢复 (.dat)";
    zh.restore_from_descriptors = "从密钥恢复";
    zh.restore_prompt_file = "选择 [1-2]: ";
    zh.restore_success = "✅ 恢复成功!\n";
    zh.restore_failed = "❌ 失败: ";
    zh.info_title = "\n--- 钱包信息 ---\n";
    zh.info_wallet_name = "钱包: ";
    zh.info_address = "地址: ";
    zh.info_balance = "余额: ";
    zh.info_no_wallet = "未加载钱包。\n";
    zh.send_title = "\n--- 转账token ---\n";
    zh.send_to_address = "收款地址: ";
    zh.send_amount = "金额 (token): ";
    zh.send_confirm = "确认? (y/n): ";
    zh.send_success = "✅ 已发送! TXID: ";
    zh.send_failed = "❌ 失败: ";
    zh.send_insufficient = "❌ 余额不足!\n";
    zh.keys_title = "\n--- 备份密钥 ---\n";
    zh.keys_unlock_prompt = "输入密码 (60秒): ";
    zh.keys_descriptor_label = "密钥:\n";
    zh.keys_warning = "⚠️ 妥善保管! 切勿泄露!\n";
    zh.press_continue = "\n按回车继续...";
    zh.invalid_input = "无效输入。";
    zh.back_to_menu = "\n返回菜单...";
    zh.yes_option = "y";
    zh.password_too_short = "错误: 密码至少需要8个字符!";
    zh.wallet_name_label = "钱包名称: ";
    zh.import_warning = "导入警告: ";
    zh.format_label = "格式: ";
    zh.descriptor_type = "密钥";
    zh.legacy_type = "传统";
    zh.available_wallets = "可用钱包:\n";
    zh.available_label = "可用: ";
    zh.error_prefix = "错误: ";
    zh.exit_message = "感谢使用 token!\n";
    zh.node_info_title = "\n--- 节点信息 ---\n";
    zh.blockchain_title = "\n--- 区块链信息 ---\n";
    zh.network_title = "\n--- 网络信息 ---\n";
    zh.block_title = "\n--- 区块浏览器 ---\n";
    zh.block_height_label = "区块高度: ";
    zh.block_hash_label = "区块哈希: ";
    zh.block_time_label = "时间: ";
    zh.block_tx_count_label = "交易数: ";
    zh.transactions_title = "\n--- 交易记录 ---\n";
    zh.no_transactions = "暂无交易记录。\n";
    zh.tx_id_label = "交易ID: ";
    zh.tx_amount_label = "金额: ";
    zh.tx_confirmations_label = "确认数: ";
    zh.tx_direction_label = "类型";
    zh.tx_address_label = "地址";
    zh.tx_from_label = "付款地址";
    zh.tx_to_label = "收款地址";
    zh.tx_time_label = "时间";
    zh.tx_send_label = "转出";
    zh.tx_receive_label = "转入";
    zh.mining_title = "\n--- 挖矿状态 ---\n";
    zh.mining_blocks_label = "区块数: ";
    zh.mining_difficulty_label = "难度: ";
    zh.mining_hashrate_label = "算力: ";
    zh.generate_addr_title = "\n--- 生成新地址 ---\n";
    zh.generate_addr_success = "新地址已生成: ";
    zh.prompt_block_hash = "输入区块哈希 (或'latest'): ";
    zh.prompt_block_height = "输入区块高度 (或'latest'): ";
    zh.prompt_tx_count = "显示交易数量 [10]: ";
    zh.sign_title = "\n--- 签名功能 ---\n";
    zh.sign_message_prompt = "待签名消息: ";
    zh.sign_result_label = "签名: ";
    zh.auto_login_msg = "正在自动登录钱包...";
    zh.wallet_ready_title = "\n========================================\n  钱包已就绪!\n========================================\n";
    zh.wallet_ready_msg = "钱包: %s\n地址: %s\n余额: %s token\n";
    // CreateNewWallet backup prompt strings
    zh.backup_important = "\n[重要] 请立即备份您的钱包!\n";
    zh.backup_goto_keys = "前往: 钱包菜单 -> 5. 备份密钥\n";
    zh.backup_descriptor_info = "这将保存您的密钥(xprv)，可用于恢复钱包。\n";
    zh.backup_confirm_prompt = "输入 'yes' 确认您将备份: ";
    zh.backup_reminder = "\n请从主菜单备份您的钱包。\n";
    zh.wallet_name_label2 = "钱包名称: ";
    zh.wallet_create_failed = "钱包创建失败。请检查节点状态后重试。\n";
    zh.wallet_create_error = "发生意外错误。请重试。\n";
    // RestoreWallet strings
    zh.prompt_backup_path = "备份文件路径: ";
    zh.err_file_empty = "文件路径不能为空。\n";
    zh.err_file_not_found = "文件未找到: ";
    zh.err_file_invalid = "无效的文件: ";
    zh.prompt_new_wallet_name = "新钱包名称: ";
    zh.err_wallet_name_empty = "钱包名称不能为空。\n";
    zh.prompt_ext_descriptor = "\n公钥: ";
    zh.prompt_int_descriptor = "密钥: ";
    zh.err_no_descriptors = ": 没有密钥\n";
    // RestoreWallet: mnemonic restore option
    zh.restore_from_mnemonic = "从助记词恢复";
    zh.prompt_enter_mnemonic = "\n输入您的12个恢复词(每行一个，输入空行结束):\n";
    zh.prompt_mnemonic_word = "  第%d个词: ";
    zh.prompt_mnemonic_passphrase = "密码短语(可选，直接回车跳过): ";
    zh.restore_mnemonic_success = "\n✅ 钱包已从恢复短语恢复!\n";
    zh.restore_mnemonic_invalid = "无效的词'%s'在第%d位。仅支持BIP39英文单词。\n";
    zh.restore_mnemonic_xprv_failed = "密钥推导失败。请检查您的恢复短语和密码短语。\n";
    zh.msg_no_wallet_create_first = "请先创建或恢复钱包。\n";
    zh.prompt_unlock_or_skip = "输入密码(或直接回车跳过): ";
    zh.msg_using_wallet = "\n正在使用钱包: ";
    zh.err_addr_empty = "地址不能为空。\n";
    zh.err_amount_nan = "无效的金额: 不是数字。\n";
    zh.err_amount_positive = "金额必须大于0。\n";
    zh.msg_no_bech32_descriptor = "钱包中未找到Bech32(token)密钥。\n";
    // Real BIP39 mnemonic strings
    zh.mnemonic_title = "\n=== 请抄写您的恢复短语 ===\n";
    zh.mnemonic_warning = "请妥善保管! 任何拥有这些词的人都可以访问您的钱包。\n";
    zh.mnemonic_word_label = "  %2d. %s\n";
    zh.mnemonic_verify_title = "\n=== 验证您的恢复短语 ===\n";
    zh.mnemonic_verify_prompt = "输入第%d个词: ";
    zh.mnemonic_verify_success = "验证通过! 钱包已就绪。\n";
    zh.mnemonic_verify_failed = "验证失败! 请仔细检查您的备份。\n";
    zh.mnemonic_backup_confirm = "\n重要: 您是否已抄写全部12个词?\n";
    zh.mnemonic_backup_prompt = "输入 'yes' 确认: ";
    zh.mnemonic_backup_confirmed = "备份确认完成! 您的钱包现已激活。\n";
    // P0 security strings
    zh.keys_warning_full_key = "警告: 显示完整私钥将允许任何看到屏幕的人盗取所有资金。\n";
    zh.keys_full_key_header = "\n=== 完整私钥(请保密!) ===\n";
    zh.keys_hidden_msg = "(密钥已隐藏 — 请仅在安全环境下使用此终端)\n";
    zh.sign_own_address_only = "您只能使用自己的地址签名消息。\n";
    // Change password
    zh.change_pass_title = "\n--- 更改密码 ---\n";
    zh.change_pass_old = "当前密码: ";
    zh.change_pass_new = "新密码: ";
    zh.change_pass_confirm = "确认新密码: ";
    zh.change_pass_success = "密码修改成功!\n";
    zh.change_pass_failed = "密码修改失败: ";
    zh.change_pass_mismatch = "错误: 新密码不一致!\n";
    zh.change_pass_not_encrypted = "此钱包未加密，无法更改密码。\n";
    // Delete address
    zh.addr_label_title = "\n--- 地址标签 ---\n";
    zh.addr_label_prompt = "输入地址: ";
    zh.addr_label_name_prompt = "标签名称(留空删除标签): ";
    zh.addr_label_success = "标签设置成功。\n";
    zh.addr_label_failed = "标签设置失败: ";
    zh.addr_label_removed = "标签已删除。\n";
    zh.addr_label_not_found = "钱包中未找到该地址。\n";
    // Sign message i18n
    zh.sign_addr_prompt = "地址: ";
    zh.sign_wallet_locked = "钱包已锁定，输入密码解锁: ";
    zh.sign_must_unlock = "钱包必须解锁才能签名消息。\n";
    zh.sign_addr_empty = "地址不能为空。\n";
    zh.sign_addr_too_long = "地址过长（最多90个字符）。\n";
    zh.sign_msg_too_long = "消息过长（最多4096个字符）。\n";
    zh.sign_addr_not_owned = "该地址不属于当前钱包。\n";
    zh.sign_verify_failed = "验证地址所有权失败。\n";
    // Security/warning strings
    zh.warn_no_passphrase = "警告：未输入密码。钱包将以未加密方式创建。\n";
    zh.warn_anyone_access = "任何能访问此计算机的人都可以花费您的资金。\n";
    zh.confirm_unencrypted = "输入 'yes' 确认创建未加密钱包，或按回车设置密码：";
    zh.cleaning_incomplete_wallet = "正在清理节点上的不完整钱包...\n";
    zh.wallet_functional_msg = "\u94B1\u5305\u5DF2\u5C31\u7EEA\u3002\u8BF7\u4ECE\u94B1\u5305\u83DC\u5355\u9009\u62E9\u3010\u751F\u6210\u65B0\u5730\u5740\u3011\u3002\n";
    zh.restore_access_denied = "钱包已恢复但访问被拒绝 — 密码错误。\n";
    zh.wallet_encrypted_must_unlock = "此钱包已加密。必须输入密码才能访问。\n";
    zh.import_descriptors_later = "您可以稍后通过备份密钥菜单导入缺失的密钥。\n";
    zh.bip39_checksum_mismatch = "无效的恢复短语：BIP39校验和不匹配。\n";
    zh.mnemonic_typo_check = "请检查您的助记词是否有拼写错误。每个词必须完全正确。\n";
    zh.too_many_failed_attempts = "失败次数过多。请等待 ";
    // Info display labels
    zh.label_chain = "链: ";
    zh.label_headers = "区块头: ";
    zh.label_version = "版本: ";
    zh.label_subversion = "子版本: ";
    zh.label_connections = "连接数: ";
    zh.label_peers = "节点数: ";
    zh.label_progress = "进度: ";
    zh.label_size_on_disk = "磁盘大小: ";
    zh.label_protocol = "协议: ";
    zh.label_connections_io = "连接(入/出): ";
    zh.label_network_active = "网络激活: ";
    zh.label_connected_peers = "--- 已连接节点 (%d) ---\n";
    zh.label_previous = "前一区块: ";
    zh.label_next = "后一区块: ";
    zh.label_merkle_root = "默克尔根: ";
    zh.label_nonce = "随机数: ";
    zh.label_tx_list = "交易列表:\n";
    zh.label_network_hs = "网络算力 H/s: ";
    zh.label_pooled_tx = "内存池交易: ";
    zh.label_current_height = "当前高度: ";
    // Connection/startup messages
    zh.msg_connecting = "正在连接 TokenCoin 节点...";
    zh.err_cannot_connect = "  无法连接到 TokenCoin 节点\n";
    zh.msg_ensure_running = "请确保 tkncd.exe 正在运行。\n\n";
    zh.msg_press_exit = "按回车退出...\n";
    zh.msg_ok = " 连接成功!\n\n";
    // CLI error messages
    zh.err_cli_error = "  token CLI 错误\n";
    zh.err_code_label = "错误代码: ";
    zh.err_details_label = "详情: ";
    zh.err_unknown = "发生未知错误。\n";
    // Menu options/prompts
    zh.block_explore_options = "1. 按哈希查询\n2. 按高度查询\n3. 最新区块\n\n";
    zh.block_explore_select = "选择 [1-3]: ";
    zh.select_wallet_fmt = "选择钱包 [1-%d]: ";
    // Lock/unlock prompts
    zh.prompt_locked_unlock = "钱包已锁定。输入密码解锁: ";
    // Display labels
    zh.label_receiving = "[接收] ";
    zh.label_change = "[找零]     ";
    zh.label_public = "[公钥] ";
    zh.label_private = "[密钥] ";
    zh.label_primary = "(主地址)";
    // Yes/No short labels
    zh.label_yes_short = "是";
    zh.label_no_short = "否";
    // LLM Service i18n
    zh.menu_inference_service = "9. LLM";
    zh.inference_title = "\n========================================\n  LLM Service / LLM 服务\n========================================\n";
    zh.inference_menu_enter_key = "1. 输入 API Key";
    zh.inference_menu_test = "2. 测试 LLM";
    zh.inference_back = "0. 返回";
    zh.inference_enter_key_prompt = "粘贴 API Key: ";
    zh.inference_key_info_title = "\n--- API Key 信息 ---\n";
    zh.inference_key_label = "Key:      ";
    zh.inference_miner_label = "Miner:    ";
    zh.inference_model_label = "Model:    ";
    zh.inference_endpoint_label = "Endpoint: ";
    zh.inference_balance_label = "余额:     ";
    zh.inference_rate_label = "兑换比例: ";
    zh.inference_used_label = "已用:     ";
    zh.inference_remaining_label = "剩余:     ";
    zh.inference_prompt_question = "输入你的问题: ";
    zh.inference_result_label = "回复: ";
    zh.inference_tokens_label = "Tokens:   ";
    zh.inference_cost_label = "费用:     ";
    zh.inference_refund_success = "退还成功! TXID: ";
    zh.inference_refund_failed = "退还失败: ";
    zh.inference_invalid_key = "无效的 API Key 格式。\n";
    zh.inference_key_not_found = "API Key 未找到或已过期。\n";
    zh.inference_test_success = "\n--- LLM 结果 ---\n";
    zh.inference_test_failed = "LLM 失败: ";
    zh.inference_refund_confirm = "退还此 Key 的剩余余额? (y/n): ";
    I18N[2] = zh;

    I18nTexts ja;
    ja.select_language = "\n========================================\n  TokenCoin ウォレットマネージャー v1.0\n  言語を選択\n========================================\n";
    ja.main_menu_title = "\n========================================\n  TokenCoin ウォレットマネージャー\n========================================\n";
    ja.menu_new_wallet = "1. 新規ウォレット作成";
    ja.menu_restore_wallet = "2. ウォレット復元";
    ja.menu_open_wallet = "3. ウォレットを開く";
    ja.menu_node_info = "4. ノード情報";
    ja.menu_blockchain_info = "5. ブロックチェーン情報";
    ja.menu_network_info = "6. ネットワーク情報";
    ja.menu_block_info = "7. ブロックエクスプローラー";
    ja.menu_mining_info = "8. マイニング状態";
    ja.menu_exit = "0. 言語選択に戻る";
    ja.menu_back = "\nESC. 戻る";
    ja.menu_quit = "e. 終了";
    ja.prompt_select = "> ";
    ja.menu_wallet_address = "1. ウォレットアドレス";
    ja.menu_send_tknc = "2. token送信";
    ja.menu_transactions = "3. 取引履歴";
    ja.menu_backup_keys = "4. キーバックアップ";
    ja.menu_generate_address = "5. 新規アドレス生成";
    ja.menu_addr_label = "6. アドレスラベル";
    ja.menu_change_password = "7. パスワード変更";
    ja.menu_sign_message = "8. メッセージ署名";
    ja.menu_inference_service = "9. LLM";
    ja.menu_logout = "10. ログアウト";
    ja.create_wallet_title = "\n--- 新規ウォレット作成 ---\n";
    ja.prompt_wallet_name = "ウォレット名: ";
    ja.prompt_password = "パスワード設定: ";
    ja.prompt_password_confirm = "パスワード確認: ";
    ja.password_mismatch = "エラー: パスワードが一致しません!";
    ja.wallet_creating = "\n作成中...";
    ja.wallet_created_success = "\n✅ ウォレット作成完了!\n";
    ja.wallet_address_label = "アドレス: ";
    ja.wallet_balance_label = "残高: ";
    ja.wallet_total_balance_label = "合計残高: ";
    ja.addr_balance_label = "アドレス残高: ";
    ja.select_address_prompt = "番号を入力してアドレス切替: ";
    ja.addr_switched_msg = "✅ 切り替えました: ";
    ja.send_from_label = "送金元: ";
    ja.no_utxo_msg = "このアドレスに利用可能なUTXOがありません。\n";
    ja.restore_title = "\n--- ウォレット復元 ---\n";
    ja.restore_from_backup = "バックアップファイルから (.dat)";
    ja.restore_from_descriptors = "キーから復元";
    ja.restore_prompt_file = "選択 [1-2]: ";
    ja.restore_success = "✅ 復元完了!\n";
    ja.restore_failed = "❌ 失敗: ";
    ja.info_title = "\n--- ウォレット情報 ---\n";
    ja.info_wallet_name = "ウォレット: ";
    ja.info_address = "アドレス: ";
    ja.info_balance = "残高: ";
    ja.info_no_wallet = "ウォレットが読み込まれていません。\n";
    ja.send_title = "\n--- token送信 ---\n";
    ja.send_to_address = "送金先アドレス: ";
    ja.send_amount = "金額 (token): ";
    ja.send_confirm = "確認? (y/n): ";
    ja.send_success = "✅ 送信完了! TXID: ";
    ja.send_failed = "❌ 失敗: ";
    ja.send_insufficient = "❌ 残高不足!\n";
    ja.keys_title = "\n--- キーバックアップ ---\n";
    ja.keys_unlock_prompt = "パスワード入力 (60秒): ";
    ja.keys_descriptor_label = "キー:\n";
    ja.keys_warning = "⚠️ 安全に保管! 共有しないでください!\n";
    ja.press_continue = "\nEnterを押して続行...";
    ja.invalid_input = "無効な入力。";
    ja.back_to_menu = "\nメニューに戻る...";
    ja.yes_option = "y";
    ja.password_too_short = "エラー: パスワードは8文字以上必要です!";
    ja.wallet_name_label = "ウォレット名: ";
    ja.import_warning = "インポート警告: ";
    ja.format_label = "形式: ";
    ja.descriptor_type = "ディスクリプター";
    ja.legacy_type = "レガシー";
    ja.available_wallets = "利用可能なウォレット:\n";
    ja.available_label = "利用可能: ";
    ja.error_prefix = "エラー: ";
    ja.exit_message = "TokenCoinをご利用いただきありがとうございます!\n";
    // CreateNewWallet backup prompt strings
    ja.backup_important = "\n[重要] 今すぐウォレットをバックアップしてください!\n";
    ja.backup_goto_keys = "移動: ウォレットメニュー -> 5. キーバックアップ\n";
    ja.backup_descriptor_info = "これによりディスクリプター(xprv)が保存され、ウォレットを復元できます。\n";
    ja.backup_confirm_prompt = "バックアップすることを確認するため 'yes' と入力: ";
    ja.backup_reminder = "\nメインメニューからウォレットをバックアップしてください。\n";
    ja.wallet_name_label2 = "ウォレット名: ";
    ja.wallet_create_failed = "ウォレットの作成に失敗しました。ノードの状態を確認して再試行してください。\n";
    ja.wallet_create_error = "予期しないエラーが発生しました。もう一度お試しください。\n";
    // RestoreWallet strings
    ja.prompt_backup_path = "バックアップファイルパス: ";
    ja.err_file_empty = "ファイルパスは空にできません。\n";
    ja.err_file_not_found = "ファイルが見つかりません: ";
    ja.err_file_invalid = "無効なファイル: ";
    ja.prompt_new_wallet_name = "新しいウォレット名: ";
    ja.err_wallet_name_empty = "ウォレット名は空にできません。\n";
    ja.prompt_ext_descriptor = "\n受信用キー: ";
    ja.prompt_int_descriptor = "お釣り用キー: ";
    ja.err_no_descriptors = ": ディスクリプターがありません\n";
    // RestoreWallet: mnemonic restore option
    ja.restore_from_mnemonic = "ニーモニックから復元";
    ja.prompt_enter_mnemonic = "\n12個のリカバリー単語を入力してください(1行に1つ、空行で終了):\n";
    ja.prompt_mnemonic_word = "  %d番目の単語: ";
    ja.prompt_mnemonic_passphrase = "パスフレーズ(任意、Enterでスキップ): ";
    ja.restore_mnemonic_success = "\n✅ ウォレットがリカバリーフレーズから復元されました!\n";
    ja.restore_mnemonic_invalid = "無効な単語'%s'が%d番目にあります。BIP39英語単語のみ使用可能です。\n";
    ja.restore_mnemonic_xprv_failed = "キー導出に失敗しました。リカバリーフレーズとパスフレーズを確認してください。\n";
    ja.msg_no_wallet_create_first = "先にウォレットを作成または復元してください。\n";
    ja.prompt_unlock_or_skip = "パスワードを入力(Enterでスキップ): ";
    ja.msg_using_wallet = "\n使用中のウォレット: ";
    ja.err_addr_empty = "アドレスは空にできません。\n";
    ja.err_amount_nan = "無効な金額: 数字ではありません。\n";
    ja.err_amount_positive = "金額は0より大きくしてください。\n";
    ja.msg_no_bech32_descriptor = "ウォレットにBech32(token)キーが見つかりません。\n";
    // Real BIP39 mnemonic strings
    ja.mnemonic_title = "\n=== リカバリーフレーズを書き留めてください ===\n";
    ja.mnemonic_warning = "この情報を安全に保管してください! これらの単語を持つ誰でもウォレットにアクセスできます。\n";
    ja.mnemonic_word_label = "  %2d. %s\n";
    ja.mnemonic_verify_title = "\n=== リカバリーフレーズを確認 ===\n";
    ja.mnemonic_verify_prompt = "%d番目の単語を入力: ";
    ja.mnemonic_verify_success = "確認完了! ウォレットの準備ができました。\n";
    ja.mnemonic_verify_failed = "確認失敗! バックアップをよく確認してください。\n";
    ja.mnemonic_backup_confirm = "\n重要: 12個の単位すべてを書き留めましたか?\n";
    ja.mnemonic_backup_prompt = "'yes' と入力して確認: ";
    ja.mnemonic_backup_confirmed = "バックアップ確認完了! ウォレットが有効になりました。\n";
    // P0 security strings
    ja.keys_warning_full_key = "警告: 完全秘密鍵を表示すると、画面を見た誰でもすべての資金を盗むことができます。\n";
    ja.keys_full_key_header = "\n=== 完全秘密鍵(秘密にしてください!) ===\n";
    ja.keys_hidden_msg = "(鍵は非表示 — この端末は安全な場所でのみ使用してください)\n";
    ja.sign_own_address_only = "自分のアドレスのみメッセージに署名できます。\n";
    // Change password
    ja.change_pass_title = "\n--- パスワード変更 ---\n";
    ja.change_pass_old = "現在のパスワード: ";
    ja.change_pass_new = "新しいパスワード: ";
    ja.change_pass_confirm = "新しいパスワード確認: ";
    ja.change_pass_success = "パスワードが変更されました!\n";
    ja.change_pass_failed = "パスワード変更失敗: ";
    ja.change_pass_mismatch = "エラー: 新しいパスワードが一致しません!\n";
    ja.change_pass_not_encrypted = "このウォレットは暗号化されていません。パスワード変更不可。\n";
    // Delete address
    ja.addr_label_title = "\n--- アドレスラベル ---\n";
    ja.addr_label_prompt = "アドレス入力: ";
    ja.addr_label_name_prompt = "ラベル名(空で削除): ";
    ja.addr_label_success = "ラベルを設定しました。\n";
    ja.addr_label_failed = "ラベル設定失敗: ";
    ja.addr_label_removed = "ラベルを削除しました。\n";
    ja.addr_label_not_found = "ウォレットにアドレスが見つかりません。\n";
    // Sign message i18n
    ja.sign_addr_prompt = "アドレス: ";
    ja.sign_wallet_locked = "ウォレットがロックされています。パスワードを入力: ";
    ja.sign_must_unlock = "メッセージに署名するにはウォレットをロック解除してください。\n";
    ja.sign_addr_empty = "アドレスは空にできません。\n";
    ja.sign_addr_too_long = "アドレスが長すぎます（最大90文字）。\n";
    ja.sign_msg_too_long = "メッセージが長すぎます（最大4096文字）。\n";
    ja.sign_addr_not_owned = "このアドレスは現在のウォレットに属していません。\n";
    ja.sign_verify_failed = "アドレスの所有権確認に失敗しました。\n";
    ja.warn_no_passphrase = "警告：パスフレーズが入力されていません。暗号化なしでウォレットが作成されます。\n";
    ja.warn_anyone_access = "このコンピュータにアクセスできる人は誰でも資金を使用できます。\n";
    ja.confirm_unencrypted = "非暗号化ウォレットの作成を確認するには 'yes' を入力、パスフレーズを設定するにはEnter: ";
    ja.cleaning_incomplete_wallet = "ノードから不完全なウォレットをクリーンアップ中...\n";
    ja.wallet_functional_msg = "ウォレットは使用可能です。ウォレットメニューから「新規アドレス生成」をお試しください。\n";
    ja.restore_access_denied = "ウォレットは復元されましたがアクセスが拒否されました — パスフレーズが間違っています。\n";
    ja.wallet_encrypted_must_unlock = "このウォレットは暗号化されています。アクセスするにはパスフレーズを入力してください。\n";
    ja.import_descriptors_later = "後でバックアップキーメニューから欠落している記述子をインポートできます。\n";
    ja.bip39_checksum_mismatch = "無効なリカバリーフレーズ：BIP39チェックサムが一致しません。\n";
    ja.mnemonic_typo_check = "単語に誤字がないか確認してください。各単語は完全に正確である必要があります。\n";
    ja.too_many_failed_attempts = "失敗回数が多すぎます。お待ちください ";
    ja.label_chain = "チェーン: ";
    ja.label_headers = "ヘッダー: ";
    ja.label_version = "バージョン: ";
    ja.label_subversion = "サブバージョン: ";
    ja.label_connections = "接続数: ";
    ja.label_peers = "ピア数: ";
    ja.label_progress = "進行状況: ";
    ja.label_size_on_disk = "ディスクサイズ: ";
    ja.label_protocol = "プロトコル: ";
    ja.label_connections_io = "接続(入/出): ";
    ja.label_network_active = "ネットワーク有効: ";
    ja.label_connected_peers = "--- 接続済みピア (%d) ---\n";
    ja.label_previous = "前ブロック: ";
    ja.label_next = "次ブロック: ";
    ja.label_merkle_root = "マークルルート: ";
    ja.label_nonce = "ノンス: ";
    ja.label_tx_list = "トランザクション:\n";
    ja.label_network_hs = "ネットワーク H/s: ";
    ja.label_pooled_tx = "プールTX: ";
    ja.label_current_height = "現在の高さ: ";
    ja.msg_connecting = "TokenCoin ノードに接続中...";
    ja.err_cannot_connect = "  TokenCoin ノードに接続できません\n";
    ja.msg_ensure_running = "tkncd.exe が実行中か確認してください。\n\n";
    ja.msg_press_exit = "Enter キーで終了...\n";
    ja.msg_ok = " 接続成功!\n\n";
    ja.err_cli_error = "  token CLI エラー\n";
    ja.err_code_label = "エラーコード: ";
    ja.err_details_label = "詳細: ";
    ja.err_unknown = "不明なエラーが発生しました。\n";
    ja.block_explore_options = "1. ハッシュで検索\n2. 高さで検索\n3. 最新ブロック\n\n";
    ja.block_explore_select = "選択 [1-3]: ";
    ja.select_wallet_fmt = "ウォレットを選択 [1-%d]: ";
    ja.prompt_locked_unlock = "ウォレットがロックされています。パスフレーズを入力して解除: ";
    ja.label_receiving = "[受信] ";
    ja.label_change = "[お釣り]     ";
    ja.label_public = "[公開] ";
    ja.label_private = "[秘密] ";
    ja.label_primary = "(プライマリ)";
    ja.label_yes_short = "はい";
    ja.label_no_short = "いいえ";
    I18N[3] = ja;

    I18nTexts ko;
    ko.select_language = "\n========================================\n  TokenCoin 지갑 관리자 v1.0\n  언어 선택\n========================================\n";
    ko.not_logged_in_title = "\n========================================\n  TokenCoin 지갑 관리자 [공개]\n========================================\n";
    ko.public_menu_title = "\n========================================\n  TokenCoin 지갑 관리자 [공개]\n========================================\n";
    ko.logged_in_title = "\n========================================\n  TokenCoin 지갑 관리자\n========================================\n";
    ko.wallet_menu_title = "\n========================================\n  TokenCoin 지갑 관리자 [지갑: %s]\n========================================\n";
    ko.menu_new_wallet = "1. 새 지갑 생성";
    ko.menu_restore_wallet = "2. 지갑 복원";
    ko.menu_open_wallet = "3. 지갑 열기";
    ko.menu_node_info = "4. 노드 정보";
    ko.menu_blockchain_info = "5. 블록체인 정보";
    ko.menu_network_info = "6. 네트워크 정보";
    ko.menu_block_info = "7. 블록 탐색기";
    ko.menu_mining_info = "8. 채굴 상태";
    ko.menu_exit = "0. 언어 선택으로";
    ko.menu_back = "\nESC. 뒤로";
    ko.menu_quit = "e. 종료";
    ko.prompt_select = "> ";
    ko.menu_wallet_address = "1. 지갑 주소";
    ko.menu_send_tknc = "2. token 송금";
    ko.menu_transactions = "3. 거래 내역";
    ko.menu_backup_keys = "4. 키 백업";
    ko.menu_generate_address = "5. 새 주소 생성";
    ko.menu_addr_label = "6. 주소 라벨";
    ko.menu_change_password = "7. 비밀번호 변경";
    ko.menu_sign_message = "8. 메시지 서명";
    ko.menu_inference_service = "9. LLM";
    ko.menu_logout = "10. 로그아웃";
    ko.create_wallet_title = "\n--- 새 지갑 생성 ---\n";
    ko.prompt_wallet_name = "지갑 이름: ";
    ko.prompt_password = "비밀번호 설정: ";
    ko.prompt_password_confirm = "비밀번호 확인: ";
    ko.password_mismatch = "오류: 비밀번호가 일치하지 않습니다!";
    ko.wallet_creating = "\n생성 중...";
    ko.wallet_created_success = "\n✅ 지갑 생성 완료!\n";
    ko.wallet_address_label = "주소: ";
    ko.wallet_balance_label = "잔액: ";
    ko.wallet_total_balance_label = "총 잔액: ";
    ko.addr_balance_label = "주소 잔액: ";
    ko.select_address_prompt = "번호 입력하여 주소 전환: ";
    ko.addr_switched_msg = "✅ 전환됨: ";
    ko.send_from_label = "송금 주소: ";
    ko.no_utxo_msg = "이 주소에 사용 가능한 UTXO가 없습니다.\n";
    ko.restore_title = "\n--- 지갑 복원 ---\n";
    ko.restore_from_backup = "백업 파일에서 (.dat)";
    ko.restore_from_descriptors = "키에서 복원";
    ko.restore_prompt_file = "선택 [1-2]: ";
    ko.restore_success = "✅ 복원 완료!\n";
    ko.restore_failed = "❌ 실패: ";
    ko.info_title = "\n--- 지갑 정보 ---\n";
    ko.info_wallet_name = "지갑: ";
    ko.info_address = "주소: ";
    ko.info_balance = "잔액: ";
    ko.info_no_wallet = "로드된 지갑이 없습니다.\n";
    ko.send_title = "\n--- token 송금 ---\n";
    ko.send_to_address = "수신 주소: ";
    ko.send_amount = "금액 (token): ";
    ko.send_confirm = "확인? (y/n): ";
    ko.send_success = "✅ 전송 완료! TXID: ";
    ko.send_failed = "❌ 실패: ";
    ko.send_insufficient = "❌ 잔액 부족!\n";
    ko.keys_title = "\n--- 키 백업 ---\n";
    ko.keys_unlock_prompt = "비밀번호 입력 (60초): ";
    ko.keys_descriptor_label = "디스크립터:\n";
    ko.keys_warning = "⚠️ 안전히 보관! 공유하지 마세요!\n";
    ko.press_continue = "\n계속하려면 Enter를 누르세요...";
    ko.invalid_input = "잘못된 입력.";
    ko.back_to_menu = "\n메뉴로 돌아가기...";
    ko.yes_option = "y";
    ko.password_too_short = "오류: 비밀번호는 최소 8자 이상이어야 합니다!";
    ko.wallet_name_label = "지갑 이름: ";
    ko.import_warning = "가져오기 경고: ";
    ko.format_label = "형식: ";
    ko.descriptor_type = "디스크립터";
    ko.legacy_type = "레거시";
    ko.available_wallets = "사용 가능한 지갑:\n";
    ko.available_label = "사용 가능: ";
    ko.error_prefix = "오류: ";
    ko.exit_message = "TokenCoin를 이용해 주셔서 감사합니다!\n";
    ko.node_info_title = "\n--- 노드 정보 ---\n";
    ko.blockchain_title = "\n--- 블록체인 정보 ---\n";
    ko.network_title = "\n--- 네트워크 정보 ---\n";
    ko.block_title = "\n--- 블록 탐색기 ---\n";
    ko.block_height_label = "블록 높이: ";
    ko.block_hash_label = "블록 해시: ";
    ko.block_time_label = "시간: ";
    ko.block_tx_count_label = "거래 수: ";
    ko.transactions_title = "\n--- 거래 내역 ---\n";
    ko.no_transactions = "거래 내역이 없습니다.\n";
    ko.tx_id_label = "TXID: ";
    ko.tx_amount_label = "금액: ";
    ko.tx_confirmations_label = "확인 수: ";
    ko.mining_title = "\n--- 채굴 상태 ---\n";
    ko.mining_blocks_label = "블록 수: ";
    ko.mining_difficulty_label = "난이도: ";
    ko.mining_hashrate_label = "해시레이트: ";
    ko.generate_addr_title = "\n--- 새 주소 생성 ---\n";
    ko.generate_addr_success = "새 주소 생성됨: ";
    ko.prompt_block_hash = "블록 해시 입력 (또는 'latest'): ";
    ko.prompt_block_height = "블록 높이 입력 (또는 'latest'): ";
    ko.prompt_tx_count = "표시할 거래 수 [10]: ";
    ko.sign_title = "\n--- 메시지 서명 ---\n";
    ko.sign_message_prompt = "서명할 메시지: ";
    ko.sign_result_label = "서명: ";
    ko.auto_login_msg = "지갑 자동 로그인 중...";
    // CreateNewWallet backup prompt strings
    ko.backup_important = "\n[중요] 지갑을 지금 백업하세요!\n";
    ko.backup_goto_keys = "이동: 지갑 메뉴 -> 5. 키 백업\n";
    ko.backup_descriptor_info = "디스크립터(xprv)를 저장하며 지갑을 복원할 수 있습니다.\n";
    ko.backup_confirm_prompt = "백업할 것임을 확인하려면 'yes' 입력: ";
    ko.backup_reminder = "\n메인 메뉴에서 지갑을 백업하세요.\n";
    ko.wallet_name_label2 = "지갑 이름: ";
    ko.wallet_create_failed = "지갑 생성 실패. 노드 상태를 확인하고 다시 시도하세요.\n";
    ko.wallet_create_error = "예기치 않은 오류가 발생했습니다. 다시 시도하세요.\n";
    // RestoreWallet strings
    ko.prompt_backup_path = "백업 파일 경로: ";
    ko.err_file_empty = "파일 경로는 비워둘 수 없습니다.\n";
    ko.err_file_not_found = "파일을 찾을 수 없음: ";
    ko.err_file_invalid = "유효하지 않은 파일: ";
    ko.prompt_new_wallet_name = "새 지갑 이름: ";
    ko.err_wallet_name_empty = "지갑 이름은 비워둘 수 없습니다.\n";
    ko.prompt_ext_descriptor = "\n수신 키: ";
    ko.prompt_int_descriptor = "잔돈 키: ";
    ko.err_no_descriptors = ": 디스크립터 없음\n";
    // RestoreWallet: mnemonic restore option
    ko.restore_from_mnemonic = "니모닉에서 복원";
    ko.prompt_enter_mnemonic = "\n12개의 복구 단어를 입력하세요 (한 줄에 하나, 빈 줄로 종료):\n";
    ko.prompt_mnemonic_word = "  %d번째 단어: ";
    ko.prompt_mnemonic_passphrase = "암호구문(선택사항, Enter로 건너뛰기): ";
    ko.restore_mnemonic_success = "\n✅ 지갑이 복구 구문에서 복원되었습니다!\n";
    ko.restore_mnemonic_invalid = "잘못된 단어 '%s'가 %d번째에 있습니다. BIP39 영어 단어만 사용 가능합니다.\n";
    ko.restore_mnemonic_xprv_failed = "키 유도 실패. 복구 구문과 암호구문을 확인하세요.\n";
    ko.msg_no_wallet_create_first = "먼저 지갑을 생성하거나 복원하세요.\n";
    ko.prompt_unlock_or_skip = "비밀번호 입력(Enter로 건너뛰기): ";
    ko.msg_using_wallet = "\n사용 중인 지갑: ";
    ko.err_addr_empty = "주소는 비워둘 수 없습니다.\n";
    ko.err_amount_nan = "잘못된 금액: 숫자가 아닙니다.\n";
    ko.err_amount_positive = "금액은 0보다 커야 합니다.\n";
    ko.msg_no_bech32_descriptor = "지갑에서 Bech32(wpkh) 디스크립터를 찾을 수 없습니다.\n";
    // Real BIP39 mnemonic strings
    ko.mnemonic_title = "\n=== 복구 구문을 적어두세요 ===\n";
    ko.mnemonic_warning = "이 정보를 안전하게 보관하세요! 이 단어들로 누구나 지갑에 접근할 수 있습니다.\n";
    ko.mnemonic_word_label = "  %2d. %s\n";
    ko.mnemonic_verify_title = "\n=== 복구 구문 확인 ===\n";
    ko.mnemonic_verify_prompt = "%d번째 단어 입력: ";
    ko.mnemonic_verify_success = "확인 완료! 지갑이 준비되었습니다.\n";
    ko.mnemonic_verify_failed = "확인 실패! 백업을 잘 확인하세요.\n";
    ko.mnemonic_backup_confirm = "\n중요: 12개 단어를 모두 적었습니까?\n";
    ko.mnemonic_backup_prompt = "'yes' 입력하여 확인: ";
    ko.mnemonic_backup_confirmed = "백업 확인 완료! 지갑이 활성화되었습니다.\n";
    // P0 보안 문자열
    ko.keys_warning_full_key = "경고: 완전 개인키를 표시하면 화면을 본 누구나 모든 자금을 훔칠 수 있습니다.\n";
    ko.keys_full_key_header = "\n=== 완전 개인키(비밀 유지!) ===\n";
    ko.keys_hidden_msg = "(키 숨김 — 이 터미널은 안전한 장소에서만 사용하세요)\n";
    ko.sign_own_address_only = "자신의 주소로만 메시지에 서명할 수 있습니다.\n";
    // Change password
    ko.change_pass_title = "\n--- 비밀번호 변경 ---\n";
    ko.change_pass_old = "현재 비밀번호: ";
    ko.change_pass_new = "새 비밀번호: ";
    ko.change_pass_confirm = "새 비밀번호 확인: ";
    ko.change_pass_success = "비밀번호가 변경되었습니다!\n";
    ko.change_pass_failed = "비밀번호 변경 실패: ";
    ko.change_pass_mismatch = "오류: 새 비밀번호가 일치하지 않습니다!\n";
    ko.change_pass_not_encrypted = "이 지갑은 암호화되지 않았습니다. 비밀번호를 변경할 수 없습니다.\n";
    // Delete address
    ko.addr_label_title = "\n--- 주소 라벨 ---\n";
    ko.addr_label_prompt = "주소 입력: ";
    ko.addr_label_name_prompt = "라벨 이름(비워두면 삭제): ";
    ko.addr_label_success = "라벨이 설정되었습니다.\n";
    ko.addr_label_failed = "라벨 설정 실패: ";
    ko.addr_label_removed = "라벨이 삭제되었습니다.\n";
    ko.addr_label_not_found = "지갑에서 주소를 찾을 수 없습니다.\n";
    // Sign message i18n
    ko.sign_addr_prompt = "주소: ";
    ko.sign_wallet_locked = "지갑이 잠겨 있습니다. 비밀번호 입력: ";
    ko.sign_must_unlock = "메시지에 서명하려면 지갑을 잠금 해제해야 합니다.\n";
    ko.sign_addr_empty = "주소는 비워둘 수 없습니다.\n";
    ko.sign_addr_too_long = "주소가 너무 깁니다 (최대 90자).\n";
    ko.sign_msg_too_long = "메시지가 너무 깁니다 (최대 4096자).\n";
    ko.sign_addr_not_owned = "이 주소는 현재 지갑에 속하지 않습니다.\n";
    ko.sign_verify_failed = "주소 소유권 확인에 실패했습니다.\n";
    ko.warn_no_passphrase = "경고: 암호가 입력되지 않았습니다. 암호화되지 않은 지갑이 생성됩니다.\n";
    ko.warn_anyone_access = "이 컴퓨터에 접근할 수 있는 누구나 자금을 사용할 수 있습니다.\n";
    ko.confirm_unencrypted = "암호화되지 않은 지갑 생성을 확인하려면 'yes' 입력, 암호 설정은 Enter: ";
    ko.cleaning_incomplete_wallet = "노드에서 불완전한 지갑 정리 중...\n";
    ko.wallet_functional_msg = "지갑이 사용 가능합니다. 지갑 메뉴에서 '새 주소 생성'을 시도해 보세요.\n";
    ko.restore_access_denied = "지갑이 복원되었지만 접근이 거부되었습니다 — 암호가 틀렸습니다.\n";
    ko.wallet_encrypted_must_unlock = "이 지갑은 암호화되어 있습니다. 접근하려면 암호를 입력해야 합니다.\n";
    ko.import_descriptors_later = "나중에 백업 키 메뉴에서 누락된 기술자를 가져올 수 있습니다.\n";
    ko.bip39_checksum_mismatch = "잘못된 복구 구문: BIP39 체크섬 불일치.\n";
    ko.mnemonic_typo_check = "단어의 오타를 확인하세요. 각 단어는 정확해야 합니다.\n";
    ko.too_many_failed_attempts = "실패 횟수가 너무 많습니다. 잠시 기다려주세요 ";
    ko.label_chain = "체인: ";
    ko.label_headers = "헤더: ";
    ko.label_version = "버전: ";
    ko.label_subversion = "서브버전: ";
    ko.label_connections = "연결수: ";
    ko.label_peers = "피어수: ";
    ko.label_progress = "진행률: ";
    ko.label_size_on_disk = "디스크 크기: ";
    ko.label_protocol = "프로토콜: ";
    ko.label_connections_io = "연결(입/출): ";
    ko.label_network_active = "네트워크 활성: ";
    ko.label_connected_peers = "--- 연결된 피어 (%d) ---\n";
    ko.label_previous = "이전 블록: ";
    ko.label_next = "다음 블록: ";
    ko.label_merkle_root = "머클루트: ";
    ko.label_nonce = "논스: ";
    ko.label_tx_list = "거래 목록:\n";
    ko.label_network_hs = "네트워크 H/s: ";
    ko.label_pooled_tx = "풀 TX: ";
    ko.label_current_height = "현재 높이: ";
    ko.msg_connecting = "TokenCoin 노드에 연결 중...";
    ko.err_cannot_connect = "  TokenCoin 노드에 연결할 수 없습니다\n";
    ko.msg_ensure_running = "tkncd.exe 실행 중인지 확인하세요.\n\n";
    ko.msg_press_exit = "Enter 키로 종료...\n";
    ko.msg_ok = " 연결 성공!\n\n";
    ko.err_cli_error = "  token CLI 오류\n";
    ko.err_code_label = "오류 코드: ";
    ko.err_details_label = "상세: ";
    ko.err_unknown = "알 수 없는 오류가 발생했습니다.\n";
    ko.block_explore_options = "1. 해시로 조회\n2. 높이로 조회\n3. 최신 블록\n\n";
    ko.block_explore_select = "선택 [1-3]: ";
    ko.select_wallet_fmt = "지갑 선택 [1-%d]: ";
    ko.prompt_locked_unlock = "지갑이 잠겨있습니다. 암호 입력하여 잠금 해제: ";
    ko.label_receiving = "[수신] ";
    ko.label_change = "[거스름]     ";
    ko.label_public = "[공개] ";
    ko.label_private = "[비밀] ";
    ko.label_primary = "(기본)";
    ko.label_yes_short = "예";
    ko.label_no_short = "아니오";
    I18N[4] = ko;

    I18nTexts es;
    es.select_language = "\n========================================\n  Gestor de Carteras TokenCoin v1.0\n  Seleccionar Idioma\n========================================\n";
    es.not_logged_in_title = "\n========================================\n  Gestor de Carteras TokenCoin [Público]\n========================================\n";
    es.public_menu_title = "\n========================================\n  Gestor de Carteras TokenCoin [Público]\n========================================\n";
    es.logged_in_title = "\n========================================\n  Gestor de Carteras TokenCoin\n========================================\n";
    es.wallet_menu_title = "\n========================================\n  Gestor de Carteras TokenCoin [Cartera: %s]\n========================================\n";
    es.menu_new_wallet = "1. Crear Nueva Cartera";
    es.menu_restore_wallet = "2. Restaurar Cartera";
    es.menu_open_wallet = "3. Abrir Cartera";
    es.menu_node_info = "4. Información del Nodo";
    es.menu_blockchain_info = "5. Info Blockchain";
    es.menu_network_info = "6. Info Red";
    es.menu_block_info = "7. Explorador Bloques";
    es.menu_mining_info = "8. Estado Minería";
    es.menu_exit = "0. Volver a Idioma";
    es.menu_back = "\nESC. Volver";
    es.menu_quit = "e. Salir";
    es.prompt_select = "> ";
    es.menu_wallet_address = "1. Dirección de Cartera";
    es.menu_send_tknc = "2. Enviar token";
    es.menu_transactions = "3. Historial de Transacciones";
    es.menu_backup_keys = "4. Copia de Seguridad";
    es.menu_generate_address = "5. Generar Nueva Dirección";
    es.menu_addr_label = "6. Etiqueta de Dirección";
    es.menu_change_password = "7. Cambiar Contraseña";
    es.menu_sign_message = "8. Firmar Mensaje";
    es.menu_inference_service = "9. LLM";
    es.menu_logout = "10. Cerrar Sesión";
    es.create_wallet_title = "\n--- Crear Nueva Cartera ---\n";
    es.prompt_wallet_name = "Nombre de cartera: ";
    es.prompt_password = "Contraseña: ";
    es.prompt_password_confirm = "Confirmar contraseña: ";
    es.password_mismatch = "ERROR: Las contraseñas no coinciden!";
    es.wallet_creating = "\nCreando cartera...";
    es.wallet_created_success = "\n✅ ¡Cartera creada exitosamente!\n";
    es.wallet_address_label = "Dirección: ";
    es.wallet_balance_label = "Saldo: ";
    es.wallet_total_balance_label = "Saldo Total: ";
    es.addr_balance_label = "Saldo de Dirección: ";
    es.select_address_prompt = "Ingrese número para cambiar dirección: ";
    es.addr_switched_msg = "✅ Cambiado a: ";
    es.send_from_label = "De: ";
    es.no_utxo_msg = "No hay UTXOs disponibles para esta dirección.\n";
    es.restore_title = "\n--- Restaurar Cartera ---\n";
    es.restore_from_backup = "Desde archivo de respaldo (.dat)";
    es.restore_from_descriptors = "Desde claves";
    es.restore_prompt_file = "Seleccionar [1-2]: ";
    es.restore_success = "✅ ¡Restaurada exitosamente!\n";
    es.restore_failed = "❌ Error: ";
    es.info_title = "\n--- Información de Cartera ---\n";
    es.info_wallet_name = "Cartera: ";
    es.info_address = "Dirección: ";
    es.info_balance = "Saldo: ";
    es.info_no_wallet = "No hay cartera cargada.\n";
    es.send_title = "\n--- Enviar token ---\n";
    es.send_to_address = "Dirección del destinatario: ";
    es.send_amount = "Cantidad (token): ";
    es.send_confirm = "¿Confirmar? (s/n): ";
    es.send_success = "✅ ¡Enviado! TXID: ";
    es.send_failed = "❌ Error: ";
    es.send_insufficient = "❌ ¡Saldo insuficiente!\n";
    es.keys_title = "\n--- Copia de Seguridad de Claves ---\n";
    es.keys_unlock_prompt = "Contraseña (60s): ";
    es.keys_descriptor_label = "Descriptores:\n";
    es.keys_warning = "⚠️ ¡Mantenga seguro! ¡No comparta!\n";
    es.press_continue = "\nPresione Enter para continuar...";
    es.invalid_input = "Entrada inválida.";
    es.back_to_menu = "\nVolviendo al menú...";
    es.yes_option = "s";
    es.password_too_short = "ERROR: ¡La contraseña debe tener al menos 8 caracteres!";
    es.wallet_name_label = "Nombre de cartera: ";
    es.import_warning = "Advertencia de importación: ";
    es.format_label = "Formato: ";
    es.descriptor_type = "Descriptor";
    es.legacy_type = "Legacy";
    es.available_wallets = "Carteras disponibles:\n";
    es.available_label = "Disponible: ";
    es.error_prefix = "Error: ";
    es.exit_message = "¡Gracias por usar token!\n";
    es.node_info_title = "\n--- Información del Nodo ---\n";
    es.blockchain_title = "\n--- Información de Blockchain ---\n";
    es.network_title = "\n--- Información de Red ---\n";
    es.block_title = "\n--- Explorador de Bloques ---\n";
    es.block_height_label = "Altura de bloque: ";
    es.block_hash_label = "Hash de bloque: ";
    es.block_time_label = "Hora: ";
    es.block_tx_count_label = "Transacciones: ";
    es.transactions_title = "\n--- Historial de Transacciones ---\n";
    es.no_transactions = "No se encontraron transacciones.\n";
    es.tx_id_label = "TXID: ";
    es.tx_amount_label = "Cantidad: ";
    es.tx_confirmations_label = "Confirmaciones: ";
    es.tx_direction_label = "Tipo";
    es.tx_address_label = "Direccion";
    es.tx_from_label = "De";
    es.tx_to_label = "Para";
    es.tx_time_label = "Hora";
    es.tx_send_label = "Envio";
    es.tx_receive_label = "Recepcion";
    es.mining_title = "\n--- Estado de Minería ---\n";
    es.mining_blocks_label = "Bloques: ";
    es.mining_difficulty_label = "Dificultad: ";
    es.mining_hashrate_label = "Hashrate: ";
    es.generate_addr_title = "\n--- Generar Nueva Dirección ---\n";
    es.generate_addr_success = "Nueva dirección generada: ";
    es.prompt_block_hash = "Ingrese hash de bloque (o 'latest'): ";
    es.prompt_block_height = "Ingrese altura de bloque (o 'latest'): ";
    es.prompt_tx_count = "Número de transacciones [10]: ";
    es.sign_title = "\n--- Firmar Mensaje ---\n";
    es.sign_message_prompt = "Mensaje a firmar: ";
    es.sign_result_label = "Firma: ";
    es.auto_login_msg = "Auto-iniciando sesión en cartera...";
    // CreateNewWallet backup prompt strings
    es.backup_important = "\n[IMPORTANTE] ¡Respalde su cartera AHORA!\n";
    es.backup_goto_keys = "Ir a: Menú Cartera -> 5. Copia de Seguridad\n";
    es.backup_descriptor_info = "Esto guarda su descriptor (xprv) que PUEDE restaurar su cartera.\n";
    es.backup_confirm_prompt = "Escriba 'yes' para confirmar que hará respaldo: ";
    es.backup_reminder = "\nPor favor respalde su cartera desde el menú principal.\n";
    es.wallet_name_label2 = "Nombre de cartera: ";
    es.wallet_create_failed = "Error al crear cartera. Verifique el estado del nodo e inténtelo de nuevo.\n";
    es.wallet_create_error = "Ocurrió un error inesperado. Por favor intente de nuevo.\n";
    // RestoreWallet strings
    es.prompt_backup_path = "Ruta del archivo de respaldo: ";
    es.err_file_empty = "La ruta del archivo no puede estar vacía.\n";
    es.err_file_not_found = "Archivo no encontrado: ";
    es.err_file_invalid = "Archivo no válido: ";
    es.prompt_new_wallet_name = "Nombre de nueva cartera: ";
    es.err_wallet_name_empty = "El nombre de la cartera no puede estar vacío.\n";
    es.prompt_ext_descriptor = "\nClave de recepción: ";
    es.prompt_int_descriptor = "Clave de cambio: ";
    es.err_no_descriptors = ": Sin descriptores\n";
    // RestoreWallet: mnemonic restore option
    es.restore_from_mnemonic = "Desde mnemónico";
    es.prompt_enter_mnemonic = "\nIngrese sus 12 palabras de recuperación (una por línea, línea vacía para terminar):\n";
    es.prompt_mnemonic_word = "  Palabra %d: ";
    es.prompt_mnemonic_passphrase = "Contraseña (opcional, presione Enter para omitir): ";
    es.restore_mnemonic_success = "\n✅ ¡Cartera restaurada desde frase de recuperación!\n";
    es.restore_mnemonic_invalid = "Palabra inválida '%s' en la posición %d. Solo palabras BIP39 en inglés.\n";
    es.restore_mnemonic_xprv_failed = "Derivación de clave fallida. Verifique su frase de recuperación y contraseña.\n";
    es.msg_no_wallet_create_first = "Por favor cree o restaure una cartera primero.\n";
    es.prompt_unlock_or_skip = "Ingrese contraseña (o presione Enter para omitir): ";
    es.msg_using_wallet = "\nUsando cartera: ";
    es.err_addr_empty = "La dirección no puede estar vacía.\n";
    es.err_amount_nan = "Cantidad inválida: no es un número.\n";
    es.err_amount_positive = "La cantidad debe ser mayor que 0.\n";
    es.msg_no_bech32_descriptor = "No se encontraron claves Bech32 (token) en la cartera.\n";
    // Real BIP39 mnemonic strings
    es.mnemonic_title = "\n=== Escriba Su Frase de Recuperación ===\n";
    es.mnemonic_warning = "¡Mantenga esta información segura! Cualquiera con estas palabras puede acceder a su cartera.\n";
    es.mnemonic_word_label = "  %2d. %s\n";
    es.mnemonic_verify_title = "\n=== Verifique Su Frase de Recuperación ===\n";
    es.mnemonic_verify_prompt = "Ingrese palabra #%d: ";
    es.mnemonic_verify_success = "¡Verificación exitosa! La cartera está lista.\n";
    es.mnemonic_verify_failed = "¡Verificación fallida! Por favor verifique su respaldo cuidadosamente.\n";
    es.mnemonic_backup_confirm = "\nIMPORTANTE: ¿Ha escrito las 12 palabras?\n";
    es.mnemonic_backup_prompt = "Escriba 'yes' para confirmar: ";
    es.mnemonic_backup_confirmed = "¡Respaldo confirmado! Su cartera ahora está activa.\n";
    // Cadenas de seguridad P0
    es.keys_warning_full_key = "ADVERTENCIA: Mostrar la clave privada completa permite a cualquiera que vea su pantalla robar TODOS los fondos.\n";
    es.keys_full_key_header = "\n=== CLAVES PRIVADAS COMPLETAS (¡mantenga secreto!) ===\n";
    es.keys_hidden_msg = "(Claves ocultas — use esta terminal solo en un lugar seguro)\n";
    es.sign_own_address_only = "Solo puede firmar mensajes con sus propias direcciones.\n";
    // Change password
    es.change_pass_title = "\n--- Cambiar Contraseña ---\n";
    es.change_pass_old = "Contraseña actual: ";
    es.change_pass_new = "Nueva contraseña: ";
    es.change_pass_confirm = "Confirmar nueva contraseña: ";
    es.change_pass_success = "¡Contraseña cambiada exitosamente!\n";
    es.change_pass_failed = "Error al cambiar contraseña: ";
    es.change_pass_mismatch = "¡ERROR: Las nuevas contraseñas no coinciden!\n";
    es.change_pass_not_encrypted = "Esta cartera no está cifrada. No se puede cambiar la contraseña.\n";
    // Delete address
    es.addr_label_title = "\n--- Etiqueta de Dirección ---\n";
    es.addr_label_prompt = "Dirección: ";
    es.addr_label_name_prompt = "Nombre de etiqueta (vacío para eliminar): ";
    es.addr_label_success = "Etiqueta establecida.\n";
    es.addr_label_failed = "Error al establecer etiqueta: ";
    es.addr_label_removed = "Etiqueta eliminada.\n";
    es.addr_label_not_found = "Dirección no encontrada en la cartera.\n";
    // Sign message i18n
    es.sign_addr_prompt = "Dirección: ";
    es.sign_wallet_locked = "Cartera bloqueada. Ingrese contraseña para desbloquear: ";
    es.sign_must_unlock = "La cartera debe estar desbloqueada para firmar mensajes.\n";
    es.sign_addr_empty = "La dirección no puede estar vacía.\n";
    es.sign_addr_too_long = "Dirección demasiado larga (máx. 90 caracteres).\n";
    es.sign_msg_too_long = "Mensaje demasiado largo (máx. 4096 caracteres).\n";
    es.sign_addr_not_owned = "La dirección no pertenece a la cartera actual.\n";
    es.sign_verify_failed = "Error al verificar la propiedad de la dirección.\n";
    es.warn_no_passphrase = "ADVERTENCIA: No se ingresó contraseña. La cartera se creará SIN cifrado.\n";
    es.warn_anyone_access = "Cualquier persona con acceso a este computador puede gastar sus fondos.\n";
    es.confirm_unencrypted = "Escriba 'yes' para confirmar una cartera sin cifrar, o presione Enter para establecer contraseña: ";
    es.cleaning_incomplete_wallet = "Limpiando cartera incompleta del nodo...\n";
    es.wallet_functional_msg = "La cartera es funcional. Pruebe 'Generar Nueva Dirección' en el menú de la cartera.\n";
    es.restore_access_denied = "Cartera restaurada pero acceso denegado — contraseña incorrecta.\n";
    es.wallet_encrypted_must_unlock = "Esta cartera está cifrada. Debe ingresar la contraseña para acceder.\n";
    es.import_descriptors_later = "Puede importar descriptores faltantes más tarde desde el menú de Claves de Respaldo.\n";
    es.bip39_checksum_mismatch = "Frase de recuperación inválida: checksum BIP39 no coincide.\n";
    es.mnemonic_typo_check = "Verifique si hay errores tipográficos en las palabras. Cada palabra debe ser exactamente correcta.\n";
    es.too_many_failed_attempts = "Demasiados intentos fallidos. Espere ";
    es.label_chain = "Cadena: ";
    es.label_headers = "Encabezados: ";
    es.label_version = "Versión: ";
    es.label_subversion = "Subversión: ";
    es.label_connections = "Conexiones: ";
    es.label_peers = "Pares: ";
    es.label_progress = "Progreso: ";
    es.label_size_on_disk = "Tamaño en disco: ";
    es.label_protocol = "Protocolo: ";
    es.label_connections_io = "Conexiones (ent/sal): ";
    es.label_network_active = "Red Activa: ";
    es.label_connected_peers = "--- Pares Conectados (%d) ---\n";
    es.label_previous = "Anterior: ";
    es.label_next = "Siguiente: ";
    es.label_merkle_root = "Raíz Merkle: ";
    es.label_nonce = "Nonce: ";
    es.label_tx_list = "Transacciones:\n";
    es.label_network_hs = "Red H/s: ";
    es.label_pooled_tx = "TX en Pool: ";
    es.label_current_height = "Altura Actual: ";
    es.msg_connecting = "Conectando al nodo TokenCoin...";
    es.err_cannot_connect = "  No se puede conectar al nodo TokenCoin\n";
    es.msg_ensure_running = "Asegúrese de que tkncd.exe esté ejecutándose.\n\n";
    es.msg_press_exit = "Presione Enter para salir...\n";
    es.msg_ok = " ¡OK!\n\n";
    es.err_cli_error = "  Error de token CLI\n";
    es.err_code_label = "Código de error: ";
    es.err_details_label = "Detalles: ";
    es.err_unknown = "Ocurrió un error desconocido.\n";
    es.block_explore_options = "1. Por Hash\n2. Por Altura\n3. Último Bloque\n\n";
    es.block_explore_select = "Seleccionar [1-3]: ";
    es.select_wallet_fmt = "Seleccionar cartera [1-%d]: ";
    es.prompt_locked_unlock = "Cartera bloqueada. Ingrese contraseña para desbloquear: ";
    es.label_receiving = "[Recibiendo] ";
    es.label_change = "[Cambio]       ";
    es.label_public = "[Público]  ";
    es.label_private = "[Privado]  ";
    es.label_primary = "(Principal)";
    es.label_yes_short = "Sí";
    es.label_no_short = "No";
    I18N[5] = es;

    I18nTexts fr;
    fr.select_language = "\n========================================\n  Gestionnaire de Portefeuilles TokenCoin v1.0\n  Sélectionner la Langue\n========================================\n";
    fr.not_logged_in_title = "\n========================================\n  Gestionnaire de Portefeuilles TokenCoin [Public]\n========================================\n";
    fr.public_menu_title = "\n========================================\n  Gestionnaire de Portefeuilles TokenCoin [Public]\n========================================\n";
    fr.logged_in_title = "\n========================================\n  Gestionnaire de Portefeuilles TokenCoin\n========================================\n";
    fr.wallet_menu_title = "\n========================================\n  Gestionnaire de Portefeuilles TokenCoin [Portefeuille: %s]\n========================================\n";
    fr.menu_new_wallet = "1. Créer Portefeuille";
    fr.menu_restore_wallet = "2. Restaurer le Portefeuille";
    fr.menu_open_wallet = "3. Ouvrir Portefeuille";
    fr.menu_node_info = "4. Infos Nœud";
    fr.menu_blockchain_info = "5. Info Blockchain";
    fr.menu_network_info = "6. Info Réseau";
    fr.menu_block_info = "7. Explorateur Blocs";
    fr.menu_mining_info = "8. État Minage";
    fr.menu_exit = "0. Retour Langue";
    fr.menu_back = "\nESC. Retour";
    fr.menu_quit = "e. Quitter";
    fr.prompt_select = "> ";
    fr.menu_wallet_address = "1. Adresse du Portefeuille";
    fr.menu_send_tknc = "2. Envoyer token";
    fr.menu_transactions = "3. Historique des Transactions";
    fr.menu_backup_keys = "4. Sauvegarder les Clés";
    fr.menu_generate_address = "5. Générer Nouvelle Adresse";
    fr.menu_addr_label = "6. Étiquette d'Adresse";
    fr.menu_change_password = "7. Changer Mot de Passe";
    fr.menu_sign_message = "8. Signer Message";
    fr.menu_inference_service = "9. LLM";
    fr.menu_logout = "10. Déconnexion";
    fr.create_wallet_title = "\n--- Créer un Nouveau Portefeuille ---\n";
    fr.prompt_wallet_name = "Nom du portefeuille: ";
    fr.prompt_password = "Mot de passe: ";
    fr.prompt_password_confirm = "Confirmer le mot de passe: ";
    fr.password_mismatch = "ERREUR: Les mots de passe ne correspondent pas!";
    fr.wallet_creating = "\nCréation du portefeuille...";
    fr.wallet_created_success = "\n✅ Portefeuille créé avec succès!\n";
    fr.wallet_address_label = "Adresse: ";
    fr.wallet_balance_label = "Solde: ";
    fr.wallet_total_balance_label = "Solde Total: ";
    fr.addr_balance_label = "Solde de l'Adresse: ";
    fr.select_address_prompt = "Entrez le numéro pour changer d'adresse: ";
    fr.addr_switched_msg = "✅ Changé à: ";
    fr.send_from_label = "De: ";
    fr.no_utxo_msg = "Pas d'UTXOs disponibles pour cette adresse.\n";
    fr.restore_title = "\n--- Restaurer le Portefeuille ---\n";
    fr.restore_from_backup = "Depuis un fichier de sauvegarde (.dat)";
    fr.restore_from_descriptors = "Depuis les clés";
    fr.restore_prompt_file = "Sélectionner [1-2]: ";
    fr.restore_success = "✅ Restauré avec succès!\n";
    fr.restore_failed = "❌ Échec: ";
    fr.info_title = "\n--- Informations du Portefeuille ---\n";
    fr.info_wallet_name = "Portefeuille: ";
    fr.info_address = "Adresse: ";
    fr.info_balance = "Solde: ";
    fr.info_no_wallet = "Aucun portefeuille chargé.\n";
    fr.send_title = "\n--- Envoyer token ---\n";
    fr.send_to_address = "Adresse du destinataire: ";
    fr.send_amount = "Montant (token): ";
    fr.send_confirm = "Confirmer? (o/n): ";
    fr.send_success = "✅ Envoyé! TXID: ";
    fr.send_failed = "❌ Échec: ";
    fr.send_insufficient = "❌ Solde insuffisant!\n";
    fr.keys_title = "\n--- Sauvegarder les Clés ---\n";
    fr.keys_unlock_prompt = "Mot de passe (60s): ";
    fr.keys_descriptor_label = "Descripteurs:\n";
    fr.keys_warning = "⚠️ Gardez sécurisé! Ne partagez pas!\n";
    fr.press_continue = "\nAppuyez sur Entrée pour continuer...";
    fr.invalid_input = "Entrée invalide.";
    fr.back_to_menu = "\nRetour au menu...";
    fr.yes_option = "o";
    fr.password_too_short = "ERREUR: Le mot de passe doit contenir au moins 8 caractères!";
    fr.wallet_name_label = "Nom du portefeuille: ";
    fr.import_warning = "Avertissement d'importation: ";
    fr.format_label = "Format: ";
    fr.descriptor_type = "Descripteur";
    fr.legacy_type = "Legacy";
    fr.available_wallets = "Portefeuilles disponibles:\n";
    fr.available_label = "Disponible: ";
    fr.error_prefix = "Erreur: ";
    fr.exit_message = "Merci d'utiliser token!\n";
    fr.node_info_title = "\n--- Informations du Nœud ---\n";
    fr.blockchain_title = "\n--- Informations Blockchain ---\n";
    fr.network_title = "\n--- Informations Réseau ---\n";
    fr.block_title = "\n--- Explorateur de Blocs ---\n";
    fr.block_height_label = "Hauteur de bloc: ";
    fr.block_hash_label = "Hash de bloc: ";
    fr.block_time_label = "Heure: ";
    fr.block_tx_count_label = "Transactions: ";
    fr.transactions_title = "\n--- Historique des Transactions ---\n";
    fr.no_transactions = "Aucune transaction trouvée.\n";
    fr.tx_id_label = "TXID: ";
    fr.tx_amount_label = "Montant: ";
    fr.tx_confirmations_label = "Confirmations: ";
    fr.tx_direction_label = "Type";
    fr.tx_address_label = "Adresse";
    fr.tx_from_label = "De";
    fr.tx_to_label = "Vers";
    fr.tx_time_label = "Heure";
    fr.tx_send_label = "Envoi";
    fr.tx_receive_label = "Reception";
    fr.mining_title = "\n--- État du Minage ---\n";
    fr.mining_blocks_label = "Blocs: ";
    fr.mining_difficulty_label = "Difficulté: ";
    fr.mining_hashrate_label = "Hashrate: ";
    fr.generate_addr_title = "\n--- Générer Nouvelle Adresse ---\n";
    fr.generate_addr_success = "Nouvelle adresse générée: ";
    fr.prompt_block_hash = "Entrez le hash de bloc (ou 'latest'): ";
    fr.prompt_block_height = "Entrez la hauteur de bloc (ou 'latest'): ";
    fr.prompt_tx_count = "Nombre de transactions [10]: ";
    fr.sign_title = "\n--- Signer Message ---\n";
    fr.sign_message_prompt = "Message à signer: ";
    fr.sign_result_label = "Signature: ";
    fr.auto_login_msg = "Connexion automatique au portefeuille...";
    // CreateNewWallet backup prompt strings
    fr.backup_important = "\n[IMPORTANT] Sauvegardez votre portefeuille MAINTENANT!\n";
    fr.backup_goto_keys = "Aller à: Menu Portefeuille -> 5. Sauvegarder les Clés\n";
    fr.backup_descriptor_info = "Ceci sauve votre descripteur (xprv) qui PEUT restaurer votre portefeuille.\n";
    fr.backup_confirm_prompt = "Tapez 'yes' pour confirmer que vous allez sauvegarder: ";
    fr.backup_reminder = "\nVeuillez sauvegarder votre portefeuille depuis le menu principal.\n";
    fr.wallet_name_label2 = "Nom du portefeuille: ";
    fr.wallet_create_failed = "Échec de la création du portefeuille. Vérifiez l'état du nœud et réessayez.\n";
    fr.wallet_create_error = "Une erreur inattendue s'est produite. Veuillez réessayer.\n";
    // RestoreWallet strings
    fr.prompt_backup_path = "Chemin du fichier de sauvegarde: ";
    fr.err_file_empty = "Le chemin du fichier ne peut pas être vide.\n";
    fr.err_file_not_found = "Fichier non trouvé: ";
    fr.err_file_invalid = "Fichier invalide: ";
    fr.prompt_new_wallet_name = "Nouveau nom de portefeuille: ";
    fr.err_wallet_name_empty = "Le nom du portefeuille ne peut pas être vide.\n";
    fr.prompt_ext_descriptor = "\nClé de réception: ";
    fr.prompt_int_descriptor = "Clé de changement: ";
    fr.err_no_descriptors = ": Pas de descripteurs\n";
    // RestoreWallet: mnemonic restore option
    fr.restore_from_mnemonic = "Depuis le mnémonique";
    fr.prompt_enter_mnemonic = "\nEntrez vos 12 mots de récupération (un par ligne, ligne vide pour finir):\n";
    fr.prompt_mnemonic_word = "  Mot %d : ";
    fr.prompt_mnemonic_passphrase = "Phrase de passe (optionnelle, appuyez sur Entrée pour ignorer) : ";
    fr.restore_mnemonic_success = "\n✅ Portefeuille restauré depuis la phrase de récupération !\n";
    fr.restore_mnemonic_invalid = "Mot invalide '%s' à la position %d. Seuls les mots BIP39 anglais sont acceptés.\n";
    fr.restore_mnemonic_xprv_failed = "Échec de la dérivation de clé. Vérifiez votre phrase de récupération et la phrase de passe.\n";
    fr.msg_no_wallet_create_first = "Veuillez créer ou restaurer un portefeuille d'abord.\n";
    fr.prompt_unlock_or_skip = "Entrez le mot de passe (ou appuyez sur Entrée pour ignorer) : ";
    fr.msg_using_wallet = "\nPortefeuille utilisé : ";
    fr.err_addr_empty = "L'adresse ne peut pas être vide.\n";
    fr.err_amount_nan = "Montant invalide : ce n'est pas un nombre.\n";
    fr.err_amount_positive = "Le montant doit être supérieur à 0.\n";
    fr.msg_no_bech32_descriptor = "Aucun descripteur Bech32 (wpkh) trouvé dans le portefeuille.\n";
    // Real BIP39 mnemonic strings
    fr.mnemonic_title = "\n=== Notez Votre Phrase de Récupération ===\n";
    fr.mnemonic_warning = "Gardez cette information en sécurité! Quiconque possède ces mots peut accéder à votre portefeuille.\n";
    fr.mnemonic_word_label = "  %2d. %s\n";
    fr.mnemonic_verify_title = "\n=== Vérifiez Votre Phrase de Récupération ===\n";
    fr.mnemonic_verify_prompt = "Entrez le mot #%d: ";
    fr.mnemonic_verify_success = "Vérification réussie! Le portefeuille est prêt.\n";
    fr.mnemonic_verify_failed = "Échec de la vérification! Veuillez vérifier votre sauvegarde attentivement.\n";
    fr.mnemonic_backup_confirm = "\nIMPORTANT: Avez-vous noté les 12 mots?\n";
    fr.mnemonic_backup_prompt = "Tapez 'yes' pour confirmer: ";
    fr.mnemonic_backup_confirmed = "Sauvegarde confirmée! Votre portefeuille est maintenant actif.\n";
    // Chaînes de sécurité P0
    fr.keys_warning_full_key = "AVERTISSEMENT: Afficher la clé privée complète permet à quiconque voit votre écran de voler TOUS les fonds.\n";
    fr.keys_full_key_header = "\n=== CLÉS PRIVÉES COMPLÈTES (gardez secret!) ===\n";
    fr.keys_hidden_msg = "(Clés masquées — utilisez ce terminal uniquement dans un endroit sécurisé)\n";
    fr.sign_own_address_only = "Vous ne pouvez signer des messages qu'avec vos propres adresses.\n";
    // Change password
    fr.change_pass_title = "\n--- Changer Mot de Passe ---\n";
    fr.change_pass_old = "Mot de passe actuel: ";
    fr.change_pass_new = "Nouveau mot de passe: ";
    fr.change_pass_confirm = "Confirmer nouveau mot de passe: ";
    fr.change_pass_success = "Mot de passe changé avec succès!\n";
    fr.change_pass_failed = "Échec du changement de mot de passe: ";
    fr.change_pass_mismatch = "ERREUR: Les nouveaux mots de passe ne correspondent pas!\n";
    fr.change_pass_not_encrypted = "Ce portefeuille n'est pas chiffré. Impossible de changer le mot de passe.\n";
    // Delete address
    fr.addr_label_title = "\n--- Étiquette d'Adresse ---\n";
    fr.addr_label_prompt = "Adresse: ";
    fr.addr_label_name_prompt = "Nom de l'étiquette (vide pour supprimer): ";
    fr.addr_label_success = "Étiquette définie.\n";
    fr.addr_label_failed = "Échec de définition d'étiquette: ";
    fr.addr_label_removed = "Étiquette supprimée.\n";
    fr.addr_label_not_found = "Adresse introuvable dans le portefeuille.\n";
    // Sign message i18n
    fr.sign_addr_prompt = "Adresse: ";
    fr.sign_wallet_locked = "Portefeuille verrouillé. Entrez le mot de passe: ";
    fr.sign_must_unlock = "Le portefeuille doit être déverrouillé pour signer des messages.\n";
    fr.sign_addr_empty = "L'adresse ne peut pas être vide.\n";
    fr.sign_addr_too_long = "Adresse trop longue (max 90 caractères).\n";
    fr.sign_msg_too_long = "Message trop long (max 4096 caractères).\n";
    fr.sign_addr_not_owned = "L'adresse n'appartient pas au portefeuille actuel.\n";
    fr.sign_verify_failed = "Échec de la vérification de la propriété de l'adresse.\n";
    fr.warn_no_passphrase = "AVERTISSEMENT : Aucune phrase de passe saisie. Le portefeuille sera créé SANS chiffrement.\n";
    fr.warn_anyone_access = "Toute personne ayant accès à cet ordinateur peut dépenser vos fonds.\n";
    fr.confirm_unencrypted = "Tapez 'yes' pour confirmer un portefeuille non chiffré, ou appuyez sur Entrée pour définir une phrase de passe: ";
    fr.cleaning_incomplete_wallet = "Nettoyage du portefeuille incomplet du nœud...\n";
    fr.wallet_functional_msg = "Le portefeuille est fonctionnel. Essayez « Générer Nouvelle Adresse » depuis le menu du portefeuille.\n";
    fr.restore_access_denied = "Portefeuille restauré mais accès refusé — phrase de passe incorrecte.\n";
    fr.wallet_encrypted_must_unlock = "Ce portefeuille est chiffré. Vous devez saisir la phrase de passe pour y accéder.\n";
    fr.import_descriptors_later = "Vous pouvez importer les descripteurs manquants plus tard depuis le menu Clés de Sauvegarde.\n";
    fr.bip39_checksum_mismatch = "Phrase de récupération invalide : somme de contrôle BIP39 non correspondante.\n";
    fr.mnemonic_typo_check = "Vérifiez les fautes de frappe dans les mots. Chaque mot doit être exactement correct.\n";
    fr.too_many_failed_attempts = "Trop d'échecs. Veuillez patienter ";
    fr.label_chain = "Chaîne: ";
    fr.label_headers = "En-têtes: ";
    fr.label_version = "Version: ";
    fr.label_subversion = "Sous-version: ";
    fr.label_connections = "Connexions: ";
    fr.label_peers = "Pairs: ";
    fr.label_progress = "Progression: ";
    fr.label_size_on_disk = "Taille disque: ";
    fr.label_protocol = "Protocole: ";
    fr.label_connections_io = "Connexions (entr/sort): ";
    fr.label_network_active = "Réseau Actif: ";
    fr.label_connected_peers = "--- Pairs Connectés (%d) ---\n";
    fr.label_previous = "Précédent: ";
    fr.label_next = "Suivant: ";
    fr.label_merkle_root = "Racine Merkle: ";
    fr.label_nonce = "Nonce: ";
    fr.label_tx_list = "Transactions:\n";
    fr.label_network_hs = "Réseau H/s: ";
    fr.label_pooled_tx = "TX en pool: ";
    fr.label_current_height = "Hauteur Actuelle: ";
    fr.msg_connecting = "Connexion au nœud TokenCoin...";
    fr.err_cannot_connect = "  Impossible de se connecter au nœud TokenCoin\n";
    fr.msg_ensure_running = "Vérifiez que tkncd.exe est en cours d'exécution.\n\n";
    fr.msg_press_exit = "Appuyez sur Entrée pour quitter...\n";
    fr.msg_ok = " OK!\n\n";
    fr.err_cli_error = "  Erreur token CLI\n";
    fr.err_code_label = "Code d'erreur: ";
    fr.err_details_label = "Détails: ";
    fr.err_unknown = "Une erreur inconnue s'est produite.\n";
    fr.block_explore_options = "1. Par Hash\n2. Par Hauteur\n3. Dernier Bloc\n\n";
    fr.block_explore_select = "Sélectionner [1-3]: ";
    fr.select_wallet_fmt = "Sélectionner portefeuille [1-%d]: ";
    fr.prompt_locked_unlock = "Portefeuille verrouillé. Entrez la phrase de passe pour déverrouiller: ";
    fr.label_receiving = "[Réception] ";
    fr.label_change = "[Monnaie]     ";
    fr.label_primary = "(Principal)";
    fr.label_yes_short = "Oui";
    fr.label_no_short = "Non";
    I18N[6] = fr;

    I18nTexts de;
    de.select_language = "\n========================================\n  TokenCoin Wallet Manager v1.0\n  Sprache auswählen\n========================================\n";
    de.not_logged_in_title = "\n========================================\n  TokenCoin Wallet Manager [Öffentlich]\n========================================\n";
    de.public_menu_title = "\n========================================\n  TokenCoin Wallet Manager [Öffentlich]\n========================================\n";
    de.logged_in_title = "\n========================================\n  TokenCoin Wallet Manager\n========================================\n";
    de.wallet_menu_title = "\n========================================\n  TokenCoin Wallet Manager [Wallet: %s]\n========================================\n";
    de.menu_new_wallet = "1. Neue Wallet";
    de.menu_restore_wallet = "2. Wallet wiederherstellen";
    de.menu_open_wallet = "3. Wallet öffnen";
    de.menu_node_info = "4. Knoteninformationen";
    de.menu_blockchain_info = "5. Blockchain-Info";
    de.menu_network_info = "6. Netzwerk-Info";
    de.menu_block_info = "7. Block-Explorer";
    de.menu_mining_info = "8. Mining-Status";
    de.menu_exit = "0. Zurück zur Sprache";
    de.menu_back = "\nESC. Zurück";
    de.menu_quit = "e. Beenden";
    de.prompt_select = "> ";
    de.menu_wallet_address = "1. Wallet-Adresse";
    de.menu_send_tknc = "2. token senden";
    de.menu_transactions = "3. Transaktionsverlauf";
    de.menu_backup_keys = "4. Schlüssel sichern";
    de.menu_generate_address = "5. Neue Adresse generieren";
    de.menu_addr_label = "6. Adress-Label";
    de.menu_change_password = "7. Passwort ändern";
    de.menu_sign_message = "8. Nachricht signieren";
    de.menu_inference_service = "9. LLM";
    de.menu_logout = "10. Abmelden";
    de.create_wallet_title = "\n--- Neue Wallet erstellen ---\n";
    de.prompt_wallet_name = "Wallet-Name: ";
    de.prompt_password = "Passwort: ";
    de.prompt_password_confirm = "Passwort bestätigen: ";
    de.password_mismatch = "FEHLER: Passwörter stimmen nicht überein!";
    de.wallet_creating = "\nErstelle Wallet...";
    de.wallet_created_success = "\n✅ Wallet erfolgreich erstellt!\n";
    de.wallet_address_label = "Adresse: ";
    de.wallet_balance_label = "Guthaben: ";
    de.wallet_total_balance_label = "Gesamtguthaben: ";
    de.addr_balance_label = "Adressguthaben: ";
    de.select_address_prompt = "Nummer eingeben um Adresse zu wechseln: ";
    de.addr_switched_msg = "✅ Gewechselt zu: ";
    de.send_from_label = "Von: ";
    de.no_utxo_msg = "Keine verfügbaren UTXOs für diese Adresse.\n";
    de.restore_title = "\n--- Wallet wiederherstellen ---\n";
    de.restore_from_backup = "Aus Sicherungsdatei (.dat)";
    de.restore_from_descriptors = "Aus Schlüsseln";
    de.restore_prompt_file = "Auswahl [1-2]: ";
    de.restore_success = "✅ Wiederhergestellt!\n";
    de.restore_failed = "❌ Fehler: ";
    de.info_title = "\n--- Wallet-Informationen ---\n";
    de.info_wallet_name = "Wallet: ";
    de.info_address = "Adresse: ";
    de.info_balance = "Guthaben: ";
    de.info_no_wallet = "Keine Wallet geladen.\n";
    de.send_title = "\n--- token senden ---\n";
    de.send_to_address = "Empfängeradresse: ";
    de.send_amount = "Betrag (token): ";
    de.send_confirm = "Bestätigen? (j/n): ";
    de.send_success = "✅ Gesendet! TXID: ";
    de.send_failed = "❌ Fehler: ";
    de.send_insufficient = "❌ Unzureichendes Guthaben!\n";
    de.keys_title = "\n--- Schlüssel sichern ---\n";
    de.keys_unlock_prompt = "Passwort (60s): ";
    de.keys_descriptor_label = "Deskriptoren:\n";
    de.keys_warning = "⚠️ Sicher aufbewahren! Nicht teilen!\n";
    de.press_continue = "\nDrücken Sie Enter zum Fortfahren...";
    de.invalid_input = "Ungültige Eingabe.";
    de.back_to_menu = "\nZurück zum Menü...";
    de.yes_option = "j";
    de.password_too_short = "FEHLER: Passwort muss mindestens 8 Zeichen haben!";
    de.wallet_name_label = "Wallet-Name: ";
    de.import_warning = "Importwarnung: ";
    de.format_label = "Format: ";
    de.descriptor_type = "Deskriptor";
    de.legacy_type = "Legacy";
    de.available_wallets = "Verfügbare Wallets:\n";
    de.available_label = "Verfügbar: ";
    de.error_prefix = "Fehler: ";
    de.exit_message = "Danke, dass Sie token verwenden!\n";
    de.node_info_title = "\n--- Knoteninformationen ---\n";
    de.blockchain_title = "\n--- Blockchain-Informationen ---\n";
    de.network_title = "\n--- Netzwerk-Informationen ---\n";
    de.block_title = "\n--- Block-Explorer ---\n";
    de.block_height_label = "Blockhöhe: ";
    de.block_hash_label = "Block-Hash: ";
    de.block_time_label = "Zeit: ";
    de.block_tx_count_label = "Transaktionen: ";
    de.transactions_title = "\n--- Transaktionsverlauf ---\n";
    de.no_transactions = "Keine Transaktionen gefunden.\n";
    de.tx_id_label = "TXID: ";
    de.tx_amount_label = "Betrag: ";
    de.tx_confirmations_label = "Bestätigungen: ";
    de.tx_direction_label = "Typ";
    de.tx_address_label = "Adresse";
    de.tx_from_label = "Von";
    de.tx_to_label = "Nach";
    de.tx_time_label = "Zeit";
    de.tx_send_label = "Gesendet";
    de.tx_receive_label = "Erhalten";
    de.mining_title = "\n--- Mining-Status ---\n";
    de.mining_blocks_label = "Blöcke: ";
    de.mining_difficulty_label = "Schwierigkeit: ";
    de.mining_hashrate_label = "Hashrate: ";
    de.generate_addr_title = "\n--- Neue Adresse generieren ---\n";
    de.generate_addr_success = "Neue Adresse generiert: ";
    de.prompt_block_hash = "Block-Hash eingeben (oder 'latest'): ";
    de.prompt_block_height = "Blockhöhe eingeben (oder 'latest'): ";
    de.prompt_tx_count = "Anzahl der Transaktionen [10]: ";
    de.sign_title = "\n--- Nachricht signieren ---\n";
    de.sign_message_prompt = "Zu signierende Nachricht: ";
    de.sign_result_label = "Signatur: ";
    de.auto_login_msg = "Auto-Login in Wallet...";
    // CreateNewWallet backup prompt strings
    de.backup_important = "\n[WICHTIG] Sichern Sie Ihre Wallet JETZT!\n";
    de.backup_goto_keys = "Gehen zu: Wallet-Menü -> 5. Schlüssel sichern\n";
    de.backup_descriptor_info = "Dies speichert Ihren Deskriptor (xprv), der Ihre Wallet WIEDERHERSTELLEN kann.\n";
    de.backup_confirm_prompt = "'yes' eingeben, um Sicherung zu bestätigen: ";
    de.backup_reminder = "\nBitte sichern Sie Ihre Wallet über das Hauptmenü.\n";
    de.wallet_name_label2 = "Wallet-Name: ";
    de.wallet_create_failed = "Wallet-Erstellung fehlgeschlagen. Bitte Knotenstatus prüfen und erneut versuchen.\n";
    de.wallet_create_error = "Ein unerwarteter Fehler ist aufgetreten. Bitte erneut versuchen.\n";
    // RestoreWallet strings
    de.prompt_backup_path = "Sicherungsdateipfad: ";
    de.err_file_empty = "Dateipfad darf nicht leer sein.\n";
    de.err_file_not_found = "Datei nicht gefunden: ";
    de.err_file_invalid = "Ungültige Datei: ";
    de.prompt_new_wallet_name = "Neuer Wallet-Name: ";
    de.err_wallet_name_empty = "Wallet-Name darf nicht leer sein.\n";
    de.prompt_ext_descriptor = "\nEmpfangsschlüssel: ";
    de.prompt_int_descriptor = "Wechselgeld-Schlüssel: ";
    de.err_no_descriptors = ": Keine Deskriptoren\n";
    // RestoreWallet: mnemonic restore option
    de.restore_from_mnemonic = "Aus Mnemonik";
    de.prompt_enter_mnemonic = "\nIhre 12 Wiederherstellungswörter eingeben (einer pro Zeile, leere Zeile zum Beenden):\n";
    de.prompt_mnemonic_word = "  Wort %d: ";
    de.prompt_mnemonic_passphrase = "Passphrase (optional, Enter zum Überspringen): ";
    de.restore_mnemonic_success = "\n✅ Wallet aus Wiederherstellungsphrase wiederhergestellt!\n";
    de.restore_mnemonic_invalid = "Ungültiges Wort '%s' an Position %d. Nur BIP39-Englischwörter erlaubt.\n";
    de.restore_mnemonic_xprv_failed = "Schlüsselableitung fehlgeschlagen. Überprüfen Sie Ihre Wiederherstellungsphrase und Passphrase.\n";
    de.msg_no_wallet_create_first = "Bitte erstellen oder stellen Sie zuerst eine Wallet wieder her.\n";
    de.prompt_unlock_or_skip = "Passwort eingeben (oder Enter zum Überspringen): ";
    de.msg_using_wallet = "\nVerwendete Wallet: ";
    de.err_addr_empty = "Adresse darf nicht leer sein.\n";
    de.err_amount_nan = "Ungültiger Betrag: keine Zahl.\n";
    de.err_amount_positive = "Betrag muss größer als 0 sein.\n";
    de.msg_no_bech32_descriptor = "Keine Bech32 (token) Schlüssel in der Wallet gefunden.\n";
    // Real BIP39 mnemonic strings
    de.mnemonic_title = "\n=== Notieren Sie Ihre Wiederherstellungsphrase ===\n";
    de.mnemonic_warning = "Bewahren Sie diese Informationen sicher auf! Jeder mit diesen Wörtern kann auf Ihre Wallet zugreifen.\n";
    de.mnemonic_word_label = "  %2d. %s\n";
    de.mnemonic_verify_title = "\n=== Überprüfen Sie Ihre Wiederherstellungsphrase ===\n";
    de.mnemonic_verify_prompt = "Wort #%d eingeben: ";
    de.mnemonic_verify_success = "Überprüfung bestanden! Wallet ist bereit.\n";
    de.mnemonic_verify_failed = "Überprüfung fehlgeschlagen! Bitte überprüfen Sie Ihr Backup sorgfältig.\n";
    de.mnemonic_backup_confirm = "\nWICHTIG: Haben Sie alle 12 Wörter notiert?\n";
    de.mnemonic_backup_prompt = "'yes' eingeben zum Bestätigen: ";
    de.mnemonic_backup_confirmed = "Sicherung bestätigt! Ihre Wallet ist jetzt aktiv.\n";
    // P0 Sicherheitszeichenketten
    de.keys_warning_full_key = "WARNUNG: Anzeigen des vollständigen Privatschlüssels ermöglicht jedem, der Ihren Bildschirm sieht, ALLE Gelder zu stehlen.\n";
    de.keys_full_key_header = "\n=== VOLLSTÄNDIGE PRIVATSCHLÜSSEL (geheim halten!) ===\n";
    de.keys_hidden_msg = "(Schlüssel versteckt — verwenden Sie dieses Terminal nur an einem sicheren Ort)\n";
    de.sign_own_address_only = "Sie können nur Nachrichten mit Ihren eigenen Adressen signieren.\n";
    // Change password
    de.change_pass_title = "\n--- Passwort ändern ---\n";
    de.change_pass_old = "Aktuelles Passwort: ";
    de.change_pass_new = "Neues Passwort: ";
    de.change_pass_confirm = "Neues Passwort bestätigen: ";
    de.change_pass_success = "Passwort erfolgreich geändert!\n";
    de.change_pass_failed = "Passwortänderung fehlgeschlagen: ";
    de.change_pass_mismatch = "FEHLER: Neue Passwörter stimmen nicht überein!\n";
    de.change_pass_not_encrypted = "Diese Wallet ist nicht verschlüsselt. Passwort kann nicht geändert werden.\n";
    // Delete address
    de.addr_label_title = "\n--- Adress-Label ---\n";
    de.addr_label_prompt = "Adresse: ";
    de.addr_label_name_prompt = "Labelname (leer zum Löschen): ";
    de.addr_label_success = "Label gesetzt.\n";
    de.addr_label_failed = "Label setzen fehlgeschlagen: ";
    de.addr_label_removed = "Label gelöscht.\n";
    de.addr_label_not_found = "Adresse in Wallet nicht gefunden.\n";
    // Sign message i18n
    de.sign_addr_prompt = "Adresse: ";
    de.sign_wallet_locked = "Wallet ist gesperrt. Passwort eingeben: ";
    de.sign_must_unlock = "Wallet muss entsperrt sein, um Nachrichten zu signieren.\n";
    de.sign_addr_empty = "Adresse darf nicht leer sein.\n";
    de.sign_addr_too_long = "Adresse zu lang (max. 90 Zeichen).\n";
    de.sign_msg_too_long = "Nachricht zu lang (max. 4096 Zeichen).\n";
    de.sign_addr_not_owned = "Adresse gehört nicht zur aktuellen Wallet.\n";
    de.sign_verify_failed = "Überprüfung des Adressbesitzes fehlgeschlagen.\n";
    de.warn_no_passphrase = "WARNUNG: Kein Passwort eingegeben. Die Wallet wird OHNE Verschlüsselung erstellt.\n";
    de.warn_anyone_access = "Jeder mit Zugriff auf diesen Computer kann Ihre Geldmittel ausgeben.\n";
    de.confirm_unencrypted = "'yes' eingeben für unverschlüsselte Wallet, oder Enter für Passwort: ";
    de.cleaning_incomplete_wallet = "Unvollständige Wallet vom Knoten wird bereinigt...\n";
    de.wallet_functional_msg = "Die Wallet ist funktionsfähig. Versuchen Sie 'Neue Adresse generieren' im Wallet-Menü.\n";
    de.restore_access_denied = "Wallet wiederhergestellt, aber Zugang verweigert — falsches Passwort.\n";
    de.wallet_encrypted_must_unlock = "Diese Wallet ist verschlüsselt. Sie müssen das Passwort eingeben, um zuzugreifen.\n";
    de.import_descriptors_later = "Sie können fehlende Deskriptoren später über das Menü Sicherungsschlüssel importieren.\n";
    de.bip39_checksum_mismatch = "Ungültige Wiederherstellungsphrase: BIP39-Prüfsumme stimmt nicht überein.\n";
    de.mnemonic_typo_check = "Überprüfen Sie die Wörter auf Tippfehler. Jedes Wort muss genau richtig sein.\n";
    de.too_many_failed_attempts = "Zu viele Fehlversuche. Bitte warten Sie ";
    de.label_chain = "Kette: ";
    de.label_headers = "Header: ";
    de.label_version = "Version: ";
    de.label_subversion = "Subversion: ";
    de.label_connections = "Verbindungen: ";
    de.label_peers = "Peers: ";
    de.label_progress = "Fortschritt: ";
    de.label_size_on_disk = "Plattengröße: ";
    de.label_protocol = "Protokoll: ";
    de.label_connections_io = "Verbindungen (ein/aus): ";
    de.label_network_active = "Netzwerk Aktiv: ";
    de.label_connected_peers = "--- Verbundene Peers (%d) ---\n";
    de.label_previous = "Vorheriger: ";
    de.label_next = "Nächster: ";
    de.label_merkle_root = "Merkle-Wurzel: ";
    de.label_nonce = "Nonce: ";
    de.label_tx_list = "Transaktionen:\n";
    de.label_network_hs = "Netzwerk H/s: ";
    de.label_pooled_tx = "Pool TX: ";
    de.label_current_height = "Aktuelle Höhe: ";
    de.msg_connecting = "Verbindung zum TokenCoin-Knoten wird hergestellt...";
    de.err_cannot_connect = "  Keine Verbindung zum TokenCoin-Knoten möglich\n";
    de.msg_ensure_running = "Stellen Sie sicher, dass tkncd.exe läuft.\n\n";
    de.msg_press_exit = "Drücken Sie Enter zum Beenden...\n";
    de.msg_ok = " OK!\n\n";
    de.err_cli_error = "  token CLI Fehler\n";
    de.err_code_label = "Fehlercode: ";
    de.err_details_label = "Details: ";
    de.err_unknown = "Unbekannter Fehler aufgetreten.\n";
    de.block_explore_options = "1. Nach Hash\n2. Nach Höhe\n3. Letzter Block\n\n";
    de.block_explore_select = "Auswahl [1-3]: ";
    de.select_wallet_fmt = "Wallet auswählen [1-%d]: ";
    de.prompt_locked_unlock = "Wallet gesperrt. Passwort eingeben zum Entsperren: ";
    de.label_receiving = "[Empfang] ";
    de.label_change = "[Wechsel]     ";
    de.label_primary = "(Primär)";
    de.label_yes_short = "Ja";
    de.label_no_short = "Nein";
    I18N[7] = de;

    I18nTexts ru;
    ru.select_language = "\n========================================\n  Менеджер кошельков TokenCoin v1.0\n  Выберите язык\n========================================\n";
    ru.not_logged_in_title = "\n========================================\n  Менеджер кошельков TokenCoin [Публичный]\n========================================\n";
    ru.public_menu_title = "\n========================================\n  Менеджер кошельков TokenCoin [Публичный]\n========================================\n";
    ru.logged_in_title = "\n========================================\n  Менеджер кошельков TokenCoin\n========================================\n";
    ru.wallet_menu_title = "\n========================================\n  Менеджер кошельков TokenCoin [Кошелек: %s]\n========================================\n";
    ru.menu_new_wallet = "1. Создать кошелек";
    ru.menu_restore_wallet = "2. Восстановить кошелек";
    ru.menu_open_wallet = "3. Открыть кошелек";
    ru.menu_node_info = "4. Информация об узле";
    ru.menu_blockchain_info = "5. Инфо блокчейна";
    ru.menu_network_info = "6. Инфо сети";
    ru.menu_block_info = "7. Блок-эксплорер";
    ru.menu_mining_info = "8. Статус майнинга";
    ru.menu_exit = "0. Вернуться к языку";
    ru.menu_back = "\nESC. Назад";
    ru.menu_quit = "e. Выход";
    ru.prompt_select = "> ";
    ru.menu_wallet_address = "1. Адрес кошелька";
    ru.menu_send_tknc = "2. Отправить token";
    ru.menu_transactions = "3. История транзакций";
    ru.menu_backup_keys = "4. Резервное копирование ключей";
    ru.menu_generate_address = "5. Генерировать новый адрес";
    ru.menu_addr_label = "6. Метка адреса";
    ru.menu_change_password = "7. Сменить пароль";
    ru.menu_sign_message = "8. Подписать сообщение";
    ru.menu_inference_service = "9. LLM";
    ru.menu_logout = "10. Выйти";
    ru.create_wallet_title = "\n--- Создание нового кошелька ---\n";
    ru.prompt_wallet_name = "Имя кошелька: ";
    ru.prompt_password = "Пароль: ";
    ru.prompt_password_confirm = "Подтвердите пароль: ";
    ru.password_mismatch = "ОШИБКА: Пароли не совпадают!";
    ru.wallet_creating = "\nСоздание кошелька...";
    ru.wallet_created_success = "\n✅ Кошелек успешно создан!\n";
    ru.wallet_address_label = "Адрес: ";
    ru.wallet_balance_label = "Баланс: ";
    ru.wallet_total_balance_label = "Общий баланс: ";
    ru.addr_balance_label = "Баланс адреса: ";
    ru.select_address_prompt = "Введите номер для смены адреса: ";
    ru.addr_switched_msg = "✅ Переключено на: ";
    ru.send_from_label = "От: ";
    ru.no_utxo_msg = "Нет доступных UTXO для этого адреса.\n";
    ru.restore_title = "\n--- Восстановление кошелька ---\n";
    ru.restore_from_backup = "Из файла резервной копии (.dat)";
    ru.restore_from_descriptors = "Из ключей";
    ru.restore_prompt_file = "Выберите [1-2]: ";
    ru.restore_success = "✅ Восстановлено!\n";
    ru.restore_failed = "❌ Ошибка: ";
    ru.info_title = "\n--- Информация о кошельке ---\n";
    ru.info_wallet_name = "Кошелек: ";
    ru.info_address = "Адрес: ";
    ru.info_balance = "Баланс: ";
    ru.info_no_wallet = "Кошелек не загружен.\n";
    ru.send_title = "\n--- Отправка token ---\n";
    ru.send_to_address = "Адрес получателя: ";
    ru.send_amount = "Сумма (token): ";
    ru.send_confirm = "Подтвердить? (д/н): ";
    ru.send_success = "✅ Отправлено! TXID: ";
    ru.send_failed = "❌ Ошибка: ";
    ru.send_insufficient = "❌ Недостаточно средств!\n";
    ru.keys_title = "\n--- Резервное копирование ключей ---\n";
    ru.keys_unlock_prompt = "Пароль (60с): ";
    ru.keys_descriptor_label = "Дескрипторы:\n";
    ru.keys_warning = "⚠️ Храните в безопасности! Не делитесь!\n";
    ru.press_continue = "\nНажмите Enter для продолжения...";
    ru.invalid_input = "Недопустимый ввод.";
    ru.back_to_menu = "\nВозврат к меню...";
    ru.yes_option = "д";
    ru.password_too_short = "ОШИБКА: Пароль должен содержать минимум 8 символов!";
    ru.wallet_name_label = "Имя кошелька: ";
    ru.import_warning = "Предупреждение импорта: ";
    ru.format_label = "Формат: ";
    ru.descriptor_type = "Дескриптор";
    ru.legacy_type = "Legacy";
    ru.available_wallets = "Доступные кошельки:\n";
    ru.available_label = "Доступно: ";
    ru.error_prefix = "Ошибка: ";
    ru.exit_message = "Спасибо за использование token!\n";
    ru.node_info_title = "\n--- Информация об узле ---\n";
    ru.blockchain_title = "\n--- Информация о блокчейне ---\n";
    ru.network_title = "\n--- Информация о сети ---\n";
    ru.block_title = "\n--- Обозреватель блоков ---\n";
    ru.block_height_label = "Высота блока: ";
    ru.block_hash_label = "Хеш блока: ";
    ru.block_time_label = "Время: ";
    ru.block_tx_count_label = "Транзакции: ";
    ru.transactions_title = "\n--- История транзакций ---\n";
    ru.no_transactions = "Транзакций не найдено.\n";
    ru.tx_id_label = "TXID: ";
    ru.tx_amount_label = "Сумма: ";
    ru.tx_confirmations_label = "Подтверждения: ";
    ru.mining_title = "\n--- Статус майнинга ---\n";
    ru.mining_blocks_label = "Блоки: ";
    ru.mining_difficulty_label = "Сложность: ";
    ru.mining_hashrate_label = "Хешрейт: ";
    ru.generate_addr_title = "\n--- Генерировать новый адрес ---\n";
    ru.generate_addr_success = "Новый адрес сгенерирован: ";
    ru.prompt_block_hash = "Введите хеш блока (или 'latest'): ";
    ru.prompt_block_height = "Введите высоту блока (или 'latest'): ";
    ru.prompt_tx_count = "Количество транзакций [10]: ";
    ru.sign_title = "\n--- Подписать сообщение ---\n";
    ru.sign_message_prompt = "Сообщение для подписи: ";
    ru.sign_result_label = "Подпись: ";
    ru.auto_login_msg = "Автоматический вход в кошелек...";
    // CreateNewWallet backup prompt strings
    ru.backup_important = "\n[ВАЖНО] Создайте резервную копию кошелька СЕЙЧАС!\n";
    ru.backup_goto_keys = "Перейдите в: Меню кошелька -> 5. Резервное копирование ключей\n";
    ru.backup_descriptor_info = "Это сохраняет ваш дескриптор (xprv), который МОЖЕТ восстановить кошелёк.\n";
    ru.backup_confirm_prompt = "Введите 'yes' для подтверждения создания резервной копии: ";
    ru.backup_reminder = "\nПожалуйста, создайте резервную копию кошелька из главного меню.\n";
    ru.wallet_name_label2 = "Имя кошелька: ";
    ru.wallet_create_failed = "Ошибка создания кошелька. Проверьте статус узла и попробуйте снова.\n";
    ru.wallet_create_error = "Произошла непредвиденная ошибка. Пожалуйста, попробуйте снова.\n";
    // RestoreWallet strings
    ru.prompt_backup_path = "Путь к файлу резервной копии: ";
    ru.err_file_empty = "Путь к файлу не может быть пустым.\n";
    ru.err_file_not_found = "Файл не найден: ";
    ru.err_file_invalid = "Неверный файл: ";
    ru.prompt_new_wallet_name = "Новое имя кошелька: ";
    ru.err_wallet_name_empty = "Имя кошелька не может быть пустым.\n";
    ru.prompt_ext_descriptor = "\nКлюч получения: ";
    ru.prompt_int_descriptor = "Ключ сдачи: ";
    ru.err_no_descriptors = ": Нет дескрипторов\n";
    // RestoreWallet: mnemonic restore option
    ru.restore_from_mnemonic = "Из мнемоники";
    ru.prompt_enter_mnemonic = "\nВведите ваши 12 слов восстановления (по одному на строку, пустая строка для завершения):\n";
    ru.prompt_mnemonic_word = "  Слово %d: ";
    ru.prompt_mnemonic_passphrase = "Парольная фраза (необязательно, Enter для пропуска): ";
    ru.restore_mnemonic_success = "\n✅ Кошелёк восстановлен из фразы восстановления!\n";
    ru.restore_mnemonic_invalid = "Недопустимое слово '%s' в позиции %d. Только английские слова BIP39.\n";
    ru.restore_mnemonic_xprv_failed = "Ошибка вывода ключа. Проверьте фразу восстановления и парольную фразу.\n";
    ru.msg_no_wallet_create_first = "Пожалуйста, сначала создайте или восстановите кошелёк.\n";
    ru.prompt_unlock_or_skip = "Введите пароль (или Enter для пропуска): ";
    ru.msg_using_wallet = "\nИспользуемый кошелек: ";
    ru.err_addr_empty = "Адрес не может быть пустым.\n";
    ru.err_amount_nan = "Недопустимая сумма: не число.\n";
    ru.err_amount_positive = "Сумма должна быть больше 0.\n";
    ru.msg_no_bech32_descriptor = "В кошельке не найдено ключей Bech32 (token).\n";
    // Real BIP39 mnemonic strings
    ru.mnemonic_title = "\n=== Запишите Вашу фразу восстановления ===\n";
    ru.mnemonic_warning = "Храните эту информацию в безопасности! Любой, у кого есть эти слова, может получить доступ к кошельку.\n";
    ru.mnemonic_word_label = "  %2d. %s\n";
    ru.mnemonic_verify_title = "\n=== Подтвердите Вашу фразу восстановления ===\n";
    ru.mnemonic_verify_prompt = "Введите слово #%d: ";
    ru.mnemonic_verify_success = "Проверка пройдена! Кошелёк готов.\n";
    ru.mnemonic_verify_failed = "Проверка не пройдена! Пожалуйста, внимательно проверьте резервную копию.\n";
    ru.mnemonic_backup_confirm = "\nВАЖНО: Вы записали все 12 слов?\n";
    ru.mnemonic_backup_prompt = "Введите 'yes' для подтверждения: ";
    ru.mnemonic_backup_confirmed = "Резервная копия подтверждена! Ваш кошелёк теперь активен.\n";
    // P0 Строки безопасности
    ru.keys_warning_full_key = "ПРЕДУПРЕЖДЕНИЕ: Отображение полного закрытого ключа позволяет любому, кто видит ваш экран, украсть ВСЕ средства.\n";
    ru.keys_full_key_header = "\n=== ПОЛНЫЕ ЗАКРЫТЫЕ КЛЮЧИ (храните в секрете!) ===\n";
    ru.keys_hidden_msg = "(Ключи скрыты — используйте этот терминал только в безопасном месте)\n";
    ru.sign_own_address_only = "Вы можете подписывать сообщения только своими адресами.\n";
    // Change password
    ru.change_pass_title = "\n--- Сменить пароль ---\n";
    ru.change_pass_old = "Текущий пароль: ";
    ru.change_pass_new = "Новый пароль: ";
    ru.change_pass_confirm = "Подтвердите новый пароль: ";
    ru.change_pass_success = "Пароль успешно изменён!\n";
    ru.change_pass_failed = "Ошибка смены пароля: ";
    ru.change_pass_mismatch = "ОШИБКА: Новые пароли не совпадают!\n";
    ru.change_pass_not_encrypted = "Этот кошелёк не зашифрован. Невозможно сменить пароль.\n";
    // Delete address
    ru.addr_label_title = "\n--- Метка адреса ---\n";
    ru.addr_label_prompt = "Адрес: ";
    ru.addr_label_name_prompt = "Имя метки (пусто для удаления): ";
    ru.addr_label_success = "Метка установлена.\n";
    ru.addr_label_failed = "Ошибка установки метки: ";
    ru.addr_label_removed = "Метка удалена.\n";
    ru.addr_label_not_found = "Адрес не найден в кошельке.\n";
    // Sign message i18n
    ru.sign_addr_prompt = "Адрес: ";
    ru.sign_wallet_locked = "Кошелёк заблокирован. Введите пароль: ";
    ru.sign_must_unlock = "Кошелёк должен быть разблокирован для подписи сообщений.\n";
    ru.sign_addr_empty = "Адрес не может быть пустым.\n";
    ru.sign_addr_too_long = "Адрес слишком длинный (макс. 90 символов).\n";
    ru.sign_msg_too_long = "Сообщение слишком длинное (макс. 4096 символов).\n";
    ru.sign_addr_not_owned = "Адрес не принадлежит текущему кошельку.\n";
    ru.sign_verify_failed = "Не удалось проверить владение адресом.\n";
    ru.warn_no_passphrase = "ПРЕДУПРЕЖДЕНИЕ: Пароль не введён. Кошелёк будет создан БЕЗ шифрования.\n";
    ru.warn_anyone_access = "Любой с доступом к этому компьютеру может потратить ваши средства.\n";
    ru.confirm_unencrypted = "Введите 'yes' для подтверждения нешифрованного кошелька, или Enter для установки пароля: ";
    ru.cleaning_incomplete_wallet = "Очистка незавершённого кошелька с узла...\n";
    ru.wallet_functional_msg = "Кошелёк работает. Попробуйте 'Сгенерировать Новый Адрес' в меню кошелька.\n";
    ru.restore_access_denied = "Кошелёк восстановлен, но доступ запрещён — неверный пароль.\n";
    ru.wallet_encrypted_must_unlock = "Этот кошелёк зашифрован. Введите пароль для доступа.\n";
    ru.import_descriptors_later = "Вы можете импортировать отсутствующие дескрипторы позже через меню Резервных Ключей.\n";
    ru.bip39_checksum_mismatch = "Недопустимая фраза восстановления: контрольная сумма BIP39 не совпадает.\n";
    ru.mnemonic_typo_check = "Проверьте слова на опечатки. Каждое слово должно быть абсолютно точным.\n";
    ru.too_many_failed_attempts = "Слишком много неудачных попыток. Пожалуйста, подождите ";
    ru.label_chain = "Цепь: ";
    ru.label_headers = "Заголовки: ";
    ru.label_version = "Версия: ";
    ru.label_subversion = "Подверсия: ";
    ru.label_connections = "Соединения: ";
    ru.label_peers = "Пиры: ";
    ru.label_progress = "Прогресс: ";
    ru.label_size_on_disk = "Размер на диске: ";
    ru.label_protocol = "Протокол: ";
    ru.label_connections_io = "Соединения (вх/исх): ";
    ru.label_network_active = "Сеть активна: ";
    ru.label_connected_peers = "--- Подключённые пиры (%d) ---\n";
    ru.label_previous = "Предыдущий: ";
    ru.label_next = "Следующий: ";
    ru.label_merkle_root = "Корень Меркла: ";
    ru.label_nonce = "Nonce: ";
    ru.label_tx_list = "Транзакции:\n";
    ru.label_network_hs = "Сеть H/s: ";
    ru.label_pooled_tx = "TX в пуле: ";
    ru.label_current_height = "Текущая высота: ";
    ru.msg_connecting = "Подключение к узлу TokenCoin...";
    ru.err_cannot_connect = "  Не удаётся подключиться к узлу TokenCoin\n";
    ru.msg_ensure_running = "Убедитесь, что tkncd.exe запущен.\n\n";
    ru.msg_press_exit = "Нажмите Enter для выхода...\n";
    ru.msg_ok = " OK!\n\n";
    ru.err_cli_error = "  Ошибка token CLI\n";
    ru.err_code_label = "Код ошибки: ";
    ru.err_details_label = "Подробности: ";
    ru.err_unknown = "Произошла неизвестная ошибка.\n";
    ru.block_explore_options = "1. По хешу\n2. По высоте\n3. Последний блок\n\n";
    ru.block_explore_select = "Выбор [1-3]: ";
    ru.select_wallet_fmt = "Выбрать кошелёк [1-%d]: ";
    ru.prompt_locked_unlock = "Кошелёк заблокирован. Введите пароль для разблокировки: ";
    ru.label_receiving = "[Получение] ";
    ru.label_change = "[Сдача]     ";
    ru.label_public = "[Открытый] ";
    ru.label_private = "[Закрытый] ";
    ru.label_primary = "(Основной)";
    ru.label_yes_short = "Да";
    ru.label_no_short = "Нет";
    I18N[8] = ru;

    I18nTexts pt;
    pt.select_language = "\n========================================\n  Gerenciador de Carteiras TokenCoin v1.0\n  Selecionar Idioma\n========================================\n";
    pt.not_logged_in_title = "\n========================================\n  Gerenciador de Carteiras TokenCoin [Público]\n========================================\n";
    pt.public_menu_title = "\n========================================\n  Gerenciador de Carteiras TokenCoin [Público]\n========================================\n";
    pt.logged_in_title = "\n========================================\n  Gerenciador de Carteiras TokenCoin\n========================================\n";
    pt.wallet_menu_title = "\n========================================\n  Gerenciador de Carteiras TokenCoin [Carteira: %s]\n========================================\n";
    pt.menu_new_wallet = "1. Criar Nova Carteira";
    pt.menu_restore_wallet = "2. Restaurar Carteira";
    pt.menu_open_wallet = "3. Abrir Carteira";
    pt.menu_node_info = "4. Informações do Nó";
    pt.menu_blockchain_info = "5. Info Blockchain";
    pt.menu_network_info = "6. Info Rede";
    pt.menu_block_info = "7. Explorador Blocos";
    pt.menu_mining_info = "8. Status de Mineração";
    pt.menu_exit = "0. Voltar ao Idioma";
    pt.menu_back = "\nESC. Voltar";
    pt.menu_quit = "e. Sair";
    pt.prompt_select = "> ";
    pt.menu_wallet_address = "1. Endereço da Carteira";
    pt.menu_send_tknc = "2. Enviar token";
    pt.menu_transactions = "3. Histórico de Transações";
    pt.menu_backup_keys = "4. Backup das Chaves";
    pt.menu_generate_address = "5. Gerar Novo Endereço";
    pt.menu_addr_label = "6. Rótulo de Endereço";
    pt.menu_change_password = "7. Alterar Senha";
    pt.menu_sign_message = "8. Assinar Mensagem";
    pt.menu_inference_service = "9. LLM";
    pt.menu_logout = "10. Sair";
    pt.create_wallet_title = "\n--- Criar Nova Carteira ---\n";
    pt.prompt_wallet_name = "Nome da carteira: ";
    pt.prompt_password = "Senha: ";
    pt.prompt_password_confirm = "Confirmar senha: ";
    pt.password_mismatch = "ERRO: As senhas não coincidem!";
    pt.wallet_creating = "\nCriando carteira...";
    pt.wallet_created_success = "\n✅ Carteira criada com sucesso!\n";
    pt.wallet_address_label = "Endereço: ";
    pt.wallet_balance_label = "Saldo: ";
    pt.wallet_total_balance_label = "Saldo Total: ";
    pt.addr_balance_label = "Saldo do Endereço: ";
    pt.select_address_prompt = "Digite o número para mudar endereço: ";
    pt.addr_switched_msg = "✅ Mudado para: ";
    pt.send_from_label = "De: ";
    pt.no_utxo_msg = "Sem UTXOs disponíveis para este endereço.\n";
    pt.restore_title = "\n--- Restaurar Carteira ---\n";
    pt.restore_from_backup = "De arquivo de backup (.dat)";
    pt.restore_from_descriptors = "Das chaves";
    pt.restore_prompt_file = "Selecionar [1-2]: ";
    pt.restore_success = "✅ Restaurada com sucesso!\n";
    pt.restore_failed = "❌ Falha: ";
    pt.info_title = "\n--- Informações da Carteira ---\n";
    pt.info_wallet_name = "Carteira: ";
    pt.info_address = "Endereço: ";
    pt.info_balance = "Saldo: ";
    pt.info_no_wallet = "Nenhuma carteira carregada.\n";
    pt.send_title = "\n--- Enviar token ---\n";
    pt.send_to_address = "Endereço do destinatário: ";
    pt.send_amount = "Quantidade (token): ";
    pt.send_confirm = "Confirmar? (s/n): ";
    pt.send_success = "✅ Enviado! TXID: ";
    pt.send_failed = "❌ Falha: ";
    pt.send_insufficient = "❌ Saldo insuficiente!\n";
    pt.keys_title = "\n--- Backup das Chaves ---\n";
    pt.keys_unlock_prompt = "Senha (60s): ";
    pt.keys_descriptor_label = "Descritores:\n";
    pt.keys_warning = "⚠️ Mantenha seguro! Não compartilhe!\n";
    pt.press_continue = "\nPressione Enter para continuar...";
    pt.invalid_input = "Entrada inválida.";
    pt.back_to_menu = "\nVoltando ao menu...";
    pt.yes_option = "s";
    pt.password_too_short = "ERRO: A senha deve ter pelo menos 8 caracteres!";
    pt.wallet_name_label = "Nome da carteira: ";
    pt.import_warning = "Aviso de importação: ";
    pt.format_label = "Formato: ";
    pt.descriptor_type = "Descritor";
    pt.legacy_type = "Legacy";
    pt.available_wallets = "Carteiras disponíveis:\n";
    pt.available_label = "Disponível: ";
    pt.error_prefix = "Erro: ";
    pt.exit_message = "Obrigado por usar token!\n";
    pt.node_info_title = "\n--- Informações do Nó ---\n";
    pt.blockchain_title = "\n--- Informações Blockchain ---\n";
    pt.network_title = "\n--- Informações de Rede ---\n";
    pt.block_title = "\n--- Explorador de Blocos ---\n";
    pt.block_height_label = "Altura do bloco: ";
    pt.block_hash_label = "Hash do bloco: ";
    pt.block_time_label = "Hora: ";
    pt.block_tx_count_label = "Transações: ";
    pt.transactions_title = "\n--- Histórico de Transações ---\n";
    pt.no_transactions = "Nenhuma transação encontrada.\n";
    pt.tx_id_label = "TXID: ";
    pt.tx_amount_label = "Quantidade: ";
    pt.tx_confirmations_label = "Confirmações: ";
    pt.tx_direction_label = "Tipo";
    pt.tx_address_label = "Endereco";
    pt.tx_time_label = "Hora";
    pt.tx_send_label = "Envio";
    pt.tx_receive_label = "Recebimento";
    pt.tx_from_label = "De";
    pt.tx_to_label = "Para";
    pt.mining_title = "\n--- Status de Mineração ---\n";
    pt.mining_blocks_label = "Blocos: ";
    pt.mining_difficulty_label = "Dificuldade: ";
    pt.mining_hashrate_label = "Hashrate: ";
    pt.generate_addr_title = "\n--- Gerar Novo Endereço ---\n";
    pt.generate_addr_success = "Novo endereço gerado: ";
    pt.prompt_block_hash = "Digite o hash do bloco (ou 'latest'): ";
    pt.prompt_block_height = "Digite a altura do bloco (ou 'latest'): ";
    pt.prompt_tx_count = "Número de transações [10]: ";
    pt.sign_title = "\n--- Assinar Mensagem ---\n";
    pt.sign_message_prompt = "Mensagem para assinar: ";
    pt.sign_result_label = "Assinatura: ";
    pt.auto_login_msg = "Auto-login na carteira...";
    // CreateNewWallet backup prompt strings
    pt.backup_important = "\n[IMPORTANTE] Faça backup da sua carteira AGORA!\n";
    pt.backup_goto_keys = "Ir para: Menu Carteira -> 5. Backup das Chaves\n";
    pt.backup_descriptor_info = "Isso salva seu descritor (xprv) que PODE restaurar sua carteira.\n";
    pt.backup_confirm_prompt = "Digite 'yes' para confirmar que fará o backup: ";
    pt.backup_reminder = "\nPor favor faça backup da sua carteira do menu principal.\n";
    pt.wallet_name_label2 = "Nome da carteira: ";
    pt.wallet_create_failed = "Falha ao criar carteira. Verifique o status do nó e tente novamente.\n";
    pt.wallet_create_error = "Ocorreu um erro inesperado. Por favor tente novamente.\n";
    // RestoreWallet strings
    pt.prompt_backup_path = "Caminho do arquivo de backup: ";
    pt.err_file_empty = "O caminho do arquivo não pode estar vazio.\n";
    pt.err_file_not_found = "Arquivo não encontrado: ";
    pt.err_file_invalid = "Arquivo inválido: ";
    pt.prompt_new_wallet_name = "Nome da nova carteira: ";
    pt.err_wallet_name_empty = "O nome da carteira não pode estar vazio.\n";
    pt.prompt_ext_descriptor = "\nChave de recebimento: ";
    pt.prompt_int_descriptor = "Chave de troco: ";
    pt.err_no_descriptors = ": Sem descritores\n";
    // RestoreWallet: mnemonic restore option
    pt.restore_from_mnemonic = "Do mnemônico";
    pt.prompt_enter_mnemonic = "\nDigite suas 12 palavras de recuperação (uma por linha, linha vazia para terminar):\n";
    pt.prompt_mnemonic_word = "  Palavra %d: ";
    pt.prompt_mnemonic_passphrase = "Frase-senha (opcional, Enter para pular): ";
    pt.restore_mnemonic_success = "\n✅ Carteira restaurada da frase de recuperação!\n";
    pt.restore_mnemonic_invalid = "Palavra inválida '%s' na posição %d. Apenas palavras BIP39 em inglês.\n";
    pt.restore_mnemonic_xprv_failed = "Derivação de chave falhou. Verifique sua frase de recuperação e frase-senha.\n";
    pt.msg_no_wallet_create_first = "Por favor crie ou restaure uma carteira primeiro.\n";
    pt.prompt_unlock_or_skip = "Digite a senha (ou Enter para pular): ";
    pt.msg_using_wallet = "\nUsando carteira: ";
    pt.err_addr_empty = "O endereço não pode estar vazio.\n";
    pt.err_amount_nan = "Quantidade inválida: não é um número.\n";
    pt.err_amount_positive = "A quantidade deve ser maior que 0.\n";
    pt.msg_no_bech32_descriptor = "Nenhuma chave Bech32 (token) encontrada na carteira.\n";
    // Real BIP39 mnemonic strings
    pt.mnemonic_title = "\n=== Anote Sua Frase de Recuperação ===\n";
    pt.mnemonic_warning = "Mantenha esta informação segura! Qualquer pessoa com estas palavras pode acessar sua carteira.\n";
    pt.mnemonic_word_label = "  %2d. %s\n";
    pt.mnemonic_verify_title = "\n=== Verifique Sua Frase de Recuperação ===\n";
    pt.mnemonic_verify_prompt = "Digite a palavra #%d: ";
    pt.mnemonic_verify_success = "Verificação aprovada! A carteira está pronta.\n";
    pt.mnemonic_verify_failed = "Verificação falhou! Por favor verifique seu backup com atenção.\n";
    pt.mnemonic_backup_confirm = "\nIMPORTANTE: Você anotou as 12 palavras?\n";
    pt.mnemonic_backup_prompt = "Digite 'yes' para confirmar: ";
    pt.mnemonic_backup_confirmed = "Backup confirmado! Sua carteira agora está ativa.\n";
    // Strings de segurança P0
    pt.keys_warning_full_key = "AVISO: Exibir a chave privada completa permite que qualquer pessoa que veja sua tela roube TODOS os fundos.\n";
    pt.keys_full_key_header = "\n=== CHAVES PRIVADAS COMPLETAS (mantenha segredo!) ===\n";
    pt.keys_hidden_msg = "(Chaves ocultas — use este terminal apenas em um local seguro)\n";
    pt.sign_own_address_only = "Você só pode assinar mensagens com seus próprios endereços.\n";
    // Change password
    pt.change_pass_title = "\n--- Alterar Senha ---\n";
    pt.change_pass_old = "Senha atual: ";
    pt.change_pass_new = "Nova senha: ";
    pt.change_pass_confirm = "Confirmar nova senha: ";
    pt.change_pass_success = "Senha alterada com sucesso!\n";
    pt.change_pass_failed = "Falha ao alterar senha: ";
    pt.change_pass_mismatch = "ERRO: As novas senhas não coincidem!\n";
    pt.change_pass_not_encrypted = "Esta carteira não está criptografada. Não é possível alterar a senha.\n";
    // Delete address
    pt.addr_label_title = "\n--- Rótulo de Endereço ---\n";
    pt.addr_label_prompt = "Endereço: ";
    pt.addr_label_name_prompt = "Nome do rótulo (vazio para remover): ";
    pt.addr_label_success = "Rótulo definido.\n";
    pt.addr_label_failed = "Falha ao definir rótulo: ";
    pt.addr_label_removed = "Rótulo removido.\n";
    pt.addr_label_not_found = "Endereço não encontrado na carteira.\n";
    // Sign message i18n
    pt.sign_addr_prompt = "Endereço: ";
    pt.sign_wallet_locked = "Carteira bloqueada. Digite a senha para desbloquear: ";
    pt.sign_must_unlock = "A carteira deve estar desbloqueada para assinar mensagens.\n";
    pt.sign_addr_empty = "O endereço não pode estar vazio.\n";
    pt.sign_addr_too_long = "Endereço muito longo (máx. 90 caracteres).\n";
    pt.sign_msg_too_long = "Mensagem muito longa (máx. 4096 caracteres).\n";
    pt.sign_addr_not_owned = "O endereço não pertence à carteira atual.\n";
    pt.sign_verify_failed = "Falha ao verificar a propriedade do endereço.\n";
    pt.warn_no_passphrase = "AVISO: Nenhuma senha inserida. A carteira será criada SEM criptografia.\n";
    pt.warn_anyone_access = "Qualquer pessoa com acesso a este computador pode gastar seus fundos.\n";
    pt.confirm_unencrypted = "Digite 'yes' para confirmar uma carteira sem criptografia, ou Enter para definir senha: ";
    pt.cleaning_incomplete_wallet = "Limpando carteira incompleta do nó...\n";
    pt.wallet_functional_msg = "A carteira está funcional. Tente 'Gerar Novo Endereço' no menu da carteira.\n";
    pt.restore_access_denied = "Carteira restaurada mas acesso negado — senha incorreta.\n";
    pt.wallet_encrypted_must_unlock = "Esta carteira está criptografada. Você deve digitar a senha para acessar.\n";
    pt.import_descriptors_later = "Você pode importar descritores faltantes mais tarde pelo menu Chaves de Backup.\n";
    pt.bip39_checksum_mismatch = "Frase de recuperação inválida: checksum BIP39 não corresponde.\n";
    pt.mnemonic_typo_check = "Verifique se há erros de digitação nas palavras. Cada palavra deve estar exatamente correta.\n";
    pt.too_many_failed_attempts = "Muitas tentativas falhadas. Aguarde ";
    pt.label_chain = "Cadeia: ";
    pt.label_headers = "Cabeçalhos: ";
    pt.label_version = "Versão: ";
    pt.label_subversion = "Subversão: ";
    pt.label_connections = "Conexões: ";
    pt.label_peers = "Pares: ";
    pt.label_progress = "Progresso: ";
    pt.label_size_on_disk = "Tamanho em disco: ";
    pt.label_protocol = "Protocolo: ";
    pt.label_connections_io = "Conexões (ent/sai): ";
    pt.label_network_active = "Rede Ativa: ";
    pt.label_connected_peers = "--- Pares Conectados (%d) ---\n";
    pt.label_previous = "Anterior: ";
    pt.label_next = "Próximo: ";
    pt.label_merkle_root = "Raiz Merkle: ";
    pt.label_nonce = "Nonce: ";
    pt.label_tx_list = "Transações:\n";
    pt.label_network_hs = "Rede H/s: ";
    pt.label_pooled_tx = "TX no Pool: ";
    pt.label_current_height = "Altura Atual: ";
    pt.msg_connecting = "Conectando ao nó TokenCoin...";
    pt.err_cannot_connect = "  Não é possível conectar ao nó TokenCoin\n";
    pt.msg_ensure_running = "Certifique-se de que tkncd.exe está em execução.\n\n";
    pt.msg_press_exit = "Pressione Enter para sair...\n";
    pt.msg_ok = " OK!\n\n";
    pt.err_cli_error = "  Erro de token CLI\n";
    pt.err_code_label = "Código de erro: ";
    pt.err_details_label = "Detalhes: ";
    pt.err_unknown = "Ocorreu um erro desconhecido.\n";
    pt.block_explore_options = "1. Por Hash\n2. Por Altura\n3. Último Bloco\n\n";
    pt.block_explore_select = "Selecionar [1-3]: ";
    pt.select_wallet_fmt = "Selecionar carteira [1-%d]: ";
    pt.prompt_locked_unlock = "Carteira bloqueada. Digite a senha para desbloquear: ";
    pt.label_receiving = "[Recebimento] ";
    pt.label_change = "[Troco]       ";
    pt.label_public = "[Público]  ";
    pt.label_private = "[Privado]  ";
    pt.label_primary = "(Principal)";
    pt.label_yes_short = "Sim";
    pt.label_no_short = "Não";
    I18N[9] = pt;
}

static int current_lang = 1;

static const I18nTexts& T() {
    auto it = I18N.find(current_lang);
    return (it != I18N.end()) ? it->second : I18N.at(1);
}

// Inline bilingual helper: returns zh string when Chinese is selected, en otherwise.
// Use for one-off strings that don't warrant a full I18nTexts field.
static const char* L(const char* en, const char* zh) {
    return (current_lang == 2) ? zh : en;
}

#ifndef WIN32
static std::string GetHiddenInput(const std::string& prompt) {
    std::string input;
    termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    std::cout << prompt;
    std::getline(std::cin, input);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::cout << std::endl;
    return input;
}
#else
static bool IsRealConsole() {
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    if (hStdin == INVALID_HANDLE_VALUE || hStdin == NULL) return false;
    
    DWORD fileType = GetFileType(hStdin);
    if (fileType != FILE_TYPE_CHAR) return false;
    
    DWORD mode = 0;
    if (!GetConsoleMode(hStdin, &mode)) return false;
    
    HWND consoleWnd = GetConsoleWindow();
    if (consoleWnd == NULL) return false;
    
    return true;
}

static std::string GetHiddenInput(const std::string& prompt) {
    std::string input;
    
    if (!IsRealConsole()) {
        // Security fix (#51): Warn user that input will be visible (no echo suppression)
        std::cerr << "[WARNING] No console detected — password input will be VISIBLE on screen.\n";
        std::cout << prompt;
        std::getline(std::cin, input);
        return input;
    }
    
    // Use _getch() for hidden input with ESC key detection.
    // _getch() does not echo characters and can detect ESC (0x1B) immediately.
    std::cout << prompt;
    while (true) {
        int ch = _getch();
        if (ch == 0x1B) { std::cout << "\n"; return "\x1B"; }  // ESC = cancel
        if (ch == '\r') { std::cout << "\n"; return input; }    // Enter = done
        if (ch == 0x08) { if (!input.empty()) input.pop_back(); continue; }  // Backspace
        if (ch == 0 || ch == 0xE0) { _getch(); continue; }      // Special keys (arrows etc)
        if (ch >= 0x20) input += (char)ch;                       // Printable chars
    }
}
#endif

static std::string GetInput(const std::string& prompt) {
    std::cout << prompt;
    std::string input;
    std::getline(std::cin, input);
    return input;
}

// GetInputEsc: reads input with ESC key detection.
// Returns "\x1B" if ESC is pressed, otherwise the entered string.
// On Windows, uses _getch() for real-time ESC detection (no Enter needed).
// On Linux, checks if input starts with ESC character.
static std::string GetInputEsc(const std::string& prompt) {
    std::cout << prompt;
    std::string input;
#ifdef _WIN32
    while (true) {
        int ch = _getch();
        if (ch == 0x1B) { std::cout << "\n"; return "\x1B"; }
        if (ch == '\r') { std::cout << "\n"; return input; }
        if (ch == '\n') { return input; }
        if (ch == 0x08) { if (!input.empty()) { input.pop_back(); std::cout << "\b \b"; } continue; }
        if (ch == 0 || ch == 0xE0) { _getch(); continue; }
        if (ch >= 0x20) { input += (char)ch; std::cout << (char)ch; }
    }
#else
    std::getline(std::cin, input);
    if (!input.empty() && (unsigned char)input[0] == 0x1B) return "\x1B";
    return input;
#endif
}

/**
 * Security fix (SEC-07): Secure memory clearing for sensitive data.
 * BTC Core uses secure_allocator + memory_cleanse() to prevent sensitive data
 * (passwords, private keys, seeds) from remaining in memory after use.
 * std::string with default allocator does NOT zero memory on destruction,
 * leaving passwords and keys in heap memory where they could be read by
 * memory scanning attacks or core dumps.
 *
 * This function uses volatile pointer + memset to prevent compiler optimization
 * from eliding the clear (compiler may optimize away memset of dead data).
 * On Windows, also uses SecureZeroMemory which guarantees no optimization.
 */
static void SecureClear(std::string& s) {
    if (s.empty()) return;
#ifdef WIN32
    SecureZeroMemory(&s[0], s.size());
#else
    // Volatile pointer prevents compiler from optimizing away the memset
    volatile char* p = &s[0];
    for (size_t i = 0; i < s.size(); i++) p[i] = 0;
#endif
    s.clear();
}

static void SecureClear(std::vector<uint8_t>& v) {
    if (v.empty()) return;
#ifdef WIN32
    SecureZeroMemory(v.data(), v.size());
#else
    volatile uint8_t* p = v.data();
    for (size_t i = 0; i < v.size(); i++) p[i] = 0;
#endif
    v.clear();
}

static void SecureClear(std::vector<std::string>& v) {
    for (auto& s : v) SecureClear(s);
    v.clear();
}

static void ClearScreen() {
#ifdef WIN32
    // Security fix (#R12-5): Avoid system() call — prevents PATH-based command injection.
    // system("cls") spawns a shell, which is vulnerable if PATH is compromised.
    // Use Windows API instead — direct console manipulation, no shell involved.
    HANDLE hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hStdOut != INVALID_HANDLE_VALUE && hStdOut != NULL) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hStdOut, &csbi)) {
            DWORD cellCount = csbi.dwSize.X * csbi.dwSize.Y;
            COORD homeCoords = {0, 0};
            DWORD count;
            FillConsoleOutputCharacter(hStdOut, ' ', cellCount, homeCoords, &count);
            FillConsoleOutputAttribute(hStdOut, csbi.wAttributes, cellCount, homeCoords, &count);
            SetConsoleCursorPosition(hStdOut, homeCoords);
        }
    }
#else
    // Use ANSI escape sequence instead of system("clear")
    std::cout << "\033[2J\033[H" << std::flush;
#endif
}

static void PressContinue() {
    std::cout << T().press_continue;
    // Only ignore if there's data in the buffer to avoid blocking on empty input
    if (std::cin.rdbuf()->in_avail() > 0) {
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    } else {
        std::string dummy;
        std::getline(std::cin, dummy);
    }
}

// ==================== Inference Service Functions ====================

// Wallet state for inference service (server-side wallet_balance is unreliable).
static std::string g_current_wallet_name;
static std::string g_current_address;
static bool g_is_logged_in = false;

// Forward declaration — defined later in the file but needed by InferenceEnterKey.
static UniValue CallRPCSimple(const std::string& method, const std::vector<std::string>& args, const std::string& wallet);
static std::string TranslateRpcError(const std::string& errMsg);

static void InferenceEnterKey() {
    ClearScreen();
    std::cout << T().inference_title;
    std::cout << T().inference_enter_key_prompt;

    std::string api_key;
    std::getline(std::cin, api_key);

    if (api_key.empty()) {
        std::cout << T().inference_invalid_key;
        PressContinue();
        return;
    }

// Call WEB server to get API Key info (WEB is the directory service).
    std::string body = R"({"api_key":")" + api_key + R"("})";
    std::string response;
    try {
        response = CallHTTP(g_web_host, g_web_port, "/api/v1/chat", body);
    } catch (const std::exception& e) {
        std::cout << T().inference_test_failed << e.what() << "\n";
        PressContinue();
        return;
    }

    UniValue result;
    if (!result.read(response)) {
        std::cout << T().inference_test_failed << "Failed to parse discovery response\n";
        PressContinue();
        return;
    }

    if (result.exists("error")) {
        std::string err = result["error"].getValStr();
        std::cout << T().inference_test_failed << err << "\n";
        PressContinue();
        return;
    }

    // Parse real API Key info from discovery response
    APIKeyInfo info;
    info.api_key = api_key;
    info.miner_wallet = result.exists("miner_wallet") ? result["miner_wallet"].getValStr() : "";
    info.model_name = result.exists("model") ? result["model"].getValStr() : "unknown";
    info.endpoint = result.exists("endpoint") ? result["endpoint"].getValStr() : "";
try {
info.tokens_per_tknc = result.exists("tokens_per_tknc") ? result["tokens_per_tknc"].getInt<int64_t>() : 0;
} catch (...) { info.tokens_per_tknc = 0; }

    if (result.exists("balance_info")) {
        UniValue balInfo = result["balance_info"];
        try {
            info.used = balInfo.exists("consumed") ? balInfo["consumed"].getInt<int64_t>() : 0;
            info.remaining = balInfo.exists("remaining") ? balInfo["remaining"].getInt<int64_t>() : 0;
        } catch (...) { info.used = 0; info.remaining = 0; }
    } else {
        info.used = 0; info.remaining = 0;
    }

    // Query LOCAL wallet balance (the user's wallet that pays for inference).
    // Server-side wallet_balance is unreliable because user wallets are not loaded
    // on the server's tkncd — the CLI's local node has the real balance.
    if (!g_current_wallet_name.empty()) {
        try {
            UniValue balResult = CallRPCSimple("getbalance", {}, g_current_wallet_name);
            if (balResult.find_value("error").isNull()) {
                UniValue balVal = balResult.find_value("result");
                if (balVal.isNum()) {
                    info.balance = balVal.get_real();
                }
            }
        } catch (...) {}
    }

    g_stored_api_keys.push_back(info);
    g_last_api_key = api_key;
    g_last_endpoint = info.endpoint;

    // Display API Key info
    std::cout << T().inference_key_info_title;
    std::cout << T().inference_key_label << api_key << "\n";
    std::cout << T().inference_miner_label << info.miner_wallet << "\n";
    std::cout << T().inference_model_label << info.model_name << "\n";
    std::cout << T().inference_endpoint_label << info.endpoint << "\n";
    std::cout << T().inference_balance_label << std::fixed << std::setprecision(8) << info.balance << "\n";
    if (info.tokens_per_tknc > 0) {
        std::cout << T().inference_rate_label << "1 TKNC = " << info.tokens_per_tknc << " tokens\n";
    }

    // ===== Auto-configure proxy =====
    // The endpoint already contains miner IP and port — no need to re-enter them.
    // Parse: http://[IPv6]:port/path  or  http://IPv4:port/path
    if (!info.endpoint.empty()) {
        std::string host_port = info.endpoint;
        // Strip protocol prefix
        size_t proto_end = host_port.find("://");
        if (proto_end != std::string::npos)
            host_port = host_port.substr(proto_end + 3);
        // Strip path
        size_t path_start = host_port.find('/');
        if (path_start != std::string::npos)
            host_port = host_port.substr(0, path_start);

        std::string miner_ip;
        int miner_port = 9313;

        if (!host_port.empty() && host_port.front() == '[') {
            // IPv6: [addr]:port
            size_t bracket_end = host_port.find(']');
            if (bracket_end != std::string::npos) {
                miner_ip = host_port.substr(1, bracket_end - 1);
                size_t colon = host_port.find(':', bracket_end);
                if (colon != std::string::npos) {
                    try { miner_port = std::stoi(host_port.substr(colon + 1)); } catch (...) {}
                }
            }
        } else {
            // IPv4 or hostname: addr:port
            size_t colon = host_port.rfind(':');
            if (colon != std::string::npos) {
                miner_ip = host_port.substr(0, colon);
                try { miner_port = std::stoi(host_port.substr(colon + 1)); } catch (...) {}
            } else {
                miner_ip = host_port;
            }
        }

        if (!miner_ip.empty()) {
            std::cout << "\n========================================\n";
            std::cout << L("  Auto-configure Proxy?\n", "  自动配置代理?\n");
            std::cout << "========================================\n";
            std::cout << "Miner IP:  " << miner_ip << "\n";
            std::cout << "Port:      " << miner_port << "\n";
            std::cout << "Model:     " << info.model_name << "\n";
            std::cout << "API Key:   " << api_key.substr(0, 20) << "...\n\n";
            std::cout << L("Configure proxy now? (y/n): ", "现在配置代理? (y/n): ");

            std::string auto_choice;
            std::getline(std::cin, auto_choice);
            if (auto_choice == "y" || auto_choice == "Y") {
                std::cout << L("\n--- Handshake verification ---\n", "\n--- 握手校验 ---\n");
std::cout << L("Connecting to miner and verifying...\n\n", "正在连接矿工并验证...\n\n");

                try {
                    // Ensure we have a valid user wallet address for escrow creation.
                    // If g_current_address is empty (shouldn't happen if wallet is open),
                    // fetch a new address from the loaded wallet.
                    if (g_current_address.empty() && !g_current_wallet_name.empty()) {
                        try {
                            UniValue addrResp = CallRPCSimple("getnewaddress", {"bech32"}, g_current_wallet_name);
                            if (addrResp.find_value("error").isNull()) {
                                g_current_address = addrResp.find_value("result").get_str();
                            }
                        } catch (...) {}
                    }

                    std::vector<std::string> rpc_args;
                    rpc_args.push_back(miner_ip);
                    rpc_args.push_back(api_key);
                    rpc_args.push_back(info.model_name);
                    rpc_args.push_back(std::to_string(miner_port));
// Always pass tokens_per_tknc (even if 0) to keep parameter indices fixed
rpc_args.push_back(info.tokens_per_tknc > 0 ? std::to_string(info.tokens_per_tknc) : "0");
                    // Pass miner_wallet and user_wallet for auto-escrow creation
                    rpc_args.push_back(info.miner_wallet);
                    rpc_args.push_back(g_current_address);

                    UniValue proxyResponse = CallRPCSimple("tknc_setinferproxytarget", rpc_args, "");

                    // CallRPCSimple returns full JSON-RPC envelope: {"result": {...}, "error": null}
                    // Must extract inner "result" object first.
                    const UniValue& proxyErr = proxyResponse.find_value("error");
                    if (!proxyErr.isNull()) {
                        std::cout << "❌ RPC Error: "
                                  << (proxyErr.isObject() && proxyErr.exists("message") ? proxyErr["message"].getValStr() : proxyErr.getValStr())
                                  << "\n";
                        PressContinue();
                        return;
                    }
                    UniValue proxyResult = proxyResponse.find_value("result");

                    if (proxyResult.exists("success") && proxyResult["success"].get_bool()) {
                        // Display handshake results
                        if (proxyResult.exists("handshake")) {
                            UniValue hs = proxyResult["handshake"].get_obj();
                            bool hs_performed = hs.exists("performed") && hs["performed"].get_bool();
                            if (hs_performed) {
                                bool hs_passed = hs.exists("passed") && hs["passed"].get_bool();
                                std::cout << "========================================\n";
                                if (hs_passed) {
                                    std::cout << L("  ✅ Handshake passed!\n", "  ✅ 握手校验通过！\n");
                                } else {
                                    std::cout << L("  ⚠️  Handshake: token anomaly!\n", "  ⚠️  握手校验：token 计数异常！\n");
                                }
                                std::cout << "========================================\n";
                                if (hs.exists("exchange_rate_display")) {
                                    std::cout << "Exchange rate: " << hs["exchange_rate_display"].getValStr() << "\n";
                                }
if (hs.exists("tokens_per_tknc")) {
std::cout << "Token rate: 1 TKNC = " << hs["tokens_per_tknc"].getValStr() << " tokens\n";
}
if (hs.exists("rate_matches")) {
bool matches = hs["rate_matches"].get_bool();
if (matches) {
std::cout << L("  ✅ Rate verified: exchange ratio confirmed!\n", "  ✅ 兑换比例核对成功！\n");
} else {
std::cout << L("\n  ⚠️  WARNING: Rate mismatch!\n", "\n  ⚠️  价格不匹配！\n");
if (hs.exists("warning") && !hs["warning"].getValStr().empty()) {
std::cout << "  " << hs["warning"].getValStr() << "\n";
                                    }
                                }
                            }
                                std::cout << "\n";
                            } else {
                                std::cout << L("  ⚠️  Miner unreachable, skipping verification.\n\n", "  ⚠️  矿工不可达，跳过验证。\n\n");
                            }
                        }

                        std::cout << "========================================\n";
                        std::cout << L("  ✅ Proxy configured!\n", "  ✅ 代理配置成功！\n");
                        std::cout << "========================================\n\n";

                        std::cout << L("--- IDE Configuration ---\n", "--- IDE 配置信息 ---\n");
                        std::cout << "Base URL: " << (proxyResult.exists("local_url") ? proxyResult["local_url"].getValStr() : "") << "\n";
                        std::cout << "API Key:  " << (proxyResult.exists("api_key") ? proxyResult["api_key"].getValStr() : "") << "\n";
                        std::cout << "Model:    " << (proxyResult.exists("model") ? proxyResult["model"].getValStr() : "(any)") << "\n\n";

                        std::cout << L("--- Instructions ---\n", "--- 使用说明 ---\n");
                        std::cout << "1. Open your IDE (VSCode, Cursor, etc.)\n";
                        std::cout << "2. Configure OpenAI-compatible API:\n";
                        std::cout << "   - Base URL: " << (proxyResult.exists("local_url") ? proxyResult["local_url"].getValStr() : "") << "\n";
                        std::cout << "   - API Key:  " << (proxyResult.exists("api_key") ? proxyResult["api_key"].getValStr() : "") << "\n";
                        std::cout << "   - Model:    " << (proxyResult.exists("model") ? proxyResult["model"].getValStr() : "(any)") << "\n";
                        std::cout << "3. All requests forwarded to [" << miner_ip << "]:" << miner_port << "\n";
                        std::cout << "4. Proxy runs on 127.0.0.1 (IPv4), compatible with all IDEs.\n\n";

// Unlock wallet for 24h (86400s) to avoid repeated password prompts during inference.
                        if (g_is_logged_in && !g_current_wallet_name.empty()) {
                            std::cout << L("--- Wallet Unlock ---\n", "--- 解锁钱包 ---\n");
                            std::cout << L("Enter passphrase to unlock wallet (valid for 24h):\n", "请输入密码解锁钱包（24小时有效，配置后不再需要重复输入）:\n");
                            std::string pass = GetHiddenInput(T().prompt_select);
                            if (!pass.empty()) {
                                UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "86400"}, g_current_wallet_name);
                                if (!unlockResult.find_value("error").isNull()) {
                                    std::string errMsg = unlockResult.find_value("error")["message"].get_str();
                                    // If wallet is not encrypted, the error is harmless
                                    if (errMsg.find("not encrypted") == std::string::npos) {
                                        std::cout << T().error_prefix << TranslateRpcError(errMsg) << "\n";
                                    }
                                }
                                std::cout << L("  ✅ Wallet unlocked (24h).\n\n", "  ✅ 钱包已解锁（24小时）。\n\n");
                            } else {
                                std::cout << L("  (Skipped — wallet may stay locked)\n\n", "  (已跳过)\n\n");
                            }
                            SecureClear(pass);
                        }
                    } else {
                        std::cout << L("❌ Failed to configure proxy.\n", "❌ 代理配置失败。\n");
                        std::cout << "Response: " << proxyResult.write() << "\n";
                    }
                } catch (const std::exception& e) {
                    std::cout << "❌ Error: " << e.what() << "\n";
                    std::cout << L("Make sure the local node (tkncd) is running.\n", "请确保本地节点 (tkncd) 正在运行。\n");
                }
            }
        }
    }

    PressContinue();
}

static void InferenceTestCall() {
    ClearScreen();
    std::cout << T().inference_title;
    
    if (g_last_api_key.empty()) {
        std::cout << T().inference_invalid_key;
        PressContinue();
        return;
    }
    
    // Wallet was already unlocked for 24 hours during proxy configuration.
    // No password prompt needed here — just proceed with inference.
    
    std::cout << T().inference_prompt_question;
    std::string question;
    std::getline(std::cin, question);
    
    if (question.empty()) {
        PressContinue();
        return;
    }
    
    try {
// Connect to LOCAL node's API Gateway (127.0.0.1:9313). The local node handles P2P routing to the remote miner.
        std::string body = R"({"model":")" + g_stored_api_keys[0].model_name + 
                          R"(","messages":[{"role":"user","content":")" + question + 
                          R"("}],"api_key":")" + g_last_api_key + R"("})";
        
        // API Gateway port: default 9313, queryable via RPC if needed
        int gateway_port = 9313;
        std::string response = CallHTTP("127.0.0.1", gateway_port, "/v1/chat/completions", body,
                                       "Authorization: Bearer " + g_last_api_key, 130);
        
        // The gateway ALWAYS responds in SSE streaming format (data: {...}\n\n),
        // even when the client doesn't request streaming. We need to handle both
        // SSE and plain JSON responses.
        std::string content;
        int totalTokens = 0;

        if (response.find("data: ") != std::string::npos) {
            // === SSE streaming response ===
            // Parse each "data: {...}" line, extract choices[0].delta.content,
            // and concatenate all content fragments.
            std::istringstream sseStream(response);
            std::string line;
            while (std::getline(sseStream, line)) {
                // Strip trailing \r
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // Only process lines starting with "data: "
                if (line.compare(0, 6, "data: ") != 0)
                    continue;

                std::string jsonStr = line.substr(6);

                // "[DONE]" marker — end of stream
                if (jsonStr == "[DONE]")
                    break;

                UniValue chunk;
                if (!chunk.read(jsonStr))
                    continue;  // skip unparseable lines

                // Check for error in this chunk
                if (chunk.exists("error")) {
                    std::string errMsg;
                    const UniValue& errVal = chunk["error"];
                    if (errVal.isObject() && errVal.exists("message")) {
                        errMsg = errVal["message"].get_str();
                    } else {
                        errMsg = errVal.getValStr();
                    }
                    std::cout << T().inference_test_failed << errMsg << "\n";
                    PressContinue();
                    return;
                }

                // Extract delta content from choices[0].delta.content
                if (chunk.exists("choices") && chunk["choices"].isArray() && chunk["choices"].size() > 0) {
                    const UniValue& choice = chunk["choices"][0];
                    if (choice.exists("delta") && choice["delta"].exists("content")) {
                        content += choice["delta"]["content"].getValStr();
                    }
                }

                // Extract usage info if present (usually in the last chunk)
                if (chunk.exists("usage") && chunk["usage"].isObject()) {
                    const UniValue& usage = chunk["usage"];
                    if (usage.exists("total_tokens")) {
                        totalTokens = usage["total_tokens"].getInt<int>();
                    }
                }
            }
        } else {
            // === Plain JSON response (non-streaming) ===
            UniValue result;
            if (!result.read(response)) {
                throw std::runtime_error("Failed to parse response: " + response.substr(0, 500));
            }

            // Check for error response (e.g., 402 Payment Required, wallet locked, etc.)
            if (result.exists("error")) {
                std::string errMsg;
                const UniValue& errVal = result["error"];
                if (errVal.isObject() && errVal.exists("message")) {
                    errMsg = errVal["message"].get_str();
                } else {
                    errMsg = errVal.getValStr();
                }
                std::cout << T().inference_test_failed << errMsg << "\n";
                PressContinue();
                return;
            }

            // Parse OpenAI-compatible response format: choices[0].message.content
            if (result.exists("choices") && result["choices"].isArray() && result["choices"].size() > 0) {
                const UniValue& choice = result["choices"][0];
                if (choice.exists("message") && choice["message"].exists("content")) {
                    content = choice["message"]["content"].getValStr();
                }
            }

            // Fallback: try direct "content" or "response" field (legacy format)
            if (content.empty()) {
                content = result.exists("content") ? result["content"].getValStr() : "";
            }
            if (content.empty()) {
                content = result.exists("response") ? result["response"].getValStr() : "";
            }

            if (result.exists("usage")) {
                UniValue usage = result["usage"];
                if (usage.exists("total_tokens")) {
                    totalTokens = usage["total_tokens"].getInt<int>();
                }
            }
        }
        
        std::cout << T().inference_test_success;
        std::cout << T().inference_result_label << content << "\n";
        std::cout << T().inference_tokens_label << totalTokens << "\n";
        
    } catch (const std::exception& e) {
        std::cout << T().inference_test_failed << e.what() << "\n";
    }
    
    PressContinue();
}

static void ShowInferenceMenu() {
    while (true) {
        ClearScreen();
        std::cout << T().inference_title;
        std::cout << T().inference_menu_enter_key << "\n";
        std::cout << T().inference_menu_test << "\n";
        std::cout << "------------------------\n";
        if (!g_last_api_key.empty()) {
            std::cout << "Current Key: " << g_last_api_key.substr(0, 20) << "...\n";
            std::cout << "Miner: " << g_last_endpoint << "\n\n";
        }
        
        std::cout << T().prompt_select;
        std::string choice = GetInputEsc("");
        
        if (choice == "1") {
            InferenceEnterKey();
        } else if (choice == "2") {
            InferenceTestCall();
        } else if (choice == "\x1B" || choice == "q" || choice == "Q" || choice.empty()) {
            return;
        }
    }
}

static UniValue CallRPCSimple(const std::string& method, const std::vector<std::string>& args = {}, const std::string& wallet = "") {
    DefaultRequestHandler rh;
    // RAII save/restore -rpcwallet: prevents global state pollution across calls and exception leaks (#R7-1, #R16-2).
    std::string prevWallet = gArgs.GetArg("-rpcwallet", "");
    struct ScopedRpcWallet {
        std::string prev;
        bool modified;
        ~ScopedRpcWallet() {
            if (modified) gArgs.ForceSetArg("-rpcwallet", prev);
        }
    } scopedWallet{prevWallet, !wallet.empty()};
    if (!wallet.empty()) gArgs.ForceSetArg("-rpcwallet", wallet);
    UniValue result = ConnectAndCallRPC(&rh, method, args, wallet.empty() ? RpcWalletName(gArgs) : wallet);
    return result;
}

/**
 * Call RPC with raw JSON params string (for complex nested params like importdescriptors).
 * Bypasses RPCConvertValues which only handles flat string arrays.
 */
static UniValue CallRPCRaw(const std::string& method, const std::string& raw_params_json, const std::string& wallet = "")
{
    // Build the full JSON-RPC request with raw params already serialized
    UniValue request{UniValue::VOBJ};
    request.pushKV("jsonrpc", "1.0");
    request.pushKV("method", method);
    request.pushKV("id", 1);

    // Parse the raw params JSON and insert as-is
    // Security fix (#R15-7): Check JSON parse result — if raw_params_json is malformed,
    // params.read() returns false and params stays VNULL. Sending null params to the node
    // would cause an opaque error. Better to fail fast with a clear message.
    UniValue params;
    if (!params.read(raw_params_json)) {
        throw std::runtime_error("Internal error: failed to parse RPC params JSON");
    }
    request.pushKV("params", params);

    DefaultRequestHandler rh;
    // Security fix (#R7-1): Save and restore -rpcwallet to prevent global state pollution
    // Security fix (#R16-2): Use RAII pattern for exception safety
    std::string prevWallet = gArgs.GetArg("-rpcwallet", "");
    struct ScopedRpcWallet {
        std::string prev;
        bool modified;
        ~ScopedRpcWallet() {
            if (modified) gArgs.ForceSetArg("-rpcwallet", prev);
        }
    } scopedWallet{prevWallet, !wallet.empty()};
    if (!wallet.empty()) gArgs.ForceSetArg("-rpcwallet", wallet);

    std::string host;
    uint16_t port{BaseParams().RPCPort()};
    const std::string rpcconnect_str = gArgs.GetArg("-rpcconnect", DEFAULT_RPCCONNECT);
    uint16_t rpcconnect_port{0};
    SplitHostPort(rpcconnect_str, rpcconnect_port, host);
    if (rpcconnect_port != 0) port = rpcconnect_port;
    if (std::optional<std::string> p = gArgs.GetArg("-rpcport")) port = ToIntegral<uint16_t>(*p).value_or(port);

    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), host, port);
    const int timeout = gArgs.GetIntArg("-rpcclienttimeout", DEFAULT_HTTP_CLIENT_TIMEOUT);
    if (timeout > 0) evhttp_connection_set_timeout(evcon.get(), timeout);

    HTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(http_request_done, (void*)&response);
    // Security fix (#54-4): Null pointer check — obtain_evhttp_request may fail under memory pressure
    if (req == nullptr) throw std::runtime_error("create http request failed (out of memory?)");
    evhttp_request_set_error_cb(req.get(), http_error_cb);

    struct evkeyvalq* output_headers = evhttp_request_get_output_headers(req.get());
    evhttp_add_header(output_headers, "Host", host.c_str());
    evhttp_add_header(output_headers, "Connection", "close");
    evhttp_add_header(output_headers, "Content-Type", "application/json");

    std::string rpc_credentials;
    if (gArgs.GetArg("-rpcpassword", "") == "") {
        // Try reading .cookie file for authentication (Bitcoin Core standard)
        auto auth_cookie_result = GetAuthCookie(rpc_credentials);
        if (auth_cookie_result != AuthCookieResult::Ok) {
            throw std::runtime_error("Could not locate RPC credentials");
        }
        (void)auth_cookie_result;  // Result checked via rpc_credentials being non-empty
    } else {
        // Use standard rpcuser:rpcpassword format for Basic Auth
        std::string rpcuser = gArgs.GetArg("-rpcuser", "");
        rpc_credentials = rpcuser + ":" + gArgs.GetArg("-rpcpassword", "");
    }
    evhttp_add_header(output_headers, "Authorization",
                       (std::string("Basic ") + EncodeBase64(rpc_credentials)).c_str());
    // Security fix (SEC-07): Clear RPC credentials from memory after use in HTTP header
    SecureClear(rpc_credentials);

    std::string strRequest = request.write() + "\n";
    struct evbuffer* output_buffer = evhttp_request_get_output_buffer(req.get());
    evbuffer_add(output_buffer, strRequest.data(), strRequest.size());

    // Security fix (#54-1): URI-encode wallet name to prevent path injection
    // Without encoding, wallet names containing "../" or special chars could access unexpected endpoints
    std::string endpoint;
    if (wallet.empty()) {
        endpoint = "/";
    } else {
        char* encodedURI = evhttp_uriencode(wallet.data(), wallet.size(), false);
        if (encodedURI) {
            endpoint = "/wallet/" + std::string(encodedURI);
            free(encodedURI);
        } else {
            throw std::runtime_error("failed to URI-encode wallet name: " + wallet);
        }
    }
    int r = evhttp_make_request(evcon.get(), req.release(), EVHTTP_REQ_POST, endpoint.c_str());
    if (r != 0) throw std::runtime_error("evhttp_make_request failed");

    event_base_dispatch(base.get());
    if (response.status == 0) throw std::runtime_error("couldn't connect to server");
    if (response.status == HTTP_UNAUTHORIZED) throw std::runtime_error("incorrect rpcuser or rpcpassword");
    // Security fix (#54-2): Handle 503 Service Unavailable (node warming up / overloaded)
    if (response.status == HTTP_SERVICE_UNAVAILABLE)
        throw std::runtime_error("server response: " + response.body);
    if (response.status >= 400 && response.status != HTTP_BAD_REQUEST && response.status != HTTP_NOT_FOUND &&
        response.status != HTTP_INTERNAL_SERVER_ERROR && response.status != HTTP_SERVICE_UNAVAILABLE)
        throw strprintf("unexpected HTTP status %d", response.status);

    // Security fix (#54-3): Check for empty response body before JSON parsing
    if (response.body.empty())
        throw std::runtime_error("empty response from server (possible connection reset)");

    UniValue reply;
    if (!reply.read(response.body))
        throw std::runtime_error("non-JSON HTTP response");
    if (reply.empty())
        throw std::runtime_error("expected reply to have result, error and id properties");
    // RAII ScopedRpcWallet handles -rpcwallet restoration automatically
    return reply;
}

static int ShowLanguageSelection() {
    ClearScreen();
    std::cout << T().select_language;
    for (const auto& lang : LANGUAGES) {
        printf("  %d. %s (%s)\n", lang.code, lang.name.c_str(), lang.native_name.c_str());
    }
    std::cout << "\n" << T().prompt_select;
    std::string choice = GetInputEsc(T().prompt_select);
    try {
        int sel = std::stoi(choice);
        for (const auto& lang : LANGUAGES) {
            if (lang.code == sel) { current_lang = sel; return sel; }
        }
    } catch (...) {}
    return 1;
}

// Security fix (#R7-10): Use RPC error codes instead of fragile string matching.
// Check if error is "wallet not encrypted". Uses RPC error code -15 (robust); string matching is fragile.
static bool IsWalletNotEncryptedError(const UniValue& errorObj) {
    // Method 1: Check error code (robust — matches BTC Core RPC error codes)
    if (errorObj.exists("code")) {
        int code = errorObj["code"].getInt<int>();
        return code == -15;  // RPC_WALLET_WRONG_ENC_STATE
    }
    // Method 2: Fallback to string matching (for compatibility with non-standard nodes)
    if (errorObj.exists("message")) {
        std::string msg = errorObj["message"].get_str();
        return msg.find("not encrypted") != std::string::npos;
    }
    return false;  // Unknown error — treat as authentication failure (fail-safe)
}

// Translate common RPC error messages to current language
static std::string TranslateRpcError(const std::string& errMsg) {
    int lang = current_lang;
    // Common RPC errors
    if (errMsg.find("passphrase") != std::string::npos && errMsg.find("incorrect") != std::string::npos) {
        const char* translations[] = {
            "", // 0: unused
            "Error: Wrong password.", // 1: en
            "错误: 密码错误。", // 2: zh
            "エラー: パスワードが正しくありません。", // 3: ja
            "오류: 잘못된 비밀번호입니다.", // 4: ko
            "Error: Contraseña incorrecta.", // 5: es
            "Erreur: Mot de passe incorrect.", // 6: fr
            "Fehler: Falsches Passwort.", // 7: de
            "Ошибка: Неверный пароль.", // 8: ru
            "Erro: Senha incorreta." // 9: pt
        };
        if (lang >= 1 && lang <= 9) return translations[lang];
    }
    if (errMsg.find("not encrypted") != std::string::npos) {
        const char* translations[] = {
            "", // 0
            "Error: Wallet is not encrypted.", // 1
            "错误: 钱包未加密。", // 2
            "エラー: ウォレットは暗号化されていません。", // 3
            "오류: 지갑이 암호화되지 않았습니다.", // 4
            "Error: La cartera no está cifrada.", // 5
            "Erreur: Le portefeuille n'est pas chiffré.", // 6
            "Fehler: Wallet ist nicht verschlüsselt.", // 7
            "Ошибка: Кошелёк не зашифрован.", // 8
            "Erro: A carteira não está criptografada." // 9
        };
        if (lang >= 1 && lang <= 9) return translations[lang];
    }
    if (errMsg.find("already exists") != std::string::npos) {
        const char* translations[] = {
            "", // 0
            "Error: Wallet already exists.", // 1
            "错误: 钱包已存在。", // 2
            "エラー: ウォレットは既に存在します。", // 3
            "오류: 지갑이 이미 존재합니다.", // 4
            "Error: La cartera ya existe.", // 5
            "Erreur: Le portefeuille existe déjà.", // 6
            "Fehler: Wallet existiert bereits.", // 7
            "Ошибка: Кошелёк уже существует.", // 8
            "Erro: A carteira já existe." // 9
        };
        if (lang >= 1 && lang <= 9) return translations[lang];
    }
    if (errMsg.find("Invalid descriptor") != std::string::npos) {
        const char* translations[] = {
            "", // 0
            "Invalid key format.", // 1
            "密钥格式无效。", // 2
            "キー形式が無効です。", // 3
            "키 형식이 잘못되었습니다.", // 4
            "Formato de clave inválido.", // 5
            "Format de clé invalide.", // 6
            "Ungültiges Schlüsselformat.", // 7
            "Неверный формат ключа.", // 8
            "Formato de chave inválido." // 9
        };
        if (lang >= 1 && lang <= 9) return translations[lang];
    }
    if (errMsg.find("walletpassphrase") != std::string::npos && errMsg.find("first") != std::string::npos) {
        const char* translations[] = {
            "", // 0
            "Wallet is locked. Please unlock it first.", // 1
            "钱包已锁定，请先解锁。", // 2
            "ウォレットがロックされています。先にロック解除してください。", // 3
            "지갑이 잠겨 있습니다. 먼저 잠금 해제하세요.", // 4
            "Cartera bloqueada. Desbloquéela primero.", // 5
            "Portefeuille verrouillé. Déverrouillez-le d'abord.", // 6
            "Wallet ist gesperrt. Entsperren Sie sie zuerst.", // 7
            "Кошелёк заблокирован. Сначала разблокируйте его.", // 8
            "Carteira bloqueada. Desbloqueie-a primeiro." // 9
        };
        if (lang >= 1 && lang <= 9) return translations[lang];
    }
    // Return original if no translation found
    return errMsg;
}

static int ShowPublicMenu() {
    ClearScreen();
    std::cout << T().not_logged_in_title;
    std::cout << T().menu_new_wallet << "\n";
    std::cout << T().menu_restore_wallet << "\n";
    std::cout << T().menu_open_wallet << "\n";
    std::cout << "------------------------\n";
    std::cout << T().menu_node_info << "\n";
    std::cout << T().menu_blockchain_info << "\n";
    std::cout << T().menu_network_info << "\n";
    std::cout << T().menu_block_info << "\n";
    std::cout << T().menu_mining_info << "\n";
    std::cout << "------------------------\n";
    std::cout << T().menu_back << "\n";
    std::cout << "\n" << T().prompt_select;
    std::string choice = GetInputEsc("");
    if (choice == "\x1B" || choice == "q" || choice == "Q") return -2;
    try { return std::stoi(choice); }
    catch (...) { return -1; }
}

// Check if an address belongs to the current wallet (ismine=true).
// Used to filter out change outputs (send to self) in transaction display.
static bool IsOwnAddress(const std::string& address, const std::string& walletName) {
    try {
        UniValue result = CallRPCSimple("getaddressinfo", {address}, walletName);
        if (!result.find_value("error").isNull()) return false;
        const UniValue& info = result.find_value("result");
        return info.exists("ismine") && info["ismine"].get_bool();
    } catch (...) { return false; }
}

// Check if an address is a change address (ischange=true) in the current wallet.
// Used to filter out change receives in transaction display.
static bool IsChangeAddress(const std::string& address, const std::string& walletName) {
    try {
        UniValue result = CallRPCSimple("getaddressinfo", {address}, walletName);
        if (!result.find_value("error").isNull()) return false;
        const UniValue& info = result.find_value("result");
        return info.exists("ischange") && info["ischange"].get_bool();
    } catch (...) { return false; }
}

// Get the sender's address for a receive transaction by decoding the raw transaction.
// listtransactions only shows the wallet's own address for receives, not the sender's.
// This function decodes the raw transaction to find the first input's source address.
// Strategy: Try gettransaction on all loaded wallets (works for same-node transfers),
// then fall back to getrawtransaction (works if txindex is enabled).
static std::string GetSenderAddress(const std::string& txid, const std::string& walletName) {
    try {
        // 1. Get the transaction hex from gettransaction (wallet RPC)
        UniValue txResult = CallRPCSimple("gettransaction", {txid}, walletName);
        if (!txResult.find_value("error").isNull()) return "";
        const UniValue& txInfo = txResult.find_value("result");
        if (!txInfo.exists("hex")) return "";
        std::string hex = txInfo["hex"].get_str();

        // 2. Decode the raw transaction to get the inputs
        UniValue decodeResult = CallRPCSimple("decoderawtransaction", {hex}, walletName);
        if (!decodeResult.find_value("error").isNull()) return "";
        const UniValue& decoded = decodeResult.find_value("result");
        if (!decoded.exists("vin") || decoded["vin"].size() == 0) return "";

        // 3. Get the first input's txid and vout
        const UniValue& firstInput = decoded["vin"][0];
        if (!firstInput.exists("txid") || !firstInput.exists("vout")) return "";
        std::string inputTxid = firstInput["txid"].get_str();
        int inputVout = firstInput["vout"].getInt<int>();

        // 4. Try to get the input transaction's hex.
        //    a) First try getrawtransaction (works if txindex is enabled)
        //    b) If that fails, try gettransaction on ALL loaded wallets
        //       (works for transfers between wallets on the same node)
        std::string inputHex;

        // 4a. Try getrawtransaction (needs txindex or blockhash)
        {
            UniValue inputTx = CallRPCSimple("getrawtransaction", {inputTxid, "1"}, walletName);
            if (inputTx.find_value("error").isNull() && inputTx.find_value("result").exists("vout")) {
                const UniValue& inputResult = inputTx.find_value("result");
                const UniValue& voutList = inputResult["vout"];
                if (inputVout >= 0 && inputVout < (int)voutList.size()) {
                    const UniValue& scriptPubKey = voutList[inputVout]["scriptPubKey"];
                    if (scriptPubKey.exists("address")) {
                        return scriptPubKey["address"].get_str();
                    }
                    if (scriptPubKey.exists("addresses") && scriptPubKey["addresses"].size() > 0) {
                        return scriptPubKey["addresses"][0].get_str();
                    }
                }
            }
        }

        // 4b. Try gettransaction on all loaded wallets
        {
            UniValue walletsResult = CallRPCSimple("listwallets", {}, walletName);
            if (!walletsResult.find_value("error").isNull()) return "";
            const UniValue& walletList = walletsResult.find_value("result");
            for (size_t wi = 0; wi < walletList.size(); wi++) {
                std::string wName = walletList[wi].get_str();
                UniValue inputTxResult = CallRPCSimple("gettransaction", {inputTxid}, wName);
                if (!inputTxResult.find_value("error").isNull()) continue;
                const UniValue& inputTxInfo = inputTxResult.find_value("result");
                if (!inputTxInfo.exists("hex")) continue;
                inputHex = inputTxInfo["hex"].get_str();
                break;
            }
        }

        if (inputHex.empty()) return "";

        // 5. Decode the input transaction and extract the sender's address
        UniValue inputDecode = CallRPCSimple("decoderawtransaction", {inputHex}, walletName);
        if (!inputDecode.find_value("error").isNull()) return "";
        const UniValue& inputDecoded = inputDecode.find_value("result");
        if (!inputDecoded.exists("vout")) return "";
        const UniValue& voutList = inputDecoded["vout"];
        if (inputVout < 0 || inputVout >= (int)voutList.size()) return "";

        const UniValue& scriptPubKey = voutList[inputVout]["scriptPubKey"];
        if (scriptPubKey.exists("address")) {
            return scriptPubKey["address"].get_str();
        }
        if (scriptPubKey.exists("addresses") && scriptPubKey["addresses"].size() > 0) {
            return scriptPubKey["addresses"][0].get_str();
        }
        return "";
    } catch (...) { return ""; }
}

static int ShowWalletMenu() {
    ClearScreen();
    char buf[256];
    snprintf(buf, sizeof(buf), T().wallet_menu_title.c_str(), g_current_wallet_name.c_str());
    std::cout << buf;
    // Show current address and total wallet balance only.
    // Per-address balance is NOT shown — it confuses users because UTXO model
    // means a single address balance can drop to 0 after sending (change goes
    // to a new or same address). Users should only care about total balance.
    if (!g_current_address.empty()) {
        std::cout << "  " << g_current_address << "\n";
    }
    try {
        UniValue bal = CallRPCSimple("getbalance", {}, g_current_wallet_name);
        if (bal.find_value("error").isNull())
            std::cout << "  " << T().wallet_total_balance_label << bal.find_value("result").getValStr() << " token\n";
    } catch (...) {}
    std::cout << "\n";
    std::cout << T().menu_wallet_address << "\n";
    std::cout << T().menu_send_tknc << "\n";
    std::cout << T().menu_transactions << "\n";
    std::cout << T().menu_backup_keys << "\n";
    std::cout << T().menu_generate_address << "\n";
    std::cout << T().menu_addr_label << "\n";
    std::cout << T().menu_change_password << "\n";
    std::cout << T().menu_sign_message << "\n";
    std::cout << T().menu_inference_service << "\n";
    std::cout << "------------------------\n";
    std::cout << T().menu_back << "\n";
    std::cout << "\n" << T().prompt_select;
    std::string choice = GetInputEsc("");
    if (choice == "\x1B" || choice == "q" || choice == "Q") return -2;
    try { return std::stoi(choice); }
    catch (...) { return -1; }
}

// ============================================================================
// BIP39 Mnemonic System - Real key derivation (not fake/rand-based)
// ============================================================================

static const char* const BIP39_WORDLIST[2048] = {
    "abandon","ability","able","about","above","absent","absorb","abstract",
    "absurd","abuse","access","accident","account","accuse","achieve","acid",
    "acoustic","acquire","across","act","action","actor","actress","actual",
    "adapt","add","addict","address","adjust","admit","adult","advance",
    "advice","aerobic","affair","afford","afraid","again","age","agent",
    "agree","ahead","aim","air","airport","aisle","alarm","album",
    "alcohol","alert","alien","all","alley","allow","almost","alone",
    "alpha","already","also","alter","always","amateur","amazing","among",
    "amount","amused","analyst","anchor","ancient","anger","angle","angry",
    "animal","ankle","announce","annual","another","answer","antenna","antique",
    "anxiety","any","apart","apology","appear","apple","approve","april",
    "arch","arctic","area","arena","argue","arm","armed","armor",
    "army","around","arrange","arrest","arrive","arrow","art","artefact",
    "artist","artwork","ask","aspect","assault","asset","assist","assume",
    "asthma","athlete","atom","attack","attend","attitude","attract","auction",
    "audit","august","aunt","author","auto","autumn","average","avocado",
    "avoid","awake","aware","away","awesome","awful","awkward","axis",
    "baby","bachelor","bacon","badge","bag","balance","balcony","ball",
    "bamboo","banana","banner","bar","barely","bargain","barrel","base",
    "basic","basket","battle","beach","bean","beauty","because","become",
    "beef","before","begin","behave","behind","believe","below","belt",
    "bench","benefit","best","betray","better","between","beyond","bicycle",
    "bid","bike","bind","biology","bird","birth","bitter","black",
    "blade","blame","blanket","blast","bleak","bless","blind","blood",
    "blossom","blouse","blue","blur","blush","board","boat","body",
    "boil","bomb","bone","bonus","book","boost","border","boring",
    "borrow","boss","bottom","bounce","box","boy","bracket","brain",
    "brand","brass","brave","bread","breeze","brick","bridge","brief",
    "bright","bring","brisk","broccoli","broken","bronze","broom","brother",
    "brown","brush","bubble","buddy","budget","buffalo","build","bulb",
    "bulk","bullet","bundle","bunker","burden","burger","burst","bus",
    "business","busy","butter","buyer","buzz","cabbage","cabin","cable",
    "cactus","cage","cake","call","calm","camera","camp","can",
    "canal","cancel","candy","cannon","canoe","canvas","canyon","capable",
    "capital","captain","car","carbon","card","cargo","carpet","carry",
    "cart","case","cash","casino","castle","casual","cat","catalog",
    "catch","category","cattle","caught","cause","caution","cave","ceiling",
    "celery","cement","census","century","cereal","certain","chair","chalk",
    "champion","change","chaos","chapter","charge","chase","chat","cheap",
    "check","cheese","chef","cherry","chest","chicken","chief","child",
    "chimney","choice","choose","chronic","chuckle","chunk","churn","cigar",
    "cinnamon","circle","citizen","city","civil","claim","clap","clarify",
    "claw","clay","clean","clerk","clever","click","client","cliff",
    "climb","clinic","clip","clock","clog","close","cloth","cloud",
    "clown","club","clump","cluster","clutch","coach","coast","coconut",
    "code","coffee","coil","coin","collect","color","column","combine",
    "come","comfort","comic","common","company","concert","conduct","confirm",
    "congress","connect","consider","control","convince","cook","cool","copper",
    "copy","coral","core","corn","correct","cost","cotton","couch",
    "country","couple","course","cousin","cover","coyote","crack","cradle",
    "craft","cram","crane","crash","crater","crawl","crazy","cream",
    "credit","creek","crew","cricket","crime","crisp","critic","crop",
    "cross","crouch","crowd","crucial","cruel","cruise","crumble","crunch",
    "crush","cry","crystal","cube","culture","cup","cupboard","curious",
    "current","curtain","curve","cushion","custom","cute","cycle","dad",
    "damage","damp","dance","danger","daring","dash","daughter","dawn",
    "day","deal","debate","debris","decade","december","decide","decline",
    "decorate","decrease","deer","defense","define","defy","degree","delay",
    "deliver","demand","demise","denial","dentist","deny","depart","depend",
    "deposit","depth","deputy","derive","describe","desert","design","desk",
    "despair","destroy","detail","detect","develop","device","devote","diagram",
    "dial","diamond","diary","dice","diesel","diet","differ","digital",
    "dignity","dilemma","dinner","dinosaur","direct","dirt","disagree","discover",
    "disease","dish","dismiss","disorder","display","distance","divert","divide",
    "divorce","dizzy","doctor","document","dog","doll","dolphin","domain",
    "donate","donkey","donor","door","dose","double","dove","draft",
    "dragon","drama","drastic","draw","dream","dress","drift","drill",
    "drink","drip","drive","drop","drum","dry","duck","dumb",
    "dune","during","dust","dutch","duty","dwarf","dynamic","eager",
    "eagle","early","earn","earth","easily","east","easy","echo",
    "ecology","economy","edge","edit","educate","effort","egg","eight",
    "either","elbow","elder","electric","elegant","element","elephant","elevator",
    "elite","else","embark","embody","emerge","emotion","employ","empower",
    "empty","enable","enact","end","endless","endorse","enemy","energy",
    "enforce","engage","engine","enhance","enjoy","enlist","enough","enrich",
    "enroll","ensure","enter","entire","entry","envelope","episode","equal",
    "equip","era","erase","erode","erosion","error","erupt","escape",
    "essay","essence","estate","eternal","ethics","evidence","evil","evoke",
    "evolve","exact","example","excess","exchange","excite","exclude","excuse",
    "execute","exercise","exhaust","exhibit","exile","exist","exit","exotic",
    "expand","expect","expire","explain","expose","express","extend","extra",
    "eye","eyebrow","fabric","face","faculty","fade","faint","faith",
    "fall","false","fame","family","famous","fan","fancy","fantasy",
    "farm","fashion","fat","fatal","father","fatigue","fault","favorite",
    "feature","february","federal","fee","feed","feel","female","fence",
    "festival","fetch","fever","few","fiber","fiction","field","figure",
    "file","film","filter","final","find","fine","finger","finish",
    "fire","firm","first","fiscal","fish","fit","fitness","fix",
    "flag","flame","flash","flat","flavor","flee","flight","flip",
    "float","flock","floor","flower","fluid","flush","fly","foam",
    "focus","fog","foil","fold","follow","food","foot","force",
    "forest","forget","fork","fortune","forum","forward","fossil","foster",
    "found","fox","fragile","frame","frequent","fresh","friend","fringe",
    "frog","front","frost","frown","frozen","fruit","fuel","fun",
    "funny","furnace","fury","future","gadget","gain","galaxy","gallery",
    "game","gap","garbage","garden","garlic","garment","gas",
    "gasp","gate","gather","gauge","gaze","general","genius","genre",
    "gentle","genuine","gesture","ghost","giant","gift","giggle","ginger",
    "giraffe","girl","give","glad","glance","glare","glass","glide",
    "glimpse","globe","gloom","glory","glove","glow","glue","goat",
    "goddess","gold","good","goose","gorilla","gospel","gossip","govern",
    "gown","grab","grace","grain","grant","grape","grass","gravity",
    "great","green","grid","grief","grit","grocery","group","grow",
    "grunt","guard","guess","guide","guilt","guitar","gun","gym",
    "habit","hair","half","hammer","hamster","hand","happy","harbor",
    "hard","harsh","harvest","hat","have","hawk","hazard","head",
    "health","heart","heavy","hedgehog","height","hello","helmet","help",
    "hen","hero","hidden","high","hill","hint","hip","hire",
    "history","hobby","hockey","hold","hole","holiday","hollow","home",
    "honey","hood","hope","horn","horror","horse","hospital","host",
    "hotel","hour","hover","hub","huge","human","humble","humor",
    "hundred","hungry","hunt","hurdle","hurry","hurt","husband","hybrid",
    "ice","icon","idea","identify","idle","ignore","ill","illegal",
    "illness","image","imitate","immense","immune","impact","impose","improve",
    "impulse","inch","include","income","increase","index","indicate","indoor",
    "industry","infant","inflict","inform","inhale","inherit","initial","inject",
    "injury","inmate","inner","innocent","input","inquiry","insane","insect",
    "inside","inspire","install","intact","interest","into","invest","invite",
    "involve","iron","island","isolate","issue","item","ivory","jacket",
    "jaguar","jar","jazz","jealous","jeans","jelly","jewel","job",
    "join","joke","journey","joy","judge","juice","jump","jungle",
    "junior","junk","just","kangaroo","keen","keep","ketchup","key",
    "kick","kid","kidney","kind","kingdom","kiss","kit","kitchen",
    "kite","kitten","kiwi","knee","knife","knock","know","lab",
    "label","labor","ladder","lady","lake","lamp","language","laptop",
    "large","later","latin","laugh","laundry","lava","law","lawn",
    "lawsuit","layer","lazy","leader","leaf","learn","leave","lecture",
    "left","leg","legal","legend","leisure","lemon","lend","length",
    "lens","leopard","lesson","letter","level","liar","liberty","library",
    "license","life","lift","light","like","limb","limit","link",
    "lion","liquid","list","little","live","lizard","load","loan",
    "lobster","local","lock","logic","lonely","long","loop","lottery",
    "loud","lounge","love","loyal","lucky","luggage","lumber","lunar",
    "lunch","luxury","lyrics","machine","mad","magic","magnet","maid",
    "mail","main","major","make","mammal","man","manage","mandate",
    "mango","mansion","manual","maple","marble","march","margin","marine",
    "market","marriage","mask","mass","master","match","material","math",
    "matrix","matter","maximum","maze","meadow","mean","measure","meat",
    "mechanic","medal","media","melody","melt","member","memory","mention",
    "menu","mercy","merge","merit","merry","mesh","message","metal",
    "method","middle","midnight","milk","million","mimic","mind","minimum",
    "minor","minute","miracle","mirror","misery","miss","mistake","mix",
    "mixed","mixture","mobile","model","modify","mom","moment","monitor",
    "monkey","monster","month","moon","moral","more","morning","mosquito",
    "mother","motion","motor","mountain","mouse","move","movie","much",
    "muffin","mule","multiply","muscle","museum","mushroom","music","must",
    "mutual","myself","mystery","myth","naive","name","napkin","narrow",
    "nasty","nation","nature","near","neck","need","negative","neglect",
    "neither","nephew","nerve","nest","net","network","neutral","never",
    "news","next","nice","night","noble","noise","nominee","noodle",
    "normal","north","nose","notable","note","nothing","notice","novel",
    "now","nuclear","number","nurse","nut","oak","obey","object",
    "oblige","obscure","observe","obtain","obvious","occur","ocean","october",
    "odor","off","offer","office","often","oil","okay","old",
    "olive","olympic","omit","once","one","onion","online","only",
    "open","opera","opinion","oppose","option","orange","orbit","orchard",
    "order","ordinary","organ","orient","original","orphan","ostrich","other",
    "outdoor","outer","output","outside","oval","oven","over","own",
    "owner","oxygen","oyster","ozone","pact","paddle","page","pair",
    "palace","palm","panda","panel","panic","panther","paper","parade",
    "parent","park","parrot","party","pass","patch","path","patient",
    "patrol","pattern","pause","pave","payment","peace","peanut","pear",
    "peasant","pelican","pen","penalty","pencil","people","pepper","perfect",
    "permit","person","pet","phone","photo","phrase","physical","piano",
    "picnic","picture","piece","pig","pigeon","pill","pilot","pink",
    "pioneer","pipe","pistol","pitch","pizza","place","planet","plastic",
    "plate","play","please","pledge","pluck","plug","plunge","poem",
    "poet","point","polar","pole","police","pond","pony","pool",
    "popular","portion","position","possible","post","potato","pottery","poverty",
    "powder","power","practice","praise","predict","prefer","prepare","present",
    "pretty","prevent","price","pride","primary","print","priority","prison",
    "private","prize","problem","process","produce","profit","program","project",
    "promote","proof","property","prosper","protect","proud","provide","public",
    "pudding","pull","pulp","pulse","pumpkin","punch","pupil","puppy",
    "purchase","purity","purpose","purse","push","put","puzzle","pyramid",
    "quality","quantum","quarter","question","quick","quit","quiz","quote",
    "rabbit","raccoon","race","rack","radar","radio","rail","rain",
    "raise","rally","ramp","ranch","random","range","rapid","rare",
    "rate","rather","raven","raw","razor","ready","real","reason",
    "rebel","rebuild","recall","receive","recipe","record","recycle","reduce",
    "reflect","reform","refuse","region","regret","regular","reject","relax",
    "release","reliance","rely","remain","remember","remind","remove","render",
    "renew","rent","reopen","repair","repeat","replace","report","require",
    "rescue","resemble","resist","resource","response","result","retire","retreat",
    "return","reunion","reveal","review","reward","rhythm","rib","ribbon",
    "rice","rich","ride","ridge","rifle","right","rigid","ring",
    "riot","ripple","risk","ritual","rival","river","road","roast",
    "robot","robust","rocket","romance","roof","rookie","room","rose",
    "rotate","rough","round","route","royal","rubber","rude","rug",
    "rule","run","runway","rural","sad","saddle","sadness","safe",
    "sail","salad","salmon","salon","salt","salute","same","sample",
    "sand","satisfy","satoshi","sauce","sausage","save","say","scale",
    "scan","scare","scatter","scene","scheme","school","science","scissors",
    "scorpion","scout","scrap","screen","script","scrub","sea","search",
    "season","seat","second","secret","section","security","seed","seek",
    "segment","select","sell","seminar","senior","sense","sentence","series",
    "service","session","settle","setup","seven","shadow","shaft","shallow",
    "share","shed","shell","sheriff","shield","shift","shine","ship",
    "shiver","shock","shoe","shoot","shop","short","shoulder","shove",
    "shrimp","shrug","shuffle","shy","sibling","sick","side","siege",
    "sight","sign","silent","silk","silly","silver","similar","simple",
    "since","sing","siren","sister","situate","six","size","skate",
    "sketch","ski","skill","skin","skirt","skull","slab","slam",
    "sleep","slender","slice","slide","slight","slim","slogan","slot",
    "slow","slush","small","smart","smile","smoke","smooth","snack",
    "snake","snap","sniff","snow","soap","soccer","social","sock",
    "soda","soft","solar","soldier","solid","solution","solve","someone",
    "song","soon","sorry","sort","soul","sound","soup","source",
    "south","space","spare","spatial","spawn","speak","special","speed",
    "spell","spend","sphere","spice","spider","spike","spin","spirit",
    "split","spoil","sponsor","spoon","sport","spot","spray","spread",
    "spring","spy","square","squeeze","squirrel","stable","stadium","staff",
    "stage","stairs","stamp","stand","start","state","stay","steak","steel",
    "stem","step","stereo","stick","still","sting","stock","stomach",
    "stone","stool","story","stove","strategy","street","strike","strong",
    "struggle","student","stuff","stumble","style","subject","submit","subway",
    "success","such","sudden","suffer","sugar","suggest","suit","summer",
    "sun","sunny","sunset","super","supply","supreme","sure","surface",
    "surge","surprise","surround","survey","suspect","sustain","swallow","swamp",
    "swap","swarm","swear","sweet","swift","swim","swing","switch","sword",
    "symbol","symptom","syrup","system","table","tackle","tag","tail",
    "talent","talk","tank","tape","target","task","taste","tattoo",
    "taxi","teach","team","tell","ten","tenant","tennis","tent","term",
    "test","text","thank","that","theme","then","theory","there",
    "they","thing","this","thought","three","thrive","throw","thumb",
    "thunder","ticket","tide","tiger","tilt","timber","time","tiny",
    "tip","tired","tissue","title","toast","tobacco","today","toe",
    "together","toilet","token","tomato","tomorrow","tone","tongue","tonight",
    "tool","tooth","top","topic","topple","torch","tornado","tortoise",
    "toss","total","tourist","toward","tower","town","toy","track",
    "trade","traffic","tragic","train","transfer","trap","trash","travel",
    "tray","treat","tree","trend","trial","tribe","trick","trigger",
    "trim","trip","trophy","trouble","truck","true","truly","trumpet",
    "trust","truth","try","tube","tuition","tumble","tuna","tunnel",
    "turkey","turn","turtle","twelve","twenty","twice","twin","twist",
    "two","type","typical","ugly","umbrella","unable","unaware","uncle",
    "uncover","under","undo","unfair","unfold","unhappy","uniform","unique",
    "unit","universe","unknown","unlock","until","unusual","unveil","update",
    "upgrade","uphold","upon","upper","upset","urban","urge","usage",
    "use","used","useful","useless","usual","utility","vacant","vacuum",
    "vague","valid","valley","valve","van","vanish","vapor","various",
    "vast","vault","vehicle","velvet","vendor","venture","venue","verb",
    "verify","version","very","vessel","veteran","viable","vibrant","vicious",
    "victory","video","view","village","vintage","violin","virtual","virus",
    "visa","visit","visual","vital","vivid","vocal","voice","void",
    "volcano","volume","vote","voyage","wage","wagon","wait","walk",
    "wall","walnut","want","warfare","warm","warrior","wash","wasp",
    "waste","water","wave","way","wealth","weapon","wear","weasel",
    "weather","web","wedding","weekend","weird","welcome","west","wet",
    "whale","what","wheat","wheel","when","where","whip","whisper",
    "wide","width","wife","wild","will","win","window","wine",
    "wing","wink","winner","winter","wire","wisdom","wise","wish",
    "witness","wolf","woman","wonder","wood","wool","word","work",
    "world","worry","worth","wrap","wreck","wrestle","wrist","write",
    "wrong","yard","year","yellow","you","young","youth","zebra",
    "zero","zone","zoo"
};

/**
 * Generate a real BIP39 mnemonic phrase from cryptographically secure entropy.
 * Process: 128-bit entropy → SHA256 checksum(4 bits) → 132 bits → 12 x 11-bit indices → words
 */
static std::vector<std::string> GenerateBIP39Mnemonic()
{
    // Step 1: Generate 128 bits (16 bytes) of secure random entropy
    std::vector<uint8_t> entropy(16);
    GetStrongRandBytes(std::span<unsigned char>(entropy.data(), entropy.size()));

    // Step 2: Compute SHA256(entropy) and take first 4 bits as checksum
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(entropy.data(), entropy.size()).Finalize(hash);
    uint8_t checksumBits = hash[0] >> 4; // top 4 bits of first byte

    // Step 3: Build 132-bit buffer (128 entropy + 4 checksum), extract 12 x 11-bit indices
    uint8_t bits[17]; // 16 bytes entropy + 1 byte (upper nibble=checksum, lower=0)
    memcpy(bits, entropy.data(), 16);
    bits[16] = checksumBits << 4;

    std::vector<std::string> words;
    for (int i = 0; i < 12; i++) {
        int startBit = i * 11;
        // Extract 11 bits from the bit stream (MSB first)
        uint16_t idx = 0;
        for (int b = 0; b < 11; b++) {
            int absBit = startBit + b;
            int bytePos = absBit / 8;
            int bitPos = 7 - (absBit % 8);
            idx |= ((bits[bytePos] >> bitPos) & 1) << (10 - b);
        }
        words.push_back(BIP39_WORDLIST[idx]);
    }
    return words;
}

/**
 * Validate BIP39 checksum: verify that the mnemonic words form a valid BIP39 phrase.
 * For 12 words: 11 bits each = 132 bits total (128 entropy + 4 checksum)
 * Checksum = first 4 bits of SHA256(128-bit entropy)
 */
static bool ValidateBIP39Checksum(const std::vector<std::string>& words)
{
    if (words.size() != 12) return false;

    // Convert words to 11-bit indices, pack into bit array
    uint8_t bits[17] = {0}; // 132 bits = 17 bytes (last byte uses 4 bits)
    for (int i = 0; i < 12; i++) {
        int idx = -1;
        for (int j = 0; j < 2048; j++) {
            if (words[i] == BIP39_WORDLIST[j]) { idx = j; break; }
        }
        if (idx < 0) return false; // Invalid word not in wordlist

        // Pack 11-bit index into bit array (MSB first within each group)
        int bitPos = i * 11;
        for (int b = 10; b >= 0; b--) {
            if (idx & (1 << b))
                bits[bitPos / 8] |= (1 << (7 - (bitPos % 8)));
            bitPos++;
        }
    }

    // Extract entropy (first 128 bits) and expected checksum (last 4 bits)
    uint8_t entropy[16];
    memcpy(entropy, bits, 16);
    uint8_t expectedChecksum = bits[16] >> 4;

    // Compute actual checksum: SHA256(first 128 bits), take top 4 bits
    uint8_t hash[CSHA256::OUTPUT_SIZE];
    CSHA256().Write(entropy, 16).Finalize(hash);
    uint8_t actualChecksum = hash[0] >> 4;

    return actualChecksum == expectedChecksum;
}

/**
 * Convert BIP39 mnemonic words to seed using PBKDF2-HMAC-SHA512.
 * BIP39 spec: PKCS5_PBKDF2_HMAC(mnemonic, "mnemonic" + passphrase, 2048 iterations, 64-byte output)
 */
static std::vector<uint8_t> MnemonicToSeed(const std::vector<std::string>& words, const std::string& passphrase = "")
{
    // Join words with spaces
    std::string mnemonicStr;
    for (size_t i = 0; i < words.size(); i++) {
        if (i > 0) mnemonicStr += ' ';
        mnemonicStr += words[i];
    }
    // Salt is "mnemonic" + passphrase
    std::string salt = "mnemonic" + passphrase;

    // PBKDF2-HMAC-SHA512 with 2048 iterations, 64-byte output
    // Note: OpenSSL PKCS5_PBKDF2_HMAC takes (const char* pass, const unsigned char* salt, ...)
    std::vector<uint8_t> seed(64);
    // Security fix (#R7-5): Check PBKDF2 return value — failure produces all-zero seed (silent data loss)
    int pbkdf2_result = PKCS5_PBKDF2_HMAC(
        mnemonicStr.c_str(), static_cast<int>(mnemonicStr.size()),
        reinterpret_cast<const unsigned char*>(salt.c_str()), static_cast<int>(salt.size()),
        2048, EVP_sha512(), static_cast<int>(seed.size()), seed.data());
    if (pbkdf2_result != 1) {
        throw std::runtime_error("PBKDF2-HMAC-SHA512 key derivation failed (OpenSSL error)");
    }
    return seed;
}

/**
 * Derive BIP32 xprv from BIP39 seed at path m/84'/0'/0' (SegWit bech32 account).
 * Returns encoded xprv string like "xprv9s21ZrQH143K3..."
 */
static std::string SeedToDescriptorXprv(const std::vector<uint8_t>& seed)
{
    CExtKey master, purpose, coin, account;

    // Convert vector<uint8_t> to span<const std::byte> for SetSeed
    auto byteSpan = std::as_bytes(std::span<const uint8_t>(seed));
    master.SetSeed(byteSpan);

    // Verify that SetSeed produced a valid key
    if (!master.key.IsValid()) {
        std::cout << "[ERROR] SetSeed produced invalid private key!\n";
        return "";
    }

    // BIP84 standard derivation path: m/84'/0'/0' (account level)
    //   purpose=84' (BIP84 Native SegWit)
    //   coin_type=0' (mainnet)
    //   account=0'
    if (!master.Derive(purpose, 0x80000084)) return "";
    if (!purpose.Derive(coin, 0x80000000)) return "";
    if (!coin.Derive(account, 0x80000000)) return "";

    return EncodeExtKey(account);
}

/**
 * Display all 12 mnemonic words for user to write down.
 * Does NOT verify — just shows the words and waits for user to press Enter.
 */
static void ShowMnemonicWords(const std::vector<std::string>& words)
{
    std::cout << T().mnemonic_title;
    std::cout << T().mnemonic_warning;
    std::cout << "\n";

    for (size_t i = 0; i < words.size(); i++) {
        printf(T().mnemonic_word_label.c_str(), static_cast<int>(i + 1), words[i].c_str());
    }
    std::cout << "\n";
}

/**
 * Verify user actually remembers the mnemonic by asking 3 random positions.
 * Call this AFTER ShowMnemonicWords AND after user confirms they wrote it down.
 * Returns true if all 3 answers are correct.
 */
static bool VerifyMnemonicRandom(const std::vector<std::string>& words)
{
    std::cout << T().mnemonic_verify_title;

    // Pick 3 random distinct positions (Fisher-Yates partial shuffle)
    int positions[3];
    int pool[12];
    for (int i = 0; i < 12; i++) pool[i] = i;
    for (int i = 0; i < 3; i++) {
        uint16_t r;
        GetStrongRandBytes(std::span<unsigned char>(reinterpret_cast<unsigned char*>(&r), sizeof(r)));
        int j = i + (r % (12 - i));
        std::swap(pool[i], pool[j]);
        positions[i] = pool[i];
    }

    for (int i = 0; i < 3; i++) {
        char promptBuf[128];
        snprintf(promptBuf, sizeof(promptBuf), T().mnemonic_verify_prompt.c_str(), positions[i] + 1);
        std::string input = GetInput(promptBuf);
        if (input != words[positions[i]]) {
            std::cout << T().mnemonic_verify_failed;
            return false;
        }
    }

    std::cout << T().mnemonic_verify_success;
    return true;
}

static void CreateNewWallet() {
    ClearScreen();
    std::cout << T().create_wallet_title;

    // Step 1: Wallet name (ESC to cancel)
    std::string name;
    while (true) {
        name = GetInputEsc(T().prompt_wallet_name);
        if (name == "\x1B") return;
        if (name.empty()) { std::cout << T().invalid_input << "\n"; continue; }
        // Security fix (#R5-1): Length limits to prevent DoS/overflow via ultra-long inputs
        if (name.size() > 100) {
            std::cout << T().error_prefix << "Wallet name too long (max 100 characters).\n";
            continue;
        }
        // Security fix (#R13-1): Reject path separators in wallet name.
        // BTC Core's createwallet allows paths as wallet names (wallet created at that path).
        // In a CLI for end users, this is dangerous — could create wallets in unexpected locations.
        if (name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
            name.find("..") != std::string::npos || name.find('\0') != std::string::npos) {
            std::cout << T().error_prefix << "Wallet name cannot contain path separators (/ \\) or '..'.\n";
            continue;
        }
        break;
    }

    // Step 2: Password (ESC to cancel)
    std::string pass1, pass2;
    while (true) {
        pass1 = GetHiddenInput(T().prompt_password);
        if (pass1 == "\x1B") return;
        if (pass1.empty()) {
            // Security fix (#R9-4): Warn user about unencrypted wallet — consistent with BTC Core.
            // BTC Core createwallet: "Empty string given as passphrase, wallet will not be encrypted."
            std::cout << "\n" << T().warn_no_passphrase;
            std::cout << T().warn_anyone_access;
            std::cout << T().confirm_unencrypted;
            std::string confirm = GetInput("");
            if (confirm != "yes" && confirm != "YES") {
                continue;  // Loop back to password prompt
            }
        }
        else if (pass1.length() < 8) { std::cout << T().password_too_short << "\n"; continue; }
        else if (pass1.length() > 128) {
            std::cout << T().error_prefix << "Password too long (max 128 characters).\n";
            continue;
        }
        pass2 = GetHiddenInput(T().prompt_password_confirm);
        if (pass1 != pass2) { std::cout << T().password_mismatch << "\n"; continue; }
        break;
    }

    std::cout << "\n" << T().wallet_creating << "\n";

    try {
        // === REAL BIP39 MNEMONIC FLOW ===
        // Step A: Generate real BIP39 mnemonic from secure entropy
        auto words = GenerateBIP39Mnemonic();

        // Step B: Show all 12 words for user to write down
        ShowMnemonicWords(words);
        PressContinue();  // Wait for user to read and write down words before clearing screen

        // Step C: Clear screen then verify user actually remembers the words
        ClearScreen();

        // Step D: Verify user actually remembers the words (random 3-position check)
        if (!VerifyMnemonicRandom(words)) {
            PressContinue();
            return;
        }

        // === Step E: Key derivation + wallet creation ===
        // Each step wrapped in its own error handling for precise diagnostics

        // E1: BIP39 seed derivation
        std::vector<uint8_t> seed;
        try {
            seed = MnemonicToSeed(words);
        } catch (const std::exception& e) {
            std::cout << T().error_prefix << "Seed derivation failed: " << e.what() << "\n";
            PressContinue(); return;
        } catch (...) {
            std::cout << T().error_prefix << "Seed derivation failed (unknown error).\n";
            PressContinue(); return;
        }

        // E2: BIP32 xprv derivation
        std::string xprv;
        try {
            xprv = SeedToDescriptorXprv(seed);
        } catch (const std::exception& e) {
            std::cout << T().error_prefix << "Key derivation failed: " << e.what() << "\n";
            PressContinue(); return;
        } catch (...) {
            std::cout << T().error_prefix << "Key derivation failed (unknown error).\n";
            PressContinue(); return;
        }

        if (xprv.empty()) {
            std::cout << T().error_prefix << "BIP32 key derivation returned empty.\n";
            PressContinue(); return;
        }

        // E3: Create wallet via RPC
        UniValue result;
        try {
            result = CallRPCSimple("createwallet", {name, "false", "false", pass1});
        } catch (const std::exception& e) {
            std::cout << T().error_prefix << "Wallet creation RPC failed: " << e.what() << "\n";
            PressContinue(); return;
        } catch (...) {
            std::cout << T().error_prefix << "Wallet creation RPC failed (unknown error).\n";
            PressContinue(); return;
        }

        if (!result.find_value("error").isNull()) {
            std::string errMsg = result.find_value("error")["message"].get_str();
            std::cout << T().restore_failed << errMsg << "\n";
            std::cout << T().wallet_create_failed;
            PressContinue(); return;
        }

        std::string wName = result.find_value("result")["name"].get_str();

        // E4: Unlock wallet
        try {
            CallRPCSimple("walletpassphrase", {pass1, "600"}, wName);
        } catch (const std::exception& e) {
            std::cout << T().error_prefix << "Unlock failed: " << e.what() << "\n";
            // Security fix (#55-2): Clean up residual wallet on node — E1 succeeded but E4 failed
            std::cout << T().cleaning_incomplete_wallet;
            CallRPCSimple("unloadwallet", {}, wName);
            PressContinue(); return;
        } catch (...) {
            std::cout << T().error_prefix << "Unlock failed (unknown error).\n";
            CallRPCSimple("unloadwallet", {}, wName);
            PressContinue(); return;
        }

        // E5: Import BIP84 descriptors (external 0/* + internal 1/*). Node requires checksums via getdescriptorinfo.
        UniValue importResult;
        try {
            std::string extDesc = "token(" + xprv + "/0/*)";
            std::string intDesc = "token(" + xprv + "/1/*)";

            // getdescriptorinfo replaces xprv with xpub, but importdescriptors needs original xprv.
            // Extract only checksum, append to original xprv descriptor manually.
            UniValue extInfo = CallRPCSimple("getdescriptorinfo", {extDesc}, wName);
            if (!extInfo.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(extInfo.find_value("error")["message"].get_str()) << "\n";
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                SecureClear(seed); SecureClear(xprv);
                PressContinue(); return;
            }
            extDesc = extDesc + "#" + extInfo.find_value("result")["checksum"].get_str();

            UniValue intInfo = CallRPCSimple("getdescriptorinfo", {intDesc}, wName);
            if (!intInfo.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(intInfo.find_value("error")["message"].get_str()) << "\n";
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                SecureClear(seed); SecureClear(xprv);
                PressContinue(); return;
            }
            intDesc = intDesc + "#" + intInfo.find_value("result")["checksum"].get_str();

            UniValue requests(UniValue::VARR);

            // External chain descriptor
            UniValue extEntry(UniValue::VOBJ);
            extEntry.pushKV("desc", extDesc);
            extEntry.pushKV("timestamp", "now");
            extEntry.pushKV("active", true);
            extEntry.pushKV("internal", false);
            requests.push_back(extEntry);

            // Internal chain descriptor
            UniValue intEntry(UniValue::VOBJ);
            intEntry.pushKV("desc", intDesc);
            intEntry.pushKV("timestamp", "now");
            intEntry.pushKV("active", true);
            intEntry.pushKV("internal", true);
            requests.push_back(intEntry);

            UniValue paramsArr(UniValue::VARR);
            paramsArr.push_back(requests);
            std::string rawParams = paramsArr.write();

            importResult = CallRPCRaw("importdescriptors", rawParams, wName);
        } catch (const std::exception& e) {
            std::cout << T().import_warning << e.what() << "\n";
            // Security fix (#55-2): Clean up residual wallet — E1+E4 succeeded but E5 failed
            std::cout << T().cleaning_incomplete_wallet;
            CallRPCSimple("unloadwallet", {}, wName);
            PressContinue(); return;
        } catch (...) {
            std::cout << T().import_warning << "Descriptor import failed (unknown error).\n";
            CallRPCSimple("unloadwallet", {}, wName);
            PressContinue(); return;
        }

        if (!importResult.find_value("error").isNull()) {
            std::cout << T().import_warning << TranslateRpcError(importResult.find_value("error")["message"].get_str()) << "\n";
            // Security fix (#55-2): Clean up residual wallet — descriptors import failed
            CallRPCSimple("unloadwallet", {}, wName);
            PressContinue(); return;
        }

        // E6: Get address + balance display
        std::string address;
        bool e6Success = true;  // Track whether E6 operations succeeded
        try {
            UniValue addr = CallRPCSimple("getnewaddress", {"bech32"}, wName);
            if (addr.find_value("error").isNull())
                address = addr.find_value("result").get_str();
            else
                e6Success = false;  // getnewaddress returned error
        } catch (...) { e6Success = false; }

        std::string balanceStr = "0.00000000";
        try {
            UniValue bal = CallRPCSimple("getbalance", {}, wName);
            if (bal.find_value("error").isNull())
                balanceStr = bal.find_value("result").getValStr();
            else
                e6Success = false;  // getbalance returned error
        } catch (...) { /* balance stays default */ }

        // Security fix (#58): Check E6 success before declaring wallet ready
        // Core operations (E1-E5) succeeded: wallet is created, unlocked, descriptors imported
        // But if getnewaddress failed, warn user instead of showing fake success
        if (!e6Success) {
            std::cout << T().error_prefix << "Wallet created but address generation failed.\n";
            std::cout << T().wallet_functional_msg;
            // Still log in — wallet is usable, just need to retry address generation
        }

        // SUCCESS: Show wallet ready — user is now IN the wallet
        std::cout << "═════════════════════════════════\n";
        std::cout << T().wallet_created_success << "\n";
        std::cout << T().wallet_name_label2 << wName << "\n";

        char readyMsg[256];
        snprintf(readyMsg, sizeof(readyMsg), T().wallet_ready_msg.c_str(),
                 wName.c_str(), address.c_str(), balanceStr.c_str());

        std::cout << T().wallet_ready_title;
        std::cout << readyMsg;

        // Security fix (SEC-07): Clear sensitive crypto data from memory before continuing
        SecureClear(seed);
        SecureClear(xprv);
        SecureClear(words);

        g_is_logged_in = true;
        g_current_wallet_name = wName;
        if (e6Success && !address.empty()) g_current_address = address;
        else { try { UniValue a = CallRPCSimple("getnewaddress", {"bech32"}, wName); if (a.find_value("error").isNull()) g_current_address = a.find_value("result").get_str(); } catch (...) {} }
        std::cout << T().auto_login_msg << " [" << wName << "]\n";

    } catch (const std::exception& e) {
        // Security fix (#52): Rollback login state on exception to prevent "fake login" state
        g_is_logged_in = false;
        g_current_wallet_name = "";
        g_current_address = "";
        std::cout << T().error_prefix << e.what() << "\n";
        std::cout << T().wallet_create_error;
    } catch (...) {
        // Security fix (#52): Rollback login state on exception
        g_is_logged_in = false;
        g_current_wallet_name = "";
        g_current_address = "";
        std::cout << T().error_prefix << "Unexpected error during wallet creation.\n";
        std::cout << T().wallet_create_error;
    }
    // Security fix (SEC-07): Clear sensitive data from memory
    SecureClear(pass1);
    SecureClear(pass2);
    // Note: seed, xprv, words are declared inside the try block above
    // and are cleared at their end-of-scope within the try block
    PressContinue();
}

// Forward declaration — used by RestoreWallet and OpenWallet
static std::string GetFirstWalletAddress(const std::string& walletName);

static void RestoreWallet() {
    ClearScreen();
    std::cout << T().restore_title;
    std::cout << "1. " << T().restore_from_backup << "\n";
    std::cout << "2. " << T().restore_from_descriptors << "\n";
    std::cout << "3. " << T().restore_from_mnemonic << "\n";
    std::cout << T().restore_prompt_file;

    std::string choice = GetInputEsc("");
    if (choice == "\x1B" || choice == "q" || choice == "Q") { return; }
    if (choice != "1" && choice != "2" && choice != "3") { PressContinue(); return; }

    // === Option 1: Restore from .dat backup file ===
    if (choice == "1") {
        std::string fPath = GetInput(T().prompt_backup_path);

        if (fPath.empty()) {
            std::cout << T().restore_failed << T().err_file_empty;
            PressContinue(); return;
        }

        // Security fix (#64): Path traversal prevention — normalize and validate
        // Block paths containing ".." sequences, null bytes, or non-printable characters
        if (fPath.find("..") != std::string::npos || fPath.find('\0') != std::string::npos) {
            std::cout << T().restore_failed << "Invalid path: path traversal detected.\n";
            PressContinue(); return;
        }
        // Normalize path to canonical form (resolves symlinks, relative components)
        try {
            std::error_code ec;
            auto canonical = std::filesystem::canonical(fPath, ec);
            if (!ec) fPath = canonical.string();
        } catch (...) {
            // If canonicalization fails, continue with original path (node will validate)
        }

        if (!std::filesystem::exists(fPath)) {
            std::cout << T().restore_failed << T().err_file_not_found << fPath << "\n";
            PressContinue(); return;
        }

        if (!std::filesystem::is_regular_file(fPath)) {
            std::cout << T().restore_failed << T().err_file_invalid << fPath << "\n";
            PressContinue(); return;
        }

        std::string newName = GetInput(T().prompt_new_wallet_name);

        if (newName.empty()) {
            std::cout << T().restore_failed << T().err_wallet_name_empty;
            PressContinue(); return;
        }
        // Security fix (#R6-5): Wallet name length limit — consistent with CreateNewWallet (#R5-1)
        if (newName.size() > 100) {
            std::cout << T().restore_failed << "Wallet name too long (max 100 characters).\n";
            PressContinue(); return;
        }
        // Security fix (#R13-1): Reject path separators in wallet name
        if (newName.find('/') != std::string::npos || newName.find('\\') != std::string::npos ||
            newName.find("..") != std::string::npos || newName.find('\0') != std::string::npos) {
            std::cout << T().restore_failed << "Wallet name cannot contain path separators (/ \\) or '..'.\n";
            PressContinue(); return;
        }

        try {
            UniValue result = CallRPCSimple("restorewallet", {newName, fPath});
            if (result.find_value("error").isNull()) {
                std::string restoredName = result.find_value("result")["name"].get_str();
                std::cout << T().restore_success << restoredName << "\n";

                // Security fix (#R7-6): Verify password before login (prevents wallet.dat theft → full menu access → privacy breach).
                std::string pass = GetHiddenInput(T().prompt_unlock_or_skip);
                if (!pass.empty()) {
                    UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "600"}, restoredName);
                    if (!unlockResult.find_value("error").isNull()) {
                        std::string errMsg = unlockResult.find_value("error")["message"].get_str();
                        std::cout << T().error_prefix << errMsg << "\n";
                        // Security fix (#R7-10): Use error code instead of string matching
                        if (!IsWalletNotEncryptedError(unlockResult.find_value("error"))) {
                            // Wrong password — BLOCK login, unload the restored wallet
                            std::cout << T().restore_access_denied;
                            try { CallRPCSimple("unloadwallet", {}, restoredName); } catch (...) {}
                            PressContinue(); return;
                        }
                        // Wallet not encrypted — continue
                    }
                } else {
                    // User skipped password — check if wallet is encrypted
                    // Try walletpassphrase with empty string to detect encryption status
                    UniValue unlockResult = CallRPCSimple("walletpassphrase", {"", "600"}, restoredName);
                    if (!unlockResult.find_value("error").isNull()) {
                        // Security fix (#R7-10): Use error code instead of string matching
                        if (!IsWalletNotEncryptedError(unlockResult.find_value("error"))) {
                            // Wallet IS encrypted but user skipped password — BLOCK login
                            std::cout << T().wallet_encrypted_must_unlock;
                            try { CallRPCSimple("unloadwallet", {}, restoredName); } catch (...) {}
                            PressContinue(); return;
                        }
                        // Wallet not encrypted — empty password is fine
                    }
                }

                // Security fix (SEC-07): Clear password from memory after use
                SecureClear(pass);

                // Security fix (#43): Set login state so user can use the wallet immediately
                g_is_logged_in = true;
                g_current_wallet_name = restoredName;
                g_current_address = GetFirstWalletAddress(restoredName);
                std::cout << T().auto_login_msg << " [" << restoredName << "]\n";
            } else {
                std::cout << T().restore_failed << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << T().restore_failed << e.what() << "\n";
        } catch (...) {
            std::cout << T().restore_failed << "Unknown error.\n";
        }
        // Security fix (SEC-07): Clear sensitive data from memory
        // Note: pass is scoped inside try block above, cleared at its end-of-scope via RAII below

    // === Option 2: Restore from descriptor (xprv) ===
    } else if (choice == "2") {
        std::string wName = GetInput(T().prompt_wallet_name);
        // Security fix (#45): Validate wallet name is not empty
        if (wName.empty()) {
            std::cout << T().restore_failed << T().err_wallet_name_empty;
            PressContinue(); return;
        }
        // Security fix (#R6-6): Wallet name length limit — consistent with CreateNewWallet (#R5-1)
        if (wName.size() > 100) {
            std::cout << T().restore_failed << "Wallet name too long (max 100 characters).\n";
            PressContinue(); return;
        }
        // Security fix (#R13-1): Reject path separators in wallet name
        if (wName.find('/') != std::string::npos || wName.find('\\') != std::string::npos ||
            wName.find("..") != std::string::npos || wName.find('\0') != std::string::npos) {
            std::cout << T().restore_failed << "Wallet name cannot contain path separators (/ \\) or '..'.\n";
            PressContinue(); return;
        }
        std::string pass1 = GetHiddenInput(T().prompt_password);
        // Security fix (#R9-4): Warn about empty password — consistent with CreateNewWallet/BTC Core
        if (pass1.empty()) {
            std::cout << "\n" << T().warn_no_passphrase;
            std::cout << T().warn_anyone_access;
            std::cout << T().confirm_unencrypted;
            std::string confirm = GetInput("");
            if (confirm != "yes" && confirm != "YES") { PressContinue(); return; }
        }
        std::string pass2 = GetHiddenInput(T().prompt_password_confirm);

        // Security fix (#61): Enforce minimum password length (consistent with CreateNewWallet)
        if (!pass1.empty() && pass1.length() < 8) {
            std::cout << T().password_too_short << "\n";
            PressContinue(); return;
        }
        // Security fix (#R6-6): Password length upper limit — consistent with CreateNewWallet (#R5-1)
        if (pass1.length() > 128) {
            std::cout << T().error_prefix << "Password too long (max 128 characters).\n";
            PressContinue(); return;
        }

        if (pass1 != pass2) { std::cout << T().password_mismatch << "\n"; PressContinue(); return; }

        // Get receiving (external) descriptor
        std::cout << T().prompt_ext_descriptor;
        std::string extDesc = GetInput("");
        if (extDesc.empty()) { std::cout << T().invalid_input << T().err_no_descriptors; PressContinue(); return; }
        // Trim leading/trailing whitespace (users often paste with extra spaces)
        {
            size_t start = extDesc.find_first_not_of(" \t\r\n");
            size_t end = extDesc.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos)
                extDesc = extDesc.substr(start, end - start + 1);
        }
        // Strip [Receiving] or [Change] prefix if present (from backup export format)
        if (extDesc.find("[Receiving] ") == 0) extDesc = extDesc.substr(12);
        else if (extDesc.find("[Change] ") == 0) extDesc = extDesc.substr(9);
        if (extDesc.size() > 500) {
            std::cout << T().error_prefix << "Descriptor too long (max 500 characters).\n";
            PressContinue(); return;
        }

        // Get change (internal) descriptor — optional, press Enter to skip
        std::cout << T().prompt_int_descriptor;
        std::string intDesc = GetInput("");
        // Trim leading/trailing whitespace
        if (!intDesc.empty()) {
            size_t start = intDesc.find_first_not_of(" \t\r\n");
            size_t end = intDesc.find_last_not_of(" \t\r\n");
            if (start != std::string::npos && end != std::string::npos)
                intDesc = intDesc.substr(start, end - start + 1);
        }
        // Strip [Receiving] or [Change] prefix if present
        if (intDesc.find("[Receiving] ") == 0) intDesc = intDesc.substr(12);
        else if (intDesc.find("[Change] ") == 0) intDesc = intDesc.substr(9);

        try {
            UniValue result = CallRPCSimple("createwallet", {wName, "false", "false", pass1, "false", "true"});
            if (!result.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
                PressContinue();
                return;
            }

            // Security fix (#46): Check walletpassphrase result before importing descriptors
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass1, "600"}, wName);
            if (!unlockResult.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(unlockResult.find_value("error")["message"].get_str()) << "\n";
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                PressContinue(); return;
            }

            // Auto-append checksum to descriptors if not already present.
            // getdescriptorinfo replaces xprv with xpub in its output, but we only need the checksum.
            // Strip existing checksum (if any), get fresh checksum, append to original descriptor.
            {
                std::string extBase = extDesc;
                size_t hashPos = extBase.rfind('#');
                if (hashPos != std::string::npos) extBase = extBase.substr(0, hashPos);

                UniValue extInfo = CallRPCSimple("getdescriptorinfo", {extBase}, wName);
                if (!extInfo.find_value("error").isNull()) {
                    std::cout << T().restore_failed << TranslateRpcError(extInfo.find_value("error")["message"].get_str()) << "\n";
                    try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                    PressContinue(); return;
                }
                extDesc = extBase + "#" + extInfo.find_value("result")["checksum"].get_str();

                if (!intDesc.empty()) {
                    std::string intBase = intDesc;
                    hashPos = intBase.rfind('#');
                    if (hashPos != std::string::npos) intBase = intBase.substr(0, hashPos);

                    UniValue intInfo = CallRPCSimple("getdescriptorinfo", {intBase}, wName);
                    if (!intInfo.find_value("error").isNull()) {
                        std::cout << T().restore_failed << TranslateRpcError(intInfo.find_value("error")["message"].get_str()) << "\n";
                        try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                        PressContinue(); return;
                    }
                    intDesc = intBase + "#" + intInfo.find_value("result")["checksum"].get_str();
                }
            }

            // Import both descriptors in a single call with proper internal marking
            UniValue requests(UniValue::VARR);

            UniValue extEntry(UniValue::VOBJ);
            extEntry.pushKV("desc", extDesc);
            extEntry.pushKV("timestamp", "now");
            extEntry.pushKV("active", true);
            extEntry.pushKV("internal", false);
            requests.push_back(extEntry);

            if (!intDesc.empty()) {
                UniValue intEntry(UniValue::VOBJ);
                intEntry.pushKV("desc", intDesc);
                intEntry.pushKV("timestamp", "now");
                intEntry.pushKV("active", true);
                intEntry.pushKV("internal", true);
                requests.push_back(intEntry);
            }

            UniValue paramsArr(UniValue::VARR);
            paramsArr.push_back(requests);
            std::string rawParams = paramsArr.write();

            int importErrors = 0;
            int totalDescs = intDesc.empty() ? 1 : 2;
            UniValue importResult = CallRPCRaw("importdescriptors", rawParams, wName);
            if (!importResult.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(importResult.find_value("error")["message"].get_str()) << "\n";
                importErrors = totalDescs;
            } else {
                UniValue results = importResult.find_value("result");
                for (int i = 0; i < (int)results.size(); i++) {
                    if (!results[i]["success"].get_bool()) {
                        importErrors++;
                        std::cout << T().import_warning << TranslateRpcError(results[i]["error"]["message"].get_str()) << "\n";
                    }
                }
            }

            UniValue addr = CallRPCSimple("getnewaddress", {"bech32"}, wName);
            if (addr.find_value("error").isNull()) {
                std::cout << T().restore_success << wName << "\n";
                std::cout << T().wallet_address_label << addr.find_value("result").get_str() << "\n";
                if (importErrors > 0) {
                    if (importErrors >= totalDescs) {
                        // All descriptors failed — wallet has no keys, clean up
                        try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                        std::cout << T().error_prefix << "All descriptors failed to import. Wallet cleaned up.\n";
                        PressContinue(); return;
                    }
                    // Partial success — wallet has some keys, warn but continue
                    std::cout << T().import_warning << importErrors << " of " << totalDescs
                              << " descriptor(s) failed to import. Wallet has partial keys.\n";
                    std::cout << T().import_descriptors_later;
                }
                g_is_logged_in = true;
                g_current_wallet_name = wName;
                g_current_address = GetFirstWalletAddress(wName);
                std::cout << T().auto_login_msg << " [" << wName << "]\n";
            } else {
                std::cout << T().restore_failed << TranslateRpcError(addr.find_value("error")["message"].get_str()) << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << T().restore_failed << e.what() << "\n";
        } catch (...) {
            std::cout << T().restore_failed << "Unknown error.\n";
        }
        // Security fix (SEC-07): Clear sensitive data from memory
        SecureClear(pass1);
        SecureClear(pass2);

    // === Option 3: Restore from BIP39 recovery phrase (12 words) ===
    } else if (choice == "3") {
        std::string wName = GetInput(T().prompt_wallet_name);
        // Security fix (#R11-1): Validate wallet name — consistent with Option2 (#45)
        if (wName.empty()) {
            std::cout << T().restore_failed << T().err_wallet_name_empty;
            PressContinue(); return;
        }
        // Security fix (#R6-7): Wallet name length limit — consistent with CreateNewWallet (#R5-1)
        if (wName.size() > 100) {
            std::cout << T().restore_failed << "Wallet name too long (max 100 characters).\n";
            PressContinue(); return;
        }
        // Security fix (#R13-1): Reject path separators in wallet name
        if (wName.find('/') != std::string::npos || wName.find('\\') != std::string::npos ||
            wName.find("..") != std::string::npos || wName.find('\0') != std::string::npos) {
            std::cout << T().restore_failed << "Wallet name cannot contain path separators (/ \\) or '..'.\n";
            PressContinue(); return;
        }
        std::string pass1 = GetHiddenInput(T().prompt_password);
        // Security fix (#R9-4): Warn about empty password — consistent with CreateNewWallet/BTC Core
        if (pass1.empty()) {
            std::cout << "\n" << T().warn_no_passphrase;
            std::cout << T().warn_anyone_access;
            std::cout << T().confirm_unencrypted;
            std::string confirm = GetInput("");
            if (confirm != "yes" && confirm != "YES") { PressContinue(); return; }
        }
        std::string pass2 = GetHiddenInput(T().prompt_password_confirm);

        // Security fix (#61): Enforce minimum password length (consistent with CreateNewWallet)
        if (!pass1.empty() && pass1.length() < 8) {
            std::cout << T().password_too_short << "\n";
            PressContinue(); return;
        }
        // Security fix (#R11-2): Password length upper limit — consistent with Option2 (#R6-6)
        if (pass1.length() > 128) {
            std::cout << T().error_prefix << "Password too long (max 128 characters).\n";
            PressContinue(); return;
        }

        if (pass1 != pass2) { std::cout << T().password_mismatch << "\n"; PressContinue(); return; }

        // Input 12 mnemonic words
        std::cout << T().prompt_enter_mnemonic;
        std::vector<std::string> inputWords;
        for (int i = 0; i < 12; i++) {
            char buf[64];
            snprintf(buf, sizeof(buf), T().prompt_mnemonic_word.c_str(), i + 1);
            std::string word = GetInput(buf);

            // Trim whitespace
            size_t start = word.find_first_not_of(" \t");
            size_t end = word.find_last_not_of(" \t");
            if (start == std::string::npos) { word = ""; }
            else { word = word.substr(start, end - start + 1); }
            // Convert to lowercase
            for (auto& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

            // Validate against BIP39 wordlist
            bool validWord = false;
            for (int j = 0; j < 2048; j++) {
                if (word == BIP39_WORDLIST[j]) { validWord = true; break; }
            }
            if (!validWord) {
                char errBuf[128];
                snprintf(errBuf, sizeof(errBuf), T().restore_mnemonic_invalid.c_str(), word.c_str(), i + 1);
                std::cout << errBuf;
                PressContinue(); return;
            }
            inputWords.push_back(word);
        }

        // Optional passphrase
        std::string passphrase = GetHiddenInput(T().prompt_mnemonic_passphrase);
        // Security fix (#R10-2): Limit passphrase length to prevent PBKDF2 DoS
        if (passphrase.size() > 256) {
            std::cout << T().error_prefix << "Passphrase too long (max 256 characters).\n";
            PressContinue(); return;
        }

        // Security fix (#59): Validate BIP39 checksum before deriving seed
        // Without this, a user who mistypes one word (but each word is valid BIP39)
        // would silently create a NEW wallet with different addresses — unrecoverable!
        if (!ValidateBIP39Checksum(inputWords)) {
            std::cout << T().restore_failed << T().bip39_checksum_mismatch;
            std::cout << T().mnemonic_typo_check;
            PressContinue(); return;
        }

        // Derive seed → xprv → dual-chain descriptors → import (BIP84 standard)
        try {
            auto seed = MnemonicToSeed(inputWords, passphrase);
            std::string xprv = SeedToDescriptorXprv(seed);

            if (xprv.empty()) {
                std::cout << T().restore_mnemonic_xprv_failed;
                PressContinue(); return;
            }

            // Create wallet + import both descriptor chains
            UniValue result = CallRPCSimple("createwallet", {wName, "false", "false", pass1});
            if (!result.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
                PressContinue(); return;
            }

            // Security fix (#55-1): Check walletpassphrase result (same pattern as #46 for Option 2)
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass1, "600"}, wName);
            if (!unlockResult.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(unlockResult.find_value("error")["message"].get_str()) << "\n";
                // Clean up the created wallet since we can't unlock it
                // Security fix (#R6-8): Add try-catch for unloadwallet — consistent with Option2 (#61-b)
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                PressContinue(); return;
            }

            // Build importdescriptors with BOTH external(0/*) and internal(1/*) chains
            // Fix: getdescriptorinfo replaces xprv with xpub — only extract checksum,
            // then append to original xprv descriptor (same fix as CreateNewWallet E5).
            std::string extDesc = "token(" + xprv + "/0/*)";
            std::string intDesc = "token(" + xprv + "/1/*)";

            UniValue extInfo = CallRPCSimple("getdescriptorinfo", {extDesc}, wName);
            if (!extInfo.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(extInfo.find_value("error")["message"].get_str()) << "\n";
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                SecureClear(seed); SecureClear(xprv);
                PressContinue(); return;
            }
            extDesc = extDesc + "#" + extInfo.find_value("result")["checksum"].get_str();

            UniValue intInfo = CallRPCSimple("getdescriptorinfo", {intDesc}, wName);
            if (!intInfo.find_value("error").isNull()) {
                std::cout << T().restore_failed << TranslateRpcError(intInfo.find_value("error")["message"].get_str()) << "\n";
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                SecureClear(seed); SecureClear(xprv);
                PressContinue(); return;
            }
            intDesc = intDesc + "#" + intInfo.find_value("result")["checksum"].get_str();

            UniValue requests(UniValue::VARR);

            UniValue extEntry(UniValue::VOBJ);
            extEntry.pushKV("desc", extDesc);
            extEntry.pushKV("timestamp", "now");
            extEntry.pushKV("active", true);
            extEntry.pushKV("internal", false);
            requests.push_back(extEntry);

            UniValue intEntry(UniValue::VOBJ);
            intEntry.pushKV("desc", intDesc);
            intEntry.pushKV("timestamp", "now");
            intEntry.pushKV("active", true);
            intEntry.pushKV("internal", true);
            requests.push_back(intEntry);

            UniValue paramsArr(UniValue::VARR);
            paramsArr.push_back(requests);
            std::string rawParams = paramsArr.write();

            UniValue importResult = CallRPCRaw("importdescriptors", rawParams, wName);

            if (!importResult.find_value("error").isNull()) {
                std::cout << T().import_warning << TranslateRpcError(importResult.find_value("error")["message"].get_str()) << "\n";
                // Security fix (#R11-3): Clean up wallet on import failure — consistent with CreateNewWallet E5
                try { CallRPCSimple("unloadwallet", {}, wName); } catch (...) {}
                PressContinue(); return;
            }

            // Show restored address
            UniValue addr = CallRPCSimple("getnewaddress", {"bech32"}, wName);
            if (addr.find_value("error").isNull()) {
                std::cout << T().restore_mnemonic_success;
                std::cout << T().wallet_name_label2 << wName << "\n";
                std::cout << T().wallet_address_label << addr.find_value("result").get_str() << "\n";
                // Security fix (SEC-07): Clear sensitive crypto data from memory
                SecureClear(seed);
                SecureClear(xprv);
                g_is_logged_in = true;
                g_current_wallet_name = wName;
                g_current_address = addr.find_value("result").get_str();
                std::cout << T().auto_login_msg << " [" << wName << "]\n";
            } else {
                std::cout << T().restore_failed << TranslateRpcError(addr.find_value("error")["message"].get_str()) << "\n";
            }
        } catch (const std::exception& e) {
            std::cout << T().restore_failed << e.what() << "\n";
        } catch (...) {
            std::cout << T().restore_failed << "Unknown error.\n";
        }
        // Security fix (SEC-07): Clear sensitive data from memory
        SecureClear(pass1);
        SecureClear(pass2);
        SecureClear(inputWords);
        SecureClear(passphrase);
        // Note: seed and xprv are declared inside the try block above
        // and are cleared at their end-of-scope within the try block
    }
    PressContinue();
}

// Helper: Get the first existing address from a wallet.
// Never calls getnewaddress — only returns already-generated addresses.
static std::string GetFirstWalletAddress(const std::string& walletName) {
    try {
        UniValue addrs = CallRPCSimple("listreceivedbyaddress", {"0", "true", "true"}, walletName);
        if (addrs.find_value("error").isNull()) {
            const UniValue& addrList = addrs.find_value("result");
            if (addrList.size() > 0) {
                return addrList[0]["address"].get_str();
            }
        }
    } catch (...) {}
    return "";
}

// Forward declaration — defined later in the file
static std::vector<std::string> GetAvailableWallets();

static void OpenWallet() {
    ClearScreen();
    std::cout << "--- " << T().menu_open_wallet.substr(T().menu_open_wallet.find(". ") + 2) << " ---\n\n";

    // Security fix (#R16-5): Defensive check — if already logged in, lock and unload the current wallet first.
    // Normal UI flow prevents reaching OpenWallet while logged in, but guard against future changes.
    if (g_is_logged_in && !g_current_wallet_name.empty()) {
        try { CallRPCSimple("walletlock", {}, g_current_wallet_name); } catch (...) {}
        try { CallRPCSimple("unloadwallet", {g_current_wallet_name}, ""); } catch (...) {}
        g_is_logged_in = false;
        g_current_wallet_name = "";
        g_current_address = "";
    }

    try {
        // List ALL available wallets from the wallet directory (not just loaded ones)
        auto availableWallets = GetAvailableWallets();
        if (availableWallets.empty()) {
            std::cout << T().info_no_wallet << "\n";
            std::cout << T().msg_no_wallet_create_first;
            PressContinue(); return;
        }

        // Also get currently loaded wallets to show status
        UniValue loaded = CallRPCSimple("listwallets");
        std::set<std::string> loadedSet;
        if (loaded.find_value("error").isNull()) {
            const UniValue& lw = loaded.find_value("result");
            if (lw.isArray()) {
                for (size_t i = 0; i < lw.size(); i++) {
                    loadedSet.insert(lw[i].get_str());
                }
            }
        }

        std::cout << T().available_wallets;
        for (size_t i = 0; i < availableWallets.size(); i++) {
            std::string status = loadedSet.count(availableWallets[i]) ? " [loaded]" : "";
            std::cout << "  " << (i + 1) << ". " << availableWallets[i] << status << "\n";
        }
        std::cout << "\n";

        char wBuf[64];
        snprintf(wBuf, sizeof(wBuf), T().select_wallet_fmt.c_str(), (int)availableWallets.size());
        std::string wChoice = GetInputEsc(wBuf);
        if (wChoice == "\x1B" || wChoice == "q" || wChoice == "Q" || wChoice.empty()) { PressContinue(); return; }

        int sel = 0;
        try { sel = std::stoi(wChoice); } catch (...) {
            std::cout << T().invalid_input << "\n";
            PressContinue(); return;
        }
        if (sel < 1 || sel > (int)availableWallets.size()) {
            std::cout << T().invalid_input << "\n";
            PressContinue(); return;
        }

        std::string selectedWallet = availableWallets[sel - 1];

        // Load the wallet into the node if not already loaded
        if (loadedSet.find(selectedWallet) == loadedSet.end()) {
            UniValue loadResult = CallRPCSimple("loadwallet", {selectedWallet});
            if (!loadResult.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(loadResult.find_value("error")["message"].get_str()) << "\n";
                PressContinue(); return;
            }
        }

        // Security fix (#R15-6): Password rate limiting (BTC Core lacks this; CLI adds global counter + cooldown after MAX_FAILED_PASSWORD_ATTEMPTS).
        static int s_failed_password_attempts = 0;
        static std::chrono::steady_clock::time_point s_last_failed_time;
        const int MAX_FAILED_PASSWORD_ATTEMPTS = 5;
        const int COOLDOWN_SECONDS = 30;

        if (s_failed_password_attempts >= MAX_FAILED_PASSWORD_ATTEMPTS) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - s_last_failed_time).count();
            if (elapsed < COOLDOWN_SECONDS) {
                std::cout << T().too_many_failed_attempts
                          << (COOLDOWN_SECONDS - elapsed) << " seconds before trying again.\n";
                PressContinue(); return;
            }
            // Cooldown expired — reset counter
            s_failed_password_attempts = 0;
        }

        // Try to unlock wallet
        // Security fix (#R7-7): CRITICAL — Empty password must NOT allow login to encrypted wallets.
        std::string pass = GetHiddenInput(T().prompt_unlock_or_skip);
        if (!pass.empty()) {
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "600"}, selectedWallet);
            if (!unlockResult.find_value("error").isNull()) {
                std::string errMsg = unlockResult.find_value("error")["message"].get_str();
                std::cout << T().error_prefix << TranslateRpcError(errMsg) << "\n";
                // Security fix (#R7-10): Use error code instead of string matching
                if (!IsWalletNotEncryptedError(unlockResult.find_value("error"))) {
                    s_failed_password_attempts++;
                    s_last_failed_time = std::chrono::steady_clock::now();
                    PressContinue(); return;
                }
            }
            // Successful unlock — reset failure counter
            s_failed_password_attempts = 0;
        } else {
            // User pressed Enter (empty password) — check if wallet is encrypted via getwalletinfo
            UniValue walletInfo = CallRPCSimple("getwalletinfo", {}, selectedWallet);
            if (!walletInfo.find_value("error").isNull()) {
                // Can't determine encryption status — try walletpassphrase as fallback
                UniValue unlockResult = CallRPCSimple("walletpassphrase", {"", "600"}, selectedWallet);
                if (!unlockResult.find_value("error").isNull() && !IsWalletNotEncryptedError(unlockResult.find_value("error"))) {
                    std::cout << T().wallet_encrypted_must_unlock;
                    s_failed_password_attempts++;
                    s_last_failed_time = std::chrono::steady_clock::now();
                    PressContinue(); return;
                }
            } else if (walletInfo.find_value("result").exists("unlocked_until")) {
                // Wallet IS encrypted — empty password is not allowed
                std::cout << T().wallet_encrypted_must_unlock;
                s_failed_password_attempts++;
                s_last_failed_time = std::chrono::steady_clock::now();
                PressContinue(); return;
            }
            // Wallet not encrypted — empty password is fine
        }

        // Security fix (SEC-07): Clear password from memory after use
        SecureClear(pass);

        g_is_logged_in = true;
        g_current_wallet_name = selectedWallet;
        g_current_address = GetFirstWalletAddress(selectedWallet);
        std::cout << T().auto_login_msg << " [" << selectedWallet << "]\n";

    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void SendTKNC() {
    ClearScreen();
    std::cout << T().send_title;

    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet << "\n";
        PressContinue(); return;
    }

    std::string selectedWallet = g_current_wallet_name;

    // Ensure wallet is still loaded on the node
    {
        UniValue wallets = CallRPCSimple("listwallets");
        if (!wallets.find_value("error").isNull()) {
            std::cout << T().error_prefix << "Failed to retrieve wallet list.\n";
            PressContinue(); return;
        }
        const UniValue& wList = wallets.find_value("result");
        bool walletFound = false;
        for (size_t i = 0; i < wList.size(); i++) {
            if (wList[i].get_str() == selectedWallet) { walletFound = true; break; }
        }
        if (!walletFound) {
            std::cout << T().error_prefix << "Logged-in wallet '" << selectedWallet
                      << "' is no longer loaded. Please log in again.\n";
            g_is_logged_in = false;
            g_current_wallet_name = "";
            g_current_address = "";
            PressContinue(); return;
        }
    }

    std::cout << L("(q to cancel)\n", "(q 取消)\n");
    std::string toAddr = GetInput(T().send_to_address);

    if (toAddr == "q" || toAddr == "Q") { PressContinue(); return; }
    if (toAddr.empty()) {
        std::cout << T().send_failed << T().err_addr_empty;
        PressContinue(); return;
    }

    // Security fix (#49): Validate Bech32 address format before sending funds
    // token uses Bech32 addresses (token1q... for mainnet, token1t... for testnet)
    // Reject obviously invalid addresses to prevent accidental fund loss
    // Security fix (#66): Enhanced Bech32 validation — prefix + charset + length
    const std::string BECH32_CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    if (toAddr.size() < 8 || toAddr.find("token1") != 0) {
        std::cout << T().send_failed << "Invalid address format. token addresses must start with 'token1'.\n";
        PressContinue(); return;
    }
    // Validate Bech32 charset — only check data part after separator '1'
    // (the HRP prefix "token" contains 'o' which is not in Bech32 charset)
    size_t sep_pos = toAddr.find('1');
    if (sep_pos == std::string::npos) {
        std::cout << T().send_failed << "Invalid address: missing Bech32 separator '1'.\n";
        PressContinue(); return;
    }
    for (size_t i = sep_pos + 1; i < toAddr.size(); i++) {
        char c = toAddr[i];
        if (BECH32_CHARSET.find(c) == std::string::npos) {
            std::cout << T().send_failed << "Invalid address: contains invalid character '" << c << "'.\n";
            PressContinue(); return;
        }
    }
    // Validate typical Bech32 length (14-90 chars for standard BIP173 addresses)
    if (toAddr.size() < 14 || toAddr.size() > 90) {
        std::cout << T().send_failed << "Invalid address length (" << toAddr.size() << "). Expected 14-90 characters.\n";
        PressContinue(); return;
    }

    std::string amount = GetInput(T().send_amount);

    // Security fix (#R5-4): Enhanced amount validation — length + format + range
    if (amount.empty()) {
        std::cout << T().send_failed << T().err_amount_nan;
        PressContinue(); return;
    }
    if (amount.size() > 30) {
        std::cout << T().send_failed << "Amount too long (max 30 characters).\n";
        PressContinue(); return;
    }
    // Reject amount with invalid characters (only digits and decimal point)
    // Security fix (#R11-4): Remove minus sign allowance — negative amounts make no sense for sending.
    // Even though amountVal <= 0 check catches negative values, allowing '-' is unnecessary attack surface.
    for (size_t i = 0; i < amount.size(); i++) {
        char c = amount[i];
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.' && c != ',') {
            std::cout << T().send_failed << "Invalid character '" << c << "' in amount.\n";
            PressContinue(); return;
        }
    }

    // VALIDATE: amount must be a valid positive number
    double amountVal = 0.0;
    try { amountVal = std::stod(amount); } catch (...) {
        std::cout << T().send_failed << T().err_amount_nan;
        PressContinue(); return;
    }
    if (amountVal <= 0) {
        std::cout << T().send_failed << T().err_amount_positive;
        PressContinue(); return;
    }
    // Reasonable upper bound: prevent absurd amounts (max 100 billion token)
    if (amountVal > 100000000000.0) {
        std::cout << T().send_failed << "Amount exceeds maximum (100 billion token).\n";
        PressContinue(); return;
    }

    // Show current address and wallet total balance.
    // Per-address balance is NOT shown — UTXO model means it can be misleading.
    // The wallet-wide coin selection (add_inputs=true) will use all available
    // UTXOs regardless of which address they belong to, and change goes back
    // to g_current_address via change_address option.
    std::cout << T().send_from_label << g_current_address << "\n";
    double walletBal = 0.0;
    try {
        UniValue wb = CallRPCSimple("getbalance", {}, selectedWallet);
        if (wb.find_value("error").isNull())
            walletBal = std::stod(wb.find_value("result").getValStr());
    } catch (...) {}
    char addrBalBuf[64];
    snprintf(addrBalBuf, sizeof(addrBalBuf), "%.8f", walletBal);
    std::cout << T().available_label << addrBalBuf << " token\n";

    // Client-side balance check:
    //   If wallet total balance is insufficient, reject.
    //   Always use wallet-wide coin selection (add_inputs=true) so all UTXOs
    //   are available, with change going back to g_current_address.
    bool useWalletSelection = true;
    if (amountVal > walletBal) {
        std::cout << T().send_insufficient << "\n";
        PressContinue(); return;
    }

    // Show transfer summary before confirmation
    std::cout << "\n  " << T().send_from_label << g_current_address << "\n";
    std::cout << "  " << amount << " token -> " << toAddr << "\n";
    std::cout << T().send_confirm;
    std::string conf = GetInput("");

    if (conf != "y" && conf != "Y" && conf != "s" && conf != "o" && conf != "j" && conf != "д") {
        std::cout << T().back_to_menu << "\n"; PressContinue();
        return;
    }

    // Unlock wallet after confirmation (per user request: password prompt should
    // appear only after user confirms the transfer, not at the start of the flow)
    {
        UniValue walletInfo = CallRPCSimple("getwalletinfo", {}, selectedWallet);
        if (!walletInfo.find_value("error").isNull()) {
            std::cout << T().error_prefix << "Failed to get wallet info.\n";
            PressContinue(); return;
        }
        bool isEncrypted = walletInfo.find_value("result").exists("unlocked_until");
        bool isLocked = false;
        if (isEncrypted) {
            try {
                int64_t unlockedUntil = walletInfo.find_value("result")["unlocked_until"].getInt<int64_t>();
                isLocked = (unlockedUntil == 0);
            } catch (...) { isLocked = true; }
        }

        if (isLocked) {
            std::string pass = GetHiddenInput(T().prompt_locked_unlock);
            if (pass.empty()) {
                std::cout << T().send_failed << "Wallet must be unlocked to send token.\n";
                PressContinue(); return;
            }
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "600"}, selectedWallet);
            if (!unlockResult.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(unlockResult.find_value("error")["message"].get_str()) << "\n";
                SecureClear(pass);
                PressContinue(); return;
            }
            SecureClear(pass);
        }
    }

    try {
        if (useWalletSelection) {
            // Wallet-wide coin selection: let the wallet pick UTXOs from any
            // address (including change addresses), but send change back to
            // the current address via change_address — this avoids generating
            // a new change address on every send.
            // Using 'send' RPC without 'inputs' makes add_inputs default to
            // true, so the wallet auto-selects coins from all its UTXOs.
            char amountBuf[64];
            snprintf(amountBuf, sizeof(amountBuf), "%.8f", amountVal);
            std::string outputsJson = "{\"" + toAddr + "\":" + amountBuf + "}";
            std::string optionsJson = "{\"add_inputs\":true,\"change_address\":\""
                + g_current_address + "\"}";
            std::string paramsJson = "[" + outputsJson
                + ",null,\"unset\",null," + optionsJson + "]";
            UniValue result = CallRPCRaw("send", paramsJson, selectedWallet);
            if (result.find_value("error").isNull()) {
                const UniValue& res = result.find_value("result");
                if (res.exists("txid")) {
                    std::cout << T().send_success << res["txid"].get_str() << "\n";
                } else {
                    std::cout << T().send_success << "(completed)\n";
                }
            } else {
                std::string msg = result.find_value("error")["message"].get_str();
                std::cout << (msg.find("Insufficient") != std::string::npos
                    ? T().send_insufficient : T().send_failed + msg) << "\n";
            }
        } else {
            // Send from current address only: use specific UTXOs and
            // change_address so change goes back to the current address.
            UniValue utxos = CallRPCSimple("listunspent",
                {"1", "9999999", "[\"" + g_current_address + "\"]"}, selectedWallet);
            if (utxos.find_value("error").isNull()) {
                const UniValue& utxoList = utxos.find_value("result");
                if (utxoList.size() == 0) {
                    std::cout << T().send_failed << T().no_utxo_msg;
                    PressContinue(); return;
                }
                // Build inputs JSON array from all UTXOs of the current address
                std::string inputsJson = "[";
                for (size_t i = 0; i < utxoList.size(); i++) {
                    if (i > 0) inputsJson += ",";
                    inputsJson += "{\"txid\":\"" + utxoList[i]["txid"].get_str()
                        + "\",\"vout\":" + utxoList[i]["vout"].getValStr() + "}";
                }
                inputsJson += "]";
                // Build outputs JSON object: {"destAddr": amount}
                char amountBuf[64];
                snprintf(amountBuf, sizeof(amountBuf), "%.8f", amountVal);
                std::string outputsJson = "{\"" + toAddr + "\":" + amountBuf + "}";
                // Build options JSON with inputs and change_address
                std::string optionsJson = "{\"inputs\":" + inputsJson
                    + ",\"change_address\":\"" + g_current_address + "\"}";
                // Build full params JSON array for 'send' RPC
                std::string paramsJson = "[" + outputsJson
                    + ",null,\"unset\",null," + optionsJson + "]";
                // Call 'send' RPC with specific inputs (only UTXOs from current address)
                UniValue result = CallRPCRaw("send", paramsJson, selectedWallet);
                if (result.find_value("error").isNull()) {
                    const UniValue& res = result.find_value("result");
                    if (res.exists("txid")) {
                        std::cout << T().send_success << res["txid"].get_str() << "\n";
                    } else {
                        std::cout << T().send_success << "(completed)\n";
                    }
                } else {
                    std::string msg = result.find_value("error")["message"].get_str();
                    std::cout << (msg.find("Insufficient") != std::string::npos
                        ? T().send_insufficient : T().send_failed + msg) << "\n";
                }
            } else {
                std::cout << T().send_failed << "Failed to get UTXOs for address.\n";
            }
        }
    } catch (const std::exception& e) {
        std::cout << T().send_failed << e.what() << "\n";
    } catch (...) {
        std::cout << T().send_failed << "Unexpected error.\n";
    }
    PressContinue();
}

static void BackupKeys() {
    ClearScreen();
    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet << "\n";
        PressContinue(); return;
    }
    std::cout << T().keys_title;
    std::cout << T().info_wallet_name << g_current_wallet_name << "\n\n";

    // Check if wallet is already unlocked — skip password prompt if so
    bool needUnlock = true;
    {
        UniValue walletInfo = CallRPCSimple("getwalletinfo", {}, g_current_wallet_name);
        if (!walletInfo.find_value("error").isNull()) {
            // Can't determine lock state — fall through to password prompt
        } else {
            bool isEncrypted = walletInfo.find_value("result").exists("unlocked_until");
            if (isEncrypted) {
                try {
                    int64_t unlockedUntil = walletInfo.find_value("result")["unlocked_until"].getInt<int64_t>();
                    if (unlockedUntil > 0) needUnlock = false;  // Already unlocked
                } catch (...) {}
            } else {
                needUnlock = false;  // Not encrypted, no password needed
            }
        }
    }

    std::string pass;
    if (needUnlock) {
        pass = GetHiddenInput(T().keys_unlock_prompt);
    }

    try {
        if (needUnlock) {
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "600"}, g_current_wallet_name);
            if (!unlockResult.find_value("error").isNull()) {
                std::string errMsg = unlockResult.find_value("error")["message"].get_str();
                std::cout << T().error_prefix << TranslateRpcError(errMsg) << "\n";
                if (!IsWalletNotEncryptedError(unlockResult.find_value("error"))) {
                    SecureClear(pass);
                    PressContinue(); return;
                }
            }
        }

        UniValue descs = CallRPCSimple("listdescriptors", {"true"}, g_current_wallet_name);

        if (descs.find_value("error").isNull()) {
            const UniValue& dList = descs.find_value("result")["descriptors"];
            std::string ext_key, int_key;
            for (size_t i = 0; i < dList.size(); i++) {
                std::string desc = dList[i]["desc"].get_str();
                bool isInternal = dList[i].exists("internal") && !dList[i]["internal"].isNull() ? dList[i]["internal"].get_bool() : false;
                if (desc.find("token(") != std::string::npos) {
                    if (!isInternal) ext_key = desc;
                    else int_key = desc;
                }
            }

            if (!ext_key.empty()) {
                std::cout << T().keys_descriptor_label;
                std::cout << "  " << T().label_public << ext_key << "\n";
                if (!int_key.empty())
                    std::cout << "  " << T().label_private << int_key << "\n";
                std::cout << "\n" << T().keys_warning;
            } else {
                std::cout << T().msg_no_bech32_descriptor;
            }
        } else {
            std::cout << T().error_prefix << TranslateRpcError(descs.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().send_failed << e.what() << "\n";
    } catch (...) {
        std::cout << T().send_failed << "Unexpected error.\n";
    }
    SecureClear(pass);
    PressContinue();
}

static void ShowNodeInfo() {
    ClearScreen();
    std::cout << T().node_info_title;
    try {
        UniValue info = CallRPCSimple("getblockchaininfo");
        if (info.find_value("error").isNull()) {
            const UniValue& r = info.find_value("result");
            std::cout << T().block_height_label << r["blocks"].getValStr() << "\n";
            std::cout << T().mining_difficulty_label << r["difficulty"].getValStr() << "\n";
            std::cout << T().label_chain << r["chain"].get_str() << "\n";
            std::cout << T().label_headers << r["headers"].getValStr() << "\n";
        }
        UniValue net = CallRPCSimple("getnetworkinfo");
        if (net.find_value("error").isNull()) {
            const UniValue& n = net.find_value("result");
            std::cout << "\n" << T().label_version << n["version"].getValStr() << "\n";
            std::cout << T().label_subversion << n["subversion"].get_str() << "\n";
            std::cout << T().label_connections << n["connections"].getValStr() << "\n";
        }
        UniValue peers = CallRPCSimple("getpeerinfo");
        if (peers.find_value("error").isNull()) {
            std::cout << "\n" << T().label_peers << peers.find_value("result").size() << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void ShowBlockchainInfo() {
    ClearScreen();
    std::cout << T().blockchain_title;
    try {
        UniValue result = CallRPCSimple("getblockchaininfo");
        if (result.find_value("error").isNull()) {
            const UniValue& r = result.find_value("result");
            std::cout << T().block_height_label << r["blocks"].getValStr() << "\n";
            std::cout << T().mining_difficulty_label << r["difficulty"].getValStr() << "\n";
            std::cout << T().label_chain << r["chain"].get_str() << "\n";
            std::cout << T().label_headers << r["headers"].getValStr() << "\n";
            std::cout << T().label_progress << std::fixed << std::setprecision(4) << r["verificationprogress"].get_real() * 100 << "%\n";
            std::cout << T().label_size_on_disk << r["size_on_disk"].getValStr() << " MB\n";
        } else {
            std::cout << T().error_prefix << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void ShowNetworkInfo() {
    ClearScreen();
    std::cout << T().network_title;
    try {
        UniValue result = CallRPCSimple("getnetworkinfo");
        if (result.find_value("error").isNull()) {
            const UniValue& r = result.find_value("result");
            std::cout << T().label_version << r["version"].getValStr() << "\n";
            std::cout << T().label_subversion << r["subversion"].get_str() << "\n";
            std::cout << T().label_protocol << r["protocol_version"].getValStr() << "\n";
            std::cout << T().label_connections_io << r["connections_in"].getValStr() << "/" << r["connections_out"].getValStr() << "\n";
            std::cout << T().label_network_active << (r["networkactive"].get_bool() ? T().label_yes_short : T().label_no_short) << "\n";
            
            UniValue peers = CallRPCSimple("getpeerinfo");
            if (peers.find_value("error").isNull()) {
                const UniValue& pList = peers.find_value("result");
                char peerBuf[128];
                snprintf(peerBuf, sizeof(peerBuf), T().label_connected_peers.c_str(), (int)pList.size());
                std::cout << "\n" << peerBuf;
                for (size_t i = 0; i < pList.size(); i++) {
                    std::string addr = pList[i]["addr"].get_str();
                    std::string version = pList[i]["subver"].get_str();
                    std::cout << (i + 1) << ". " << addr << " [" << version << "]\n";
                }
            }
        } else {
            std::cout << T().error_prefix << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void BlockExplorer() {
    ClearScreen();
    std::cout << T().block_title;
    std::cout << T().block_explore_options;
    
    std::string choice = GetInputEsc(T().block_explore_select);
    
    if (choice == "\x1B" || choice == "q" || choice == "Q") { PressContinue(); return; }

    try {
        std::string blockHash;
        
        if (choice == "1") {
            blockHash = GetInput(T().prompt_block_hash);
            if (blockHash == "latest" || blockHash.empty()) {
                UniValue best = CallRPCSimple("getbestblockhash");
                // Security fix (#R15-2): Check for error before accessing result
                if (!best.find_value("error").isNull()) {
                    std::cout << T().error_prefix << TranslateRpcError(best.find_value("error")["message"].get_str()) << "\n";
                    PressContinue(); return;
                }
                blockHash = best.find_value("result").get_str();
            } else {
                // Security fix (#R10-3): Validate block hash format (64 hex chars)
                if (blockHash.size() != 64) {
                    std::cout << T().error_prefix << "Invalid block hash length (expected 64 hex characters).\n";
                    PressContinue(); return;
                }
                for (char c : blockHash) {
                    if (!std::isxdigit(static_cast<unsigned char>(c))) {
                        std::cout << T().error_prefix << "Invalid block hash (must be hexadecimal).\n";
                        PressContinue(); return;
                    }
                }
            }
        } else if (choice == "2") {
            std::string heightStr = GetInput(T().prompt_block_height);
            int height = -1;
            if (heightStr == "latest" || heightStr.empty()) {
                UniValue bc = CallRPCSimple("getblockcount");
                height = std::stoi(bc.find_value("result").getValStr());
            } else {
                // Security fix (#R6-9): stoi exception protection — consistent with OpenWallet/SendTKNC pattern
                try { height = std::stoi(heightStr); } catch (...) {
                    std::cout << T().error_prefix << "Invalid block height. Please enter a number or 'latest'.\n";
                    PressContinue(); return;
                }
                if (height < 0) {
                    std::cout << T().error_prefix << "Block height cannot be negative.\n";
                    PressContinue(); return;
                }
            }
            UniValue hash = CallRPCSimple("getblockhash", {std::to_string(height)});
            // Security fix (#R15-2): Check for error before accessing result.
            // Without this, an out-of-range height causes get_str() on NullUniValue →
            // cryptic "type_error: JSON value of type null is not of expected type string"
            if (!hash.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(hash.find_value("error")["message"].get_str()) << "\n";
                PressContinue(); return;
            }
            blockHash = hash.find_value("result").get_str();
        } else {
            UniValue best = CallRPCSimple("getbestblockhash");
            // Security fix (#R15-2): Check for error before accessing result
            if (!best.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(best.find_value("error")["message"].get_str()) << "\n";
                PressContinue(); return;
            }
            blockHash = best.find_value("result").get_str();
        }
        
        UniValue block = CallRPCSimple("getblock", {blockHash});
        if (block.find_value("error").isNull()) {
            const UniValue& b = block.find_value("result");
            std::cout << "\n" << T().block_height_label << b["height"].getValStr() << "\n";
            std::cout << T().block_hash_label << b["hash"].get_str() << "\n";
            std::cout << T().label_previous << b["previousblockhash"].get_str().substr(0, 16) << "...\n";
            std::cout << T().label_next << b["nextblockhash"].get_str().substr(0, 16) << "...\n";
            // Convert Unix timestamp to human-readable local time
            int64_t blockTime = b["time"].getInt<int64_t>();
            time_t bt = static_cast<time_t>(blockTime);
            struct tm* btm = localtime(&bt);
            char timeBuf[32];
            strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", btm);
            std::cout << T().block_time_label << timeBuf << "\n";
            std::cout << T().block_tx_count_label << b["tx"].size() << "\n";
            std::cout << T().label_merkle_root << b["merkleroot"].get_str().substr(0, 16) << "...\n";
            std::cout << T().label_nonce << b["nonce"].getValStr() << "\n";
            std::cout << T().mining_difficulty_label << b["difficulty"].getValStr() << "\n";
            
            if (!b["tx"].empty()) {
                std::cout << "\n" << T().label_tx_list;
                for (size_t i = 0; i < b["tx"].size(); i++) {
                    std::cout << "  " << (i + 1) << ". " << b["tx"][i].get_str().substr(0, 16) << "...\n";
                }
            }
        } else {
            std::cout << T().error_prefix << TranslateRpcError(block.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void ShowTransactions() {
    ClearScreen();
    std::cout << T().transactions_title;

    // Security fix (#R11-7): Require login — transaction history is private data
    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet << "\n";
        PressContinue(); return;
    }

    // Show current address filter
    std::cout << T().send_from_label << g_current_address << "\n";
    std::cout << std::string(70, '-') << "\n";

    int count = 100; // Fetch all recent transactions

    try {
        UniValue txs = CallRPCSimple("listtransactions", {"*", std::to_string(count), "0", "true"}, g_current_wallet_name);
        if (txs.find_value("error").isNull()) {
            const UniValue& tList = txs.find_value("result");
            if (tList.empty()) {
                std::cout << T().no_transactions;
            } else {
                int shown = 0;
                for (size_t i = 0; i < tList.size(); i++) {
                    const UniValue& tx = tList[i];

                    // Direction: send / receive / generate(mining)
                    std::string category = tx.exists("category") ? tx["category"].get_str() : "unknown";
                    bool isSend = (category == "send");
                    bool isGenerate = (category == "generate" || category == "immature");

                    // Filter out internal change outputs/receives to avoid confusing the user.
                    // Bitcoin Core's listtransactions creates separate entries for each output:
                    //   - send -10 to recipient (actual transfer) → SHOW
                    //   - send -171443 to own change address (internal) → SKIP
                    //   - receive +171443 at own change address (internal) → SKIP
                    // This makes the transaction list match what the user actually did.
                    {
                        std::string entryAddr = tx.exists("address") ? tx["address"].get_str() : "";
                        if (isSend && IsOwnAddress(entryAddr, g_current_wallet_name)) {
                            continue; // change output (send to self) — skip
                        }
                        if (!isSend && !isGenerate && IsChangeAddress(entryAddr, g_current_wallet_name)) {
                            continue; // change receive (internal transfer) — skip
                        }
                    }

                    std::string dirStr;
                    if (isGenerate) {
                        dirStr = T().tx_receive_label; // mining rewards display as receive
                    } else {
                        dirStr = isSend ? T().tx_send_label : T().tx_receive_label;
                    }

                    // Amount
                    double amount = 0.0;
                    if (tx.exists("amount")) amount = tx["amount"].get_real();

                    // TXID (needed for sender lookup)
                    std::string txid = tx.exists("txid") ? tx["txid"].get_str() : "N/A";

                    // Address display:
                    //   send     → show recipient address (the "address" field IS the recipient)
                    //   receive  → show SENDER address (decode raw tx to find input source)
                    //   generate → show wallet's own address (mining reward)
                    std::string addr = "N/A";
                    std::string addrLabel = T().tx_address_label;

                    if (isSend) {
                        addr = tx.exists("address") ? tx["address"].get_str() : "N/A";
                        addrLabel = T().tx_to_label;
                    } else if (isGenerate) {
                        addr = tx.exists("address") ? tx["address"].get_str() : "N/A";
                        addrLabel = T().tx_address_label;
                    } else {
                        // For receive transactions, listtransactions returns the wallet's
                        // OWN address in the "address" field, not the sender's address.
                        // Decode the raw transaction to find the actual sender.
                        addrLabel = T().tx_from_label;
                        addr = GetSenderAddress(txid, g_current_wallet_name);
                        if (addr.empty()) {
                            // Fallback: show wallet's own address if sender lookup fails
                            addr = tx.exists("address") ? tx["address"].get_str() : "N/A";
                            addrLabel = T().tx_address_label;
                        }
                    }
                    if (addr.length() > 48) addr = addr.substr(0, 48);

                    // Time
                    std::string timeStr = "N/A";
                    if (tx.exists("time")) {
                        int64_t ttime = tx["time"].getInt<int64_t>();
                        char tbuf[64];
                        time_t tt = (time_t)ttime;
                        struct tm tmv;
#ifdef _WIN32
                        localtime_s(&tmv, &tt);
#else
                        localtime_r(&tt, &tmv);
#endif
                        strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M", &tmv);
                        timeStr = tbuf;
                    }

                    // Confirmations
                    int confs = 0;
                    if (tx.exists("confirmations")) confs = std::stoi(tx["confirmations"].getValStr());

                    // Print transaction detail
                    std::cout << std::string(70, '-') << "\n";
                    printf("%-4s %-6s %s\n", "#", "", dirStr.c_str());
                    printf("    %s: %s%.8f\n", T().tx_amount_label.c_str(), isSend ? "-" : "+", amount);
                    printf("    %s: %s\n", addrLabel.c_str(), addr.c_str());
                    printf("    %s: %s\n", T().tx_time_label.c_str(), timeStr.c_str());
                    printf("    %s: %d\n", T().tx_confirmations_label.c_str(), confs);
                    printf("    %s: %s\n", T().tx_id_label.c_str(), txid.c_str());
                    shown++;
                }
                if (shown == 0) {
                    std::cout << T().no_transactions;
                }
                std::cout << std::string(70, '-') << "\n";
            }
        } else {
            std::cout << T().error_prefix << TranslateRpcError(txs.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void ShowMiningInfo() {
    ClearScreen();
    std::cout << T().mining_title;
    try {
        UniValue mining = CallRPCSimple("getmininginfo");
        if (mining.find_value("error").isNull()) {
            const UniValue& m = mining.find_value("result");
            std::cout << T().mining_blocks_label << m["blocks"].getValStr() << "\n";
            std::cout << T().mining_difficulty_label << m["difficulty"].getValStr() << "\n";
            std::cout << T().label_network_hs << m["networkhashps"].getValStr() << "\n";
            std::cout << T().label_pooled_tx << m["pooledtx"].getValStr() << "\n";
            std::cout << T().label_chain << m["chain"].get_str() << "\n";
            
            UniValue bc = CallRPCSimple("getblockcount");
            if (bc.find_value("error").isNull()) {
                std::cout << "\n" << T().label_current_height << bc.find_value("result").getValStr() << "\n";
            }
        } else {
            std::cout << T().error_prefix << TranslateRpcError(mining.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    }
    PressContinue();
}

static void GenerateNewAddress() {
    ClearScreen();
    std::cout << T().generate_addr_title;

    // Security fix (#R11-7): Require login — generating addresses for unauthorized wallets
    // could lead to users sending funds to addresses they don't control
    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet << "\n";
        PressContinue(); return;
    }
    
    try {
        // token only uses Bech32 — no address type selection needed
        UniValue addr = CallRPCSimple("getnewaddress", {"bech32"}, g_current_wallet_name);
        
        if (addr.find_value("error").isNull()) {
            std::string newAddr = addr.find_value("result").get_str();
            g_current_address = newAddr;  // Update cached address
            std::cout << "\n" << T().generate_addr_success << newAddr << "\n";
        } else {
            std::cout << T().error_prefix << TranslateRpcError(addr.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

static void ChangePassword() {
    ClearScreen();
    std::cout << T().change_pass_title;

    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet;
        PressContinue();
        return;
    }

    // Check if wallet is encrypted
    {
        UniValue walletInfo = CallRPCSimple("getwalletinfo", {}, g_current_wallet_name);
        if (!walletInfo.find_value("error").isNull()) {
            std::cout << T().error_prefix << TranslateRpcError(walletInfo.find_value("error")["message"].get_str()) << "\n";
            PressContinue(); return;
        }
        bool isEncrypted = walletInfo.find_value("result").exists("unlocked_until");
        if (!isEncrypted) {
            std::cout << T().change_pass_not_encrypted;
            PressContinue(); return;
        }
    }

    std::string oldPass = GetHiddenInput(T().change_pass_old);
    if (oldPass.empty()) {
        std::cout << T().invalid_input << "\n";
        PressContinue(); return;
    }

    std::string newPass = GetHiddenInput(T().change_pass_new);
    if (newPass.length() < 8) {
        std::cout << T().password_too_short << "\n";
        PressContinue(); return;
    }
    if (newPass.length() > 128) {
        std::cout << T().error_prefix << "Password too long (max 128 characters).\n";
        PressContinue(); return;
    }

    std::string confirmPass = GetHiddenInput(T().change_pass_confirm);
    if (newPass != confirmPass) {
        std::cout << T().change_pass_mismatch;
        SecureClear(oldPass); SecureClear(newPass); SecureClear(confirmPass);
        PressContinue(); return;
    }

    try {
        UniValue result = CallRPCSimple("walletpassphrasechange", {oldPass, newPass}, g_current_wallet_name);
        SecureClear(oldPass); SecureClear(newPass); SecureClear(confirmPass);
        if (result.find_value("error").isNull()) {
            std::cout << T().change_pass_success;
        } else {
            std::cout << T().change_pass_failed << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        SecureClear(oldPass); SecureClear(newPass); SecureClear(confirmPass);
        std::cout << T().change_pass_failed << e.what() << "\n";
    } catch (...) {
        SecureClear(oldPass); SecureClear(newPass); SecureClear(confirmPass);
        std::cout << T().change_pass_failed << "Unknown error.\n";
    }
    PressContinue();
}

static void SetAddressLabel() {
    ClearScreen();
    std::cout << T().addr_label_title;

    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet;
        PressContinue(); return;
    }

    // Show all addresses with current labels
    try {
        UniValue addrs = CallRPCSimple("listreceivedbyaddress", {"0", "true", "true"}, g_current_wallet_name);
        if (addrs.find_value("error").isNull()) {
            const UniValue& addrList = addrs.find_value("result");
            for (size_t i = 0; i < addrList.size(); i++) {
                std::string addr = addrList[i]["address"].get_str();
                std::string label = addrList[i].exists("label") ? addrList[i]["label"].get_str() : "";
                if (addr == g_current_address)
                    std::cout << "  " << addr << (label.empty() ? "" : " [" + label + "]") << " " << T().label_primary << "\n";
                else
                    std::cout << "  " << addr << (label.empty() ? "" : " [" + label + "]") << "\n";
            }
        }
    } catch (...) {}

    std::string addr = GetInputEsc(T().addr_label_prompt);
    if (addr == "\x1B" || addr.empty()) {
        std::cout << T().invalid_input << "\n";
        PressContinue(); return;
    }

    // Verify address belongs to this wallet
    try {
        UniValue addrInfo = CallRPCSimple("getaddressinfo", {addr}, g_current_wallet_name);
        const UniValue& addrResult = addrInfo.find_value("result");
        bool isMine = addrResult.exists("ismine") && addrResult["ismine"].isBool() && addrResult["ismine"].get_bool();
        if (!addrInfo.find_value("error").isNull() || !isMine) {
            std::cout << T().addr_label_not_found;
            PressContinue(); return;
        }
    } catch (...) {
        std::cout << T().addr_label_not_found;
        PressContinue(); return;
    }

    // Get label name (empty to remove label)
    std::string labelName = GetInput(T().addr_label_name_prompt);

    try {
        UniValue result = CallRPCSimple("setlabel", {addr, labelName}, g_current_wallet_name);
        if (result.find_value("error").isNull()) {
            std::cout << (labelName.empty() ? T().addr_label_removed : T().addr_label_success);
        } else {
            std::cout << T().addr_label_failed << TranslateRpcError(result.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().addr_label_failed << e.what() << "\n";
    } catch (...) {
        std::cout << T().addr_label_failed << "Unknown error.\n";
    }
    PressContinue();
}

static void SignMessage() {
    ClearScreen();
    std::cout << T().sign_title;
    
    if (!g_is_logged_in) {
        std::cout << T().info_no_wallet << "\n";
        PressContinue();
        return;
    }

    // Security fix (#R9-3): Ensure wallet is unlocked before signing.
    // BTC Core: signmessage calls EnsureWalletIsUnlocked() internally.
    // Same pattern as SendTKNC (#R9-2) — check lock status, prompt if needed.
    {
        UniValue walletInfo = CallRPCSimple("getwalletinfo", {}, g_current_wallet_name);
        if (!walletInfo.find_value("error").isNull()) {
            std::cout << T().error_prefix << TranslateRpcError(walletInfo.find_value("error")["message"].get_str()) << "\n";
            PressContinue(); return;
        }
        bool isEncrypted = walletInfo.find_value("result").exists("unlocked_until");
        bool isLocked = false;
        if (isEncrypted) {
            try {
                int64_t unlockedUntil = walletInfo.find_value("result")["unlocked_until"].getInt<int64_t>();
                isLocked = (unlockedUntil == 0);
            } catch (...) { isLocked = true; }
        }
        if (isLocked) {
            std::string pass = GetHiddenInput(T().sign_wallet_locked);
            if (pass.empty()) {
                std::cout << T().error_prefix << T().sign_must_unlock;
                PressContinue(); return;
            }
            UniValue unlockResult = CallRPCSimple("walletpassphrase", {pass, "600"}, g_current_wallet_name);
            if (!unlockResult.find_value("error").isNull()) {
                std::cout << T().error_prefix << TranslateRpcError(unlockResult.find_value("error")["message"].get_str()) << "\n";
                SecureClear(pass);
                PressContinue(); return;
            }
            SecureClear(pass);
        }
    }

    // ===== Unified signing logic (same as command-line: tknc-cli signmessage <address> <message>) =====
    // Show the current wallet's addresses and let the user pick which one to sign with.
    // This ensures the interactive menu uses the SAME logic as the command-line path:
    //   tknc-cli signmessage <address> <message>
    // The user can copy the address from the web page and paste it here.

    // List all addresses in the current wallet
    std::vector<std::string> walletAddresses;
    try {
        UniValue addrs = CallRPCSimple("listreceivedbyaddress", {"0", "true", "true"}, g_current_wallet_name);
        if (addrs.find_value("error").isNull()) {
            const UniValue& addrList = addrs.find_value("result");
            for (size_t i = 0; i < addrList.size(); i++) {
                if (addrList[i].exists("address")) {
                    walletAddresses.push_back(addrList[i]["address"].get_str());
                }
            }
        }
    } catch (...) {}

    // Default to g_current_address (first address with balance, or first overall)
    std::string defaultAddress = g_current_address;
    if (defaultAddress.empty() && !walletAddresses.empty()) {
        defaultAddress = walletAddresses[0];
        g_current_address = defaultAddress;
    }
    if (defaultAddress.empty()) {
        try {
            UniValue a = CallRPCSimple("getnewaddress", {"bech32"}, g_current_wallet_name);
            if (a.find_value("error").isNull()) {
                defaultAddress = a.find_value("result").get_str();
                g_current_address = defaultAddress;
            }
        } catch (...) {}
    }

    // Display available addresses so the user knows which address will be used
    std::cout << "\n" << T().wallet_name_label2 << g_current_wallet_name << "\n";
    if (!walletAddresses.empty()) {
        std::cout << "Addresses in this wallet:\n";
        for (size_t i = 0; i < walletAddresses.size(); i++) {
            std::cout << "  [" << (i + 1) << "] " << walletAddresses[i];
            if (walletAddresses[i] == defaultAddress) std::cout << "  <- default";
            std::cout << "\n";
        }
        std::cout << "\n";
    }

    // Let the user enter an address (default = current address, or select by number)
    // This matches the command-line: tknc-cli signmessage <address> <message>
    std::string address = defaultAddress;
    std::cout << "Enter address to sign with (or number from list above, or Enter for default):\n";
    std::string addrInput = GetInputEsc("> ");
    if (addrInput == "\x1B") { PressContinue(); return; }
    if (!addrInput.empty()) {
        // Check if user entered a number (select from list)
        bool isNumber = true;
        for (char c : addrInput) { if (!std::isdigit(static_cast<unsigned char>(c))) { isNumber = false; break; } }
        if (isNumber && !walletAddresses.empty()) {
            int idx = std::stoi(addrInput);
            if (idx >= 1 && idx <= static_cast<int>(walletAddresses.size())) {
                address = walletAddresses[idx - 1];
            } else {
                std::cout << "Invalid selection. Using default address.\n";
            }
        } else {
            // User entered a full address (e.g., pasted from web page)
            address = addrInput;
        }
    }

    if (address.empty()) {
        std::cout << T().send_failed << T().sign_addr_empty;
        PressContinue(); return;
    }
    if (address.size() > 90) {
        std::cout << T().send_failed << T().sign_addr_too_long;
        PressContinue(); return;
    }

    // Show which address will be used for signing (so user can verify it matches the web page)
    std::cout << "\nSigning with address: " << address << "\n";
    std::cout << "(This address must match the one you entered on the web page.)\n\n";

    std::string message = GetInput(T().sign_message_prompt);

    // Security fix (#R10-4): Limit message length to prevent excessive signing time
    if (message.size() > 4096) {
        std::cout << T().error_prefix << T().sign_msg_too_long;
        PressContinue(); return;
    }

    // Security fix (#50): Verify address belongs to current wallet before signing
    try {
        UniValue addrInfo = CallRPCSimple("getaddressinfo", {address}, g_current_wallet_name);
        const UniValue& addrResult = addrInfo.find_value("result");
        bool isMine = addrResult.exists("ismine") && addrResult["ismine"].isBool() && addrResult["ismine"].get_bool();
        if (!addrInfo.find_value("error").isNull() || !isMine) {
            std::cout << T().send_failed << T().sign_addr_not_owned;
            std::cout << T().sign_own_address_only;
            PressContinue(); return;
        }
    } catch (...) {
        std::cout << T().send_failed << T().sign_verify_failed;
        PressContinue(); return;
    }

    try {
        UniValue sign = CallRPCSimple("signmessage", {address, message}, g_current_wallet_name);
        if (sign.find_value("error").isNull()) {
            std::cout << "\n" << T().sign_result_label << sign.find_value("result").get_str() << "\n";
        } else {
            std::cout << T().error_prefix << TranslateRpcError(sign.find_value("error")["message"].get_str()) << "\n";
        }
    } catch (const std::exception& e) {
        std::cout << T().error_prefix << e.what() << "\n";
    } catch (...) {
        std::cout << T().error_prefix << "Unexpected error.\n";
    }
    PressContinue();
}

// Get list of available wallets from wallet directory (without loading them into the node).
// Returns wallet names found via listwalletdir RPC.
static std::vector<std::string> GetAvailableWallets() {
    std::vector<std::string> result;

    // 1. Discover wallets in the node's wallet directory via listwalletdir RPC
    UniValue dirResult = CallRPCSimple("listwalletdir");
    if (!dirResult.find_value("error").isNull()) {
        return result; // listwalletdir not available
    }

    const UniValue& dirObj = dirResult.find_value("result");
    if (dirObj.isObject() && dirObj.exists("wallets")) {
        const UniValue& wallets = dirObj["wallets"];
        for (size_t i = 0; i < wallets.size(); i++) {
            if (!wallets[i].isObject() || !wallets[i].exists("name")) continue;
            result.push_back(wallets[i]["name"].get_str());
        }
    }

    return result;
}

static void AutoLoadWallets() {
    // Do NOT auto-load all wallets into the node.
    // Wallets should only be loaded when the user explicitly opens them.
    // This function is kept for compatibility but does nothing — wallet loading
    // is deferred to OpenWallet() which calls loadwallet on user selection.
    GetAvailableWallets(); // just probes availability; does not load
}

static void RunInteractiveMode() {
    InitI18N();
    
    std::cout << T().msg_connecting;
    try {
        UniValue test = CallRPCSimple("getblockcount");
        if (!test.find_value("error").isNull()) {
            const UniValue& err = test.find_value("error");
            std::string errMsg = err.isObject() ? err["message"].getValStr() : err.getValStr();
            ClearScreen();
            std::cout << "\n========================================\n";
            std::cout << T().err_cannot_connect;
            std::cout << "========================================\n\n";
            std::cout << "Error: " << errMsg << "\n\n";
            std::cout << T().msg_ensure_running;
            std::cout << T().msg_press_exit;
            std::cin.get();
            return;
        }
        std::cout << T().msg_ok;
    } catch (const std::exception& e) {
        ClearScreen();
        std::cout << "\n========================================\n";
        std::cout << T().err_cannot_connect;
        std::cout << "========================================\n\n";
        std::cout << "Error: " << e.what() << "\n\n";
        std::cout << T().msg_ensure_running;
        std::cout << T().msg_press_exit;
        std::cin.get();
        return;
    } catch (...) {
        ClearScreen();
        std::cout << "\n========================================\n";
        std::cout << T().err_cannot_connect;
        std::cout << "========================================\n\n";
        std::cout << T().msg_ensure_running;
        std::cout << T().msg_press_exit;
        std::cin.get();
        return;
    }
    
    ShowLanguageSelection();

    // Auto-load wallets from wallets/ directory next to the CLI executable
    AutoLoadWallets();

    while (true) {
        int choice;
        if (g_is_logged_in) {
            // Security fix (#R15-5): Defensive check — g_current_wallet_name should never
            // be empty when g_is_logged_in is true, but guard against future code changes
            // that might break this invariant.
            if (g_current_wallet_name.empty()) {
                std::cout << T().error_prefix << "No wallet selected. Please log in again.\n";
                g_is_logged_in = false;
                g_current_address = "";
                PressContinue(); continue;
            }
            choice = ShowWalletMenu();
            switch (choice) {
                case 1:
                    // Show wallet addresses (address-book only, no change addresses)
                    // and total wallet balance. Per-address balances are NOT shown
                    // because the UTXO model makes them misleading — a send can
                    // drain one address to 0 while the total wallet balance stays
                    // nearly the same. Users should only care about total balance.
                    {
                        ClearScreen();
                        std::cout << T().info_title << "\n";
                        std::vector<std::string> addrVec;
                        try {
                            // Fetch address-book addresses only (listreceivedbyaddress
                            // already filters out change addresses via "is_change" check).
                            UniValue addrs = CallRPCSimple("listreceivedbyaddress",
                                {"0", "true", "true"}, g_current_wallet_name);
                            const UniValue& addrList = addrs.find_value("error").isNull()
                                ? addrs.find_value("result") : NullUniValue;

                            // Build ordered, de-duplicated address list:
                            //   1) primary address first (if set)
                            //   2) address-book addresses (non-change)
                            std::set<std::string> seen;
                            auto pushAddr = [&](const std::string& addr) {
                                if (addr.empty() || seen.count(addr)) return;
                                seen.insert(addr);
                                addrVec.push_back(addr);
                            };

                            if (!g_current_address.empty()) pushAddr(g_current_address);
                            for (size_t i = 0; i < addrList.size(); i++) {
                                if (addrList[i].exists("address"))
                                    pushAddr(addrList[i]["address"].get_str());
                            }

                            // Show total wallet balance (not per-address balances)
                            double totalBal = 0.0;
                            try {
                                UniValue bal = CallRPCSimple("getbalance", {}, g_current_wallet_name);
                                if (bal.find_value("error").isNull())
                                    totalBal = std::stod(bal.find_value("result").getValStr());
                            } catch (...) {}
                            char totalBuf[64];
                            snprintf(totalBuf, sizeof(totalBuf), "%.8f", totalBal);
                            std::cout << "  " << T().wallet_total_balance_label << totalBuf << " token\n\n";

                            // Display address-book addresses (without individual balances)
                            if (addrVec.empty()) {
                                std::cout << "  (no addresses)\n";
                            } else {
                                for (size_t i = 0; i < addrVec.size(); i++) {
                                    const std::string& addr = addrVec[i];
                                    std::string marker = (addr == g_current_address)
                                        ? " " + T().label_primary : "";
                                    std::cout << "  " << (i+1) << ". " << addr << marker << "\n";
                                }
                            }
                        } catch (...) {
                            if (!g_current_address.empty()) {
                                std::cout << "  1. " << g_current_address
                                          << " " << T().label_primary << "\n";
                                addrVec.push_back(g_current_address);
                            }
                        }
                        // Allow selecting an address to switch to
                        if (!addrVec.empty()) {
                            std::cout << "\n" << T().select_address_prompt;
                            std::string sel = GetInput("");
                            if (!sel.empty() && sel != "q" && sel != "Q") {
                                try {
                                    int idx = std::stoi(sel);
                                    if (idx > 0 && idx <= (int)addrVec.size()) {
                                        g_current_address = addrVec[idx - 1];
                                        std::cout << T().addr_switched_msg << g_current_address << "\n";
                                    }
                                } catch (...) {}
                            }
                        }
                    }
                    PressContinue(); break;
                case 2: SendTKNC(); break;
                case 3: ShowTransactions(); break;
                case 4: BackupKeys(); break;
                case 5: GenerateNewAddress(); break;
                case 6: SetAddressLabel(); break;
                case 7: ChangePassword(); break;
                case 8: SignMessage(); break;
                case 9: ShowInferenceMenu(); break;
                case -2: // q. back / ESC — back to public menu
                    if (!g_current_wallet_name.empty()) {
                        try { CallRPCSimple("walletlock", {}, g_current_wallet_name); } catch (...) {}
                        try {
                            UniValue unloadResult = CallRPCSimple("unloadwallet", {g_current_wallet_name}, "");
                            if (!unloadResult.find_value("error").isNull()) {
                                std::cerr << "Warning: unloadwallet failed: "
                                          << TranslateRpcError(unloadResult.find_value("error")["message"].get_str()) << "\n";
                            }
                        } catch (...) {}
                    }
                    g_is_logged_in = false;
                    g_current_wallet_name = "";
                    g_current_address = "";
                    break;
                default:
                    std::cout << T().invalid_input << "\n";
                    PressContinue();
                    break;
            }
        } else {
            choice = ShowPublicMenu();
            switch (choice) {
                case 1: CreateNewWallet(); break;
                case 2: RestoreWallet(); break;
                case 3: OpenWallet(); break;
                case 4: ShowNodeInfo(); break;
                case 5: ShowBlockchainInfo(); break;
                case 6: ShowNetworkInfo(); break;
                case 7: BlockExplorer(); break;
                case 8: ShowMiningInfo(); break;
                case 0:
                    ShowLanguageSelection();
                    break;
                case -2: // ESC — back to language selection
                    ShowLanguageSelection();
                    break;
                default:
                    std::cout << T().invalid_input << "\n";
                    PressContinue();
                    break;
            }
        }
    }
}

// ==================== End Interactive Menu System ====================

MAIN_FUNCTION
{
    SetupEnvironment();

#ifdef _WIN32
    // UTF-8 console: SetupEnvironment() set SetConsoleOutputCP(CP_UTF8), but MSVCRT stdout
    // still translates bytes via the local codepage (GBK on zh-CN), garbling multilingual text.
    // Binary mode lets UTF-8 bytes pass through unchanged. Same as tknc-miner.
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    // CRITICAL: Initialize ECC (secp256k1 context) before any crypto operations
    // Without this, secp256k1_context_sign = nullptr → GetPubKey() crashes with ACCESS_VIOLATION
    ECC_Context ecc_context;

    if (!SetupNetworking()) {
        tfm::format(std::cerr, "Error: Initializing networking failed\n");
        return EXIT_FAILURE;
    }
    // Ensure wallets directory exists at root level (CLI owns wallet management)
    TryCreateDirectories(GetExeDir() / "wallets");
    event_set_log_callback(&libevent_log_cb);

    bool hasRpcPort = false;
    bool hasRpcConnect = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-rpcport") == 0 || strncmp(argv[i], "-rpcport=", 9) == 0) hasRpcPort = true;
        if (strcmp(argv[i], "-rpcconnect") == 0 || strncmp(argv[i], "-rpcconnect=", 12) == 0) hasRpcConnect = true;
    }

    if (!hasRpcPort) gArgs.ForceSetArg("-rpcport", "9331");
    // Do NOT ForceSetArg rpcuser/rpcpassword — let CLI/node share config file (matches Bitcoin Core: credentials auto-match via cmd line, tknc.conf, or .cookie).
    if (!hasRpcConnect) gArgs.ForceSetArg("-rpcconnect", "127.0.0.1");
    
    bool hasChain = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-chain") == 0 || strncmp(argv[i], "-chain=", 7) == 0 ||
            strcmp(argv[i], "-testnet") == 0 || strcmp(argv[i], "-signet") == 0) {
            hasChain = true;
            break;
        }
    }
    if (!hasChain) gArgs.ForceSetArg("-chain", "main");

    // CRITICAL: Initialize full chain parameters (including globalChainParams)
    // Without this, Params() asserts nullptr → crash at tknc_chainparams.cpp:283
    SelectParams(gArgs.GetChainType());
    
    bool hasCommand = false;
    for (int i = 1; i < argc; i++) {
        if (!IsSwitchChar(argv[i][0])) {
            hasCommand = true;
            break;
        }
        if (strcmp(argv[i], "-getinfo") == 0 || strcmp(argv[i], "-netinfo") == 0 ||
            strcmp(argv[i], "-generate") == 0 || strcmp(argv[i], "-addrinfo") == 0) {
            hasCommand = true;
            break;
        }
    }

    if (!hasCommand) {
        try {
            int ret = AppInitRPC(argc, argv);
            // Security fix (#42): CONTINUE_EXECUTION(-1) means "run CLI RPC command"
            // EXIT_SUCCESS(0) or EXIT_FAILURE(1) means "done showing help/error → exit"
            // Original code had this condition INVERTED
            if (ret == CONTINUE_EXECUTION) {
                // Has a CLI RPC command to execute — run it below (falls through to CommandLineRPC)
            } else {
                // No command / help shown / error — enter interactive mode or exit
                if (ret == EXIT_SUCCESS) {
                    // -help or similar: show help then enter interactive mode
                    RunInteractiveMode();
                    return 0;
                }
                // Error: just exit with error code
                return ret;
            }
        } catch (...) {
            // AppInitRPC threw exception — fall through to interactive mode
            RunInteractiveMode();
            return 0;
        }
        // No CLI command and no error from AppInitRPC → interactive mode
        RunInteractiveMode();
        return 0;
    }

    try {
        int ret = AppInitRPC(argc, argv);
        if (ret != CONTINUE_EXECUTION) {
            std::cout << "\n========================================\n";
            std::cout << T().err_cli_error;
            std::cout << "========================================\n\n";
            std::cout << T().err_code_label << ret << "\n\n";
            std::cout << T().msg_press_exit;
            std::cin.get();
            return ret;
        }
    }
    catch (const std::exception& e) {
        std::cout << "\n========================================\n";
        std::cout << T().err_cli_error;
        std::cout << "========================================\n\n";
        std::cout << T().err_details_label << e.what() << "\n\n";
        std::cout << T().msg_press_exit;
        std::cin.get();
        PrintExceptionContinue(&e, "AppInitRPC()");
        return EXIT_FAILURE;
    } catch (...) {
        std::cout << "\n" << T().err_unknown;
        std::cout << T().msg_press_exit;
        std::cin.get();
        PrintExceptionContinue(nullptr, "AppInitRPC()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRPC(argc, argv);
    }
    catch (const std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRPC()");
    } catch (...) {
        PrintExceptionContinue(nullptr, "CommandLineRPC()");
    }
    return ret;
}
