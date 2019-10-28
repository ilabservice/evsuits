/*
module: evpusher
description:
author: Bruce.Lu <lzbgt@icloud.com>
created: 2019/08/23
update: 2019/09/10
*/

#include <stdlib.h>
#include <string>
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <ctime>

#include "inc/zmqhelper.hpp"
#include "inc/tinythread.hpp"
#include "inc/common.hpp"
#include "inc/database.h"
#include "inc/spdlog/spdlog.h"

using namespace std;
using namespace zmqhelper;

class EvPusher: public TinyThread {
private:
    void *pSubCtx = nullptr, *pDealerCtx = nullptr; // for packets relay
    void *pSub = nullptr, *pDealer = nullptr, *pDaemonCtx = nullptr, *pDaemon = nullptr;
    string urlOut, urlPub, urlDealer, devSn, pullerGid, mgrSn, selfId;
    int iid;
    int *streamList = nullptr;
    AVFormatContext *pAVFormatRemux = nullptr;
    AVFormatContext *pAVFormatInput = nullptr;
    json config;
    thread thCloudMsgHandler, thEdgeMsgHandler;
    string drport = "5549";
    condition_variable cvMsg;
    mutex mutMsg;
    bool gotFormat = false;

    int init()
    {
        int ret = 0;
        bool found = false;
        try {
            spdlog::info("evpusher boot config: {} -> {}", selfId, config.dump());
            json evpusher;
            json &evmgr = this->config;
            json ipc;
            json ipcs = evmgr["ipcs"];
            for(auto &j: ipcs) {
                json pullers = j["modules"]["evpusher"];
                for(auto &p:pullers) {
                    if(p["sn"] == devSn && p["enabled"] != 0 && p["iid"] == iid ) {
                        evpusher = p;
                        break;
                    }
                }

                if(evpusher.size() != 0) {
                    ipc = j;
                    break;
                }
            }

            spdlog::info("evpusher {} {}, evpusher: {}",devSn, iid, evpusher.dump());
            spdlog::info("evpusher {} {}, ipc: {}",devSn, iid, ipc.dump());

            if(ipc.size()!=0 && evpusher.size()!=0) {
                found = true;
            }

            if(!found) {
                spdlog::error("evpusher {} : no valid config found: {}", selfId, config.dump());
                exit(1);
            }

            // TODO: currently just take the first puller, but should test connectivity
            json evpuller = ipc["modules"]["evpuller"][0];
            pullerGid = evpuller["sn"].get<string>() + ":evpuller:" + to_string(evpuller["iid"]);
            mgrSn = evmgr["sn"];

            int portPub = 5556;
            if(evpuller.count("portPub") != 0 && evpuller["portPub"].is_number_integer()) {
                portPub = evpuller["portPub"];
            }
            else if(evpuller.count("port-pub") != 0 && evpuller["port-pub"].is_number_integer()) {
                portPub = evpuller["port-pub"];
            }

            int portRouter = 5550;
            if(evmgr.count("portRouter") != 0 && evmgr["portRouter"].is_number_integer()) {
                portRouter = evmgr["portRouter"];
            }
            else if(evmgr.count("port-router") != 0 && evmgr["port-router"].is_number_integer()) {
                portRouter = evmgr["port-router"];
            }


            urlPub = string("tcp://") + evpuller["addr"].get<string>() + ":" + to_string(portPub);
            urlDealer = string("tcp://") + evmgr["addr"].get<string>() + ":" + to_string(portRouter);
            spdlog::info("evpusher {} connect to {} for sub, {} for router", selfId, urlPub, urlDealer);
            // TODO: multiple protocols support
            urlOut = evpusher["urlDest"].get<string>();
            // setup sub
            pSubCtx = zmq_ctx_new();
            pSub = zmq_socket(pSubCtx, ZMQ_SUB);
            //ZMQ_TCP_KEEPALIVE
            //ZMQ_TCP_KEEPALIVE_IDLE
            //ZMQ_TCP_KEEPALIVE_INTVL
            ret = 1;
            zmq_setsockopt(pSub, ZMQ_TCP_KEEPALIVE, &ret, sizeof (ret));
            ret = 5;
            zmq_setsockopt(pSub, ZMQ_TCP_KEEPALIVE_IDLE, &ret, sizeof (ret));
            zmq_setsockopt(pSub, ZMQ_TCP_KEEPALIVE_INTVL, &ret, sizeof (ret));
            ret = zmq_setsockopt(pSub, ZMQ_SUBSCRIBE, "", 0);
            if(ret != 0) {
                spdlog::error("evpusher {} {} failed set setsockopt: {}", devSn, iid, urlPub);

            }
            ret = zmq_connect(pSub, urlPub.c_str());
            if(ret != 0) {
                spdlog::error("evpusher {} {} failed connect pub: {}", devSn, iid, urlPub);
                exit(1);
            }

            // setup dealer
            pDealerCtx = zmq_ctx_new();
            pDealer = zmq_socket(pDealerCtx, ZMQ_DEALER);
            //ZMQ_TCP_KEEPALIVE
            //ZMQ_TCP_KEEPALIVE_IDLE
            //ZMQ_TCP_KEEPALIVE_INTVL
            ret = 1;
            zmq_setsockopt(pDealer, ZMQ_TCP_KEEPALIVE, &ret, sizeof (ret));
            ret = 5;
            zmq_setsockopt(pDealer, ZMQ_TCP_KEEPALIVE_IDLE, &ret, sizeof (ret));
            zmq_setsockopt(pDealer, ZMQ_TCP_KEEPALIVE_INTVL, &ret, sizeof (ret));
            ret = zmq_setsockopt(pDealer, ZMQ_IDENTITY, selfId.c_str(), selfId.size());
            ret += zmq_setsockopt (pDealer, ZMQ_ROUTING_ID, selfId.c_str(), selfId.size());
            if(ret < 0) {
                spdlog::error("evpusher {} failed setsockopts router {}: {}", selfId, urlDealer, zmq_strerror(zmq_errno()));
                exit(1);
            }
            ret = zmq_connect(pDealer, urlDealer.c_str());
            if(ret != 0) {
                spdlog::error("evpusher {} {} failed connect dealer: {}", devSn, iid, urlDealer);
                exit(1);
            }

            if(ret < 0) {
                spdlog::error("evpusher {} failed to set config: {}", selfId, config.dump());
            }
            spdlog::info("evpusher new config: {}", config.dump());
            ping();
        }
        catch(exception &e) {
            spdlog::error("evpusher {} {} exception in EvPuller.init {:s} retrying", devSn, iid, e.what());
            this_thread::sleep_for(chrono::seconds(3));
            exit(1);
        }

        return 0;
    }

    int ping()
    {
        // send hello to router
        int ret = 0;
        vector<vector<uint8_t> >body = {str2body(mgrSn+":evmgr:0"), str2body(EV_MSG_META_PING),str2body(MSG_HELLO)};
        ret = z_send_multiple(pDealer, body);
        if(ret < 0) {
            spdlog::error("evpusher {} {} failed to send multiple: {}", devSn, iid, zmq_strerror(zmq_errno()));
        }
        else {
            spdlog::info("evpusher {} sent hello to router: {}", selfId, mgrSn);
        }

        return ret;
    }

    int handleCloudMsg(vector<vector<uint8_t> > v)
    {
        int ret = 0;
        string peerId, metaType, metaValue, msg;
        json data;
        for(auto &b:v) {
            msg +=body2str(b) + ";";
        }

        bool bProcessed = false;
        if(v.size() == 3) {
            try {
                peerId = body2str(v[0]);
                json meta = json::parse(body2str(v[1]));
                metaType = meta["type"];
                if(meta.count("value") != 0) {
                    metaValue = meta["value"];
                }

                // msg from cluster mgr
                string daemonId = this->devSn + ":evdaemon:0";
                if(peerId == daemonId) {
                    if(metaValue == EV_MSG_META_VALUE_CMD_STOP || metaValue == EV_MSG_META_VALUE_CMD_RESTART) {
                        spdlog::info("evpusher {} received {} cmd from cluster mgr {}", selfId, metaValue, daemonId);
                        bProcessed = true;
                        exit(0);
                    }
                }
            }
            catch(exception &e) {
                spdlog::error("evpusher {} exception to process msg {}: {}", selfId, msg, e.what());
            }
        }

        if(!bProcessed) {
            spdlog::error("evpusher {} received msg having no implementation from peer: {}", selfId, msg);
        }

        return ret;
    }

    int handleEdgeMsg(vector<vector<uint8_t> > v)
    {
        int ret = 0;
        string peerId, metaType, metaValue, msg;
        json data;
        for(auto &b:v) {
            msg +=body2str(b) + ";";
        }

        msg = msg.substr(0, msg.size()> EV_MSG_DEBUG_LEN? EV_MSG_DEBUG_LEN:msg.size());

        bool bProcessed = false;
        if(v.size() == 3) {
            try {
                peerId = body2str(v[0]);
                json meta = json::parse(body2str(v[1]));
                metaType = meta["type"];
                if(meta.count("value") != 0) {
                    metaValue = meta["value"];
                }

                // msg from cluster mgr
                string clusterMgrId = this->mgrSn + ":evmgr:0";
                if(peerId == clusterMgrId) {
                    //
                }
                else if(peerId == pullerGid) {
                    if(metaType == EV_MSG_META_AVFORMATCTX) {
                        lock_guard<mutex> lock(this->mutMsg);
                        if(pAVFormatInput == nullptr) {
                            pAVFormatInput = (AVFormatContext *)malloc(sizeof(AVFormatContext));
                            AVFormatCtxSerializer::decode((char *)(v[2].data()), v[2].size(), pAVFormatInput);
                            gotFormat = true;
                            cvMsg.notify_one();
                        }
                        else {
                            spdlog::warn("evpusher {} received avformatctx msg from {}, but already proceessed before, ignored. TODO: reinit", selfId, peerId);
                        }
                        bProcessed = true;
                    }
                }
            }
            catch(exception &e) {
                spdlog::error("evpusher {} exception to process msg {}: {}", selfId, msg, e.what());
            }
        }

        if(!bProcessed) {
            spdlog::error("evpusher {} received msg having no implementation from peer: {}", selfId, msg);
        }

        return ret;
    }

    int getInputFormat()
    {
        int ret = 0;
        // req avformatcontext packet
        // send hello to puller
        spdlog::info("evpusher {} send hello to puller: {}", selfId, pullerGid);
        vector<vector<uint8_t> > body;
        body.push_back(str2body(pullerGid));
        json meta;
        meta["type"] = EV_MSG_META_AVFORMATCTX;
        body.push_back(str2body(meta.dump()));
        body.push_back(str2body(MSG_HELLO));
        this->gotFormat = false;

        ret = z_send_multiple(pDealer, body);
        if(ret < 0) {
            spdlog::error("evpusher {} {}, failed to send hello to puller: {}. exiting...", devSn, iid, zmq_strerror(zmq_errno()));
            exit(1);
        }
        unique_lock<mutex> lk(this->mutMsg);
        this->cvMsg.wait(lk, [this] {return this->gotFormat;});

        return ret;
    }

    int setupStream()
    {
        int ret = 0;
        AVDictionary *pOptsRemux = nullptr;
        string proto = urlOut.substr(0, 4);
        if(proto == "rtsp") {
            // rtsp tcp
            if(av_dict_set(&pOptsRemux, "rtsp_transport", "tcp", 0) < 0) {
                spdlog::error("evpusher {} {} failed set output pOptsRemux", devSn, iid);
                ret = AVERROR_UNKNOWN;
            }
            av_dict_set_int(&pOptsRemux, "stimeout", (int64_t)(1000* 1000 * 1), 0);
            ret = avformat_alloc_output_context2(&pAVFormatRemux, nullptr, "rtsp", urlOut.c_str());
        }
        else if(proto == "rtmp") {
            ret = avformat_alloc_output_context2(&pAVFormatRemux, nullptr, "rtmp", urlOut.c_str());
        }
        else {
            ret = avformat_alloc_output_context2(&pAVFormatRemux, nullptr, nullptr, urlOut.c_str());
        }

        if (ret < 0) {
            spdlog::error("evpusher {} {} failed create avformatcontext for output: %s", devSn, iid, av_err2str(ret));
            exit(1);
        }

        streamList = (int *)av_mallocz_array(pAVFormatInput->nb_streams, sizeof(*streamList));
        spdlog::info("evpusher {} {} numStreams: {:d}", devSn, iid, pAVFormatInput->nb_streams);
        if (!streamList) {
            ret = AVERROR(ENOMEM);
            spdlog::error("evpusher {} {} failed create avformatcontext for output: %s", devSn, iid, av_err2str(AVERROR(ENOMEM)));
            exit(1);
        }

        int streamIdx = 0;
        // find all video & audio streams for remuxing
        for (int i = 0; i < pAVFormatInput->nb_streams; i++) {
            AVStream *out_stream;
            AVStream *in_stream = pAVFormatInput->streams[i];
            AVCodecParameters *in_codecpar = in_stream->codecpar;
            if (in_codecpar->codec_type != AVMEDIA_TYPE_AUDIO &&
                    in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
                streamList[i] = -1;
                continue;
            }
            streamList[i] = streamIdx++;
            out_stream = avformat_new_stream(pAVFormatRemux, NULL);
            if (!out_stream) {
                spdlog::error("evpusher {} {} failed allocating output stream", devSn, iid);
                ret = AVERROR_UNKNOWN;

            }
            ret = avcodec_parameters_copy(out_stream->codecpar, in_codecpar);
            spdlog::info("evpusher {} {}  copied codepar", devSn, iid);
            if (ret < 0) {
                spdlog::error("evpusher {} {}  failed to copy codec parameters", devSn, iid);
            }
        }

        for(int i = 0; i < pAVFormatInput->nb_streams; i++ ) {
            spdlog::info("evpusher {} streamList[{:d}]: {:d}", selfId, i, streamList[i]);
        }

        av_dump_format(pAVFormatRemux, 0, urlOut.c_str(), 1);

        if (!(pAVFormatRemux->oformat->flags & AVFMT_NOFILE)) {
            spdlog::error("evpusher {} {} failed allocating output stream", devSn,iid);
            ret = avio_open2(&pAVFormatRemux->pb, urlOut.c_str(), AVIO_FLAG_WRITE, NULL, &pOptsRemux);
            if (ret < 0) {
                spdlog::error("evpusher {} {} could not open output file '%s'", devSn, iid, urlOut);
                exit(1);
            }
        }

        ret = avformat_write_header(pAVFormatRemux, &pOptsRemux);
        if (ret < 0) {
            spdlog::error("evpusher {} {} error occurred when opening output file", devSn, iid);
            exit(1);
        }

        return ret;
    }

    void freeStream()
    {
        // close output context
        if(pAVFormatRemux) {
            if(pAVFormatRemux->pb) {
                avio_closep(&pAVFormatRemux->pb);
            }

            avformat_free_context(pAVFormatRemux);
        }
        pAVFormatRemux = nullptr;
        // free avformatcontex
        if(pAVFormatInput != nullptr) {
            AVFormatCtxSerializer::freeCtx(pAVFormatInput);
            pAVFormatInput = nullptr;
        }

        pAVFormatInput = nullptr;
    }
protected:
    void run()
    {
        int ret = 0;
        zmq_msg_t msg;
        AVPacket packet;
        uint64_t pktCnt = 0;
        while (true) {
            ret =zmq_msg_init(&msg);
            if(ret != 0) {
                spdlog::error("evpusher {} failed to init zmq msg", selfId);
                continue;
            }
            // receive packet
            ret = zmq_recvmsg(pSub, &msg, 0);
            if(ret < 0) {
                spdlog::error("evpusher {} failed to recv zmq msg: {}", selfId, zmq_strerror(ret));
                continue;
            }

            if(pktCnt % EV_LOG_PACKET_CNT == 0) {
                spdlog::info("evpusher {} seq: {}, pts: {}, dts: {}, idx: {}", selfId, pktCnt, packet.pts, packet.dts, packet.stream_index);
            }

            pktCnt++;
            // decode
            ret = AVPacketSerializer::decode((char*)zmq_msg_data(&msg), ret, &packet);
            {
                if (ret < 0) {
                    spdlog::error("evpusher {} packet decode failed: {:d}", selfId, ret);
                    continue;
                }
            }
            zmq_msg_close(&msg);

            spdlog::debug("packet stream indx: {:d}", packet.stream_index);
            // relay
            AVStream *in_stream =NULL, *out_stream = nullptr;
            in_stream  = pAVFormatInput->streams[packet.stream_index];
            packet.stream_index = streamList[packet.stream_index];
            out_stream = pAVFormatRemux->streams[packet.stream_index];

            /* copy packet */
            // spdlog::info("evpusher {} packet pts: {} dts: {}", selfId, packet.pts, packet.dts);
            // if(pktCnt == 0) {
            //     packet.pts = AV_NOPTS_VALUE;
            //     packet.dts = AV_NOPTS_VALUE;
            //     packet.duration = 0;
            //     packet.pos = -1;
            // }else{
            //     packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            //     packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
            //     packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
            //     packet.pos = -1;
            //     lastPts = packet.dts;
            // }
            // spdlog::info("evpusher {} packet new pts: {} dts: {}", selfId, packet.pts, packet.dts);

            ret = av_interleaved_write_frame(pAVFormatRemux, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                spdlog::error("evpusher {} error muxing packet: {}, {}, {}, {}, restreaming...", selfId, av_err2str(ret), packet.dts, packet.pts, packet.dts==AV_NOPTS_VALUE);
                if(packet.pts == AV_NOPTS_VALUE) {
                    // reset
                    // av_write_trailer(pAVFormatRemux);
                    freeStream();
                    getInputFormat();
                    setupStream();
                    pktCnt = 0;
                    continue;
                }
            }
        }
        av_write_trailer(pAVFormatRemux);
    }

public:
    EvPusher()
    {
        const char *strEnv = getenv("DR_PORT");
        if(strEnv != nullptr) {
            drport = strEnv;
        }

        strEnv = getenv("PEERID");
        if(strEnv != nullptr) {
            selfId = strEnv;
            auto v = strutils::split(selfId, ':');
            if(v.size() != 3||v[1] != "evpusher") {
                spdlog::error("evpusher received invalid gid: {}", selfId);
                exit(1);
            }
            devSn = v[0];
            iid = stoi(v[2]);
        }
        else {
            spdlog::error("evpusher failed to start. no SN set");
            exit(1);
        }

        spdlog::info("evpusher {} boot", selfId);

        //
        string addr = string("tcp://127.0.0.1:") + drport;
        int ret = zmqhelper::setupDealer(&pDaemonCtx, &pDaemon, addr, selfId);
        if(ret != 0) {
            spdlog::error("evpusher {} failed to setup dealer {}", devSn, addr);
            exit(1);
        }

        ret = zmqhelper::recvConfigMsg(pDaemon, config, addr, selfId);
        if(ret != 0) {
            spdlog::error("evpusher {} failed to receive configration message {}", devSn, addr);
        }

        init();

        thCloudMsgHandler = thread([this] {
            while(true)
            {
                auto body = z_recv_multiple(pDaemon,false);
                if(body.size() == 0) {
                    spdlog::error("evslicer {} failed to receive multiple cloud msg: {}", selfId, zmq_strerror(zmq_errno()));
                }else{
                    // full proto msg received.
                    this->handleCloudMsg(body);
                }    
            }
        });
        thCloudMsgHandler.detach();

        thEdgeMsgHandler = thread([this] {
            while(true)
            {
                auto body = z_recv_multiple(pDealer,false);
                if(body.size() == 0) {
                    spdlog::error("evslicer {} failed to receive multiple edge msg: {}", selfId, zmq_strerror(zmq_errno()));
                }else{
                    // full proto msg received.
                    this->handleEdgeMsg(body);
                }     
            }
        });
        thEdgeMsgHandler.detach();

        getInputFormat();
        setupStream();
    }

    ~EvPusher()
    {
        if(pSub != nullptr) {
            int i = 0;
            zmq_setsockopt(pSub, ZMQ_LINGER, &i, sizeof(i));
            zmq_close(pSub);
            pSub = nullptr;
        }
        if(pSubCtx != nullptr) {
            zmq_ctx_destroy(pSubCtx);
            pSubCtx = nullptr;
        }
        if(pDealer != nullptr) {
            int i = 0;
            zmq_setsockopt(pDealer, ZMQ_LINGER, &i, sizeof(i));
            zmq_close(pDealer);
            pDealer = nullptr;
        }
        if(pDealerCtx != nullptr) {
            zmq_ctx_destroy(pDealerCtx);
            pDealerCtx = nullptr;
        }

        if(pDaemon != nullptr) {
            int i = 0;
            zmq_setsockopt(pDaemon, ZMQ_LINGER, &i, sizeof(i));
            zmq_close(pDaemon);
            pDaemon = nullptr;
        }

        if(pDaemonCtx != nullptr) {
            zmq_ctx_destroy(pDaemonCtx);
            pDaemonCtx = nullptr;
        }
        

        freeStream();
    }
};

int main(int argc, char *argv[])
{
    av_log_set_level(AV_LOG_INFO);
    spdlog::set_level(spdlog::level::info);
    EvPusher pusher;
    pusher.join();
}