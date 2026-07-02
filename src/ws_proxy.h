#ifndef TKN_WS_PROXY_H
#define TKN_WS_PROXY_H

#include <string>

bool StartWSProxyServer(int port = 9332);
bool StartReverseConnect(const std::string& target_ws_url);

#endif // TKN_WS_PROXY_H