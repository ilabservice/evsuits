#include "utils.hpp"


// cloudutils
namespace cloudutils {
/// [deprecated] ref: ../config.json
json registry(json &conf, string sn, string module)
{
    json ret;
    string api;
    try {
        api = conf.at("data").at(sn).at("api-cloud").get<string>() + "/register";
        Uri uri=Uri::Parse(api);
        if(uri.Host.empty()||uri.Port.empty()||uri.Protocol.find("http") == string::npos||uri.Path.empty()) {
            string msg = "registry error. invalid api-cloud in config: " + api;
            ret["code"] = 1;
            ret["msg"] = msg;
            spdlog::error(msg);
            return ret;
        }

        Params params;
        params.emplace("sn", sn);
        params.emplace("module", module);
        Client cli(uri.Host.c_str(), stoi(uri.Port));

        auto res = cli.Post("/register", Headers(), params, conf.dump(),  "text/json");
        spdlog::debug("{} {} registry res from cloud : {}", __FILE__, __LINE__, res->body);
        ret = json::parse(res->body);
    }
    catch(exception &e) {
        ret["code"] = -1;
        string msg = string(__FILE__) + ":" + to_string(__LINE__) + string(": registry exception - ") + e.what();
        ret["msg"] = msg;
        spdlog::error(msg);
    }

    // /Client cli;
    return ret;
}

/// req config
json reqConfig(json &info)
{
    json ret;
    string api;
    try {
        api = info.at("api-cloud").get<string>();
        Uri uri=Uri::Parse(api);
        string sn = info.at("sn").get<string>();
        if(uri.Host.empty()||uri.Port.empty()||uri.Protocol.find("http") == string::npos) {
            string msg = string(__FILE__) +":" + to_string(__LINE__) + ": request cloud configuration error. invalid api-cloud in info: " + api;
            ret["code"] = EVCLOUD_REQ_E_PARAM;
            ret["msg"] = msg;
            spdlog::error(msg);
            return ret;
        }

        Params params;
        params.emplace("sn", sn);
        Client cli(uri.Host.c_str(), stoi(uri.Port));

        auto res = cli.Get("/config", Headers(), params);
        if(res == nullptr || res->status != 200) {
            const char *msg = NULL;
            if(res == nullptr) {
                msg = (string("error to connect to server: ") + api + "/config").c_str();
                ret["code"] = EVCLOUD_REQ_E_CONN;
            }
            else {
                msg = httplib::detail::status_message(res->status);
                ret["code"] = res->status;
            }
            spdlog::debug("failed to reqConfig. {}", msg);
            ret["msg"] = msg;
        }
        else {
            spdlog::debug("{} {} registry res from cloud : {}", __FILE__, __LINE__, res->body);
            ret = json::parse(res->body);
        }
    }
    catch(exception &e) {
        ret["code"] = EVCLOUD_REQ_E_DATA;
        string msg = string(__FILE__) + ":" + to_string(__LINE__) + string(": registry exception - ") + e.what();
        ret["msg"] = msg;
        spdlog::error(msg);
    }

    // /Client cli;
    return ret;
}

} // namespace cloudutils


///
namespace strutils {

vector<string> split(const std::string& s, char delimiter)
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(s);
    while (getline(tokenStream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

bool isIpStr(string ip)
{
    int cnt = 3*4 + 3;
    if(ip.size() == 0 || ip.size() > cnt) {
        return false;
    }
    auto v = strutils::split(ip, '.');
    if(v.size() == 0 || v.size () != 4) {
        return false;
    }

    return true;
}

}//namespace strutils

namespace cfgutils {
int getPeerId(string modName, json& modElem, string &peerId, string &peerName)
{
    try {
        if(modName == "evmgr") {
            peerId = modElem["sn"].get<string>() + ":evmgr:0";
            peerName = modName;
        }
        else if(modName == "evml") {
            peerId = modElem["sn"].get<string>() + ":evml" + modElem["type"].get<string>() + ":" + to_string(modElem["iid"]);
            peerName = modName + modElem["type"].get<string>();
        }
        else {
            peerId = modElem["sn"].get<string>() + ":" + modName + ":" + to_string(modElem["iid"]);
            peerName = modName;
        }
    }
    catch(exception &e) {
        spdlog::error("failed to get gid for {} in {}: {}", modName, modElem.dump(), e.what());
        return -1;
    }

    return 0;
}

/// ret["data"] is json array contains gids
/// ret["code"], ["msg"] indicates error if not 0.
/// ipcIdx: -1 - all IPCS, otherwise - ipc specified by ipcIdx
json getModuleGidsFromCfg(string sn, json &data, string caller, int ipcIdx)
{
    json ret;
    bool hasError = false;
    ret["code"] = 0;
    ret["msg"] = "ok";
    ret["data"] = json();
    try {
        // lock_guard<mutex> lock(cacheLock);
        string peerId;
        pid_t pid;
        for(auto &[k,v]:data.items()) {
            if(ipcIdx == -1) {
                if(k == sn) {
                    peerId = v["sn"].get<string>() + ":evmgr:0";
                    ret["data"].push_back(peerId);
                }
            }


            // startup other submodules
            json &ipcs = v["ipcs"];
            int idx = 0;
            for(auto &ipc : ipcs) {
                if(ipcIdx != -1 && idx != ipcIdx) {
                    continue;
                }

                json &modules = ipc["modules"];
                for(auto &[mn, ml] : modules.items()) {
                    for(auto &m : ml) {
                        if(m["sn"] != sn) {
                            continue;
                        }

                        string peerName;
                        int iret = cfgutils::getPeerId(mn, m, peerId, peerName);
                        if(iret != 0) {
                            // TODO: do we need to treat it more strictly, to make it fails fast???
                            spdlog::error("{} getModuleGidsFromCfg for {} invalid config found in module {} of {}",caller, sn, m.dump(), data.dump());
                            continue;
                        }
                        else {
                            ret["data"].push_back(peerId);
                        }
                    }
                }

                if(ipcIdx != - 1) {
                    break;
                }

                idx++;
            }
        }
    }
    catch(exception &e) {
        string msg = fmt::format("{} getModuleGidsFromCfg  exception {} in config {}",caller, sn, e.what(), data.dump());
        spdlog::error(msg);
        ret["msg"] = msg;
        ret["code"] = -1;
    }

    return ret;
}

json *findModuleConfig(string peerId, json &data)
{
    json *ret = NULL;
    auto pp = strutils::split(peerId, ':');
    if(pp.size() != 3) {
        spdlog::error("invalid peerId: {}", peerId);
        return ret;
    }

    string sn = pp[0];
    string modName = pp[1];
    string iid = pp[2];
    //
    string subMn = modName.substr(0,4);
    if(subMn == "evml") {
        subMn = modName.substr(4, modName.size());
    }
    else {
        subMn = "";
    }

    try {
        for(auto &[k,v]: data.items()) {
            // it's evmgr
            if(modName == "evmgr") {
                if(k == sn) {
                    ret = &v;
                    break;
                }
            }
            else {
                json &ipcs = v["ipcs"];

                for(auto &ipc: ipcs) {
                    json &modules = ipc["modules"];
                    // not evml
                    if(subMn.empty()) {
                        if(modules.count(modName) != 0) {
                            json &ml = modules[modName];
                            for(auto &m : ml) {
                                if(m["sn"] == sn && to_string(m["iid"]) == iid && m["enabled"] != 0) {
                                    ret = &v;
                                    break;
                                }
                            }
                        }
                    }
                    else {
                        if(modules.count("evml") != 0) {
                            json &ml = modules["evml"];
                            for(auto &m: ml) {
                                if(subMn == m["type"] && to_string(m[iid]) == iid && m["sn"] == sn && m["enabled"] != 0) {
                                    ret = &v;
                                    break;
                                }
                            }
                        }
                    }

                    if(ret != NULL) break;
                }
            }
        }
    }
    catch(exception &e) {
        spdlog::error("find module {} in {} exception: {}", peerId, data.dump(), e.what());
        return NULL;
    }

    return ret;
}

/// return json key: gid; value: 0 - stop, 1 - start, 3 - restart
json getModulesOperFromConfDiff(json& oldConfig, json &newConfig, json &diff, string sn)
{
    json ret;
    ret["code"] = 0;
    ret["msg"] = "ok";
    ret["data"] = json();
    bool hasError = false;
    if(diff.size() == 0) {
        return ret;
    }
    spdlog::info("getModulesOperFromConfDiff for sn {}. diff: {}, newConfig: {}", sn, diff.dump(), newConfig.dump());
    try {
        for(auto &d: diff) {
            spdlog::info("getModulesOperFromConfDiff path :{}", d.dump());
            if(d.count("path") != 0) {
                string path_ = d["path"];
                bool matched = false;
                // match ipc config
                // eg: /NMXH73Y2/ipcs/0/addr
                // \\w+ could be: addr, user, password, port
                set<string> oprations{"add", "replace", "remove"};
                set<string> pullerTag{"addr", "user", "password", "proto", "port" /*, "sn"*/};

                string ipcRegStr = "/(\\w+)/ipcs/(\\d+)/(\\w+)";
                std::smatch results;
                std::regex ipcRegex(ipcRegStr);
                if (std::regex_match(path_, results, ipcRegex)) {
                    if (results.size() == 4) {
                        matched = true;
                        string mgrSn = results[1].str();
                        int ipcIdx = stoi(results[2].str());
                        string tag = results[3].str();
                        if(pullerTag.find(tag) != pullerTag.end()) {
                            // TODO: op = remove
                            if(d["op"] == "add" || d["op"] == "replace") {
                                // start
                                auto ipc = newConfig[mgrSn]["ipcs"][ipcIdx];
                                auto ipcOld = oldConfig[mgrSn]["ipcs"][ipcIdx];
                                if(ipc.count("modules") == 0 || ipc["modules"].size() == 0 || ipc["moudles"].count("evpuller") ==0 || ipc["modules"]["evpuller"].size() == 0 ) {
                                    string msg = fmt::format("invalid config for ipc[{}]['modules']['evpuller']: {}", ipcIdx, newConfig.dump());
                                    spdlog::error(msg);
                                    ret["msg"] = msg;
                                    hasError = true;
                                    break;
                                }
                                else {
                                    auto &evpullers = ipc["module"]["evpuller"];
                                    int idx = 0;
                                    for(auto &puller:evpullers) {
                                        // strutil
                                        if(puller.count("sn") == 0) {
                                            string msg = fmt::format("invalid config for ipc[{}]['modules']['evpuller'][{}] no sn field: {}", ipcIdx, idx, newConfig.dump());
                                            ret["msg"] = msg;
                                            spdlog::error(msg);
                                            hasError = true;
                                            break;
                                        }

                                        if(puller["sn"].get<string>() != sn) {
                                            spdlog::debug("skip {} for expecting sn: {}", puller.dump(), sn);
                                            continue;
                                        }

                                        if(puller.count("iid") == 0 || puller.count("addr") == 0) {
                                            string msg = fmt::format("invliad config as of having no iid/addr/enabled field in ipc[{}]['modules']['evpuller'][{}]: {}", ipcIdx, idx, newConfig.dump());
                                            spdlog::error(msg);
                                            ret["msg"] = msg;
                                            hasError = true;
                                            break;
                                        }
                                        else {
                                            string gid = sn + ":evpuller:" + to_string(puller["iid"].get<int>());
                                            if(puller.count("enabled") == 0 || puller["enabled"].get<int>() == 0) {
                                                ret["data"][gid] = 0; // stop
                                            }
                                            else {
                                                ret["data"][gid] = 2;
                                            }
                                        }
                                        idx++;
                                    }
                                }
                            }
                        }
                    }
                }
                // else{
                //     spdlog::info("no match for ipc", path_);
                // }

                // match module config
                if(!matched && !hasError) {
                    // /NMXH73Y2/ipcs/0/modules/evpusher/0/urlDest
                    string  moduleRegStr = "/(\\w+)/ipcs/(\\d+)/modules/(\\w+)/(\\d+)/(\\w+)";
                    std::regex moduleRegex(moduleRegStr);
                    std::smatch results;
                    if (std::regex_match(path_, results, moduleRegex)) {
                        if (results.size() == 6) {
                            matched = true;
                            string mgrSn = results[1].str();
                            int ipcIdx = stoi(results[2].str());
                            int modIdx = stoi(results[4].str());
                            string modName = results[3].str();
                            string propName = results[5].str();
                            if(d["op"] == "replace"||d["op"] == "add" || d["op"] == "remove") {
                                auto &oldMod = oldConfig[mgrSn]["ipcs"][ipcIdx]["modules"][modName][modIdx];
                                auto &newMod = newConfig[mgrSn]["ipcs"][ipcIdx]["modules"][modName][modIdx];
                                if(oldMod.count("iid") == 0 || newMod.count("iid") == 0) {
                                    string msg = fmt::format("invalid module config ipcs[{}]['modules'][{}][{}] having no iid field", ipcIdx, modName, modIdx);
                                    spdlog::error(msg);
                                    ret["msg"] = msg;
                                    hasError = true;
                                    break;
                                }
                                else {
                                    if(modName == "evml") {
                                        if(newMod.count("type") == 0) {
                                            string msg = fmt::format("invalid evml module config ipcs[{}]['modules'][{}][{}] having no type field", ipcIdx, modName, modIdx);
                                            spdlog::error(msg);
                                            hasError = true;
                                            break;
                                        }
                                        else {
                                            modName = modName + newMod["type"].get<string>();
                                        }
                                    }

                                    if(newMod.count("sn") == 0) {
                                        string msg = fmt::format("invalid module config ipcs[{}]['modules'][{}][{}] having no sn field", ipcIdx, modName, modIdx);
                                        spdlog::error(msg);
                                        hasError = true;
                                        break;
                                    }

                                    if(newMod["sn"].get<string>() != sn && oldMod.count("sn") != 0 && oldMod["sn"].get<string>() == sn) {
                                        string oldGid = sn + ":" + modName + ":" + to_string(oldMod["iid"].get<int>());
                                        ret["data"][oldGid] = 0;
                                        continue;
                                    }
                                    else if(newMod["sn"].get<string>() != sn && (oldMod.count("sn") == 0 ||(oldMod.count("sn") != 0 && oldMod["sn"].get<string>() != sn))) {
                                        // ignore
                                        continue;
                                    }
                                    else {
                                        // oldSn == newSn == sn, below
                                    }

                                    string oldGid = sn + ":" + modName + ":" + to_string(oldMod["iid"].get<int>());
                                    string newGid = sn + ":" + modName + ":" + to_string(newMod["iid"].get<int>());

                                    if(oldGid != newGid) {
                                        ret["data"][oldGid] = 0;
                                    }

                                    if(propName == "enabled") {
                                        if(newMod.count("enabled") == 0||newMod["enabled"].get<int>() == 0) {
                                            ret["data"][newGid] = 0;
                                        }
                                        else {
                                            ret["data"][newGid] = 1;
                                        }
                                    }
                                    else { // other prop modification
                                        // it was disabled. just ignore
                                        if(ret["data"].count(newGid) != 0 && ret["data"][newGid].get<int>() == 0) {
                                            // nop
                                        }
                                        else {
                                            // restart
                                            ret["data"][newGid] = 2;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // else{
                    //     spdlog::info("no match for module {}", path_);
                    // }
                }

                // whole submodule
                if(!matched && !hasError) {
                    // /PSBV7GKN/ipcs/0/modules/evpusher/0
                    // {"enabled":0,"iid":1,"password":"","sn":"PSBV7GKN","token":"","urlDest":"rtsp://40.73.41.176/PSBV7GKN","user":""}
                    string  moduleRegStr = "/(\\w+)/ipcs/(\\d+)/modules/(\\w+)/(\\d+)";
                    std::regex moduleRegex(moduleRegStr);
                    std::smatch results;
                    if (std::regex_match(path_, results, moduleRegex)) {
                        if (results.size() == 5) {
                            matched = true;
                            string mgrSn = results[1].str();
                            int ipcIdx = stoi(results[2].str());
                            int modIdx = stoi(results[4].str());
                            string modName = results[3].str();
                            json modObj;
                            if(d["op"] == "remove") {
                                modObj = oldConfig[mgrSn]["ipcs"][ipcIdx]["modules"][modName][modIdx];
                            }
                            else {
                                modObj = newConfig[mgrSn]["ipcs"][ipcIdx]["modules"][modName][modIdx];
                            }
                            if(modObj.count("sn") == 0) {
                                if(d["op"] != "remove") {
                                    string msg = fmt::format("invalid modue config having no sn /{}/ipcs/{}/modules/{}/{}", mgrSn, ipcIdx, modName, modIdx);
                                    spdlog::error(msg);
                                    hasError = true;
                                    ret["msg"] = msg;
                                    break;
                                }
                                else {
                                    // nop
                                }
                            }
                            else {
                                if(modObj["sn"].get<string>() == sn) {
                                    if(modName == "evml") {
                                        if(modObj.count("type") == 0) {
                                            string msg = fmt::format("invalid evml module config ipcs[{}]['modules'][{}][{}] having no type field", ipcIdx, modName, modIdx);
                                            spdlog::error(msg);
                                            hasError = true;
                                            break;
                                        }
                                        else {
                                            modName = modName + modObj["type"].get<string>();
                                        }
                                    }

                                    if(modObj.count("iid") == 0) {
                                        string msg = fmt::format("invalid evml module config ipcs[{}]['modules'][{}][{}] having no iid field", ipcIdx, modName, modIdx);
                                        spdlog::error(msg);
                                        hasError = true;
                                        break;
                                    }

                                    string modGid = sn + ":" + modName + ":" + to_string(modObj["iid"].get<int>());
                                    if(d["op"] == "remove") {
                                        ret["data"][modGid] = 0;
                                    }
                                    else {
                                        ret["data"][modGid] = 1;
                                    }
                                }
                                else {
                                    // nop
                                }
                            }
                        }
                    }
                }

                // whole cluster
                if(!matched && !hasError) {
                    // /PSBV7GKN
                    // "value":{"addr":"127.0.0.1","api-cloud":"http://127.0.0.1:8089","ipcs":[{"addr":"172.31.0.129","modules":{"evml":[{"area":200,"enabled":1,"entropy":0.3,"iid":1,"post":30,"pre":3,"sn":"PSBV7GKN","thresh":30,"type":"motion"}],"evpuller":[{"addr":"127.0.0.1","enabled":1,"iid":1,"port-pub":5556,"sn":"PSBV7GKN"}],"evpusher":[{"enabled":0,"iid":1,"password":"","sn":"PSBV7GKN","token":"","urlDest":"rtsp://40.73.41.176/PSBV7GKN","user":""}],"evslicer":[{"enabled":1,"iid":1,"path":"slices","sn":"PSBV7GKN","video-server-addr":"http://40.73.41.176:10009/upload/evtvideos/"}]},"password":"iLabService","port":554,"proto":"rtsp","sn":"iLabService","user":"admin"}],"mqtt-cloud":"<cloud_addr>","port-cloud":5556,"port-router":5550,"proto":"zmq","sn":"PSBV7GKN"}
                    string  clusterRegStr = "/(\\w+)";
                    std::regex clusterRegex(clusterRegStr);
                    std::smatch results;
                    if (std::regex_match(path_, results, clusterRegex)) {
                        if (results.size() == 2) {
                            matched = true;
                            string mgrSn = results[1].str();
                            json mgr;
                            if(d["op"] == "remove") {
                                mgr[mgrSn] = oldConfig[mgrSn];
                            }
                            else {
                                mgr[mgrSn] = newConfig[mgrSn];
                            }

                            json jret = cfgutils::getModuleGidsFromCfg(sn, mgr, "getModulesOperFromConfDiff");
                            spdlog::info("jret: {}", jret.dump());
                            if(jret["code"] != 0) {
                                ret["msg"] = jret["msg"];
                                hasError = true;
                                break;
                            }
                            else {
                                for(auto &k: jret["data"]) {
                                    if(d["op"] == "remove") {
                                        ret["data"][string(k)] = 0;
                                    }
                                    else {
                                        ret["data"][string(k)] = 2;
                                    }
                                }
                            }
                        }
                    }
                }

                // one ipc
                if(!matched && !hasError) {
                    // /PSBV7GKN/ipcs/0"
                    // {"addr":"172.31.0.129","modules":{"evml":[{"area":200,"enabled":1,"entropy":0.3,"iid":1,"post":30,"pre":3,"sn":"PSBV7GKN","thresh":30,"type":"motion"}],"evpuller":[{"addr":"127.0.0.1","enabled":1,"iid":1,"port-pub":5556,"sn":"PSBV7GKN"}],"evpusher":[{"enabled":0,"iid":1,"password":"","sn":"PSBV7GKN","token":"","urlDest":"rtsp://40.73.41.176/PSBV7GKN","user":""}],"evslicer":[{"enabled":1,"iid":1,"path":"slices","sn":"PSBV7GKN","video-server-addr":"http://40.73.41.176:10009/upload/evtvideos/"}]},"password":"iLabService","port":554,"proto":"rtsp","sn":"iLabService","user":"admin"}
                    string  clusterRegStr = "/(\\w+)/ipcs/(\\d+)";
                    std::regex clusterRegex(clusterRegStr);
                    std::smatch results;
                    if (std::regex_match(path_, results, clusterRegex)) {
                        if (results.size() == 3) {
                            matched = true;
                            string mgrSn = results[1].str();
                            int ipcIdx = stoi(results[2].str());
                            json mgr;
                            if(d["op"] == "remove") {
                                mgr[mgrSn] = oldConfig[mgrSn];
                            }
                            else {
                                mgr[mgrSn] = newConfig[mgrSn];
                            }

                            json jret = cfgutils::getModuleGidsFromCfg(sn, mgr, "getModulesOperFromConfDiff", ipcIdx);
                            spdlog::info("jret: {}", jret.dump());
                            if(jret["code"] != 0) {
                                ret["msg"] = jret["msg"];
                                hasError = true;
                                break;
                            }
                            else {
                                for(auto &k: jret["data"]) {
                                    if(d["op"] == "remove") {
                                        ret["data"][string(k)] = 0;
                                    }
                                    else {
                                        ret["data"][string(k)] = 2;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        if(hasError) {
            ret["code"] = 1;
        }
    }
    catch(exception &e) {
        spdlog::error("getModulesOperFromConfDiff exception: {}", e.what());
        ret["code"] = -1;
        ret["msg"] = e.what();
    }

    return ret;
}
} // cfgutils
