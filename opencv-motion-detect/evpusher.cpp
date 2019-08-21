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

#include "zmqhelper.hpp"
#include "tinythread.hpp"
#include "common.hpp"
#include "database.h"
#include "spdlog/spdlog.h"
#define MAX_ZMQ_MSG_SIZE 1204 * 1024 * 2

using namespace std;

class EvPusher: public TinyThread {
private:
    void *pSubCtx = NULL, *pDealerCtx = NULL; // for packets relay
    void *pSub = NULL, *pDealer = NULL;
    string urlOut, urlPub, urlDealer, devSn, pullerGid, mgrSn;
    int iid;
    bool enablePush = false;
    int *streamList = NULL;
    AVFormatContext *pAVFormatRemux = NULL;
    AVFormatContext *pAVFormatInput = NULL;
    json config;

    int init()
    {
        bool inited = false;
        // TODO: read db to get devSn
        devSn = "ILSEVPUSHER1";
        iid = 1;
        while(!inited) {
            // TODO: req config
            bool found = false;
            try{
                config = json::parse(cloudutils::config);
                spdlog::info("config: {:s}", config.dump());
                json evpusher;
                json evmgr;
                json ipc;
                
                json data = config["data"];
                for (auto& [key, value] : data.items()) {
                    //std::cout << key << " : " << dynamic_cast<json&>(value).dump() << "\n";
                    evmgr = value;
                    json ipcs = evmgr["ipcs"];
                    for(auto &j: ipcs) {
                        json pullers = j["modules"]["evpusher"];
                        for(auto &p:pullers) {
                            if(p["devSn"] == devSn && p["iid"] == iid) {
                                evpusher = p;
                                break;
                            }
                        }
                        if(evpusher.size() != 0) {
                            ipc = j;
                            break;
                        }
                    }

                    if(ipc.size()!=0 && evpusher.size()!=0) {
                        found = true;
                        break;
                    }
                }

                if(!found) {
                    spdlog::error("evpusher {} {} no valid config found. retrying load config...", devSn, iid);
                    this_thread::sleep_for(chrono::seconds(3));
                    continue;
                }

                // TODO: currently just take the first puller, but should test connectivity
                json evpuller = ipc["modules"]["evpuller"][0];
                pullerGid = evpuller["sn"].get<string>() + ":evpuller:" + to_string(evpuller["iid"]);
                mgrSn = evmgr["sn"];

                urlPub = string("tcp://") + evpuller["addr"].get<string>() + ":" + to_string(evpuller["port-pub"]);
                urlDealer = string("tcp://") + evmgr["addr"].get<string>() + ":" + to_string(evmgr["port-router"]);
                spdlog::info("evpusher {} {} will connect to {} for sub, {} for router", devSn, iid, urlPub, urlDealer);
                // TODO: multiple protocols support
                urlOut = evpusher["urlDest"].get<string>();
            }
            catch(exception &e) {
                spdlog::error("evpusher {} {} exception in EvPuller.init {:s} retrying", devSn, iid, e.what());
                    this_thread::sleep_for(chrono::seconds(3));
                    continue;
            }
            
            inited = true;
        }

        return 0;
    }
    int setupMq()
    {
        teardownMq();
        int ret = 0;

        // setup sub
        pSubCtx = zmq_ctx_new();
        pSub = zmq_socket(pSubCtx, ZMQ_SUB);
        ret = zmq_setsockopt(pSub, ZMQ_SUBSCRIBE, "", 0);
        if(ret != 0) {
            spdlog::error("evpusher failed connect to pub: {}, {}", devSn, iid);
            return -1;
        }
        ret = zmq_connect(pSub, urlPub.c_str());
        if(ret != 0) {
            spdlog::error("evpusher {} {} failed create sub", devSn, iid);
            return -2;
        }

        // setup req
        pDealerCtx = zmq_ctx_new();
        pDealer = zmq_socket(pDealerCtx, ZMQ_REQ);
        spdlog::info("evpusher {} {} try create req to {}", devSn, iid, urlDealer);
        ret = zmq_connect(pDealer, urlDealer.c_str());
        
        if(ret != 0) {
            spdlog::error("evpusher {} {} failed create req to {}", devSn, iid, urlDealer);
            return -3;
        }

        spdlog::info("evpusher {} {} success setupMq", devSn, iid);

        return 0;
    }

    int teardownMq()
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

        return 0;
    }

    int setupStream()
    {
        int ret = 0;
        AVDictionary *pOptsRemux = NULL;

        // req avformatcontext packet
        // send first packet to init connection
        zmq_msg_t msg;
        zmq_send(pDealer, "hello", 5, 0);
        spdlog::info("evpusher {} {} success send hello", devSn, iid);
        ret =zmq_msg_init(&msg);
        if(ret != 0) {
            spdlog::error("failed to init zmq msg");
            exit(1);
        }
        // receive packet
        ret = zmq_recvmsg(pDealer, &msg, 0);
        spdlog::info("evpusher {} {} recv", devSn, iid);
        if(ret < 0) {
            spdlog::error("evpusher {} {} failed to recv zmq msg: {}", devSn, iid, zmq_strerror(ret));
            exit(1);
        }

        pAVFormatInput = (AVFormatContext *)malloc(sizeof(AVFormatContext));
        AVFormatCtxSerializer::decode((char *)zmq_msg_data(&msg), ret, pAVFormatInput);

        // close req
        {
            zmq_msg_close(&msg);
            if(pDealer != NULL) {
                zmq_close(pDealer);
                pDealer = NULL;
            }
            if(pDealerCtx != NULL) {
                zmq_ctx_destroy(pDealerCtx);
                pDealerCtx = NULL;
            }
        }
        
        ret = avformat_alloc_output_context2(&pAVFormatRemux, NULL, "rtsp", urlOut.c_str());
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
            spdlog::info("streamList[{:d}]: {:d}", i, streamList[i]);
        }

        av_dump_format(pAVFormatRemux, 0, urlOut.c_str(), 1);

        if (!(pAVFormatRemux->oformat->flags & AVFMT_NOFILE)) {
            spdlog::error("evpusher {} {} failed allocating output stream", devSn ,iid);
            ret = avio_open2(&pAVFormatRemux->pb, urlOut.c_str(), AVIO_FLAG_WRITE, NULL, &pOptsRemux);
            if (ret < 0) {
                spdlog::error("evpusher {} {} could not open output file '%s'", devSn, iid, urlOut);
                exit(1);
            }
        }

        // rtsp tcp
        if(av_dict_set(&pOptsRemux, "rtsp_transport", "tcp", 0) < 0) {
            spdlog::error("evpusher {} {} failed set output pOptsRemux", devSn, iid);
            ret = AVERROR_UNKNOWN;
        }

        ret = avformat_write_header(pAVFormatRemux, &pOptsRemux);
        if (ret < 0) {
            spdlog::error("evpusher {} {} error occurred when opening output file", devSn, iid);
        }

        return ret;
    }
protected:
    void run()
    {
        int ret = 0;
        bool bStopSig = false;
        zmq_msg_t msg;
        AVPacket packet;
        uint64_t pktCnt = 0;
        while (true) {
            if(checkStop() == true) {
                bStopSig = true;
                break;
            }
            int ret =zmq_msg_init(&msg);
            if(ret != 0) {
                spdlog::error("failed to init zmq msg");
                continue;
            }
            // receive packet
            ret = zmq_recvmsg(pSub, &msg, 0);
            if(ret < 0) {
                spdlog::error("failed to recv zmq msg: {}", zmq_strerror(ret));
                continue;
            }

            // decode
            pktCnt++;
            ret = AVPacketSerializer::decode((char*)zmq_msg_data(&msg), ret, &packet);
            {
                if (ret < 0) {
                    spdlog::error("packet decode failed: {:d}", ret);
                    continue;
                }
            }
            zmq_msg_close(&msg);

            spdlog::debug("packet stream indx: {:d}", packet.stream_index);
            // relay
            AVStream *in_stream =NULL, *out_stream = NULL;
            in_stream  = pAVFormatInput->streams[packet.stream_index];
            packet.stream_index = streamList[packet.stream_index];
            out_stream = pAVFormatRemux->streams[packet.stream_index];

            //calc pts
            {
                spdlog::debug("seq: {:lld}, pts: {:lld}, dts: {:lld}, dur: {:lld}, idx: {:d}", pktCnt, packet.pts, packet.dts, packet.duration, packet.stream_index);
                /* copy packet */
                packet.pts = av_rescale_q_rnd(packet.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                packet.dts = av_rescale_q_rnd(packet.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
                packet.duration = av_rescale_q(packet.duration, in_stream->time_base, out_stream->time_base);
                packet.pos = -1;
            }

            ret = av_interleaved_write_frame(pAVFormatRemux, &packet);
            av_packet_unref(&packet);
            if (ret < 0) {
                spdlog::error("error muxing packet");
            }
        }
        av_write_trailer(pAVFormatRemux);
        if(!bStopSig && ret < 0) {
            //TOOD: reconnect
            spdlog::error("TODO: failed, reconnecting");
        }
        else {
            spdlog::error("exit on command");
        }
    }

public:
    EvPusher()
    {
        init();
        if(setupMq() < 0) {
            // TODO: reconnect
            exit(1);
        }
        setupStream();
    }

    ~EvPusher()
    {
        teardownMq();
        // free avformatcontex
        if(pAVFormatInput != NULL) {
            AVFormatCtxSerializer::freeCtx(pAVFormatInput);
            pAVFormatInput = NULL;
        }
    }
};

int main(int argc, char *argv[])
{
    av_log_set_level(AV_LOG_INFO);
    spdlog::set_level(spdlog::level::debug);
    EvPusher pusher;
    pusher.join();
}