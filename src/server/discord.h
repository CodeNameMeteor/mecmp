#pragma once
// Discord webhook for the dedicated server: posts who joins and leaves, and when the server goes up and
// down, to a Discord channel, so a community can see "my friend is on, let me join".
//
// Posting runs on its own thread and never blocks the relay. Lines are queued, gathered for a moment and
// sent together (Discord allows about 30 posts a minute per webhook); a 429 answer is waited out and
// retried. Player names are escaped for Markdown and no message can mention anyone (allowed_mentions).
// HTTPS: WinHTTP on Windows; libcurl loaded at run time on Linux (libcurl.so.4), so the server builds and
// runs without it, only without the webhook.
//
// The webhook URL is a secret (anyone who has it can post to the channel): it is read from the server's
// config and never sent to players or written to the log.

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mecmp::server::discord {

struct Config {
    std::string url;              // https://discord.com/api/webhooks/<id>/<token>; empty = off
    std::string username;         // the name the posts appear under (empty: the webhook's own name)
    bool joins = true;            // player joined / left
    bool status = false;          // posts when the server goes online / offline (the live message shows it anyway)
    bool results = true;          // finished dashes and time trials (as players report them)
    bool live = true;             // one status message (who is online), edited in place as players come and go
    std::string statusIdFile;     // where that message's id is kept, so a restart edits the same one
    // The live message is moved back to the bottom of the channel (posted again, the old one deleted) once this
    // many of our own posts are below it (0 = never for that reason)...
    int statusBumpAfter = 10;
    // ...or once this many minutes have passed with at least one of our posts below it (0 = never for that reason).
    int statusBumpMinutes = 5;
    // Where the ids of this session's posts (joins, leaves, finished runs) are kept until the server is empty
    // again and they are deleted, so a restart (or a stop with Discord slow to answer) still cleans them up.
    std::string trackFile;
};

// --- Formatting (pure; tested in tests/server_test.cpp) ---------------------------------------------
bool isWebhookUrl(const std::string& url);     // https://(ptb.|canary.)discord(app).com/api/webhooks/...
// Markdown characters are escaped (* _ ` ~ | > \ # [ ] ( )), so a name is shown as written. Not escaped: - : and @,
// which do no harm mid-line; @ mentions are blocked by allowed_mentions, and a bare URL in a name may be linked.
std::string escapeMarkdown(const std::string& text);
std::string jsonEscape(const std::string& text);
// The JSON body for one post: content (cut to Discord's 2000 characters), username, no mentions.
std::string payload(const std::string& content, const std::string& username);
// Seconds to wait from a 429 answer's body ({"retry_after": 1.5, ...}); `fallback` if absent.
double retryAfter(const std::string& body, double fallback);
// Splits queued lines into posts of at most `limit` characters (lines longer than that are cut).
std::deque<std::string> pack(std::deque<std::string> lines, size_t limit = 1900);
bool isMessageId(const std::string& id);         // a Discord snowflake: digits only
// Is the live status message due to move back to the bottom? `below`: our posts since it was posted;
// `minutes`: how long ago it was posted. A webhook cannot see other people's messages, so only ours count.
bool statusBumpDue(int below, double minutes, int bumpAfter, int bumpMinutes);
std::string messageId(const std::string& body);  // the "id" of a posted message (?wait=true answer); "" if none
std::string withPath(const std::string& url, const std::string& more);   // inserted before any ?query
std::string withQuery(const std::string& url, const std::string& param); // ?param or &param

// The message ids of the player activity (joins and leaves) posted in the current session, so they can be
// deleted when the server is empty again. Worker thread only.
class ActivityLog {
public:
    static constexpr size_t kMax = 1000; // a session this long keeps only its first kMax messages
    void add(const std::string& id);       // ignored unless it is a message id
    std::vector<std::string> take();       // every id so far, in order; forgets them
    size_t size() const { return ids_.size(); }
    const std::vector<std::string>& ids() const { return ids_; }
private:
    std::vector<std::string> ids_;
};

// --- Poster ------------------------------------------------------------------------------------------
class Webhook {
public:
    using Log = std::function<void(const std::string&)>;
    Webhook(Config cfg, Log log);
    ~Webhook(); // sends what is queued (a few seconds at most), then stops
    Webhook(const Webhook&) = delete;
    Webhook& operator=(const Webhook&) = delete;

    bool enabled() const { return running_; }
    const Config& config() const { return cfg_; }
    void post(const std::string& line); // one line; gathered with others queued within ~2 s
    // A post of this session (a join, a leave, a finished run): posted like post(), and its message id is kept
    // for endSession().
    void postActivity(const std::string& line);
    // The server is empty again: the activity posted since the last endSession() is deleted. Lines queued
    // before this call are posted first, so none of them survives.
    void endSession();
    // The live status message's text (Config::live): the latest text is shown, at most one edit every 5 s.
    void setStatus(const std::string& text);

private:
    using clock = std::chrono::steady_clock;
    struct Item {
        enum Kind { Line, Activity, EndSession } kind;
        std::string text;
    };
    void run();
    // Edits the live message; false if it was not sent (retry later), including when a rate limit would wait
    // past `deadline`.
    // `bump`: post the text as a new message at the bottom and delete the old one, instead of editing it.
    bool sendStatus(const std::string& text, clock::time_point& nextPost, clock::time_point deadline, bool bump = false);
    bool bumpDue(clock::time_point now) const;
    clock::time_point bumpAt() const; // when bumpDue() turns true by time alone (max() if it cannot)
    // Posts `posts` in order; with `track`, the id of each posted message goes to activity_.
    void sendPosts(std::deque<std::string> posts, bool track, clock::time_point& nextPost, clock::time_point giveUp);
    // Deletes the messages in toDelete_ (those left over are kept for the next pass, unless stopping).
    void deleteMessages(clock::time_point& nextPost, bool stopping);
    // Config::trackFile: the ids still to delete (activity_ and toDelete_), one per line; read once at start.
    void saveTracked();
    void loadTracked();
    Config cfg_;
    Log log_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Item> queue_;                // guarded by m_
    ActivityLog activity_;                  // worker thread only
    std::deque<std::string> toDelete_;      // worker thread only
    int failuresLogged_ = 0;                // worker thread only
    int statusFailuresLogged_ = 0;          // worker thread only
    bool playerLogged_ = false;             // worker thread only: the "no id" note was written
    std::string statusText_;
    bool statusDirty_ = false;
    std::string statusId_; // the status message (worker thread only)
    // Our posts below the status message (worker thread only): lines that stay, and session activity (deleted at
    // endSession, so it stops counting then), and when the status message was posted.
    int linesBelowStatus_ = 0, activityBelowStatus_ = 0;
    clock::time_point statusPostedAt_ = clock::now();
    bool stop_ = false;
    bool running_ = false;
    std::thread thread_;
};

} // namespace mecmp::server::discord
