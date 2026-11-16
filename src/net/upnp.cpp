#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "iphlpapi.lib") // GetAdaptersAddresses; also for the tests that compile this file by hand
#endif

#include "net/upnp.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

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

// The router must describe itself on its own address: a LOCATION (or control URL) pointing at
// another machine would make us send it this PC's address and port requests (S-11).
bool sameHostIp(const std::string& host, const in_addr& sender) {
    in_addr a{};
    return inet_pton(AF_INET, host.c_str(), &a) == 1 && a.s_addr == sender.s_addr;
}

struct Iface { in_addr addr{}; ULONG prefix = 0; };

bool isPrivateV4(const in_addr& a) {
    const uint32_t h = ntohl(a.s_addr);
    return (h >> 24) == 10 || (h >> 20) == 0xAC1 || (h >> 16) == 0xC0A8 || (h >> 16) == 0xA9FE ||
           (h >> 22) == 0x191; // 10/8, 172.16/12, 192.168/16, link-local, CGNAT 100.64/10
}

bool onSameSubnet(const Iface& i, const in_addr& other) {
    if (i.prefix == 0 || i.prefix > 32) return false;
    const uint32_t mask = i.prefix == 32 ? 0xFFFFFFFFu : ~(0xFFFFFFFFu >> i.prefix);
    return (ntohl(i.addr.s_addr) & mask) == (ntohl(other.s_addr) & mask);
}

// Unicast IPv4 addresses of every up, non-loopback interface that can multicast.
std::vector<Iface> localInterfaces() {
    std::vector<Iface> out;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf;
    const ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3 && rc == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Iface f;
            f.addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            f.prefix = u->OnLinkPrefixLength;
            const uint32_t h = ntohl(f.addr.s_addr);
            if ((h >> 24) == 127 || (h >> 16) == 0xA9FE) continue; // loopback, link-local
            out.push_back(f);
        }
    }
    return out;
}

struct Candidate { std::string location; in_addr sender{}; bool onLink = false; };

// SSDP M-SEARCH on every interface. Returns every distinct Internet Gateway Device that answered
// with a LOCATION on its own (private) address, devices on a directly attached subnet first, so a
// VPN gateway or second router that answers sooner doesn't win over the real LAN router.
std::vector<Candidate> discover() {
    std::vector<Candidate> found;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return found;
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
    const DWORD ttl = 2;
    setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    // Send out of every active interface: with VPN/virtual adapters the OS-default multicast
    // interface is often the wrong one and the router never sees the search.
    const std::vector<Iface> ifaces = localInterfaces();
    bool haveOnLink = false;
    for (int round = 0; round < 3 && !haveOnLink; ++round) {
        for (const char* st : targets) {
            std::string msg = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") +
                              st + "\r\n\r\n";
            if (ifaces.empty()) {
                sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
                continue;
            }
            for (const Iface& f : ifaces) {
                if (setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&f.addr), sizeof(f.addr)) != 0)
                    continue;
                sendto(s, msg.data(), static_cast<int>(msg.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
        }
        for (int i = 0; i < 16; ++i) {
            char buf[2048];
            sockaddr_in from{};
            int fromLen = sizeof(from);
            const int n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) break;
            buf[n] = 0;
            const std::string loc = headerValue(std::string("\n") + buf, "location");
            Url u;
            char ip[64] = {};
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            if (loc.empty() || !parseUrl(loc, u)) continue;
            if (!sameHostIp(u.host, from.sin_addr)) {
                MLOG("net: UPnP: ignoring an answer from %s that points at %s", ip, u.host.c_str());
                continue;
            }
            if (!isPrivateV4(from.sin_addr)) {
                MLOG("net: UPnP: ignoring an answer from non-private address %s", ip);
                continue;
            }
            bool dup = false;
            for (const Candidate& c : found) dup = dup || c.location == loc;
            if (dup) continue;
            Candidate c;
            c.location = loc;
            c.sender = from.sin_addr;
            for (const Iface& f : ifaces) c.onLink = c.onLink || onSameSubnet(f, from.sin_addr);
            haveOnLink = haveOnLink || c.onLink;
            found.push_back(c);
            if (c.onLink) break; // a LAN router answered: no need to wait for the rest
        }
    }
    closesocket(s);
    std::stable_sort(found.begin(), found.end(), [](const Candidate& a, const Candidate& b) { return a.onLink && !b.onLink; });
    return found;
}

// Minimal HTTP/1.1 request; returns the body. localIp receives this PC's address on that network.
bool http(const Url& u, const std::string& method, const std::string& extraHeaders, const std::string& body,
          std::string& respBody, std::string* localIp = nullptr, int* status = nullptr) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(u.host.c_str(), std::to_string(u.port).c_str(), &hints, &res) != 0 || !res) return false;
    // Only ever talk to a LAN device: never send this PC's address/port requests to a public host.
    if (res->ai_addr->sa_family != AF_INET ||
        !isPrivateV4(reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr)) {
        MLOG("net: UPnP: refusing to contact non-private host %s", u.host.c_str());
        freeaddrinfo(res);
        return false;
    }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { freeaddrinfo(res); return false; }
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    // Connect with a timeout: SO_SNDTIMEO doesn't apply to connect(), and an unreachable candidate
    // (e.g. a router on a dead virtual adapter) would otherwise stall ~21 s.
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    bool ok = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) == 0;
    if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set wr, ex;
        FD_ZERO(&wr); FD_ZERO(&ex);
        FD_SET(s, &wr); FD_SET(s, &ex);
        timeval tv{3, 0};
        ok = select(0, nullptr, &wr, &ex, &tv) > 0 && FD_ISSET(s, &wr) && !FD_ISSET(s, &ex);
    }
    freeaddrinfo(res);
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
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
    if (send(s, req.data(), static_cast<int>(req.size()), 0) <= 0) { closesocket(s); return false; }
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
    if (status) {
        const size_t sp = resp.find(' ');
        if (sp == std::string::npos || sp > hdrEnd) return false;
        *status = atoi(resp.c_str() + sp + 1);
    }
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

bool tryGateway(const Candidate& cand, Gateway& gw, std::string& error) {
    const std::string& location = cand.location;
    const in_addr sender = cand.sender;
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
            // Absolute control URLs (or a URLBase) must stay on the router that answered.
            if (parseUrl(ctl, gw.control) && sameHostIp(gw.control.host, sender)) {
                gw.serviceType = type;
                return true;
            }
        }
        at = svc + 9;
    }
    if (gw.control.host.size() && !sameHostIp(gw.control.host, sender)) {
        error = "the router's control address points at another machine; not using it";
        return false;
    }
    error = "the router does not offer port mapping";
    return false;
}

// Tries each router that answered (LAN-attached first) until one offers port mapping.
bool findGateway(Gateway& gw, std::string& error) {
    const std::vector<Candidate> cands = discover();
    if (cands.empty()) { error = "no UPnP router answered (UPnP may be turned off on the router)"; return false; }
    std::string first;
    for (const Candidate& c : cands) {
        Gateway g;
        std::string err;
        if (tryGateway(c, g, err)) { gw = g; return true; }
        char ip[64] = {};
        inet_ntop(AF_INET, &c.sender, ip, sizeof(ip));
        MLOG("net: UPnP: %s unusable: %s", ip, err.c_str());
        if (first.empty()) first = err;
    }
    error = first;
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
    if (gw.localIp.empty() || gw.localIp == "0.0.0.0") {
        r.error = "could not work out this PC's address on the router's network";
        return r;
    }
    std::string reply;
    int status = 0;
    if (soap(gw, "GetExternalIPAddress", "", reply, status) && status == 200)
        r.externalIp = between(reply, "<NewExternalIPAddress>", "</NewExternalIPAddress>");
    const std::string p = std::to_string(port);
    auto add = [&](uint32_t lease) {
        reply.clear();
        status = 0;
        const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p +
                                 "</NewExternalPort><NewProtocol>UDP</NewProtocol><NewInternalPort>" + p +
                                 "</NewInternalPort><NewInternalClient>" + gw.localIp +
                                 "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + description +
                                 "</NewPortMappingDescription><NewLeaseDuration>" + std::to_string(lease) +
                                 "</NewLeaseDuration>";
        return soap(gw, "AddPortMapping", args, reply, status) && status == 200;
    };
    // Only a timed lease: a permanent mapping (lease 0) would stay open on the router forever if the
    // game crashed. Routers that only take permanent leases (UPnP error 725) need the port forwarded
    // by hand; the log says so.
    r.mapped = leaseSeconds > 0 && add(leaseSeconds);
    if (!r.mapped && leaseSeconds > 0 && between(reply, "<errorCode>", "</errorCode>") == "718") {
        // 718 ConflictInMappingEntry: the port is still mapped, often to a stale lease from a crashed
        // session or to this PC on another address. Drop it and map it to this address.
        std::string ignored;
        int st = 0;
        soap(gw, "DeletePortMapping",
             "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + p + "</NewExternalPort><NewProtocol>UDP</NewProtocol>",
             ignored, st);
        r.mapped = add(leaseSeconds);
    }
    if (!r.mapped) {
        const std::string code = between(reply, "<errorCode>", "</errorCode>");
        r.error = "the router refused the port mapping" + (code.empty() ? std::string() : " (UPnP error " + code + ")");
        return r;
    }
    in_addr ext{};
    if (inet_pton(AF_INET, r.externalIp.c_str(), &ext) != 1 || ext.s_addr == 0 || isPrivateV4(ext))
        MLOG("net: UPnP: mapped, but the router's external address is '%s' (double NAT / CGNAT?): "
             "players on the internet may still not reach this PC", r.externalIp.c_str());
    std::lock_guard<std::mutex> lock(g_m);
    g_gw = gw;
    g_haveGw = true;
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
