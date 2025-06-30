#pragma once

extern "C" {
#include <libswscale/swscale.h>
#include <libavutil/frame.h>
}

#include <Processing.NDI.Lib.h>
#include <string>

class NDISender
{
public:
    NDISender(const std::string& ndiName);
    ~NDISender();

    bool initialize();
    void sendFrame(AVFrame* yuvFrame);

private:
    NDIlib_send_instance_t ndi_sender_ = nullptr;
    SwsContext* sws_ctx_ = nullptr;
    AVFrame* rgb_frame_ = nullptr;
    int last_width_ = 0;
    int last_height_ = 0;
};
