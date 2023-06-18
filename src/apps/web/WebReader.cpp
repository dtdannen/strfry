#include "WebServer.h"

#include "Bech32Utils.h"
#include "WebRenderUtils.h"
#include "WebTemplates.h"
#include "DBQuery.h"




struct User {
    std::string pubkey;

    std::string npubId;
    std::string username;
    tao::json::value kind0Json = tao::json::null;

    User(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : pubkey(pubkey) {
        std::string prefix = pubkey;
        prefix += lmdb::to_sv<uint64_t>(0);

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, prefix, "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);

            if (parsedKey.s == pubkey && parsedKey.n1 == 0) {
                auto levId = lmdb::from_sv<uint64_t>(v);
                tao::json::value json = tao::json::from_string(getEventJson(txn, decomp, levId));

                try {
                    kind0Json = tao::json::from_string(json.at("content").get_string());
                    username = kind0Json.at("name").get_string();
                } catch (std::exception &e) {
                }
            }

            return false;
        });

        if (username.size() == 0) username = to_hex(pubkey.substr(0,4));
        npubId = encodeBech32Simple("npub", pubkey);
    }

    bool kind0Found() const {
        return kind0Json.is_object();
    }

    std::string getMeta(std::string_view field) const {
        if (kind0Json.get_object().contains(field) && kind0Json.at(field).is_string()) return kind0Json.at(field).get_string();
        return "";
    }
};

struct UserCache {
    std::unordered_map<std::string, User> cache;

    const User *getUser(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) {
        auto u = cache.find(pubkey);
        if (u != cache.end()) return &u->second;

        cache.emplace(pubkey, User(txn, decomp, pubkey));
        return &cache.at(pubkey);
    }
};


struct Event {
    defaultDb::environment::View_Event ev;

    tao::json::value json = tao::json::null;
    std::string parent;
    std::string root;

    uint64_t upVotes = 0;
    uint64_t downVotes = 0;


    Event(defaultDb::environment::View_Event ev) : ev(ev) {
    }

    static Event fromLevId(lmdb::txn &txn, uint64_t levId) {
        return Event(lookupEventByLevId(txn, levId));
    }

    static Event fromId(lmdb::txn &txn, std::string_view id) {
        auto existing = lookupEventById(txn, id);
        if (!existing) throw herr("unable to find event");
        return Event(std::move(*existing));
    }

    static Event fromIdExternal(lmdb::txn &txn, std::string_view id) {
        if (id.starts_with("note1")) {
            return fromId(txn, decodeBech32Simple(id));
        } else {
            return fromId(txn, from_hex(id));
        }
    }


    std::string getId() const {
        return std::string(sv(ev.flat_nested()->id()));
    }

    uint64_t getKind() const {
        return ev.flat_nested()->kind();
    }

    std::string getPubkey() const {
        return std::string(sv(ev.flat_nested()->pubkey()));
    }

    std::string getNoteId() const {
        return encodeBech32Simple("note", getId());
    }

    std::string getParentNoteId() const {
        return encodeBech32Simple("note", parent);
    }

    std::string getRootNoteId() const {
        return encodeBech32Simple("note", root);
    }

    std::string summary() const {
        // FIXME: Use "subject" tag if present?
        // FIXME: Don't truncate UTF-8 mid-sequence
        // FIXME: Don't put ellipsis if truncated text ends in punctuation

        const size_t maxLen = 100;
        const auto &content = json.at("content").get_string();
        if (content.size() <= maxLen) return content;
        return content.substr(0, maxLen-3) + "...";
    }


    void populateJson(lmdb::txn &txn, Decompressor &decomp) {
        if (!json.is_null()) return;

        json = tao::json::from_string(getEventJson(txn, decomp, ev.primaryKeyId));
    }

    void populateRootParent(lmdb::txn &txn, Decompressor &decomp) {
        populateJson(txn, decomp);

        const auto &tags = json.at("tags").get_array();

        // Try to find a e-tags with root/reply types
        for (const auto &t : tags) {
            const auto &tArr = t.get_array();
            if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "root") {
                root = from_hex(tArr.at(1).get_string());
            } else if (tArr.at(0) == "e" && tArr.size() >= 4 && tArr.at(3) == "reply") {
                parent = from_hex(tArr.at(1).get_string());
            }
        }

        if (!root.size()) {
            // Otherwise, assume first e tag is root

            for (auto it = tags.begin(); it != tags.end(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    root = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }

        if (!parent.size()) {
            // Otherwise, assume last e tag is root

            for (auto it = tags.rbegin(); it != tags.rend(); ++it) {
                const auto &tArr = it->get_array();
                if (tArr.at(0) == "e") {
                    parent = from_hex(tArr.at(1).get_string());
                    break;
                }
            }
        }
    }
};




struct EventThread {
    std::string rootEventId;
    bool isRootEventThreadRoot;
    flat_hash_map<std::string, Event> eventCache;

    flat_hash_map<std::string, flat_hash_set<std::string>> children; // parentEventId -> childEventIds


    // Load all events under an eventId

    EventThread(std::string rootEventId, bool isRootEventThreadRoot, flat_hash_map<std::string, Event> &&eventCache)
        : rootEventId(rootEventId), isRootEventThreadRoot(isRootEventThreadRoot), eventCache(eventCache) {}

    EventThread(lmdb::txn &txn, Decompressor &decomp, std::string_view id_) : rootEventId(std::string(id_)) {
        try {
            eventCache.emplace(rootEventId, Event::fromId(txn, rootEventId));
        } catch (std::exception &e) {
            return;
        }


        eventCache.at(rootEventId).populateRootParent(txn, decomp);
        isRootEventThreadRoot = eventCache.at(rootEventId).root.empty();


        std::vector<std::string> pendingQueue;
        pendingQueue.emplace_back(rootEventId);

        while (pendingQueue.size()) {
            auto currId = std::move(pendingQueue.back());
            pendingQueue.pop_back();

            std::string prefix = "e";
            prefix += currId;

            env.generic_foreachFull(txn, env.dbi_Event__tag, prefix, "", [&](std::string_view k, std::string_view v){
                ParsedKey_StringUint64 parsedKey(k);
                if (parsedKey.s != prefix) return false;

                auto levId = lmdb::from_sv<uint64_t>(v);
                Event e = Event::fromLevId(txn, levId);
                std::string childEventId = e.getId();

                if (eventCache.contains(childEventId)) return true;

                eventCache.emplace(childEventId, std::move(e));
                if (!isRootEventThreadRoot) pendingQueue.emplace_back(childEventId);

                return true;
            });
        }

        for (auto &[id, e] : eventCache) {
            e.populateRootParent(txn, decomp);

            auto kind = e.getKind();

            if (e.parent.size()) {
                if (kind == 1) {
                    if (!children.contains(e.parent)) children.emplace(std::piecewise_construct, std::make_tuple(e.parent), std::make_tuple());
                    children.at(e.parent).insert(id);
                } else if (kind == 7) {
                    auto p = eventCache.find(e.parent);
                    if (p != eventCache.end()) {
                        auto &parent = p->second;

                        if (e.json.at("content").get_string() == "-") {
                            parent.downVotes++;
                        } else {
                            parent.upVotes++;
                        }
                    }
                }
            }
        }
    }


    TemplarResult render(lmdb::txn &txn, Decompressor &decomp, UserCache &userCache, std::optional<std::string> focusOnPubkey = std::nullopt) {
        auto now = hoytech::curr_time_s();
        flat_hash_set<uint64_t> processedLevIds;

        struct RenderedEvent {
            std::string content;
            std::string timestamp;
            const Event *ev = nullptr;
            const User *user = nullptr;
            bool eventPresent = true;
            bool abbrev = false;
            std::vector<TemplarResult> replies;
        };

        std::function<TemplarResult(const std::string &)> process = [&](const std::string &id){
            RenderedEvent ctx;

            auto p = eventCache.find(id);
            if (p != eventCache.end()) {
                const auto &elem = p->second;
                processedLevIds.insert(elem.ev.primaryKeyId);

                auto pubkey = elem.getPubkey();
                ctx.abbrev = focusOnPubkey && *focusOnPubkey != pubkey;

                ctx.content = ctx.abbrev ? elem.summary() : elem.json.at("content").get_string();
                ctx.timestamp = renderTimestamp(now, elem.json.at("created_at").get_unsigned());
                ctx.user = userCache.getUser(txn, decomp, elem.getPubkey());
                ctx.eventPresent = true;

                ctx.ev = &elem;
            } else {
                ctx.eventPresent = false;
            }

            if (children.contains(id)) {
                for (const auto &childId : children.at(id)) {
                    ctx.replies.emplace_back(process(childId));
                }
            }

            return tmpl::event::event(ctx);
        };


        struct {
            TemplarResult foundEvents;
            std::vector<TemplarResult> orphanNodes;
        } ctx;

        ctx.foundEvents = process(rootEventId);

        for (auto &[id, e] : eventCache) {
            if (processedLevIds.contains(e.ev.primaryKeyId)) continue;
            if (e.getKind() != 1) continue;

            ctx.orphanNodes.emplace_back(process(id));
        }

        return tmpl::events(ctx);
    }
};



struct UserEvents {
    User u;

    struct EventCluster {
        std::string rootEventId;
        flat_hash_map<std::string, Event> eventCache; // eventId (non-root) -> Event
        bool isRootEventFromUser = false;
        bool isRootPresent = false;
        uint64_t rootEventTimestamp = 0;

        EventCluster(std::string rootEventId) : rootEventId(rootEventId) {}
    };

        std::vector<EventCluster> eventClusterArr;

    UserEvents(lmdb::txn &txn, Decompressor &decomp, const std::string &pubkey) : u(txn, decomp, pubkey) {
        flat_hash_map<std::string, EventCluster> eventClusters; // eventId (root) -> EventCluster

        env.generic_foreachFull(txn, env.dbi_Event__pubkeyKind, makeKey_StringUint64Uint64(pubkey, 1, MAX_U64), "", [&](std::string_view k, std::string_view v){
            ParsedKey_StringUint64Uint64 parsedKey(k);
            if (parsedKey.s != pubkey || parsedKey.n1 != 1) return false;

            Event ev = Event::fromLevId(txn, lmdb::from_sv<uint64_t>(v));
            ev.populateRootParent(txn, decomp);
            auto id = ev.getId();

            auto installRoot = [&](std::string rootId, Event &&rootEvent){
                rootEvent.populateRootParent(txn, decomp);

                eventClusters.emplace(rootId, rootId);
                auto &cluster = eventClusters.at(rootId);

                cluster.isRootPresent = true;
                cluster.isRootEventFromUser = rootEvent.getPubkey() == u.pubkey;
                cluster.rootEventTimestamp = rootEvent.ev.flat_nested()->created_at();
                cluster.eventCache.emplace(rootId, std::move(rootEvent));
            };

            if (ev.root.size()) {
                // Event is not root

                if (!eventClusters.contains(ev.root)) {
                    try {
                        installRoot(ev.root, Event::fromId(txn, ev.root));
                    } catch (std::exception &e) {
                        // no root event
                        eventClusters.emplace(ev.root, ev.root);
                        auto &cluster = eventClusters.at(ev.root);

                        cluster.isRootPresent = true;
                    }
                }

                eventClusters.at(ev.root).eventCache.emplace(id, std::move(ev));
            } else {
                // Event is root

                if (!eventClusters.contains(ev.root)) {
                    installRoot(id, std::move(ev));
                }
            }

            return true;
        }, true);

        for (auto &[k, v] : eventClusters) {
            eventClusterArr.emplace_back(std::move(v));
        }

        std::sort(eventClusterArr.begin(), eventClusterArr.end(), [](auto &a, auto &b){ return b.rootEventTimestamp < a.rootEventTimestamp; });
    }

    TemplarResult render(lmdb::txn &txn, Decompressor &decomp) {
        std::vector<TemplarResult> renderedThreads;
        UserCache userCache;

        for (auto &cluster : eventClusterArr) {
            EventThread eventThread(cluster.rootEventId, cluster.isRootEventFromUser, std::move(cluster.eventCache));
            renderedThreads.emplace_back(eventThread.render(txn, decomp, userCache, u.pubkey));
        }

        struct {
            std::vector<TemplarResult> &renderedThreads;
            User &u;
        } ctx = {
            renderedThreads,
            u,
        };

        return tmpl::user::comments(ctx);
    }
};









void WebServer::reply(const MsgReader::Request *msg, std::string_view r, std::string_view status) {
    std::string payload = "HTTP/1.0 ";
    payload += status;
    payload += "\r\nContent-Length: ";
    payload += std::to_string(r.size());
    payload += "\r\nContent-Type: text/html; charset=utf-8\r\n\r\n";
    payload += r;

    tpHttpsocket.dispatch(0, MsgHttpsocket{MsgHttpsocket::Send{msg->connId, msg->res, std::move(payload)}});
    hubTrigger->send();
}


struct Url {
    std::vector<std::string_view> path;
    std::string_view query;

    Url(std::string_view u) {
        size_t pos;

        if ((pos = u.find("?")) != std::string::npos) {
            query = u.substr(pos + 1);
            u = u.substr(0, pos);
        }

        while ((pos = u.find("/")) != std::string::npos) {
            if (pos != 0) path.emplace_back(u.substr(0, pos));
            u = u.substr(pos + 1);
        }

        if (u.size()) path.emplace_back(u);
    }
};

void WebServer::handleRequest(lmdb::txn &txn, Decompressor &decomp, const MsgReader::Request *msg) {
    LI << "GOT REQUEST FOR " << msg->url;
    auto startTime = hoytech::curr_time_us();

    Url u(msg->url);

    TemplarResult body;
    std::string_view code = "200 OK";

    if (u.path.size() == 0) {
        body = TemplarResult{ "root" };
    } else if (u.path[0] == "e" && u.path.size() == 2) {
        EventThread et(txn, decomp, decodeBech32Simple(u.path[1]));
        UserCache userCache;
        body = et.render(txn, decomp, userCache);
    } else if (u.path[0] == "u" && u.path.size() == 2) {
        User user(txn, decomp, decodeBech32Simple(u.path[1]));
        body = tmpl::user::metadata(user);
    } else if (u.path[0] == "u" && u.path.size() == 3 && u.path[2] == "notes") {
        UserEvents uc(txn, decomp, decodeBech32Simple(u.path[1]));
        body = uc.render(txn, decomp);
    } else {
        body = TemplarResult{ "Not found" };
        code = "404 Not Found";
    }


    std::string html;

    {
        struct {
            TemplarResult body;
        } ctx = {
            body,
        };

        html = tmpl::main(ctx).str;
    }

    LI << "Reply: " << code << " / " << html.size() << " bytes in " << (hoytech::curr_time_us() - startTime) << "us";
    reply(msg, html, code);
}



void WebServer::runReader(ThreadPool<MsgReader>::Thread &thr) {
    Decompressor decomp;

    while(1) {
        auto newMsgs = thr.inbox.pop_all();

        auto txn = env.txn_ro();

        for (auto &newMsg : newMsgs) {
            if (auto msg = std::get_if<MsgReader::Request>(&newMsg.msg)) {
                try {
                    handleRequest(txn, decomp, msg);
                } catch (std::exception &e) {
                    reply(msg, "Server error", "500 Server Error");
                    LE << "500 server error: " << e.what();
                }
            }
        }
    }
}
