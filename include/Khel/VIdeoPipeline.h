#pragma once
#include <string>
#include <memory>
#include "VideoDecoder.h"

extern "C" {
#include <Processing.NDI.Lib.h>
#include <libswscale/swscale.h>
}

class VideoPipeline
{
public:
    VideoPipeline(const std::string& ip,
        const std::string& ndiName,
        VideoDecoder::CodecType codecType);

    ~VideoPipeline();

    // Must be called before processing
    bool initialize();

    // Called when a frame arrives (e.g., from SspClient)
    void processPacket(uint8_t* data, int size, int frameNo);

private:
    std::string ip_;
    std::string ndiName_;
    VideoDecoder::CodecType codecType_;

    std::unique_ptr<VideoDecoder> decoder_;
    NDIlib_send_instance_t ndiSender_;
    SwsContext* swsCtx_ = nullptr;

    int lastWidth_ = 0;
    int lastHeight_ = 0;
    AVFrame* rgbFrame_ = nullptr;
};
