#pragma GCC diagnostic ignored "-Wunused-private-field"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include <stdlib.h>
#include <string>
#include <thread>
#include <iostream>
#include <chrono>
#include <future>

#ifdef OS_LINUX
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include "vendor/include/zmq.h"
#include "tinythread.hpp"
#include "common.hpp"
#include "database.h"

using namespace std;

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

    void init(){
        
    }
    protected:
    public:
    EvMgr() {
        init();
    }
    ~EvMgr(){

    }

};


int main(int argc, const char *argv[]){
    return 0;
}