#pragma GCC diagnostic ignored "-Wunused-private-field"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include <stdlib.h>
#include <string>
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <queue>

#ifdef OS_LINUX
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include "zmqhelper.hpp"
#include "tinythread.hpp"
#include "common.hpp"
#include "database.h"

using namespace std;
using namespace zmqhelper;

/**
 *  functions:
 *  app update
 *  control msg
 *
 **/

class EvMgr:public TinyThread {
private:
    void *pRouterCtx = NULL;
    void *pRouter = NULL;
    json config;
    string devSn;
    json peerStatus;
    json jmgr;
    unordered_map<string, queue<vector<vector<uint8_t> >> > cachedMsg;
    mutex cacheLock;
    queue<json> *eventQue;
    void init()
    {
        int ret;
        bool inited = false;
        // TODO: load config from local db
        devSn = "ILSEVMGR1";
        while(!inited) {
            try {
                config = json::parse(cloudutils::config);
                spdlog::info("config dumps: \n{}", config.dump());
                // TODO: verify sn
                if(!config.count("data")||!config["data"].count(devSn)||!config["data"][devSn].count("ipcs")){
                    spdlog::error("evmgr {} invalid config. reload now...", devSn);
                    this_thread::sleep_for(chrono::seconds(3));
                    continue;
                }
                jmgr =  config["data"][devSn];
                string proto = jmgr["proto"];
                string addr;
                
                if(proto != "zmq"){
                    spdlog::error("evmgr {} unsupported protocol: {}, try fallback to zmq instead now...", devSn, proto);
                }
                addr = "tcp://" + jmgr["addr"].get<string>() + ":" + to_string(jmgr["port-router"]);
                // setup zmq
                // TODO: connect to cloud

                // router service
                pRouterCtx = zmq_ctx_new();
                pRouter = zmq_socket(pRouterCtx, ZMQ_ROUTER);
                ret = zmq_bind(pRouter, addr.c_str());
                if(ret < 0) {
                    spdlog::error("evmgr {} failed to bind zmq at {} for reason: {}, retrying load configuration...", devSn, addr, zmq_strerror(zmq_errno()));
                    this_thread::sleep_for(chrono::seconds(3));
                    continue;
                }
                inited = true;
            }
            catch(exception &e) {
                spdlog::error("evmgr {} exception on init() for: {}, retrying load configuration...", devSn, e.what());
                this_thread::sleep_for(chrono::seconds(3));
                continue;
            }
        }
        spdlog::info("evmgr {} successfuly inited", devSn);
    }

    int mqErrorMsg(string cls, string devSn, string extraInfo, int ret) {
        if(ret < 0) {
            spdlog::error("{} {} {}:{} ", cls, devSn, extraInfo, zmq_strerror(zmq_errno()));
        }
        return ret;
    }

    int handleMsg(vector<vector<uint8_t> > &body) {
        int ret = 0;
        zmq_msg_t msg;
        // ID_SENDER, ID_TARGET, meta ,MSG
        if(body.size() != 4) {
            spdlog::warn("evmgr {} dropped a message, since its size is incorrect: {}", devSn, body.size());
            return 0;
        }
        
        string meta = body2str(body[2]);
        string selfId = body2str(body[0]);
        string peerId = body2str(body[1]);
        // update status;
        this->peerStatus[selfId] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();

        if(memcmp((void*)(body[1].data()), (devSn +":0:0").data(), body[1].size()) != 0) {
            // message to other peer
            // check peer status
            vector<vector<uint8_t> >v = {body[1], body[0], body[2], body[3]};
            if(peerStatus.count(peerId)!= 0) {
                auto t = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count() - peerStatus[peerId].get<long long>();
                if(t > EV_HEARTBEAT_SECONDS){
                    peerStatus[peerId] = 0;
                    // need cache
                }else{
                    spdlog::info("evmgr {} route msg from {} to {}", devSn, selfId, peerId);
                    ret = z_send_multiple(pRouter, v);
                    if(ret < 0) {
                        spdlog::error("evmgr {} failed to send multiple: {}", devSn, zmq_strerror(zmq_errno()));
                    }
                }
            }else{
                peerStatus[peerId] = 0;
                // need cache
            }

            if(peerStatus[peerId] == 0) {
                // cache
                spdlog::warn("evmgr {} cached msg from {} to {}", devSn, selfId, peerId);
                lock_guard<mutex> lock(cacheLock);
                cachedMsg[peerId].push(v);
                if(cachedMsg[peerId].size() > EV_NUM_CACHE_PERPEER) {
                    cachedMsg[peerId].pop();
                }
            }
        }else{
            // message to mgr
            spdlog::info("evmgr {} subsystem report msg received: {}; {}; {}", devSn, zmqhelper::body2str(body[0]), zmqhelper::body2str(body[1]), zmqhelper::body2str(body[2]));
            if(meta == "pong"||meta == "ping") {
                // update status
                spdlog::info("evmgr {}, ping msg from {}", devSn, selfId);
                if(meta=="ping") {
                    if(cachedMsg.find(selfId) != cachedMsg.end()) {
                        while(!cachedMsg[selfId].empty()){
                            lock_guard<mutex> lock(cacheLock);
                            auto v = cachedMsg[selfId].front();
                            cachedMsg[selfId].pop();
                            ret = z_send_multiple(pRouter, v);
                            if(ret < 0) {
                                spdlog::error("evmgr {} failed to send multiple: {}", devSn, zmq_strerror(zmq_errno()));
                            }  
                        }
                    }
                }
            }else{
                // TODO:
                spdlog::warn("evmgr {} received unknown meta {} from {}", devSn, meta, selfId);
            }
        }

        return ret;
    }

protected:
    void run(){
        bool bStopSig = false;
        int ret = 0;
        zmq_msg_t msg;

        while (true) {
            if(checkStop() == true) {
                bStopSig = true;
                break;
            }
            auto body = z_recv_multiple(pRouter,false);
            if(body.size() == 0) {
                spdlog::error("evmgr {} failed to receive multiple msg: {}", devSn, zmq_strerror(zmq_errno()));
                continue;   
            }
            // full proto msg received.
            handleMsg(body);
        }
    }
public:
    EvMgr() = delete;
    EvMgr(EvMgr &&) = delete;
    EvMgr(EvMgr &) = delete;
    EvMgr(const EvMgr &) = delete;
    EvMgr& operator=(const EvMgr &) = delete;
    EvMgr& operator=(EvMgr &&) = delete;
    EvMgr(queue<json> *queue):eventQue(queue)
    {
        init();
    }
    ~EvMgr()
    {
        if(pRouter != NULL) {
            zmq_close(pRouter);
            pRouter = NULL;
        }
        if(pRouterCtx != NULL){
            zmq_ctx_destroy(pRouterCtx);
            pRouterCtx = NULL;
        }
    }
};

int main(int argc, const char *argv[])
{
    av_log_set_level(AV_LOG_ERROR);
    spdlog::set_level(spdlog::level::debug);
    queue<json> queue;
    EvMgr mgr(&queue);
    mgr.join();
    return 0;
}