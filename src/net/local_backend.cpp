#include <net/local_backend.h>
#include <util/log.h>
#include <cstring>  // for memset (Linux compatibility)
#include <vector>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif

LocalBackend::LocalBackend(const std::string& host, uint16_t port)
    : m_host(host), m_port(port)
{
}

ComputeResponse LocalBackend::Infer(const ComputeRequest& request)
{
    ComputeResponse result;

#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif

#ifdef WIN32
    if (sock == INVALID_SOCKET) {
#else
    if (sock < 0) {
#endif
        result.error_message = "Failed to create socket";
        result.health = HealthStatus::UNHEALTHY;
        return result;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#ifdef WIN32
    if (cr != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv; tv.tv_sec = 2; tv.tv_usec = 0;
        if (select(0, NULL, &ws, NULL, &tv) > 0) cr = 0;
    }
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    if (cr != 0) {
        closesocket(sock);
        result.success = false;
        result.error_message = "Local miner not available at " + m_host + ":" + std::to_string(m_port);
        result.health = HealthStatus::OFFLINE;
        return result;
    }

    // Build JSON body from request messages
    std::string combined_prompt;
    for (const auto& msg : request.messages) {
        if (!combined_prompt.empty()) combined_prompt += " ";
        combined_prompt += msg.content;
    }
    std::string escaped_prompt = EscapeJson(combined_prompt);

    // D-M03-FIX: Reject empty API keys instead of using hardcoded test bypass (was security backdoor)
    // Empty api_key means no authentication - must reject, not auto-bypass with fake key
    if (request.api_key.empty()) {
        closesocket(sock);
        result.success = false;
        result.error_message = "API key required for local backend requests";
        result.health = HealthStatus::OFFLINE;
        return result;
    }
    std::string effective_key = request.api_key;
    std::string escaped_model = EscapeJson(request.model);

    std::string jsonBody = "{\"api_key\":\"" + effective_key
        + "\",\"model\":\"" + escaped_model
        + "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + escaped_prompt + "\"}]}";

    // Build HTTP request
    std::string httpRequest = "POST /api/v1/chat HTTP/1.1\r\n";
    httpRequest += "Host: " + m_host + ":" + std::to_string(m_port) + "\r\n";
    httpRequest += "Content-Type: application/json\r\n";
    httpRequest += "Content-Length: " + std::to_string(jsonBody.length()) + "\r\n";
    httpRequest += "Connection: close\r\n\r\n";
    httpRequest += jsonBody;

    send(sock, httpRequest.c_str(), (int)httpRequest.length(), 0);

    // Receive response
    std::vector<char> buffer(32768);  // Heap allocation to avoid stack overflow
    int total = 0;
    while (total < (int)buffer.size() - 1) {
        int r = recv(sock, buffer.data() + total, (int)(buffer.size() - total - 1), 0);
        if (r <= 0) break;
        total += r;
    }
    buffer[total] = '\0';
    closesocket(sock);

    std::string responseBody(buffer.data(), total);
    size_t headerEnd = responseBody.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        result.error_message = "Invalid HTTP response from local miner";
        result.health = HealthStatus::UNHEALTHY;
        return result;
    }

    std::string jsonData = responseBody.substr(headerEnd + 4);

    // Extract content field (try multiple field names for compatibility)
    std::string content;
    size_t cpos = std::string::npos;
    const char* fields[] = {"content", "response", "text"};
    for (auto f : fields) {
        std::string pattern = std::string("\"") + f + "\"";
        size_t pos = jsonData.find(pattern);
        if (pos != std::string::npos) { cpos = pos; break; }
    }

    if (cpos != std::string::npos) {
        size_t colonPos = jsonData.find(":", cpos);
        if (colonPos != std::string::npos) {
            size_t start = colonPos + 1;
            while (start < jsonData.size() && (jsonData[start] == ' ' || jsonData[start] == '"')) start++;
            size_t end = jsonData.find("\"", start);
            if (end != std::string::npos) {
                content = jsonData.substr(start, end - start);
                // Unescape JSON strings
                size_t ep = 0;
                while ((ep = content.find("\\n", ep)) != std::string::npos) { content.replace(ep, 2, "\n"); ep++; }
                while ((ep = content.find("\\\"", ep)) != std::string::npos) { content.replace(ep, 2, "\""); ep++; }
            }
        }
    }

    // Node independently computes tokens_used from prompt_tokens + completion_tokens
    int64_t prompt_tokens = 0;
    int64_t completion_tokens = 0;
    {
        size_t pt_pos = jsonData.find("\"prompt_tokens\"");
        if (pt_pos != std::string::npos) {
            size_t colon = jsonData.find(":", pt_pos);
            if (colon != std::string::npos) {
                size_t valEnd = jsonData.find(",", colon);
                if (valEnd == std::string::npos) valEnd = jsonData.find("}", colon);
                if (valEnd != std::string::npos) {
                    try { prompt_tokens = std::stoll(jsonData.substr(colon + 1, valEnd - colon - 1)); } catch (...) {}
                }
            }
        }
        size_t ct_pos = jsonData.find("\"completion_tokens\"");
        if (ct_pos != std::string::npos) {
            size_t colon = jsonData.find(":", ct_pos);
            if (colon != std::string::npos) {
                size_t valEnd = jsonData.find(",", colon);
                if (valEnd == std::string::npos) valEnd = jsonData.find("}", colon);
                if (valEnd != std::string::npos) {
                    try { completion_tokens = std::stoll(jsonData.substr(colon + 1, valEnd - colon - 1)); } catch (...) {}
                }
            }
        }
    }
    result.tokens_used = prompt_tokens + completion_tokens;

    if (!content.empty()) {
        result.success = true;
        result.content = content;
        result.cost = result.tokens_used;
        result.health = HealthStatus::HEALTHY;
    } else {
        result.success = false;
        result.error_message = "Local miner returned unparseable response. Raw: " + jsonData.substr(0, 200);
        result.health = HealthStatus::DEGRADED;
    }

    return result;
}

HealthStatus LocalBackend::CheckHealth() const
{
    // Quick TCP connect test to determine health
#ifdef WIN32
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
#ifdef WIN32
    if (sock == INVALID_SOCKET) return HealthStatus::UNHEALTHY;
#else
    if (sock < 0) return HealthStatus::UNHEALTHY;
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);

#ifdef WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    int cr = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

#ifdef WIN32
    if (cr != 0 && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set ws; FD_ZERO(&ws); FD_SET(sock, &ws);
        timeval tv; tv.tv_sec = 1; tv.tv_usec = 0;
        cr = select(0, NULL, &ws, NULL, &tv) > 0 ? 0 : -1;
    }
    mode = 0;
    ioctlsocket(sock, FIONBIO, &mode);
#endif

    closesocket(sock);
    return (cr == 0) ? HealthStatus::HEALTHY : HealthStatus::OFFLINE;
}

std::string LocalBackend::EscapeJson(const std::string& raw)
{
    std::string escaped = raw;
    size_t p = 0;
    while ((p = escaped.find("\"", p)) != std::string::npos) {
        escaped.replace(p, 1, "\\\"");
        p += 2;
    }
    p = 0;
    while ((p = escaped.find("\n", p)) != std::string::npos) {
        escaped.replace(p, 1, "\\n");
        p++;
    }
    return escaped;
}
