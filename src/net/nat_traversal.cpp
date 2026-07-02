#include <net/nat_traversal.h>
#include <util/log.h>
#include <common/args.h>
#include <util/strencodings.h>
#include <cstring>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <objbase.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ole32.lib")
#endif

// UPnP COM interface definition
MIDL_INTERFACE("B171C812-CC76-11D0-949F-00A024D55535")
IUPnPNAT : public IUnknown
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_StaticPortMappingCollection(
        struct IStaticPortMappingCollection** ppCollection) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DynamicPortMappingCollection(
        struct IDynamicPortMappingCollection** ppCollection) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_UPnPService(
        struct IUPnPService** ppUPnPService) = 0;
};

MIDL_INTERFACE("A25836C2-CC76-11D0-949F-00A024D55535")
IStaticPortMapping : public IDispatch
{
public:
    virtual HRESULT STDMETHODCALLTYPE get_ExternalIPAddress(BSTR* pbstrExternalIPAddress) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ExternalIPAddress(BSTR bstrExternalIPAddress) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Protocol(BSTR* pbstrProtocol) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Protocol(BSTR bstrProtocol) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalPort(long* plInternalPort) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_InternalPort(long plInternalPort) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ExternalPort(long* plExternalPort) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_ExternalPort(long plExternalPort) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Enabled(VARIANT_BOOL* pbEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Enabled(VARIANT_BOOL bEnabled) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Description(BSTR* pbstrDescription) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Description(BSTR bstrDescription) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_InternalClient(BSTR* pbstrInternalClient) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_InternalClient(BSTR bstrInternalClient) = 0;
};

MIDL_INTERFACE("A25836C3-CC76-11D0-949F-00A024D55535")
IStaticPortMappingCollection : public IDispatch
{
public:
    virtual HRESULT STDMETHODCALLTYPE Add(
        long lExternalPort,
        BSTR bstrProtocol,
        long lInternalPort,
        BSTR bstrInternalClient,
        VARIANT_BOOL bEnabled,
        BSTR bstrDescription,
        struct IStaticPortMapping** ppPortMapping) = 0;
    virtual HRESULT STDMETHODCALLTYPE Remove(
        long lExternalPort,
        BSTR bstrProtocol) = 0;
};

// CLSID and IID definitions
const CLSID CLSID_UPnPNAT = {0xE75A4F04, 0x949F, 0x11D0, {0x94, 0x9F, 0x00, 0xA0, 0x24, 0xD5, 0x55, 0x35}};
const IID IID_IUPnPNAT = {0xB171C812, 0xCC76, 0x11D0, {0x94, 0x9F, 0x00, 0xA0, 0x24, 0xD5, 0x55, 0x35}};
#elif defined(HAVE_MINIUPNPC)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

#include <chrono>
#include <thread>
#include <algorithm>

// UPnP constants
static const int UPNP_DISCOVER_TIMEOUT_MS = 2000;
static const int UPNP_LEASE_DURATION = 3600; // 1 hour

// NAT-PMP constants
static const int NAT_PMP_PORT = 5351;
static const int NAT_PMP_TIMEOUT_MS = 1000;

// Hole Punching constants
static const int HOLE_PUNCHING_TIMEOUT_MS = 5000;
static const int HOLE_PUNCHING_RETRY_COUNT = 3;
static const int HOLE_PUNCHING_INTERVAL_MS = 1000;

// UPnP port mapping implementation
bool NATTraversal::TryUPnP(uint16_t port) {
    LogInfo("NAT: Attempting UPnP port mapping, port %d...", port);
    
#ifdef WIN32
    // Windows UPnP implementation
    IUPnPNAT* upnp_nat = nullptr;
    IStaticPortMappingCollection* collection = nullptr;
    IStaticPortMapping* mapping = nullptr;
    
    HRESULT hr = CoCreateInstance(
        CLSID_UPnPNAT,
        nullptr,
        CLSCTX_ALL,
        IID_IUPnPNAT,
        (void**)&upnp_nat
    );
    
    if (FAILED(hr) || !upnp_nat) {
        LogInfo("NAT: UPnP not available, HRESULT: 0x%x", hr);
        return false;
    }

    hr = upnp_nat->get_StaticPortMappingCollection(&collection);
    if (FAILED(hr) || !collection) {
        LogInfo("NAT: Failed to get UPnP port mapping collection");
        upnp_nat->Release();
        return false;
    }
    
    // Add port mapping
    BSTR protocol = SysAllocString(L"TCP");
    BSTR description = SysAllocString(L"TKNC P2P Node");
    
    hr = collection->Add(
        port,
        protocol,
        port,
        nullptr,
        VARIANT_TRUE,
        description,
        &mapping
    );
    
    SysFreeString(protocol);
    SysFreeString(description);
    
    if (SUCCEEDED(hr) && mapping) {
        local_port = port;
        external_port = port;
        current_strategy = NATStrategy::UPNP;

        // Get external IP
        BSTR external_ip_bstr = nullptr;
        hr = mapping->get_ExternalIPAddress(&external_ip_bstr);
        if (SUCCEEDED(hr) && external_ip_bstr) {
            int len = WideCharToMultiByte(CP_UTF8, 0, external_ip_bstr, -1, nullptr, 0, nullptr, nullptr);
            if (len > 0) {
                std::string ip_str(len - 1, 0);
                WideCharToMultiByte(CP_UTF8, 0, external_ip_bstr, -1, &ip_str[0], len, nullptr, nullptr);
                external_ip = ip_str;
            }
            SysFreeString(external_ip_bstr);
        }
        
        LogInfo("NAT: UPnP port mapping successful: external port %d -> internal port %d", external_port, local_port);
        
        mapping->Release();
        collection->Release();
        upnp_nat->Release();
        return true;
    } else {
        LogInfo("NAT: UPnP port mapping failed, HRESULT: 0x%x", hr);
        collection->Release();
        upnp_nat->Release();
        return false;
    }
#else
    LogInfo("NAT: UPnP not available on this platform");
    return false;
#endif
}

// NAT-PMP implementation
bool NATTraversal::TryNATPMP(uint16_t port) {
    LogInfo("NAT: Attempting NAT-PMP port mapping, port %d...", port);
    
#ifdef WIN32
    // Windows NAT-PMP implementation
    // Create UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LogInfo("NAT: Failed to create NAT-PMP socket");
        return false;
    }
    
    // Set timeout
    DWORD timeout = NAT_PMP_TIMEOUT_MS;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    
    // Construct NAT-PMP request
    uint8_t request[12];
    memset(request, 0, sizeof(request));
    request[0] = 0; // Version 0
    request[1] = 1; // Request external address
    // Rest is zero
    
    // Send to gateway
    sockaddr_in gateway_addr;
    memset(&gateway_addr, 0, sizeof(gateway_addr));
    gateway_addr.sin_family = AF_INET;
    gateway_addr.sin_port = htons(NAT_PMP_PORT);
    
    // Get default gateway
    // Simplified: should use GetBestRoute in production
    inet_pton(AF_INET, "192.168.1.1", &gateway_addr.sin_addr);
    
    int sent = sendto(sock, (const char*)request, sizeof(request), 0,
                      (sockaddr*)&gateway_addr, sizeof(gateway_addr));
    
    if (sent < 0) {
        LogInfo("NAT: NAT-PMP request send failed");
        closesocket(sock);
        return false;
    }
    
    // Receive response
    uint8_t response[16];
    sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);
    
    int received = recvfrom(sock, (char*)response, sizeof(response), 0,
                            (sockaddr*)&from_addr, &from_len);
    
    if (received < 0) {
        LogInfo("NAT: NAT-PMP response receive failed");
        closesocket(sock);
        return false;
    }
    
    // Parse response
    if (response[0] == 0 && response[1] == 1 && received >= 12) {
        uint32_t external_ip_raw;
        memcpy(&external_ip_raw, &response[8], 4);
        external_ip = std::to_string((external_ip_raw >> 24) & 0xFF) + "." +
                      std::to_string((external_ip_raw >> 16) & 0xFF) + "." +
                      std::to_string((external_ip_raw >> 8) & 0xFF) + "." +
                      std::to_string(external_ip_raw & 0xFF);
        
        local_port = port;
        external_port = port;
        current_strategy = NATStrategy::NAT_PMP;
        
        LogInfo("NAT: NAT-PMP successful, external IP: %s", external_ip.c_str());
        closesocket(sock);
        return true;
    }
    
    closesocket(sock);
    return false;
#else
    // Linux NAT-PMP implementation
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        LogInfo("NAT: Failed to create NAT-PMP socket");
        return false;
    }
    
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    uint8_t request[12];
    memset(request, 0, sizeof(request));
    request[0] = 0;
    request[1] = 1;
    
    sockaddr_in gateway_addr;
    memset(&gateway_addr, 0, sizeof(gateway_addr));
    gateway_addr.sin_family = AF_INET;
    gateway_addr.sin_port = htons(NAT_PMP_PORT);
    inet_pton(AF_INET, "192.168.1.1", &gateway_addr.sin_addr);
    
    ssize_t sent = sendto(sock, request, sizeof(request), 0,
                          (sockaddr*)&gateway_addr, sizeof(gateway_addr));
    
    if (sent < 0) {
        LogInfo("NAT: NAT-PMP request send failed");
        close(sock);
        return false;
    }
    
    uint8_t response[16];
    socklen_t from_len = sizeof(gateway_addr);
    ssize_t received = recvfrom(sock, response, sizeof(response), 0,
                                (sockaddr*)&gateway_addr, &from_len);
    
    if (received < 0) {
        LogInfo("NAT: NAT-PMP response receive failed");
        close(sock);
        return false;
    }
    
    if (response[0] == 0 && response[1] == 1 && received >= 12) {
        uint32_t external_ip_raw;
        memcpy(&external_ip_raw, &response[8], 4);
        external_ip = std::to_string((external_ip_raw >> 24) & 0xFF) + "." +
                      std::to_string((external_ip_raw >> 16) & 0xFF) + "." +
                      std::to_string((external_ip_raw >> 8) & 0xFF) + "." +
                      std::to_string(external_ip_raw & 0xFF);
        
        local_port = port;
        external_port = port;
        current_strategy = NATStrategy::NAT_PMP;
        
        LogInfo("NAT: NAT-PMP successful, external IP: %s", external_ip.c_str());
        close(sock);
        return true;
    }
    
    close(sock);
    return false;
#endif
}

// Hole Punching implementation
bool NATTraversal::TryHolePunching(const std::string& target_ip, uint16_t target_port) {
    LogInfo("NAT: Attempting Hole Punching, target: %s:%d", target_ip.c_str(), target_port);
    
    for (int attempt = 0; attempt < HOLE_PUNCHING_RETRY_COUNT; ++attempt) {
        LogInfo("NAT: Hole Punching attempt %d/%d...", attempt + 1, HOLE_PUNCHING_RETRY_COUNT);
        
#ifdef WIN32
        // Windows Hole Punching implementation
        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            LogInfo("NAT: Failed to create Hole Punching socket");
            continue;
        }
        
        // Set non-blocking mode
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);
        
        // Set timeout
        DWORD timeout = HOLE_PUNCHING_TIMEOUT_MS;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
        
        // Bind to local port
        sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(local_port > 0 ? local_port : 9333);
        
        if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) == SOCKET_ERROR) {
            LogInfo("NAT: Hole Punching bind failed");
            closesocket(sock);
            continue;
        }
        
        // Send punch packet
        sockaddr_in target_addr;
        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port);
        inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr);
        
        const char* punch_msg = "TKN_HOLE_PUNCH";
        sendto(sock, punch_msg, strlen(punch_msg), 0,
               (sockaddr*)&target_addr, sizeof(target_addr));
        
        // Wait for response
        char buffer[1024];
        sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        int received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                (sockaddr*)&from_addr, &from_len);
        
        if (received > 0) {
            buffer[received] = '\0';
            if (strstr(buffer, "TKN_HOLE_PUNCH_ACK")) {
                LogInfo("NAT: Hole Punching successful, direct connection: %s:%d", target_ip.c_str(), target_port);
                closesocket(sock);
                current_strategy = NATStrategy::HOLE_PUNCHING;
                return true;
            }
        }
        
        closesocket(sock);
#else
        // Linux Hole Punching implementation
        int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            LogInfo("NAT: Failed to create Hole Punching socket");
            continue;
        }
        
        // Set non-blocking mode
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        
        struct timeval tv;
        tv.tv_sec = HOLE_PUNCHING_TIMEOUT_MS / 1000;
        tv.tv_usec = (HOLE_PUNCHING_TIMEOUT_MS % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        
        sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(local_port > 0 ? local_port : 9333);
        
        if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
            LogInfo("NAT: Hole Punching bind failed");
            close(sock);
            continue;
        }
        
        sockaddr_in target_addr;
        memset(&target_addr, 0, sizeof(target_addr));
        target_addr.sin_family = AF_INET;
        target_addr.sin_port = htons(target_port);
        inet_pton(AF_INET, target_ip.c_str(), &target_addr.sin_addr);
        
        const char* punch_msg = "TKN_HOLE_PUNCH";
        sendto(sock, punch_msg, strlen(punch_msg), 0,
               (sockaddr*)&target_addr, sizeof(target_addr));
        
        char buffer[1024];
        socklen_t from_len = sizeof(target_addr);
        ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0,
                                    (sockaddr*)&target_addr, &from_len);
        
        if (received > 0) {
            buffer[received] = '\0';
            if (strstr(buffer, "TKN_HOLE_PUNCH_ACK")) {
                LogInfo("NAT: Hole Punching successful, direct connection: %s:%d", target_ip.c_str(), target_port);
                close(sock);
                current_strategy = NATStrategy::HOLE_PUNCHING;
                return true;
            }
        }
        
        close(sock);
#endif
        
        // Wait before next attempt
        std::this_thread::sleep_for(std::chrono::milliseconds(HOLE_PUNCHING_INTERVAL_MS));
    }
    
    LogInfo("NAT: Hole Punching failed after %d retries", HOLE_PUNCHING_RETRY_COUNT);
    return false;
}

// Cleanup port mapping
void NATTraversal::Cleanup() {
    LogInfo("NAT: Cleaning up port mapping...");
    
    if (current_strategy == NATStrategy::UPNP && local_port > 0) {
#ifdef WIN32
        // Windows UPnP cleanup
        IUPnPNAT* upnp_nat = nullptr;
        IStaticPortMappingCollection* collection = nullptr;
        
        HRESULT hr = CoCreateInstance(
            CLSID_UPnPNAT,
            nullptr,
            CLSCTX_ALL,
            IID_IUPnPNAT,
            (void**)&upnp_nat
        );
        
        if (SUCCEEDED(hr) && upnp_nat) {
            hr = upnp_nat->get_StaticPortMappingCollection(&collection);
            if (SUCCEEDED(hr) && collection) {
                BSTR protocol = SysAllocString(L"TCP");
                
                hr = collection->Remove(local_port, protocol);
                if (SUCCEEDED(hr)) {
                    LogInfo("NAT: UPnP port mapping cleaned up: port %d", local_port);
                }
                
                SysFreeString(protocol);
                collection->Release();
            }
            upnp_nat->Release();
        }
#else
        LogInfo("NAT: UPnP cleanup not available on this platform");
#endif
    }
    
    local_port = 0;
    external_port = 0;
    external_ip.clear();
    current_strategy = NATStrategy::NONE;
    
    LogInfo("NAT: Cleanup complete");
}
