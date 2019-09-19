/*
module: zmqhelper
description: 
author: Bruce.Lu <lzbgt@icloud.com>
update: 2019/08/23
*/

#ifndef __ZMQ_HELPER_H__
#define __ZMQ_HELPER_H__

#undef ZMQ_BUILD_DRAFT_API
#define ZMQ_BUILD_DRAFT_API 1

#include "zmq.h"
#include <vector>
#include <spdlog/spdlog.h>
#include "json.hpp"
#include "utils.hpp"
// #include <unistd.h>

using namespace std;
using namespace nlohmann;

namespace zmqhelper {
#define EV_HEARTBEAT_SECONDS 30
#define MSG_HELLO "hello"
#define EV_MSG_META_PING "ping"
#define EV_MSG_META_PONG "pong"
#define EV_MSG_META_EVENT "event"
#define EV_MSG_META_CMD "cmd"
#define EV_MSG_META_CONFIG "config"
#define EV_MSG_META_AVFORMATCTX "afctx"

#define EV_MSG_TYPE_AI_MOTION "ai_motion"
#define EV_MSG_TYPE_CONN_STAT "connstat"
#define EV_MSG_TYPE_SYS_STAT "sysstat"
// #define EV_MSG_CMD_RESTART "restart"
// #define EV_MSG_CMD_UPDATE "update"

#define EV_MSG_EVENT_MOTION_START "start"
#define EV_MSG_EVENT_MOTION_END "end"
#define EV_MSG_EVENT_CONN_CONN "connect"
#define EV_MSG_EVENT_CONN_DISCONN "disconnect"

#define EV_NUM_CACHE_PERPEER 100
#define MAX_EVENT_QUEUE_SIZE 50

//
string body2str(vector<uint8_t> body);
vector<uint8_t> data2body(char* data, int len);
vector<uint8_t> str2body(string const &str);
// proto: 1. on router [sender_id] [target_id] [body]
//        2. on dealer [sender_id] [body]
vector<vector<uint8_t> > z_recv_multiple(void *s, bool nowait=false);
// proto [sender_id(only when no identifier set in setsockopts)] [target_id] [body]
int z_send_multiple(void *s, vector<vector<uint8_t> >&body);
/// setup router
int setupRouter(void **ctx, void **s, string addr);
/// setup dealer
/// @return 0 success, otherwise failed
int setupDealer(void **ctx, void **s, string addr, string ident);
/// recv config msg:
/// @return 0 success, otherwise failed.
int recvConfigMsg(void *s, json &config, string addr, string ident);
int forkSubsystem(string devSn, string peerId, int drPort, pid_t &pid);
}

#endif