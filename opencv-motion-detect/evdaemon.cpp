/*
module: evdaemon
description: to monitor and configure all other components. runs only one instance per host.
author: Bruce.Lu <lzbgt@icloud.com>
created: 2019/09/04
update: 2019/09/10
*/


#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored  "-Wunused-lambda-capture"

#include <queue>
#include <cstdlib>
#include <algorithm>
#include "inc/tinythread.hpp"
#include "inc/httplib.h"
#include "inc/zmqhelper.hpp"
#include "inc/database.h"
#include "inc/json.hpp"
#include "inc/utils.hpp"
#include <unistd.h>

using namespace std;
using namespace httplib;
using namespace nlohmann;
using namespace zmqhelper;


#define EV_FILE_LVDB_DAEMON "/opt/lvl/daemon"

class EvDaemon{
    private:
    Server svr;
    json config;
    json deltaCfg;
    json info;
    int port = 8088;
    thread thMon;
    string devSn, daemonId;
    int portRouter = 5549;
    thread::id thIdMain;
    thread thRouter;
    thread thCloud;
    bool bReload = false;
    bool bBootstrap = true;
    // peerData["status"];
    // peerData["pids"];
    // peerData["config"];
    json peerData;
    unordered_map<string, queue<vector<vector<uint8_t> >> > cachedMsg;
    mutex cacheLock;
    queue<string> eventQue;
    mutex eventQLock;
    mutex cfgLock;

    /// module gid to process id
    json mapModsToPids;

    // for zmq
    void *pRouterCtx = nullptr, *pRouter = nullptr;
    void *pDealerCtx = nullptr, *pDealer = nullptr;
    string cloudAddr = "tcp://127.0.0.1:5548";

    /// tracking sub-systems: evmgr, evpuller, evpusher, evml*, evslicer etc.
    json mapSubSystems;

    int reloadCfg(string subModGid) {
        int bootType = 0;
        if(subModGid == "ALL") {
            bootType = 1;
        }else if(subModGid.empty()){
            bootType = 2;
        }else{
            bootType = 3;
        }

        int ret = LVDB::getSn(this->info);
        if(ret < 0) {
            spdlog::error("evdaemon {} failed to get info", this->devSn);
            return 1;
        }

        this->devSn = this->info["sn"];
        this->daemonId = this->devSn + ":evdaemon:0";
    
        // apply config
        try{
            // lock_guard<mutex> lock(cacheLock);
            json &data = this->config;
            string peerId;
            pid_t pid;
            for(auto &[k,v]:data.items()) {
                if(k == this->devSn) {
                    // startup evmgr
                    peerId = v["sn"].get<string>() + ":evmgr:0";
                    // offline
                    this->peerData["config"][peerId] = v;
                    if(this->peerData["status"].count(peerId) == 0||this->peerData["status"][peerId] == 0) {
                        this->peerData["status"][peerId] = 0;
                        if(bootType == 1 || (bootType == 3 && subModGid == peerId)) {
                            ret = zmqhelper::forkSubsystem(devSn, peerId, portRouter, pid);
                            if(ret != 0) {
                                spdlog::error("evdaemon {} failed to fork subsystem: {}", devSn, peerId);
                                // TODO: clean up and reload config
                                return -3;
                            }
                            this->peerData["pids"][peerId] = pid;
                            spdlog::info("evdaemon {} created subsystem {}", devSn, peerId); 
                        }  
                    }else{
                        // TODO
                    }
                }

                // startup other submodules
                spdlog::info("dump: {}", v.dump());
                json &ipcs = v["ipcs"];
                for(auto &ipc : ipcs) {
                    json &modules = ipc["modules"];
                    for(auto &[mn, ml] : modules.items()) {
                        for(auto &m : ml) {
                            if(m["sn"] != this->devSn) {
                                continue;
                            }
                            if(m.count("enabled") == 0 || m["enabled"] == 0) {
                                spdlog::warn("evdaemon {} {} was disabled, ignore", this->devSn, mn);
                            }else{
                                string peerName;
                                ret = cfgutils::getPeerId(mn, m, peerId, peerName);
                                if(ret != 0) {
                                    continue;
                                }

                                this->peerData["config"][peerId] = v;

                                if(this->peerData["status"].count(peerId) == 0||this->peerData["status"][peerId] == 0) {
                                    this->peerData["status"][peerId] = 0;
                                    if(bootType == 1 || (bootType == 3 && subModGid == peerId)){
                                        ret = zmqhelper::forkSubsystem(devSn, peerId, portRouter, pid);
                                        if(ret != 0) {
                                            spdlog::error("evdaemon {} failed to fork subsystem: {}", devSn, peerId);
                                            // TODO: cleanup and reload 
                                            return -2;
                                        }
                                        this->peerData["pids"][peerId] = pid;
                                        spdlog::info("evdaemon {} created subsystem {}", devSn, peerId); 
                                    }
                                }else{
                                    // TODO:
                                }
                            }
                        }
                    }
                }
            }
        }catch(exception &e) {
            spdlog::error("evdaemon {} exception reload and apply configuration: {}:\n{}", this->devSn, e.what(), this->config.dump());
            return -1;
        }

        return 0;
    }

    void cleanupSubSystems(){
        spdlog::info("evdaemon {} peerData {}", this->devSn, this->peerData.dump());
        json &pd = this->peerData;
        for(auto &[k,v]: pd["pids"].items()){
            //kill(v, SIGTERM);
            if(this->peerData["status"].count(k) != 0){
                this->peerData["status"].erase(k);
            }

            // if(this->peerData["config"].count(k) != 0){
            //     this->peerData["config"].erase(k);
            // }
            if(this->peerData["pids"].count(k) != 0) {
                this->peerData["pids"].erase(k);
            }
            
        }
    }

    void setupSubSystems() {
        thMon = thread([this](){
            while(true) {
                if(this->bReload) {
                    //cleanupSubSystems();
                    int ret;
                    if(bBootstrap){
                        ret = reloadCfg("ALL");
                    }else{
                        ret = reloadCfg("");
                    }

                    if(ret != 0) {
                        cleanupSubSystems();
                    }else{
                        bReload = false;
                    }
                }

                this_thread::sleep_for(chrono::seconds(5));
            }
        });

        thMon.detach();
    }

    int handleEdgeMsg(vector<vector<uint8_t> > &body)
    {
        int ret = 0;
        zmq_msg_t msg;
        // ID_SENDER, ID_TARGET, meta ,MSG
        string selfId, peerId, meta;
        if(body.size() == 2 && body[1].size() == 0) {
            selfId = body2str(body[0]);
            bool eventConn = false;
            // XTF2BJR9:evslicer:1
            auto sp = strutils::split(selfId, ':');
            if(sp.size() != 3) {
                spdlog::warn("evdaemon {} inproper peer id: {}", devSn, selfId);
                return -1;
            }

            if(peerData["status"].count(selfId) == 0 || peerData["status"][selfId] == 0) {
                peerData["status"][selfId] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
                spdlog::info("evdaemon {} peer connected: {}", devSn, selfId);
                eventConn = true;
                spdlog::debug("evdaemon {} update status of {} to 1 and send config", devSn, selfId);
                string cfg = peerData["config"][selfId].dump();
                json j;
                j["type"] = EV_MSG_META_CONFIG;
                string meta = j.dump();
                vector<vector<uint8_t> > v = {str2body(selfId), str2body(this->daemonId), str2body(meta), str2body(cfg)};
                z_send_multiple(pRouter, v);
            }
            else {
                peerData["status"][selfId] = 0;
                if(peerData["pids"].count(selfId) != 0) {
                    peerData["pids"].erase(selfId);
                }

                spdlog::warn("evdaemon {} peer disconnected: {}", devSn, selfId);

                if(bBootstrap){
                    ret = reloadCfg(selfId);
                }else{
                    ret = reloadCfg("");
                }
                
                if(ret != 0) {
                    cleanupSubSystems();
                }
                       
            }

            // event
            json jEvt;
            jEvt["type"] = EV_MSG_TYPE_CONN_STAT;
            jEvt["gid"] = selfId;
            jEvt["ts"] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
            if(eventConn) {
                jEvt["event"] = EV_MSG_EVENT_CONN_CONN;
            }
            else {
                jEvt["event"] = EV_MSG_EVENT_CONN_DISCONN;
            }

            eventQue.push(jEvt.dump());
            if(eventQue.size() > MAX_EVENT_QUEUE_SIZE) {
                eventQue.pop();
            }

            return 0;
        }
        else if(body.size() != 4) {
            spdlog::warn("evdaemon {} dropped an invalid message, size: {}", devSn, body.size());
            return 0;
        }

        meta = body2str(body[2]);
        selfId = body2str(body[0]);
        peerId = body2str(body[1]);
        // update status;
        this->peerData["status"][selfId] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();

        // msg to peer
        string myId = devSn + ":0:0";
        int minLen = std::min(body[1].size(), myId.size());
        if(memcmp((void*)(body[1].data()), myId.data(), minLen) != 0) {
            // message to other peer
            // check peer status
            vector<vector<uint8_t> >v = {body[1], body[0], body[2], body[3]};
            if(peerData["status"].count(peerId)!= 0 && peerData["status"][peerId] != 0) {
                spdlog::info("evdaemon {} route msg from {} to {}", devSn, selfId, peerId);
                ret = z_send_multiple(pRouter, v);
                if(ret < 0) {
                    spdlog::error("evdaemon {} failed to send multiple: {}", devSn, zmq_strerror(zmq_errno()));
                }
            }
            else {
                // cache
                spdlog::warn("evdaemon {} cached msg from {} to {}", devSn, selfId, peerId);
                lock_guard<mutex> lock(cacheLock);
                cachedMsg[peerId].push(v);
                if(cachedMsg[peerId].size() > EV_NUM_CACHE_PERPEER) {
                    cachedMsg[peerId].pop();
                }
            }

            // check if event
            try {
                string metaType = json::parse(meta)["type"];
                if(metaType == EV_MSG_META_EVENT) {
                    eventQue.push(body2str(body[3]));
                    if(eventQue.size() > MAX_EVENT_QUEUE_SIZE) {
                        eventQue.pop();
                    }
                }
            }

            catch(exception &e) {
                spdlog::error("evdaemon {} exception parse event msg from {} to {}: ", devSn, selfId, peerId, e.what());
            }
        }
        else {
            // message to mgr
            // spdlog::info("evdaemon {} subsystem report msg received: {}; {}; {}", devSn, zmqhelper::body2str(body[0]), zmqhelper::body2str(body[1]), zmqhelper::body2str(body[2]));
            if(meta == "pong"||meta == "ping") {
                // update status
                spdlog::info("evdaemon {}, ping msg from {}", devSn, selfId);
                if(meta=="ping") {
                    if(cachedMsg.find(selfId) != cachedMsg.end()) {
                        while(!cachedMsg[selfId].empty()) {
                            lock_guard<mutex> lock(cacheLock);
                            auto v = cachedMsg[selfId].front();
                            cachedMsg[selfId].pop();
                            ret = z_send_multiple(pRouter, v);
                            if(ret < 0) {
                                spdlog::error("evdaemon {} failed to send multiple: {}", devSn, zmq_strerror(zmq_errno()));
                            }else{
                                spdlog::info("evdaemon {} cached msg sent from {} to {} of type: {}, content: {}", body2str(v[1]), body2str(v[0]), body2str(v[2]), body2str(v[3]));
                            }
                        }
                    }
                }
            }
            else {
                // TODO:
                spdlog::warn("evdaemon {} received unknown meta {} from {}", devSn, meta, selfId);
            }
        }

        return ret;
    }

    int handleCloudMsg(vector<vector<uint8_t> > &v) {
        int ret = 0;
        zmq_msg_t msg;
        // ID_SENDER, meta ,MSG
        string peerId, meta;
        if(v.size() != 3) {
            string msg;
            for(auto &s:v) {
                msg += body2str(s) + ";";
            }
            spdlog::error("evdaemon {} received invalid msg from cloud {}", devSn, msg);
        }else{
            try{
                string meta = json::parse(v[1])["type"];
                string peerId = body2str(v[0]);
                json data = json::parse(body2str(v[2]));

                // from cloudsvc
                if(peerId == "evcloudsvc") {
                    // its configuration message
                    if(meta == EV_MSG_META_CONFIG) {
                        if(data.size() == 0) {
                            spdlog::error("evdaemon {} received invalid empty config", devSn);
                        }else{
                            this->deltaCfg = json::diff(this->config, data);
                            if(this->deltaCfg.size() != 0) {
                                this->config = data;
                                this->bReload = true;
                                spdlog::info("evdaemon {} received cloud config diff:\n{}\nnew\n{}", devSn, this->deltaCfg.dump(4), data.dump());
                                ret = LVDB::setLocalConfig(data, "", EV_FILE_LVDB_DAEMON);
                                if(ret < 0) {
                                    spdlog::error("evdameon {} failed to save new config to local db: {}", devSn, data.dump());
                                }
                                // TODO: detailed diff on submodules
                            }else{
                                spdlog::info("evdaemon {} received same configuration and ignored: {}", devSn, data.dump());
                            }
                        }
                    }
                }else{
                    // from peer
                    spdlog::info("evdaemon {} msg from peer {}: {}", devSn, peerId, data.dump());
                }            

            }catch(exception &e) {
                spdlog::error("evdaemon {} file {}:{} exception {}", devSn, __FILE__, __LINE__, e.what());
            }
        }
        return 0;
    }

    protected:
    public:
    void run(){

        setupSubSystems();

        // get config
        svr.Get("/info", [this](const Request& req, Response& res){
            LVDB::getSn(this->info);
            res.set_content(this->info.dump(), "text/json");
        });

        svr.Post("/info", [this](const Request& req, Response& res){
            json ret;
            ret["code"] = 0;
            ret["msg"] = "ok";
            string sn = req.get_param_value("sn");
            if(sn.empty()){
                ret["code"] = 1;
                ret["msg"] = "no sn in param";
            }else{
                json info;
                info["sn"] = sn;
                // TODO:

                info["lastboot"] =  chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
                LVDB::setSn(info);
            }
            res.set_content(this->info.dump(), "text/json");
        });

        svr.Get("/config", [this](const Request& req, Response& res){
            LVDB::getLocalConfig(this->config);
            res.set_content(this->config.dump(), "text/json");
        });

        svr.Post("/config", [this](const Request& req, Response& res){
            json ret;
            ret["code"] = 0;
            ret["msg"] = "ok";
            ret["time"] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
            try{
                json newConfig;
                newConfig["data"] = json::parse(req.body)["data"];
                
                LVDB::setLocalConfig(newConfig);
                spdlog::info("evmgr new config: {}", newConfig.dump());
                // TODO: restart other components
                //
            }catch(exception &e) {
                ret.clear();
                ret["code"] = -1;
                ret["msg"] = e.what();
                ret["data"] = req.body;
            }
            res.set_content(ret.dump(), "text/json");
        });

        svr.Get("/sync-cloud", [this](const Request& req, Response& res){
            json ret;
            ret["code"] = 0;
            ret["msg"] = "syncing ...";
            res.set_content(ret.dump(), "text/json");
            this->bReload = true;
        });

        svr.Get("/reset", [](const Request& req, Response& res){
            json ret;
            ret["code"] = 0;
            ret["msg"] = "resetting ...";
            res.set_content(ret.dump(), "text/json");
            kill(getpid(), SIGTERM);
        });

        svr.listen("0.0.0.0", 8088);
    }

    EvDaemon(){
        int ret = 0;
        string dir_ = string("mkdir -p") + EV_FILE_LVDB_DAEMON;
        system(dir_.c_str());
        // get sn of device
        json info;
        try{
            LVDB::getSn(info);
        }catch(exception &e) {
            spdlog::error("evdaemon failed to get sn: {}", e.what());
            exit(1);
        }
        
        spdlog::info("evdaemon boot \n{}",info.dump());

        devSn = info["sn"];
        
        char* strEnv = getenv("BOOTSTRAP");
        if(strEnv != nullptr && memcmp(strEnv, "false", 5) == 0) {
            bBootstrap = false;
        }

        // http port
        strEnv = getenv("DAEMON_PORT");
        if(strEnv != nullptr) {
            port = stoi(strEnv);
        }

        json cfg;
        ret = LVDB::getLocalConfig(cfg, "", EV_FILE_LVDB_DAEMON);
        if(ret < 0) {
            spdlog::info("evdameon {} no local config", devSn, cfg.dump());
        }else{
            this->config = cfg;
        }

        // zmq router port
        strEnv = getenv("ROUTER_PORT");
        if(strEnv != nullptr) {
            portRouter = stoi(strEnv);
        }

        string addr = string("tcp://*:") + to_string(portRouter);

        // setup router
        ret = zmqhelper::setupRouter(&pRouterCtx, &pRouter, addr);
        if(ret < 0) {
            spdlog::error("evdaemon {} setup router: {}", this->devSn, addr);
            exit(1);
        }
        // setup edge msg processor
        thRouter = thread([this](){
            while(true){
                auto v = zmqhelper::z_recv_multiple(this->pRouter);
                if(v.size() == 0) {
                    spdlog::error("evdaemon {} failed to receive msg {}", this->devSn, zmq_strerror(zmq_errno()));
                }else{
                    handleEdgeMsg(v);
                }
            }
        });
        thRouter.detach();

        spdlog::info("evdaemon {} edge message processor had setup {}", devSn, addr);


        // dealer port
        strEnv = getenv("CLOUD_ADDR");
        if(strEnv != nullptr) {
            cloudAddr = strEnv;
        }

        // setup dealer
        ret = zmqhelper::setupDealer(&pDealerCtx, &pDealer, cloudAddr, devSn);
        if(ret != 0) {
            spdlog::error("evdaemon {} failed to setup dealer", devSn);
            exit(1);
        }
        spdlog::info("evdaemon {} connected to cloud {}", devSn, cloudAddr);
        // setup cloud msg processor
        thCloud = thread([this](){
            while(true){
                auto v = zmqhelper::z_recv_multiple(this->pDealer);
                if(v.size() == 0) {
                    spdlog::error("evdaemon {} failed to receive msg {}", this->devSn, zmq_strerror(zmq_errno()));
                }else{
                    handleCloudMsg(v);
                }
            }
        });

        thCloud.detach();
        spdlog::info("evdaemon {} cloud message processor had setup {}", devSn, cloudAddr);

        this->thIdMain = this_thread::get_id();

        /// peerId -> value
        peerData["status"] = json();
        peerData["pids"] = json();
        peerData["config"] = json();
    };
    ~EvDaemon(){};
};

void cleanup(int signal) {
  int status;
  while (waitpid((pid_t) (-1), 0, WNOHANG) > 0) {}
}

int main(){
    signal(SIGCHLD, cleanup);
    //sigignore(SIGCHLD);
    EvDaemon srv;
    srv.run();
}