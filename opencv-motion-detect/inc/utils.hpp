#ifndef __EV_UTILS_H__
#define __EV_UTILS_H__

#include <vector>
#include <chrono>
#include <thread>
#include <map>
#include <sstream>
#include "json.hpp"
#include "spdlog/spdlog.h"
#include "httplib.h"

#define EVCLOUD_REQ_E_CONN -2
#define EVCLOUD_REQ_E_DATA -3
#define EVCLOUD_REQ_E_PARAM -4
#define EVCLOUD_REQ_E_ABORT -5
#define EVCLOUD_REQ_E_NONE 0

using namespace std;
using namespace nlohmann;
using namespace httplib;

// cloudutils
namespace cloudutils
{
/// [deprecated] ref: ../config.json
json registry(json &conf, string sn, string module);
/// req config
json reqConfig(json &info);
} // namespace cloudutils


///
namespace strutils{
vector<string> split(const std::string& s, char delimiter);
}//namespace strutils

namespace cfgutils {
   int getPeerId(string modName, json& modElem, string &peerId, string &peerName);
   json *findModuleConfig(string peerId, json &data);
}

struct StrException;

#endif
