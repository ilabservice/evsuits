/*
module: evslicer
description:
author: Bruce.Lu <lzbgt@icloud.com>
update: 2019/08/20
*/

#pragma GCC diagnostic ignored "-Wunused-private-field"
#pragma GCC diagnostic ignored "-Wunused-variable"

#include <stdlib.h>
#include <string>
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <vector>

#ifdef OS_LINUX
#include <filesystem>
namespace fs = std::filesystem;
#endif

#include <cstdlib>
#include "zmqhelper.hpp"
#include "tinythread.hpp"
#include "common.hpp"
#include "database.h"

using namespace std;
using namespace zmqhelper;

class EvSlicer: public TinyThread {
private:
#define URLOUT_DEFAULT "slices"
#define NUM_DAYS_DEFAULT 5
#define MINUTES_PER_SLICE_DEFAULT 2
// 2 days, 10 minutes per record
#define NUM_SLICES_DEFAULT (24 * NUM_DAYS_DEFAULT * 60 / MINUTES_PER_SLICE_DEFAULT)
    void *pSubCtx = NULL, *pDealerCtx = NULL; // for packets relay
    void *pSub = NULL, *pDealer = NULL;
    string urlOut, urlPub, urlRouter, devSn, mgrSn, selfId, pullerGid;
    int iid, days, minutes, numSlices, lastSliceId;
    bool enablePush = false;
    AVFormatContext *pAVFormatRemux = NULL;
    AVFormatContext *pAVFormatInput = NULL;
    AVDictionary *pOptsRemux = NULL;
    // load from db
    vector<int> *sliceIdxToName = NULL;
    int *streamList = NULL;
    json config;
    thread thPing;

    int init()
    {
        int ret = 0;
        bool inited = false;
        // TODO: read db to get devSn
        devSn = "ILSEVSLICER1";
        iid = 1;
        selfId = devSn + ":evslicer:" + to_string(iid);
        while(!inited) {
            // TODO: req config
            bool found = false;
            try {
                config = json::parse(cloudutils::config);
                spdlog::info("config: {:s}", config.dump());
                json evslicer;
                json evmgr;
                json ipc;

                json data = config["data"];
                for (auto& [key, value] : data.items()) {
                    //std::cout << key << " : " << dynamic_cast<json&>(value).dump() << "\n";
                    evmgr = value;
                    json ipcs = evmgr["ipcs"];
                    for(auto &j: ipcs) {
                        json pullers = j["modules"]["evslicer"];
                        for(auto &p:pullers) {
                            if(p["sn"] == devSn && p["iid"] == iid) {
                                evslicer = p;
                                break;
                            }
                        }
                        if(evslicer.size() != 0) {
                            ipc = j;
                            break;
                        }
                    }

                    if(ipc.size()!=0 && evslicer.size()!=0) {
                        found = true;
                        break;
                    }
                }

                if(!found) {
                    spdlog::error("evslicer {} {}: no valid config found. retrying load config...", devSn, iid);
                    this_thread::sleep_for(chrono::seconds(3));
                    continue;
                }

                json evpuller = ipc["modules"]["evpuller"][0];
                pullerGid = evpuller["sn"].get<string>() + ":evpuller:" + to_string(evpuller["iid"]);
                mgrSn = evmgr["sn"];
                if(evslicer.count("path") == 0) {
                    spdlog::warn("evslicer {} {} no params for path, using default: {}", devSn, iid, URLOUT_DEFAULT);
                    urlOut = URLOUT_DEFAULT;
                }
                else {
                    urlOut = evslicer["path"];
                }
                if(evslicer.count("days") == 0) {
                    spdlog::warn("evslicer {} {} no params for days, using default: {}", devSn, iid, NUM_DAYS_DEFAULT);
                    days = NUM_DAYS_DEFAULT;
                }
                else {
                    days = evslicer["days"].get<int>();
                }

                if(evslicer.count("minutes") == 0) {
                    spdlog::warn("evslicer {} {} no params for minutes, using default: {}", devSn, iid, MINUTES_PER_SLICE_DEFAULT);
                    minutes = MINUTES_PER_SLICE_DEFAULT;
                }
                else {
                    minutes = evslicer["minutes"].get<int>();
                }

                numSlices = 24 * days * 60 /minutes;
                // alloc memory
                sliceIdxToName = new vector<int>(numSlices);
                // TODO: load db
                // DB::exec(NULL, "select id, ts, last from slices;", DB::get_slices, sliceIdxToName);
                spdlog::info("mkdir -p {}", urlOut);
                ret = system((string("mkdir -p ") + urlOut).c_str());
                if(ret == -1) {
                    spdlog::error("failed to create {} dir", urlOut);
                    return -1;
                }

                urlPub = string("tcp://") + evpuller["addr"].get<string>() + ":" + to_string(evpuller["port-pub"]);
                urlRouter = string("tcp://") + evmgr["addr"].get<string>() + ":" + to_string(evmgr["port-router"]);
                spdlog::info("evslicer {} {} will connect to {} for sub, {} for router", devSn, iid, urlPub, urlRouter);
            }
            catch(exception &e) {
                spdlog::error("evslicer {} {} exception in init {:s} retrying", devSn, iid, e.what());
                this_thread::sleep_for(chrono::seconds(3));
                continue;
            }

            inited = true;
        }

        return 0;
    }

    int ping()
    {
        // send hello to router
        int ret = 0;
        vector<vector<uint8_t> >body;
        // since identity is auto set
        body.push_back(str2body(mgrSn+":0:0"));
        body.push_back(str2body(EV_MSG_META_PING)); // blank meta
        body.push_back(str2body(MSG_HELLO));

        ret = z_send_multiple(pDealer, body);
        if(ret < 0) {
            spdlog::error("evslicer {} {} failed to send multiple: {}", devSn, iid, zmq_strerror(zmq_errno()));
            //TODO:
        }
        else {
            spdlog::info("evslicer {} {} sent hello to router: {}", devSn, iid, mgrSn);
        }

        return ret;
    }

    int setupMq()
    {
        int ret = 0;

        // setup sub
        pSubCtx = zmq_ctx_new();
        pSub = zmq_socket(pSubCtx, ZMQ_SUB);
        ret = zmq_setsockopt(pSub, ZMQ_SUBSCRIBE, "", 0);
        if(ret != 0) {
            spdlog::error("evslicer {} {} failed set setsockopt: {}", devSn, iid, urlPub);
            return -1;
        }
        ret = zmq_connect(pSub, urlPub.c_str());
        if(ret != 0) {
            spdlog::error("evslicer {} {} failed connect pub: {}", devSn, iid, urlPub);
            return -2;
        }

        // setup dealer
        pDealerCtx = zmq_ctx_new();
        pDealer = zmq_socket(pDealerCtx, ZMQ_DEALER);
        spdlog::info("evslicer {} {} try create req to {}", devSn, iid, urlRouter);
        ret = zmq_setsockopt(pDealer, ZMQ_IDENTITY, selfId.c_str(), selfId.size());
        ret += zmq_setsockopt (pDealer, ZMQ_ROUTING_ID, selfId.c_str(), selfId.size());
        if(ret < 0) {
            spdlog::error("evpusher {} {} failed setsockopts router: {}", devSn, iid, urlRouter);
            return -3;
        }
        if(ret < 0) {
            spdlog::error("evslicer {} {} failed setsockopts router: {}", devSn, iid, urlRouter);
            return -3;
        }
        ret = zmq_connect(pDealer, urlRouter.c_str());
        if(ret != 0) {
            spdlog::error("evslicer {} {} failed connect dealer: {}", devSn, iid, urlRouter);
            return -4;
        }
        //ping
        ret = ping();
        // TODO: don't need this anymore, since I've used the draft feature of ZOUTER_NOTIFICATION instead
        // thPing = thread([&,this]() {
        //     while(true) {
        //         this_thread::sleep_for(chrono::seconds(EV_HEARTBEAT_SECONDS-2));
        //         ping();
        //     }
        // });

        // thPing.detach();

        return ret;
    }

    int getInputFormat()
    {
        int ret = 0;
        // req avformatcontext packet
        // send hello to puller
        spdlog::info("evslicer {} {} send hello to puller: {}", devSn, iid, pullerGid);
        vector<vector<uint8_t> > body;
        body.push_back(str2body(pullerGid));
        json meta;
        meta["type"] = EV_MSG_META_AVFORMATCTX;
        body.push_back(str2body(meta.dump()));
        body.push_back(str2body(MSG_HELLO));
        bool gotFormat = false;
        uint64_t failedCnt = 0;
        while(!gotFormat) {
            ret = z_send_multiple(pDealer, body);
            if(ret < 0) {
                spdlog::error("evslicer {} {}, failed to send hello to puller: {}", devSn, iid, zmq_strerror(zmq_errno()));
                continue;
            }

            // expect response with avformatctx
            auto v = z_recv_multiple(pDealer);
            if(v.size() != 3) {
                ret = zmq_errno();
                if(ret != 0) {
                    if(failedCnt % 100 == 0) {
                        spdlog::error("evslicer {} {}, error receive avformatctx: {}, {}", devSn, iid, v.size(), zmq_strerror(ret));
                        spdlog::info("evslicer {} {} retry connect to peers", devSn, iid);
                    }
                    this_thread::sleep_for(chrono::seconds(5));
                    failedCnt++;
                }
                else {
                    spdlog::error("evslicer {} {}, received bad size zmq msg for avformatctx: {}", devSn, iid, v.size());
                }
            }
            else if(body2str(v[0]) != pullerGid) {
                spdlog::error("evslicer {} {}, invalid sender for avformatctx: {}, should be: {}", devSn, iid, body2str(v[0]), pullerGid);
            }
            else {
                try {
                    auto cmd = json::parse(body2str(v[1]));
                    if(cmd["type"].get<string>() == EV_MSG_META_AVFORMATCTX) {
                        pAVFormatInput = (AVFormatContext *)malloc(sizeof(AVFormatContext));
                        AVFormatCtxSerializer::decode((char *)(v[2].data()), v[2].size(), pAVFormatInput);
                        gotFormat = true;
                    }
                }
                catch(exception &e) {
                    spdlog::error("evslicer {} {}, exception in parsing avformatctx packet: {}", devSn, iid, e.what());
                }
            }
        }
        return ret;
    }

    int setupStream()
    {
        int ret = 0;
        int streamIdx = 0;
        // find all video & audio streams for remuxing
        streamList = (int *)av_mallocz_array(pAVFormatInput->nb_streams, sizeof(*streamList));
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
        }

        for(int i = 0; i < pAVFormatInput->nb_streams; i++ ) {
            spdlog::info("streamList[{:d}]: {:d}", i, streamList[i]);
        }

        //av_dict_set(&pOptsRemux, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
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
        pAVFormatRemux = NULL;
        // free avformatcontex
        if(pAVFormatInput != NULL) {
            AVFormatCtxSerializer::freeCtx(pAVFormatInput);
            pAVFormatInput = NULL;
        }

        pAVFormatInput = NULL;
    }

protected:
    void run()
    {
        bool bStopSig = false;
        int ret = 0;
        int idx = 0;
        int pktCnt = 0;
        AVStream * out_stream = NULL;
        zmq_msg_t msg;
        AVPacket packet;
        while (true) {
            auto start = chrono::system_clock::now();
            auto end = start;
            string name = to_string(chrono::duration_cast<chrono::seconds>(start.time_since_epoch()).count()) + ".mp4";
            name = urlOut + "/" + name;
            ret = avformat_alloc_output_context2(&pAVFormatRemux, NULL, "mp4", name.c_str());
            if (ret < 0) {
                spdlog::error("evslicer {} {} failed create avformatcontext for output: %s", devSn, iid, av_err2str(ret));
                exit(1);
            }

            // build output avformatctx
            for(int i =0; i < pAVFormatInput->nb_streams; i++) {
                if(streamList[i] != -1) {
                    out_stream = avformat_new_stream(pAVFormatRemux, NULL);
                    if (!out_stream) {
                        spdlog::error("evslicer {} {} failed allocating output stream 1", devSn, iid);
                        ret = AVERROR_UNKNOWN;
                    }
                    ret = avcodec_parameters_copy(out_stream->codecpar, pAVFormatInput->streams[i]->codecpar);
                    if (ret < 0) {
                        spdlog::error("evslicer {} {} failed to copy codec parameters", devSn, iid);
                    }
                }
            }

            //av_dump_format(pAVFormatRemux, 0, name.c_str(), 1);
            if (!(pAVFormatRemux->oformat->flags & AVFMT_NOFILE)) {
                ret = avio_open2(&pAVFormatRemux->pb, name.c_str(), AVIO_FLAG_WRITE, NULL, &pOptsRemux);
                if (ret < 0) {
                    spdlog::error("evslicer {} {} could not open output file {}", devSn, iid, name);
                }
            }

            ret = avformat_write_header(pAVFormatRemux, &pOptsRemux);
            if (ret < 0) {
                spdlog::error("evslicer {} {} error occurred when opening output file", devSn, iid);
            }

            // TODO:

            spdlog::info("writing new slice {}", name.c_str());
            while(chrono::duration_cast<chrono::seconds>(end-start).count() < minutes * 60) {
                if(checkStop() == true) {
                    bStopSig = true;
                    break;
                }
                // business logic
                int ret =zmq_msg_init(&msg);
                ret = zmq_recvmsg(pSub, &msg, 0);
                if(ret < 0) {
                    spdlog::error("failed to recv zmq msg: {}", zmq_strerror(ret));
                    continue;
                }
                ret = AVPacketSerializer::decode((char*)zmq_msg_data(&msg), ret, &packet);
                {
                    if (ret < 0) {
                        spdlog::error("packet decode failed: {:d}", ret);
                        continue;
                    }
                }

                zmq_msg_close(&msg);

                AVStream *in_stream =NULL, *out_stream = NULL;
                in_stream  = pAVFormatInput->streams[packet.stream_index];
                packet.stream_index = streamList[packet.stream_index];
                out_stream = pAVFormatRemux->streams[packet.stream_index];
                //calc pts

                if(pktCnt % EV_LOG_PACKET_CNT == 0) {
                    spdlog::info("seq: {}, pts: {}, dts: {}, idx: {}", pktCnt, packet.pts, packet.dts, packet.stream_index);
                }
                /* copy packet */
                if(pktCnt == 0) {
                    packet.pts = 0;
                    packet.dts = 0;
                    packet.duration = 0;
                    packet.pos = -1;
                }
                else {
                    packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                    packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
                    packet.pos = -1;
                }
                pktCnt++;


                ret = av_interleaved_write_frame(pAVFormatRemux, &packet);
                av_packet_unref(&packet);
                if (ret < 0) {
                    spdlog::error("error muxing packet: {}, {}, {}, {}, restreaming...", av_err2str(ret), packet.dts, packet.pts, packet.dts==AV_NOPTS_VALUE);
                    if(pktCnt != 0 && packet.pts == AV_NOPTS_VALUE) {
                        // reset
                        av_write_trailer(pAVFormatRemux);
                        this_thread::sleep_for(chrono::seconds(5));
                        freeStream();
                        getInputFormat();
                        setupStream();
                        pktCnt = 0;
                        break;
                    }
                }

                end = chrono::system_clock::now();
            }// while in slice
            // write tail
            // close output context
            if (pAVFormatRemux != NULL) {
                if(pAVFormatRemux->pb != NULL) {
                    avio_closep(&pAVFormatRemux->pb);
                }
                avformat_free_context(pAVFormatRemux);
            }
        }// outer while
    }
public:
    EvSlicer()
    {
        init();
        setupMq();
        getInputFormat();
        setupStream();
    };
    ~EvSlicer()
    {
        if(pSub != NULL) {
            zmq_close(pSub);
            pSub = NULL;
        }
        if(pSubCtx != NULL) {
            zmq_ctx_destroy(pSubCtx);
            pSubCtx = NULL;
        }
        if(pDealer != NULL) {
            zmq_close(pSub);
            pDealer = NULL;
        }
        if(pDealerCtx != NULL) {
            zmq_ctx_destroy(pSub);
            pDealerCtx = NULL;
        }
        freeStream();
    };
};

int main(int argc, const char *argv[])
{
    av_log_set_level(AV_LOG_ERROR);
    spdlog::set_level(spdlog::level::info);
    EvSlicer es;
    es.join();
    return 0;
}