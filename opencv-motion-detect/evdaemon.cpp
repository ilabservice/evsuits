/*
module: evdaemon
description: to monitor and configure all other components. runs only one instance per host.
author: Bruce.Lu <lzbgt@icloud.com>
update: 2019/08/30
*/

#include "inc/tinythread.hpp"
#include "inc/httplib.h"
#include "inc/zmqhelper.hpp"
#include "inc/database.h"
#include "inc/json.hpp"

using namespace std;
using namespace httplib;
using namespace nlohmann;

class HttpSrv{
    private:
    Server svr;
    json config;
    json info;

    void setMonitorThread() {

    }

    protected:
    public:
    void run(){
        setMonitorThread();
        // get config
        svr.Get("/info", [this](const Request& req, Response& res){
            LVDB::getSn(this->info);
            res.set_content(this->info.dump(), "text/json");
        });

        svr.Get("/config", [this](const Request& req, Response& res){
            LVDB::getSn(this->info);
            LVDB::getLocalConfig(this->config);
            res.set_content(this->config.dump(), "text/json");
        });

        svr.Post("/config", [this](const Request& req, Response& res){
            try{
                json newConfig = json::parse(req.body);
                LVDB::setLocalConfig(newConfig);
                this->config = newConfig;

            }catch(exception &e) {
                json ret;
                ret["code"] = 1;
                ret["msg"] = e.what();
                ret["time"] = chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count();
                res.set_content(ret.dump(), "text/json");
            }
        });

        svr.Post("/reset", [](const Request& req, Response& res){
            
        });

        svr.listen("0.0.0.0", 8088);
    }

    HttpSrv(){

    };
    ~HttpSrv(){};
};

int main(){
    HttpSrv srv;
    srv.run();
}