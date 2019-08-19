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
#include <opencv2/opencv.hpp>
#include "vendor/include/zmq.h"
#include "tinythread.hpp"
#include "common.hpp"
#include "database.h"

using namespace std;

#define URLOUT_DEFAULT "frames"

class EvMLMotion: public TinyThread {
private:
    void *pSubCtx = NULL, *pReqCtx = NULL; // for packets relay
    void *pSub = NULL, *pReq = NULL;
    string urlOut, urlPub, urlRep, sn;
    int iid;
    bool enablePush = false;
    AVFormatContext *pAVFormatInput = NULL;
    AVCodecContext *pCodecCtx = NULL;
    AVDictionary *pOptsRemux = NULL;
    // load from db
    int streamIdx = -1;

    int init()
    {
        int ret = 0;
        bool inited = false;
        // TODO: read db to get sn
        sn = "ILS-3";
        iid = 3;
        while(!inited) {
            // req config
            json jr = cloudutils::registry(sn.c_str(), "evmlmotion", iid);
            bool bcnt = false;
            try {
                spdlog::info("registry: {:s}", jr.dump());
                json data = jr["data"]["services"]["evpuller"];
                string addr = data["addr"].get<string>();
                if(addr == "0.0.0.0") {
                    addr = "localhost";
                }
                urlPub = string("tcp://") + addr + ":" + to_string(data["port-pub"]);
                urlRep = string("tcp://") + addr + ":" + to_string(data["port-rep"]);
                spdlog::info("evmlmotion {} {} will connect to {} for sub, {} for req", sn, iid, urlPub, urlRep);

                data = jr["data"]["services"]["evml"];
                for(auto &j: data) {
                    try {
                        j.at("path").get_to(urlOut);
                    }
                    catch(exception &e) {
                        spdlog::warn("evslicer {} {} exception get params for storing slices: {}, using default: {}", sn, iid, e.what(), URLOUT_DEFAULT);
                        urlOut = URLOUT_DEFAULT;
                    }
                    ret = system(("mkdir -p " +urlOut).c_str());
                    if(ret == -1) {
                        spdlog::error("failed mkdir {}", urlOut);
                        return -1;
                    }
                    //TODO
                    break;
                }

            }
            catch(exception &e) {
                bcnt = true;
                spdlog::error("evmlmotion {} {} exception in EvPuller.init {:s},  retrying...", sn, iid, e.what());
            }

            if(bcnt || urlOut.empty()) {
                // TODO: waiting for command
                spdlog::warn("evmlmotion {} {} waiting for command & retrying", sn, iid);
                this_thread::sleep_for(chrono::milliseconds(1000*20));
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
            spdlog::error("evmlmotion failed connect to pub: {}, {}", sn, iid);
            return -1;
        }
        ret = zmq_connect(pSub, urlPub.c_str());
        if(ret != 0) {
            spdlog::error("evmlmotion {} {} failed create sub", sn, iid);
            return -2;
        }

        // setup req
        pReqCtx = zmq_ctx_new();
        pReq = zmq_socket(pReqCtx, ZMQ_REQ);
        spdlog::info("evmlmotion {} {} try create req to {}", sn, iid, urlRep);
        ret = zmq_connect(pReq, urlRep.c_str());

        if(ret != 0) {
            spdlog::error("evmlmotion {} {} failed create req to {}", sn, iid, urlRep);
            return -3;
        }

        spdlog::info("evmlmotion {} {} success setupMq", sn, iid);

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
        if(pReq != NULL) {
            zmq_close(pSub);
            pReq = NULL;
        }
        if(pReqCtx != NULL) {
            zmq_ctx_destroy(pSub);
            pReqCtx = NULL;
        }

        return 0;
    }

    int setupStream()
    {
        int ret = 0;

        // req avformatcontext packet
        // send first packet to init connection
        zmq_msg_t msg;
        zmq_send(pReq, "hello", 5, 0);
        spdlog::info("evmlmotion {} {} success send hello", sn, iid);
        ret =zmq_msg_init(&msg);
        if(ret != 0) {
            spdlog::error("failed to init zmq msg");
            exit(1);
        }
        // receive packet
        ret = zmq_recvmsg(pReq, &msg, 0);
        spdlog::info("evmlmotion {} {} recv", sn, iid);
        if(ret < 0) {
            spdlog::error("evmlmotion {} {} failed to recv zmq msg: {}", sn, iid, zmq_strerror(ret));
            exit(1);
        }

        pAVFormatInput = (AVFormatContext *)malloc(sizeof(AVFormatContext));
        AVFormatCtxSerializer::decode((char *)zmq_msg_data(&msg), ret, pAVFormatInput);

        // close req
        {
            zmq_msg_close(&msg);
            if(pReq != NULL) {
                zmq_close(pReq);
                pReq = NULL;
            }
            if(pReqCtx != NULL) {
                zmq_ctx_destroy(pReqCtx);
                pReqCtx = NULL;
            }
        }

        //  find video
        for (int i = 0; i < pAVFormatInput->nb_streams; i++) {
            AVStream *out_stream;
            AVStream *in_stream = pAVFormatInput->streams[i];
            AVCodecParameters *in_codecpar = in_stream->codecpar;
            if (in_codecpar->codec_type != AVMEDIA_TYPE_VIDEO) {
                continue;
            }
            streamIdx = i;
            break;
        }

        if(streamIdx == -1) {
            spdlog::error("no video stream found.");
            return -1;
        }

        AVCodec *pCodec = avcodec_find_decoder(pAVFormatInput->streams[streamIdx]->codecpar->codec_id);
        if (pCodec==NULL) {
            spdlog::error("ERROR unsupported codec!");
            return -1;
        }

        pCodecCtx = avcodec_alloc_context3(pCodec);
        if (!pCodecCtx) {
            spdlog::error("failed to allocated memory for AVCodecContext");
            return -1;
        }
        if (avcodec_parameters_to_context(pCodecCtx, pAVFormatInput->streams[streamIdx]->codecpar) < 0) {
            spdlog::error("failed to copy codec params to codec context");
            return -1;
        }

        if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
            spdlog::error("failed to open codec through avcodec_open2");
            return -1;
        }

        return ret;
    }

    int decode_packet(AVPacket *pPacket, AVCodecContext *pCodecContext, AVFrame *pFrame)
    {
        int response = avcodec_send_packet(pCodecContext, pPacket);
        if (response < 0) {
            spdlog::error("Error while sending a packet to the decoder: {}", av_err2str(response));
            return response;
        }

        while (response >= 0) {
            response = avcodec_receive_frame(pCodecContext, pFrame);
            if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
                break;
            }

            if (response < 0) {
                spdlog::error("Error while receiving a frame from the decoder: {}", av_err2str(response));
                return response;
            } else {
                spdlog::info(
                    "Frame {} (type={}, size={} bytes) pts {} key_frame {} [DTS {}]",
                    pCodecContext->frame_number,
                    av_get_picture_type_char(pFrame->pict_type),
                    pFrame->pkt_size,
                    pFrame->pts,
                    pFrame->key_frame,
                    pFrame->coded_picture_number
                );

    
                // save a grayscale frame into a .pgm file
                string name = urlOut + "/"+ to_string(chrono::duration_cast<chrono::seconds>(chrono::system_clock::now().time_since_epoch()).count()) + ".pgm";
                save_gray_frame(pFrame->data[0], pFrame->linesize[0], pFrame->width, pFrame->height, const_cast<char*>( name.c_str()));
            }
            spdlog::debug("ch4");
        }
        return 0;
    }

    void save_gray_frame(unsigned char *buf, int wrap, int xsize, int ysize, char *filename)
    {
        FILE *f;
        int i;
        f = fopen(filename,"w");
        fprintf(f, "P5\n%d %d\n%d\n", xsize, ysize, 255);

        // writing line by line
        for (i = 0; i < ysize; i++)
            fwrite(buf + i * wrap, 1, xsize, f);
        fclose(f);
    }
protected:
    void run()
    {
        bool bStopSig = false;
        int ret = 0;
        int idx = 0;
        int pktCnt = 0;
        zmq_msg_t msg;
        AVPacket packet;

        AVFrame *pFrame = av_frame_alloc();
        if (!pFrame) {
            spdlog::error("failed to allocated memory for AVFrame");
            exit(1);
        }
        while(true) {
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
            if(pktCnt % 1024 == 0) {
                spdlog::info("seq: {}, pts: {}, dts: {}, dur: {}, idx: {}", pktCnt, packet.pts, packet.dts, packet.duration, packet.stream_index);
            }
            pktCnt++;

            if (packet.stream_index == streamIdx) {
                spdlog::debug("AVPacket.pts {}", packet.pts);
                ret = decode_packet(&packet, pCodecCtx, pFrame);
                if (ret < 0)
                    break;
            }

            av_packet_unref(&packet);
            if (ret < 0) {
                spdlog::error("error muxing packet");
            }
        }
    }
public:
    EvMLMotion()
    {
        init();
        setupMq();
        setupStream();
    };
    ~EvMLMotion() {};
};

int main(int argc, const char *argv[])
{
    spdlog::set_level(spdlog::level::debug);
    EvMLMotion es;
    es.join();
    return 0;
}