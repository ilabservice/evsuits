#include <fstream>
#include <sstream>
#include <iostream>
#include <tuple>
#include "fs.h"
#include "spdlog/spdlog.h"

#ifdef _MY_HEADERS_
#include <opencv2/core/types_c.h>
#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/highgui.hpp>
#else
#include <opencv2/core/types_c.h>
#include <opencv2/opencv.hpp>
#endif

using namespace cv;
using namespace dnn;
using namespace std;

// Remove the bounding boxes with low confidence using non-maxima suppression
int postprocess(Mat& frame, const vector<Mat>& out);

// Draw the predicted bounding box
void drawPred(int classId, float conf, int left, int top, int right, int bottom, Mat& frame);

// Get the names of the output layers
vector<String> getOutputsNames(const Net& net);


class YoloDectect {
private:
    // Initialize the parameters
    const string selfId = "YoloDetector";
    float confThreshold = 0.5; // Confidence threshold
    float nmsThreshold = 0.4;  // Non-maximum suppression threshold
    int inpWidth = 416;  // Width of network's input image
    int inpHeight = 416; // Height of network's input image
    vector<string> classes;
    Net net;
    Mat blob;
    VideoCapture cap;
    VideoWriter video;
    bool bOutputIsImg = false;
    string outFileBase;
    bool cmdStop = false;

    // Get the names of the output layers
    vector<String> getOutputsNames(const Net& net)
    {
        static vector<String> names;
        if (names.empty()) {
            //Get the indices of the output layers, i.e. the layers with unconnected outputs
            vector<int> outLayers = net.getUnconnectedOutLayers();

            //get the names of all the layers in the network
            vector<String> layersNames = net.getLayerNames();

            // Get the names of the output layers in names
            names.resize(outLayers.size());
            for (size_t i = 0; i < outLayers.size(); ++i)
                names[i] = layersNames[outLayers[i] - 1];
        }
        return names;
    }

    // draw the predicted bounding box
    void drawPred(int classId, float conf, int left, int top, int right, int bottom, Mat& frame)
    {
        // draw a rectangle displaying the bounding box
        rectangle(frame, Point(left, top), Point(right, bottom), Scalar(255, 178, 50), 3);

        //get the label for the class name and its confidence
        string label = format("%.2f", conf);
        if (!classes.empty()) {
            CV_Assert(classId < (int)classes.size());
            label = classes[classId] + ":" + label;
        }

        // display the label at the top of the bounding box
        int baseLine;
        Size labelSize = getTextSize(label, FONT_HERSHEY_SIMPLEX, 0.5, 1, &baseLine);
        top = max(top, labelSize.height);
        rectangle(frame, Point(left, top - round(1.5*labelSize.height)), Point(left + round(1.5*labelSize.width), top + baseLine), Scalar(255, 255, 255), FILLED);
        putText(frame, label, Point(left, top), FONT_HERSHEY_SIMPLEX, 0.75, Scalar(0,0,0),1);
    }

    // post process
    vector<tuple<string, double, Rect>> postprocess(Mat& frame, const vector<Mat>& outs)
    {
        vector<int> classIds;
        vector<float> confidences;
        vector<Rect> boxes;

        for (size_t i = 0; i < outs.size(); ++i) {
            // Scan through all the bounding boxes output from the network and keep only the
            // ones with high confidence scores. Assign the box's class label as the class
            // with the highest score for the box.
            float* data = (float*)outs[i].data;
            for (int j = 0; j < outs[i].rows; ++j, data += outs[i].cols) {
                Mat scores = outs[i].row(j).colRange(5, outs[i].cols);
                Point classIdPoint;
                double confidence;
                // Get the value and location of the maximum score
                minMaxLoc(scores, 0, &confidence, 0, &classIdPoint);
                if (confidence > confThreshold) {
                    int centerX = (int)(data[0] * frame.cols);
                    int centerY = (int)(data[1] * frame.rows);
                    int width = (int)(data[2] * frame.cols);
                    int height = (int)(data[3] * frame.rows);
                    int left = centerX - width / 2;
                    int top = centerY - height / 2;

                    classIds.push_back(classIdPoint.x);
                    confidences.push_back((float)confidence);
                    boxes.push_back(Rect(left, top, width, height));
                }
            }
        }

        // Perform non maximum suppression to eliminate redundant overlapping boxes with lower confidences
        vector<int> indices;
        NMSBoxes(boxes, confidences, confThreshold, nmsThreshold, indices);
        vector<tuple<string, double, Rect>> ret;
        for (size_t i = 0; i < indices.size(); ++i) {
            int idx = indices[i];
            Rect box = boxes[idx];
            ret.push_back(tuple<string, double, Rect>(classes[classIds[idx]], confidences[idx], box));
            drawPred(classIds[idx], confidences[idx], box.x, box.y, box.x + box.width, box.y + box.height, frame);
        }

        return ret;
    }

    //
protected:

    //
public:
    typedef int (*callback)(vector<tuple<string, double, Rect>>, Mat);
    YoloDectect(string path = "")
    {
        if(path.empty()) {
            path = ".";
        }

        // Load names of classes
        string classesFile = path + "/coco.names";
        // Give the configuration and weight files for the model
        String modCfg = path + "/yolov3-tiny.cfg";
        String modWeights = path + "/yolov3-tiny.weights";

        if(!fs::exists(classesFile) || !fs::exists(modCfg) || !fs::exists(modWeights)) {
            spdlog::error("{} failed to load configration files", selfId);
            exit(1);
        }

        ifstream ifs(classesFile.c_str());
        string line;
        while (getline(ifs, line)) {
            classes.push_back(line);
        }

        // Load the network
        net = readNetFromDarknet(modCfg, modWeights);
        net.setPreferableBackend(DNN_BACKEND_OPENCV);
        net.setPreferableTarget(DNN_TARGET_CPU);
        spdlog::info("{} inited", selfId);
    }

    vector<tuple<string, double, Rect>> process(Mat &inFrame, Mat &outFrame)
    {
        if(inFrame.empty()) {
            return vector<tuple<string, double, Rect>>();
        }

        // Create a 4D blob from a frame.
        blobFromImage(inFrame, blob, 1/255.0, cvSize(inpWidth, inpHeight), Scalar(0,0,0), true, false);

        //Sets the input to the network
        net.setInput(blob);

        // Runs the forward pass to get output of the output layers
        vector<Mat> outs;
        net.forward(outs, getOutputsNames(net));

        // Remove the bounding boxes with low confidence
        auto ret = postprocess(inFrame, outs);

        // The function getPerfProfile returns the overall time for inference(t) and the timings for each of the layers(in layersTimes)
        vector<double> layersTimes;
        double freq = getTickFrequency() / 1000;
        double t = net.getPerfProfile(layersTimes) / freq;
        spdlog::info("{} infer time: {} ms", selfId, t);

        inFrame.convertTo(outFrame, CV_8U);
        return ret;
    }


    int process(string inVideoUri, callback cb = nullptr, string outFile = "processed.jpg")
    {
        if(inVideoUri.empty()) {
            inVideoUri = "0";
        }

        ghc::filesystem::path p(outFile);
        auto dir = p.parent_path();

        if((outFile.substr(outFile.find_last_of(".") + 1) == "jpg")) {
            bOutputIsImg = true;
            outFileBase = string(dir / p.stem());
            spdlog::info("{} outFileBase {}", selfId, outFileBase);
        }
        else {
            bOutputIsImg = false;
            if(!video.open(outFile, VideoWriter::fourcc('M','J','P','G'), 28, Size(cap.get(CAP_PROP_FRAME_WIDTH), cap.get(CAP_PROP_FRAME_HEIGHT)))) {
                spdlog::error("{} failed to open output video {}", selfId, inVideoUri);
                return -1;
            }
        }

        if(!cap.open(inVideoUri)) {
            spdlog::error("{} failed to open input video {}", selfId, inVideoUri);
            return -1;
        }

        spdlog::info("{} try to process video {} to {}", selfId, inVideoUri, outFile);

        long frameCnt = 0;
        long detCnt = 0, skipCnt = 0;
        Mat frame, outFrame;
        while (waitKey(1) < 0) {
            // get frame from the video
            if(cmdStop) {
                break;
            }

            if(!cap.read(frame)) {
                spdlog::error("{} failed to read frame from {}", selfId, inVideoUri);
                break;
            }

            frameCnt++;

            // Stop the program if reached end of video
            if (frame.empty()) {
                continue;
            }

            vector<tuple<string, double, Rect>> ret = process(frame, outFrame);
            if(ret.size() == 0 && bOutputIsImg) {
                // no detection
                if(skipCnt % 100 == 0) {
                    spdlog::info("{} no valid object detected skipped frame count {}", selfId, skipCnt);
                }
                skipCnt++;    
                continue;
            }

            if (bOutputIsImg) {
                string ofname = outFileBase + to_string(detCnt) + ".jpg";
                imwrite(ofname, outFrame);
                detCnt++;
            }
            else {
                video.write(outFrame);
            }

        }

        cap.release();
        if(!bOutputIsImg) video.release();

        return 0;
    }
};

int main(int argc, char** argv)
{
    YoloDectect det;
    det.process("rtsp://admin:ZQEAAI@192.168.0.101:554/h264/ch1/main/av_stream");

    return 0;
}
