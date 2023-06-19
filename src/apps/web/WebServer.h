#pragma once

#include <iostream>
#include <memory>
#include <algorithm>

#include <hoytech/time.h>
#include <hoytech/hex.h>
#include <hoytech/file_change_monitor.h>
#include <uWebSockets/src/uWS.h>
#include <tao/json.hpp>

#include "golpe.h"

#include "ThreadPool.h"
#include "Decompressor.h"



struct MsgHttpsocket : NonCopyable {
    struct Send {
        uint64_t connId;
        uWS::HttpResponse *res;
        std::string payload;
    };

    using Var = std::variant<Send>;
    Var msg;
    MsgHttpsocket(Var &&msg_) : msg(std::move(msg_)) {}
};

struct MsgReader : NonCopyable {
    struct Request {
        uint64_t connId;
        uWS::HttpResponse *res;
        std::string url;
        bool acceptGzip;
    };

    using Var = std::variant<Request>;
    Var msg;
    MsgReader(Var &&msg_) : msg(std::move(msg_)) {}
};



struct WebServer {
    std::unique_ptr<uS::Async> hubTrigger;

    // Thread Pools

    ThreadPool<MsgHttpsocket> tpHttpsocket;
    ThreadPool<MsgReader> tpReader;

    void run();

    void runHttpsocket(ThreadPool<MsgHttpsocket>::Thread &thr);

    void runReader(ThreadPool<MsgReader>::Thread &thr);
    void reply(const MsgReader::Request *msg, std::string_view r, std::string_view status = "200 OK", std::string_view contentType = "text/html; charset=utf-8");
    void handleRequest(lmdb::txn &txn, Decompressor &decomp, const MsgReader::Request *msg);
};
