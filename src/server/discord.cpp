#include "server/discord.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#endif

namespace mecmp::server::discord {

// --- Formatting ---------------------------------------------------------------------------------------

bool isWebhookUrl(const std::string& url) {
    static const char* const kPrefixes[] = {
        "https://discord.com/api/webhooks/",       "https://discordapp.com/api/webhooks/",
        "https://ptb.discord.com/api/webhooks/",   "https://canary.discord.com/api/webhooks/",
    };
    for (const char* p : kPrefixes) {
        const size_t n = strlen(p);
        if (url.size() > n + 10 && url.compare(0, n, p) == 0) {
            for (size_t i = n; i < url.size(); ++i) {
                const unsigned char c = static_cast<unsigned char>(url[i]);
                if (c <= ' ' || c == '"' || c == '\\' || c >= 0x7f) return false;
            }
            return true;
        }
    }
    return false;
}

std::string escapeMarkdown(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (strchr("\\*_`~|>#[]()", c) && c) out += '\\';
        out += c;
    }
    return out;
}

std::string jsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char u[8];
                snprintf(u, sizeof(u), "\\u%04x", c);
                out += u;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

namespace {
// Cuts UTF-8 text to at most `max` bytes without splitting a character.
std::string cutUtf8(const std::string& s, size_t max) {
    if (s.size() <= max) return s;
    size_t n = max;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    return s.substr(0, n);
}
} // namespace

std::string payload(const std::string& content, const std::string& username) {
    std::string body = "{\"content\":\"" + jsonEscape(cutUtf8(content, 1990)) + "\"";
    if (!username.empty()) body += ",\"username\":\"" + jsonEscape(cutUtf8(username, 80)) + "\"";
    body += ",\"allowed_mentions\":{\"parse\":[]}}";
    return body;
}

double retryAfter(const std::string& body, double fallback) {
    const size_t k = body.find("\"retry_after\"");
    if (k == std::string::npos) return fallback;
    const size_t colon = body.find(':', k);
    if (colon == std::string::npos) return fallback;
    const double v = std::strtod(body.c_str() + colon + 1, nullptr);
    return v > 0 && v < 600 ? v : fallback;
}

bool isMessageId(const std::string& id) {
    if (id.empty() || id.size() > 25) return false;
    for (char c : id)
        if (c < '0' || c > '9') return false;
    return true;
}

std::string messageId(const std::string& body) {
    const size_t k = body.find("\"id\"");
    if (k == std::string::npos) return {};
    const size_t q1 = body.find('"', body.find(':', k) + 1);
    const size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
    if (q2 == std::string::npos) return {};
    const std::string id = body.substr(q1 + 1, q2 - q1 - 1);
    return isMessageId(id) ? id : std::string();
}

std::string withPath(const std::string& url, const std::string& more) {
    const size_t q = url.find('?');
    return q == std::string::npos ? url + more : url.substr(0, q) + more + url.substr(q);
}

std::string withQuery(const std::string& url, const std::string& param) {
    return url + (url.find('?') == std::string::npos ? "?" : "&") + param;
}

std::deque<std::string> pack(std::deque<std::string> lines, size_t limit) {
    std::deque<std::string> posts;
    std::string cur;
    for (auto& l : lines) {
        std::string line = cutUtf8(l, limit);
        if (!cur.empty() && cur.size() + 1 + line.size() > limit) {
            posts.push_back(std::move(cur));
            cur.clear();
        }
        if (!cur.empty()) cur += '\n';
        cur += line;
    }
    if (!cur.empty()) posts.push_back(std::move(cur));
    return posts;
}

// --- HTTPS POST ------------------------------------------------------------------------------------------
namespace {

struct Answer {
    long status = 0;      // 0 = no answer (network error)
    std::string body;
    std::string error;
};

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

bool httpReady(std::string& why) {
    (void)why;
    return true;
}

Answer sendJson(const char* method, const std::string& url, const std::string& body) {
    Answer a;
    const std::wstring wurl = widen(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, pathOnly[2048] = {}, query[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = pathOnly;
    uc.dwUrlPathLength = 2047;
    uc.lpszExtraInfo = query;
    uc.dwExtraInfoLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { a.error = "bad URL"; return a; }
    const std::wstring path = std::wstring(pathOnly) + query;
    const std::wstring verb = widen(method);
    HINTERNET s = WinHttpOpen(L"mecmp_server", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { a.error = "WinHttpOpen failed"; return a; }
    WinHttpSetTimeouts(s, 10000, 10000, 10000, 10000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, verb.c_str(), path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    const wchar_t* headers = L"Content-Type: application/json\r\n";
    if (r && WinHttpSendRequest(r, headers, static_cast<DWORD>(-1), const_cast<char*>(body.data()),
                                static_cast<DWORD>(body.size()), static_cast<DWORD>(body.size()), 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code,
                            &len, WINHTTP_NO_HEADER_INDEX);
        a.status = static_cast<long>(code);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0 || a.body.size() > 16384) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
            a.body.append(chunk.data(), got);
        }
    } else {
        a.error = "request failed (error " + std::to_string(GetLastError()) + ")";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return a;
}
#else
// libcurl, loaded at run time: only the few calls needed, with the option numbers of curl's stable ABI.
using CurlInit = void* (*)();
using CurlSetopt = int (*)(void*, int, ...);
using CurlPerform = int (*)(void*);
using CurlGetinfo = int (*)(void*, int, ...);
using CurlCleanup = void (*)(void*);
using CurlSlistAppend = void* (*)(void*, const char*);
using CurlSlistFree = void (*)(void*);
using CurlGlobalInit = int (*)(long);
using CurlStrerror = const char* (*)(int);
struct Curl {
    bool ok = false;
    CurlInit init = nullptr;
    CurlSetopt setopt = nullptr;
    CurlPerform perform = nullptr;
    CurlGetinfo getinfo = nullptr;
    CurlCleanup cleanup = nullptr;
    CurlSlistAppend slistAppend = nullptr;
    CurlSlistFree slistFree = nullptr;
    CurlStrerror strerror = nullptr;
};
constexpr int kOptWriteData = 10001, kOptUrl = 10002, kOptPostFields = 10015, kOptUserAgent = 10018,
              kOptHttpHeader = 10023, kOptWriteFunction = 20011, kOptTimeout = 13, kOptPostFieldSize = 60,
              kOptNoSignal = 99, kOptCustomRequest = 10036;
constexpr int kInfoResponseCode = 0x200002;

const Curl& curl() {
    static const Curl c = [] {
        Curl x;
        void* lib = dlopen("libcurl.so.4", RTLD_NOW);
        if (!lib) lib = dlopen("libcurl.so", RTLD_NOW);
        if (!lib) return x;
        x.init = reinterpret_cast<CurlInit>(dlsym(lib, "curl_easy_init"));
        x.setopt = reinterpret_cast<CurlSetopt>(dlsym(lib, "curl_easy_setopt"));
        x.perform = reinterpret_cast<CurlPerform>(dlsym(lib, "curl_easy_perform"));
        x.getinfo = reinterpret_cast<CurlGetinfo>(dlsym(lib, "curl_easy_getinfo"));
        x.cleanup = reinterpret_cast<CurlCleanup>(dlsym(lib, "curl_easy_cleanup"));
        x.slistAppend = reinterpret_cast<CurlSlistAppend>(dlsym(lib, "curl_slist_append"));
        x.slistFree = reinterpret_cast<CurlSlistFree>(dlsym(lib, "curl_slist_free_all"));
        x.strerror = reinterpret_cast<CurlStrerror>(dlsym(lib, "curl_easy_strerror"));
        auto global = reinterpret_cast<CurlGlobalInit>(dlsym(lib, "curl_global_init"));
        x.ok = x.init && x.setopt && x.perform && x.getinfo && x.cleanup && x.slistAppend && x.slistFree && global &&
               global(3 /* CURL_GLOBAL_DEFAULT */) == 0;
        return x;
    }();
    return c;
}

size_t onBody(char* data, size_t size, size_t count, void* user) {
    auto* out = static_cast<std::string*>(user);
    if (out->size() < 16384) out->append(data, size * count);
    return size * count;
}

bool httpReady(std::string& why) {
    if (curl().ok) return true;
    why = "libcurl (libcurl.so.4) is not installed; install it (e.g. sudo apt install libcurl4) for the Discord webhook";
    return false;
}

Answer sendJson(const char* method, const std::string& url, const std::string& body) {
    Answer a;
    const Curl& c = curl();
    void* h = c.init();
    if (!h) { a.error = "curl_easy_init failed"; return a; }
    void* headers = c.slistAppend(nullptr, "Content-Type: application/json");
    c.setopt(h, kOptUrl, url.c_str());
    if (strcmp(method, "POST") != 0) c.setopt(h, kOptCustomRequest, method);
    c.setopt(h, kOptHttpHeader, headers);
    c.setopt(h, kOptPostFields, body.c_str());
    c.setopt(h, kOptPostFieldSize, static_cast<long>(body.size()));
    c.setopt(h, kOptUserAgent, "mecmp_server");
    c.setopt(h, kOptTimeout, 10L);
    c.setopt(h, kOptNoSignal, 1L);
    c.setopt(h, kOptWriteFunction, &onBody);
    c.setopt(h, kOptWriteData, &a.body);
    const int rc = c.perform(h);
    if (rc == 0) c.getinfo(h, kInfoResponseCode, &a.status);
    else a.error = c.strerror ? c.strerror(rc) : ("curl error " + std::to_string(rc));
    c.slistFree(headers);
    c.cleanup(h);
    return a;
}
#endif

} // namespace

// --- Poster ------------------------------------------------------------------------------------------

Webhook::Webhook(Config cfg, Log log) : cfg_(std::move(cfg)), log_(std::move(log)) {
    if (cfg_.url.empty()) return;
    if (!isWebhookUrl(cfg_.url)) {
        log_("discord: webhook_url is not a Discord webhook address (https://discord.com/api/webhooks/...); not posting");
        return;
    }
    std::string why;
    if (!httpReady(why)) {
        log_("discord: " + why);
        return;
    }
    running_ = true;
    thread_ = std::thread([this] { run(); });
    log_("discord: posting to the webhook (joins and leaves: " + std::string(cfg_.joins ? "on" : "off") +
         ", server status: " + (cfg_.status ? "on" : "off") + ", results: " + (cfg_.results ? "on" : "off") + ")");
}

Webhook::~Webhook() {
    if (!running_) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    thread_.join();
}

void Webhook::post(const std::string& line) {
    if (!running_ || line.empty()) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (queue_.size() >= 200) return; // Discord far behind: keep the newest only up to a bound
        queue_.push_back(line);
    }
    cv_.notify_all();
}

void Webhook::setStatus(const std::string& text) {
    if (!running_ || !cfg_.live) return;
    {
        std::lock_guard<std::mutex> lock(m_);
        if (text == statusText_ && !statusDirty_) return;
        statusText_ = text;
        statusDirty_ = true;
    }
    cv_.notify_all();
}

namespace {
std::string readFileTrimmed(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char buf[64] = {};
        const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        out.assign(buf, n);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}
void writeFile(const std::string& path, const std::string& text) {
    if (FILE* f = fopen(path.c_str(), "wb")) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
}
} // namespace

// The live status message: edited in place (PATCH .../messages/<id>); posted the first time, or again if it
// was deleted. Returns false to retry later (network, rate limit).
bool Webhook::sendStatus(const std::string& text, std::chrono::steady_clock::time_point& nextPost) {
    using clock = std::chrono::steady_clock;
    if (statusId_.empty() && !cfg_.statusIdFile.empty()) {
        const std::string saved = readFileTrimmed(cfg_.statusIdFile);
        if (isMessageId(saved)) statusId_ = saved;
    }
    const auto now = clock::now();
    if (now < nextPost) std::this_thread::sleep_for(nextPost - now);
    Answer a;
    if (!statusId_.empty()) {
        a = sendJson("PATCH", withPath(cfg_.url, "/messages/" + statusId_), payload(text, ""));
        if (a.status == 404) { // deleted in Discord: post a new one
            log_("discord: the status message was deleted; posting a new one");
            statusId_.clear();
        }
    }
    if (statusId_.empty()) {
        a = sendJson("POST", withQuery(cfg_.url, "wait=true"), payload(text, cfg_.username));
        if (a.status >= 200 && a.status < 300) {
            statusId_ = messageId(a.body);
            if (!statusId_.empty() && !cfg_.statusIdFile.empty()) writeFile(cfg_.statusIdFile, statusId_);
        }
    }
    nextPost = clock::now() + std::chrono::milliseconds(2100);
    if (a.status == 429) {
        nextPost = clock::now() + std::chrono::milliseconds(static_cast<long long>(retryAfter(a.body, 5.0) * 1000) + 100);
        return false;
    }
    if (a.status >= 200 && a.status < 300) return true;
    log_("discord: status update failed (" + (a.status ? "HTTP " + std::to_string(a.status) : a.error) + ")");
    return a.status >= 400 && a.status < 500; // a 4xx will not get better by retrying
}

void Webhook::run() {
    using clock = std::chrono::steady_clock;
    clock::time_point nextPost = clock::now();
    clock::time_point nextStatus = clock::now(); // at most one edit every kStatusGap (the latest text wins)
    constexpr auto kStatusGap = std::chrono::seconds(5);
    int failuresLogged = 0;
    for (;;) {
        std::deque<std::string> lines;
        std::string status;
        bool haveStatus = false;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty() || statusDirty_; });
            if (stop_ && queue_.empty() && !statusDirty_) return;
            // Gather what arrives in the next 2 s into the same post (unless stopping).
            if (!stop_ && !queue_.empty()) cv_.wait_for(lock, std::chrono::seconds(2), [this] { return stop_; });
            lines.swap(queue_);
            if (statusDirty_ && (stop_ || clock::now() >= nextStatus)) {
                status = statusText_;
                statusDirty_ = false;
                haveStatus = true;
            } else if (statusDirty_ && lines.empty()) {
                cv_.wait_until(lock, nextStatus, [this] { return stop_; }); // the next edit is not due yet
                continue;
            }
        }
        // Stopping: the live message says "offline" first (a closed console window leaves about 5 s), and our
        // own spacing between requests is cut short (Discord allows 5 per 2 s; a 429 still makes us wait).
        if (stop_) nextPost = std::min(nextPost, clock::now() + std::chrono::milliseconds(300));
        if (haveStatus && stop_) {
            sendStatus(status, nextPost);
            haveStatus = false;
            nextPost = std::min(nextPost, clock::now() + std::chrono::milliseconds(300));
        }
        std::deque<std::string> posts = pack(std::move(lines));
        const clock::time_point giveUp = clock::now() + std::chrono::seconds(stop_ ? 3 : 120);
        while (!posts.empty()) {
            const auto now = clock::now();
            if (now >= giveUp) break;
            if (now < nextPost) std::this_thread::sleep_for(nextPost - now);
            const Answer a = sendJson("POST", cfg_.url, payload(posts.front(), cfg_.username));
            nextPost = clock::now() + std::chrono::milliseconds(2100); // ~28 posts a minute at most
            if (a.status >= 200 && a.status < 300) {
                posts.pop_front();
                failuresLogged = 0;
            } else if (a.status == 429) {
                const double wait = retryAfter(a.body, 5.0);
                nextPost = clock::now() + std::chrono::milliseconds(static_cast<long long>(wait * 1000) + 100);
            } else {
                if (failuresLogged++ < 3)
                    log_("discord: post failed (" + (a.status ? "HTTP " + std::to_string(a.status) : a.error) + ")" +
                         (a.status == 401 || a.status == 404 ? "; is webhook_url right? the webhook may have been deleted" : ""));
                if (a.status >= 400 && a.status < 500) posts.pop_front(); // will not work if retried
                else nextPost = clock::now() + std::chrono::seconds(10);
            }
        }
        if (haveStatus) {
            if (!sendStatus(status, nextPost)) {
                std::lock_guard<std::mutex> lock(m_);
                if (!statusDirty_) { statusText_ = status; statusDirty_ = !stop_; } // retry unless stopping
            }
            nextStatus = clock::now() + kStatusGap;
        }
    }
}

} // namespace mecmp::server::discord
