#ifndef TKNC_NET_RPC_H
#define TKNC_NET_RPC_H

#include <string>

// Get the public IP override set via setpublicip RPC
std::string GetPublicIPOverride();

// Get the public IP to use: override first, then auto-detect
std::string GetEffectivePublicIP();

#endif