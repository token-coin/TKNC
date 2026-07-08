#ifndef TKN_NET_NAT_TRAVERSAL_H
#define TKN_NET_NAT_TRAVERSAL_H

#include <cstdint>
#include <string>
#include <vector>

#ifdef WIN32
#include <objbase.h>
#endif

enum class NATStrategy {
    NONE = 0,
    UPNP = 1,
    NAT_PMP = 2,
    HOLE_PUNCHING = 3
};

class NATTraversal {
private:
    uint16_t local_port;
    std::string external_ip;
    uint16_t external_port;
    NATStrategy current_strategy;
    bool com_initialized;

public:
    NATTraversal() : local_port(0), external_port(0), current_strategy(NATStrategy::NONE), com_initialized(false) {
#ifdef WIN32
        HRESULT hr = CoInitialize(nullptr);
        com_initialized = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
#endif
    }

    ~NATTraversal() {
#ifdef WIN32
        if (com_initialized) {
            CoUninitialize();
        }
#endif
    }

    bool TryUPnP(uint16_t port);

    bool TryNATPMP(uint16_t port);

    bool TryHolePunching(const std::string& target_ip, uint16_t target_port);

    std::string GetExternalIP() const { return external_ip; }
    uint16_t GetExternalPort() const { return external_port; }

    void Cleanup();
};

#endif // TKN_NET_NAT_TRAVERSAL_H