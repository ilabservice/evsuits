#include "inc/httplib.h"
#include "inc/zmqhelper.hpp"
#include "inc/json.hpp"
#include "spdlog/spdlog.h"
#include "fmt/format.h"
#include "database.h"
#include <thread>
#include <future>
#include <regex>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <array>

std::string exec(const char* cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd, "r"), pclose);
    if (!pipe) {
        // throw std::runtime_error("popen() failed!");
        return result;
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

using namespace std;
using namespace nlohmann;
using namespace httplib;


class WifiMgr {
    private:
    json info;
    string devSn;
    Server srv;
    promise<int> p;
    thread monitor;
    json wifiData;
    int mode, lastMode; // 0:no; network 1: ap; 2: ste
    mutex mutMode;
    const string apdCfgPath = "/etc/apd.conf";
    const string wpaCfgPath = "/etc/wpa_supplicant/wpa_supplicant-wlan1.conf";

    void scanWifi(){
        string res = exec("iwlist wlan1 scan|grep ESSID");
        wifiData["wifi"]["ssids"].clear();
        httplib::detail::split(&res[0], &res[res.size()], '\n', [&](const char *b, const char *e) {
            string ssid;
            ssid.assign(b,e);
            wifiData["wifi"]["ssids"].push_back(ssid);
        });
    }

    json enableMode(int mode){
        json ret;
        ret["code"] = 0;
        ret["msg"] = "ok";

        if( mode == 1) {
            // ap
            this->mode = 1;
            spdlog::info("evwifi {} prepare to enter AP mode", devSn);
            // exec("systemctl dsiable wpa_supplicant@wlan1 ")
            string apdContent = fmt::format("interface=wlan1\ndriver=nl80211\nssid=EVB-{}\nhw_mode=g\n"
            "channel=6\nmacaddr_acl=0\nignore_broadcast_ssid=0\nwpa=0\n", this->info["sn"].get<string>());
            ofstream fileApd(apdCfgPath, ios::out|ios::trunc);
            if(fileApd.is_open()){
                fileApd << apdContent;
                fileApd.close();
                // start hostapd
                auto t = thread([this](){
                    system("pkill hostapd;systemctl stop wpa_supplicant@wlan1;ifconfig wlan1 down;"
                    "ifconfig wlan1 up;ifconfig wlan1 192.168.0.1;hostapd /etc/apd.conf -B");
                    // TODO: check result
                    this->scanWifi();
                });
                t.detach();
            }else{
                ret["code"] = 1;
                string msg = fmt::format("failed to write ap config file to {}", apdCfgPath);
                spdlog::error("evwifi {} {}", devSn,msg);
                ret["msg"] = msg;
            }
        }else if(mode == 2) {
            // station mode
            this->mode = 2;
            spdlog::info("evwifi {} prepare to enter Station mode", devSn);
            if( wifiData["wifi"].count("ssid") == 0 ||  wifiData["wifi"]["ssid"].size() == 0 ||
             wifiData["wifi"].count("password") == 0 ||  wifiData["wifi"]["password"].size() == 0) {
                 string msg = fmt::format("no valid ssid/password provided");
                 spdlog::error("evwifi {} {}", devSn, msg);
                 ret["msg"] = msg;
                 ret["code"] = 3;
             }
             else{
                string wpaContent = fmt::format("ctrl_interface=/run/wpa_supplicant\nupdate_config=1\nap_scan=1\n"
                "network={{\nssid=\"{}\"\npsk=\"{}\"\n}}\n", this->wifiData["wifi"]["ssid"].get<string>(), this->wifiData["wifi"]["password"].get<string>());
                ofstream wpaFile(wpaCfgPath, ios::out|ios::trunc);
                if(wpaFile.is_open()){
                    wpaFile << wpaContent;
                    wpaFile.close();
                    // TODO: verify
                    auto t = thread([this](){
                        // delay for rest return (ifdown caused no networking available)
                        this_thread::sleep_for(chrono::seconds(1));
                        system("pkill hostapd; pkill dhclient;systemctl enable wpa_supplicant@wlan1;systemctl restart wpa_supplicant@wlan1;"
                        "/sbin/ifdown -a --read-environment; /sbin/ifup -a --read-environment");
                        // check status
                        auto s = exec("ifconfig wlan1|grep -v inet6|grep inet");
                        if(s.empty()) {
                            spdlog::error("evwifi {} failed to connect to wifi {} with password {}. initiazing AP mode", this->devSn, 
                            this->wifiData["wifi"]["ssid"].get<string>(), this->wifiData["wifi"]["password"].get<string>());
                            this->mode = 0;
                        }else{
                            system("systemctl restart evdaemon");
                            spdlog::info("evwifi {} successfully connected to wifi {}", this->devSn, this->wifiData["wifi"]["ssid"].get<string>());
                        }
                    });
                    t.detach();
                }else{
                    string msg = fmt::format("failed write wpa config to {}", wpaCfgPath);
                    ret["code"] = 2;
                    ret["msg"] = msg;
                    spdlog::error("evwifi {} {}", devSn, msg);
                }
             }
        }

        return ret;
    }

    public:
    WifiMgr(){
        LVDB::getSn(this->info);
        devSn = this->info["sn"];
        mode = 0;
        wifiData["info"] = this->info;
        wifiData["wifi"] = json();
        wifiData["wifi"]["ssids"] = json();
        //wifiData["wifi"]["ssid"] = string;
        //wifiData["wifi"]["password"] = string;

        monitor = thread([this](){
            // check /etc/systemd/wpa_supplicant@wlan1.service
            // get wlan1 status
            // get wlan1 ip
            // ping outside address

            // this->lastMode = 0;
            while(1){
                // check wifi interface
                {
                    lock_guard<mutex> lk(mutMode);
                    auto s = exec("ifconfig wlan1|grep -v inet6|grep inet");
                    if(s.empty() && this->mode == 0) {
                        spdlog::info("evwifi {} detects no wifi connection, enabling AP mode", this->devSn);
                        this->enableMode(1);
                        this->mode = 1;
                        this->scanWifi();
                    }

                    // TODO: flash light
                    
                }
                
                this_thread::sleep_for(chrono::seconds(10));
            }
        });

        monitor.detach();

        srv.Get("/wifi", [this](const Request& req, Response& res) {
            string mode = req.get_param_value("mode");
            json ret;
            ret["code"] = 0;
            ret["msg"] = "ok";
            string scan = req.get_param_value("scan");
            if(!scan.empty() && scan != "false"){
                this->scanWifi();
                ret["wifiData"] = this->wifiData;
            }

            if(scan.empty() && !mode.empty()){
                try{
                    auto i = stoi(mode);
                    if(i == 2) {
                        string ssid = req.get_param_value("ssid");
                        string password = req.get_param_value("password");
                        if(ssid.empty()||password.empty()){
                            string msg = fmt::format("no valid ssid/password provided");
                            spdlog::error("evwifi {} {}", this->devSn, msg);
                            ret["msg"] = msg;
                            ret["code"] = 3;
                        }else{
                            this->wifiData["wifi"]["ssid"] = ssid;
                            this->wifiData["wifi"]["password"] = password;
                        }
                    }

                    if(ret["code"] == 0) {
                        ret = this->enableMode(i);
                    }
                    
                }catch(exception &e){
                    string msg = fmt::format("exception in convert mode {} to int:{}", mode, e.what());
                    ret["code"] = -1;
                    ret["msg"] = msg;
                    spdlog::error("evwifi {} {}", devSn, msg);
                }
            }
            
            res.set_content(ret.dump(), "text/json");
        });

        srv.listen("0.0.0.0", 80);
    }
};

int main(){
    WifiMgr mgr;
}