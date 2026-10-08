#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "net/upnp.h"

#include <cstring>
#include <mutex>

#include "core/log.h"

namespace mecmp::net::upnp {
namespace {

struct Url { std::string host; uint16_t port = 80; std::string path = "/"; };

bool parseUrl(const std::string& url, Url& out) {
    const size_t p = url.find("://");
    if (p == std::string::npos) return false;
    const size_t hs = p + 3;
    const size_t slash = url.find('/', hs);
    std::string hostport = url.substr(hs, slash == std::string::npos ? std::string::npos : slash - hs);
    out.path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        out.port = static_cast<uint16_t>(atoi(hostport.c_str() + colon + 1));
        hostport.resize(colon);
    }
    out.host = hostport;
    return !out.host.empty();
}

std::string headerValue(const std::string& resp, const char* name) {
    // Case-insensitive search for "\r\nNAME:".
    std::string lower = resp;
    for (auto& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    std::string key = std::string("\n") + name + ":";
    for (auto& c : key) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    const size_t at = lower.find(key);
    if (at == std::string::npos) return {};
    size_t b = at + key.size();
    while (b < resp.size() && resp[b] == ' ') ++b;
    const size_t e = resp.find_first_of("\r\n", b);
    return resp.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

std::string between(const std::string& s, const std::string& open, const std::string& close, size_t from = 0) {
    const size_t a = s.find(open, from);
    if (a == std::string::npos) return {};
    const size_t b = s.find(close, a + open.size());
    if (b == std::string::npos) return {};
    return s.substr(a + open.size(), b - a - open.size());
}

// SSDP M-SEARCH: the LOCATION of the first Internet Gateway Device that answers.
std::string discover() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return {};
    DWORD timeout = 700;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
    const char* targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:1",
                             "urn:schemas-upnp-org:service:WANIPConnection:2",
                             "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
    std::string location;
    for (int round = 0; round < 3 && location.empty(); ++round) {
        for (const char* st : targets) {
            std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                              st + "\r\n\r\n";
            sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
        }
        for (int i = 0; i < 8 && location.empty(); ++i) {
            char buf[2048];
            const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, nullptr, nullptr);
            if (n <= 0) break;
            buf[n] = 0;
            location = headerValue(std::string("\n") + buf, "location");
        }
    }
    closesocket(s);
    return location;
}

// Minimal HTTP/1.1 request; returns the body. localIp receives this PC's address on that network.
bool http(const Url& u, const std::string& method, const std::string& extraHeaders, const std::string& body,
          std::string& respBody, std::string* localIp = nullptr, int* status = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    const bool ok = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    freeaddrinfo(res);
    if (!ok) { closesocket(s); return false; }
    if (localIp) {
        sockaddr_in me{};
        int len = sizeof(me);
        getsockname(s, reinterpret_cast<sockaddr*>(&me), &len);
        char ip[64] = {};
        inet_ntop(AF_INET, &me.sin_addr, ip, sizeof(ip));
        *localIp = ip;
    }
    std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + std::to_string(u.port) +
                      "\r\nConnection: close\r\n" + extraHeaders;
    if (!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "\r\n" + body;
    send(s, req.data(), static_cast<int>(req.size()), 0);
    std::string resp;
    char buf[4096];
    for (;;) {
        const int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        resp.append(buf, n);
        if (resp.size() > 256 * 1024) break;
    }
    closesocket(s);
    const size_t hdrEnd = resp.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) return false;
    if (status) *status = atoi(resp.c_str() + resp.find(' ') + 1);
    respBody = resp.substr(hdrEnd + 4);
    // Chunked bodies (rare for these small replies): strip chunk size lines naively.
    if (headerValue(resp.substr(0, hdrEnd), "transfer-encoding").find("chunked") != std::string::npos) {
        std::string out;
        size_t p = 0;
        while (p < respBody.size()) {
            const size_t eol = respBody.find("\r\n", p);
            if (eol == std::string::npos) break;
            const size_t len = strtoul(respBody.substr(p, eol - p).c_str(), nullptr, 16);
            if (!len) break;
            out += respBody.substr(eol + 2, len);
            p = eol + 2 + len + 2;
        }
        respBody = out;
    }
    return true;
}

struct Gateway { Url control; std::string serviceType; std::string localIp; };

bool findGateway(Gateway& gw, std::string& error) {
    const std::string location = discover();
    if (location.empty()) { error = "no UPnP router answered (UPnP may be turned off on the router)"; return false; }
    Url loc;
    if (!parseUrl(location, loc)) { error = "bad router description URL"; return false; }
    std::string xml;
    if (!http(loc, "GET", "", "", xml, &gw.localIp)) { error = "could not read the router's description"; return false; }
    // Find a WANIPConnection or WANPPPConnection service and its control URL.
    size_t at = 0;
    for (;;) {
        const size_t svc = xml.find("<service>", at);
        if (svc == std::string::npos) break;
        const size_t end = xml.find("</service>", svc);
        const std::string block = xml.substr(svc, end == std::string::npos ? std::string::npos : end - svc);
        const std::string type = between(block, "<serviceType>", "</serviceType>");
        if (type.find("WANIPConnection") != std::string::npos || type.find("WANPPPConnection") != std::string::npos) {
            std::string ctl = between(block, "<controlURL>", "</controlURL>");
            if (ctl.find("://") == std::string::npos) {
                std::string base = between(xml, "<URLBase>", "</URLBase>");
                if (base.empty()) base = "http://" + loc.host + ":" + std::to_string(loc.port);
                while (!base.empty() && base.back() == '/') base.pop_back();
                if (!ctl.empty() && ctl[0] != '/') ctl = "/" + ctl;
                ctl = base + ctl;
            }
            if (parseUrl(ctl, gw.control)) {
                gw.serviceType = type;
                return true;
            }
        }
        at = svc + 9;
    }
    error = "the router does not offer port mapping";
    return false;
}

bool soap(const Gateway& gw, const char* action, const std::string& args, std::string& reply, int& status) {
    const std::string body =
        "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" + std::string(action) +
        " xmlns:u=\"" + gw.serviceType + "\">" + args + "</u:" + action + "></s:Body></s:Envelope>\r\n";
    const std::string headers = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + gw.serviceType + "#" +
                                action + "\"\r\n";
    return http(gw.control, "POST", headers, body, reply, nullptr, &status);
}

std::mutex g_m;
Gateway g_gw;
bool g_haveGw = false;
} // namespace

Result mapUdpPort(uint16_t port, const char* description, uint32_t leaseSeconds) {
    Result r;
    Gateway gw;
    if (!findGateway(gw, r.error)) return r;
    r.localIp = gw.localIp;
    std::string reply;
    int status = 0;
    if (soap(gw, "GetExternalIPAddress", "", reply, status))
        r.externalIp = between(reply, "<NewExternalIPAddress>", "</NewExternalIPAddress>");
    const std::string p = std::to_string(port);
    auto add = [&](uint32_t lease) {
        const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p +
                                 "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + p +
                                 "</NewInternalPort><NewInternalClient>" + gw.localIp +
                                 "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + description +
                                 "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) +
                                 "</NewLeaseDuration>";
        return soap(gw, "AddPortMapping", args, reply, status) && status == 200;
    };
    // Some routers only accept permanent mappings (lease 0); try the lease first.
    r.mapped = add(leaseSeconds) || add(0);
    if (!r.mapped) {
        const std::string code = between(reply, "<errorCode>", "</errorCode>");
        r.error = "the router refused the port mapping" + (code.empty() ? std::string() : " (UPnP error " + code + ")");
    } else {
        std::lock_guard<std::mutex> lock(g_m);
        g_gw = gw;
        g_haveGw = true;
    }
    return r;
}

void unmapUdpPort(uint16_t port) {
    Gateway gw;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (!g_haveGw) return;
        gw = g_gw;
    }
    std::string reply;
    int status = 0;
    soap(gw, "DeletePortMapping",
         "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(port) + "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
         reply, status);
}

} // namespace mecmp::net::upnp
