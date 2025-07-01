#define _GLIBCXX_USE_CXX11_ABI 1
#include <functional>
#include <memory>
#include <thread>

#include <stdlib.h>
#include <iostream>
#include "imf/net/loop.h"
#include "imf/net/threadloop.h"
#include "imf/ssp/sspclient.h"
#include <mutex>
#include <vector>
#include <string>
#include <string>
#include <thread>
#include <mutex>
#include <curl/curl.h>
#include <algorithm>  // for std::min
#include <map>
#include <cuda_runtime.h> // For CUDA API calls
#include <nppi.h>         // For NPP functions
#include <nppi_color_conversion.h> // Specific NPP color conversion functions
#include "nv12_to_bgra.cuh" // Custom header for NV12 to BGRA conversion

using namespace std::placeholders;

#ifdef _DEBUG
#pragma comment (lib, "libsspd.lib")
#else
#pragma comment (lib, "libssp.lib")
#endif



extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavcodec/bsf.h>
}

#include <Processing.NDI.Lib.h>
std::atomic<bool> running(true);
std::mutex out_mutex;


enum class DecoderType {
	SOFTWARE,
	HW_CUDA
};

enum class HWCodecType {
	H264_CUVID,
	HEVC_CUVID
};

struct ClientContext {
	int client_id;                        // Unique ID per client
	std::string name;                    // NDI sender name

	DecoderType type = DecoderType::SOFTWARE;
	HWCodecType hwCodecType = HWCodecType::H264_CUVID;
	// FFmpeg decoding
	const AVCodec* codec = nullptr;
	AVCodecContext* codec_ctx = nullptr;
	AVBufferRef* hw_device_ctx = nullptr;  // Only used in HW mode

	// Conversion (YUV → BGRA)
	SwsContext* sws_ctx = nullptr;

	// RGB frame for rendering or NDI
	AVFrame* rgb_frame = nullptr;
	int last_width = 0;
	int last_height = 0;

	// NDI sender
	NDIlib_send_instance_t ndi_sender = nullptr;

	// Thread safety
	std::mutex decoder_mutex;

	// Members for general decode paths, holding CPU-resident data
	AVFrame* host_frame_bgr = nullptr; // Will hold CPU-resident BGR data after sws_scale
	SwsContext* sws_ctx_yuv_to_bgr = nullptr; // For YUV to BGR conversion on CPU


	// No need for ctx.rgb_frame (AVFrame) or ctx.sws_ctx (SwsContext) for the CUDA path
	// if you fully offload to GPU. Keep them for SW decode path if still supported.

	// A flag to indicate if CUDA resources are initialized/allocated
	bool cuda_resources_initialized = false;
};


// Assume imf::SspH264Data and ClientContext, DecoderType enum are defined elsewhere
// struct imf::SspH264Data { /* ... */ };
// enum DecoderType { SW, HW_CUDA, /* ... */ };


// Helper to check CUDA errors
#define CHECK_CUDA(call) \
    do { \
        cudaError_t err = call; \
        if (err != cudaSuccess) { \
            fprintf(stderr, "CUDA Error at %s:%d - %s\n", __FILE__, __LINE__, cudaGetErrorString(err)); \
            /* Handle error, e.g., exit or return */ \
        } \
    } while (0)

// Helper to check NPP errors
#define CHECK_NPP(call) \
    do { \
        NppStatus status = call; \
        if (status != NPP_SUCCESS) { \
            fprintf(stderr, "NPP Error at %s:%d - %s\n", __FILE__, __LINE__, nppiGetErrorString(status)); \
            /* Handle error */ \
        } \
    } while (0)


bool init_decoder(ClientContext& ctx) {
	std::string decoderName;
	if (ctx.type == DecoderType::HW_CUDA) {
		if (ctx.hwCodecType == HWCodecType::H264_CUVID) {
			decoderName = "h264_cuvid";
		}else if (ctx.hwCodecType == HWCodecType::HEVC_CUVID) {
			decoderName = "hevc_cuvid";
		}else {
			return false;
		}
		ctx.codec = avcodec_find_decoder_by_name(decoderName.c_str());  // or "h264_nvdec"
		if (!ctx.codec) {
			printf("Client[%d] CUDA decoder not found\n", ctx.client_id);
			return false;
		}

		ctx.codec_ctx = avcodec_alloc_context3(ctx.codec);
		if (!ctx.codec_ctx) return false;

		if (av_hwdevice_ctx_create(&ctx.hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) < 0) {
			printf("Failed to create CUDA HW device for client %d\n", ctx.client_id);
			return false;
		}
		ctx.codec_ctx->hw_device_ctx = av_buffer_ref(ctx.hw_device_ctx);

		ctx.codec_ctx->get_format = [](AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {
			while (*pix_fmts != AV_PIX_FMT_NONE) {
				if (*pix_fmts == AV_PIX_FMT_CUDA)
					return *pix_fmts;
				pix_fmts++;
			}
			return pix_fmts[0];
			};
	}
	else {  // SOFTWARE
		if (ctx.hwCodecType == HWCodecType::H264_CUVID) {
			ctx.codec = avcodec_find_decoder(AV_CODEC_ID_H264);
		}
		else if (ctx.hwCodecType == HWCodecType::HEVC_CUVID) {
			ctx.codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
		}else {
			return false;
		}
		
		if (!ctx.codec) {
			printf("Client[%d] software decoder not found\n", ctx.client_id);
			return false;
		}

		ctx.codec_ctx = avcodec_alloc_context3(ctx.codec);
		if (!ctx.codec_ctx) return false;
	}

	if (avcodec_open2(ctx.codec_ctx, ctx.codec, nullptr) < 0) {
		printf("Could not open codec for client %d\n", ctx.client_id);
		return false;
	}

	printf("Codec pixel format: %s\n", av_get_pix_fmt_name(ctx.codec_ctx->pix_fmt));

	printf("Client[%d] decoder (%s) initialized.\n", ctx.client_id,
		(ctx.type == DecoderType::HW_CUDA ? "HW_CUDA" : "SOFTWARE"));
	return true;
}

#include <cstdio>
#include <mutex>
#include <vector> // Potentially for temporary buffers if needed


#if 0
// --- handle_h264_data function ---
void handle_h264_data(ClientContext& ctx, struct imf::SspH264Data* h264) {
	std::lock_guard<std::mutex> lock(ctx.decoder_mutex);

	if (!ctx.codec_ctx) {
		fprintf(stderr, "Codec context not initialized for client %d\n", ctx.client_id);
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		fprintf(stderr, "Failed to allocate AVPacket\n");
		return;
	}

	pkt->data = h264->data;
	pkt->size = h264->len;

	int send_ret = avcodec_send_packet(ctx.codec_ctx, pkt);
	if (send_ret < 0) {
		fprintf(stderr, "Error sending packet to decoder: \n");
		av_packet_free(&pkt);
		return;
	}

	AVFrame* frame = av_frame_alloc(); // Frame from decoder (can be HW or SW)
	if (!frame) {
		fprintf(stderr, "Failed to allocate AVFrame\n");
		av_packet_free(&pkt);
		return;
	}

	AVFrame* decoded_frame_on_cpu = av_frame_alloc(); // Will hold CPU-accessible data after transfer
	if (!decoded_frame_on_cpu) {
		fprintf(stderr, "Failed to allocate decoded_frame_on_cpu\n");
		av_packet_free(&pkt);
		av_frame_free(&frame);
		return;
	}

	while (true) {
		int ret = avcodec_receive_frame(ctx.codec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			// No more frames to receive for this packet
			break;
		}
		if (ret < 0) {
			fprintf(stderr, "Error receiving frame\n");
			break;
		}

		printf("Decoded frame %d size %dx%d, format %s\n", h264->frm_no, frame->width, frame->height, av_get_pix_fmt_name((AVPixelFormat)frame->format));

		AVFrame* frame_for_conversion_on_cpu = nullptr;

		// --- Handle Hardware CUDA Decode Path: Transfer GPU frame to CPU ---
		if (ctx.type == DecoderType::HW_CUDA) {
			// This step is NECESSARY because bgrToBgra_export expects host pointers.
			// It copies the GPU-resident frame data to a CPU-resident AVFrame.
			ret = av_hwframe_transfer_data(decoded_frame_on_cpu, frame, 0);
			if (ret < 0) {
				fprintf(stderr, "Error transferring frame data from HW to CPU:\n");
				av_frame_unref(frame); // Unreference GPU frame
				continue; // Skip this frame and try next if available
			}
			frame_for_conversion_on_cpu = decoded_frame_on_cpu;
		}
		else { // --- Software Decode Path: Frame is already on CPU ---
			frame_for_conversion_on_cpu = frame;
		}

		// --- Common CPU-based Conversion to BGR using sws_scale ---
		// This converts the CPU-resident YUV (or other format) frame to CPU-resident BGR.
		if (!ctx.host_frame_bgr || ctx.last_width != frame_for_conversion_on_cpu->width || ctx.last_height != frame_for_conversion_on_cpu->height) {
			if (ctx.host_frame_bgr) av_frame_free(&ctx.host_frame_bgr);
			if (ctx.sws_ctx_yuv_to_bgr) sws_freeContext(ctx.sws_ctx_yuv_to_bgr);

			ctx.host_frame_bgr = av_frame_alloc();
			if (!ctx.host_frame_bgr) { fprintf(stderr, "Failed to allocate host_frame_bgr\n"); break; }

			ctx.host_frame_bgr->format = AV_PIX_FMT_BGR24; // Intermediate BGR format (on CPU)
			ctx.host_frame_bgr->width = frame_for_conversion_on_cpu->width;
			ctx.host_frame_bgr->height = frame_for_conversion_on_cpu->height;
			// Allocate buffer for the BGR24 frame (on CPU)
			int buffer_alloc_ret = av_frame_get_buffer(ctx.host_frame_bgr, 32);
			if (buffer_alloc_ret < 0) {
				fprintf(stderr, "Failed to allocate buffer for host_frame_bgr:\n");
				av_frame_free(&ctx.host_frame_bgr); ctx.host_frame_bgr = nullptr;
				break;
			}

			ctx.sws_ctx_yuv_to_bgr = sws_getContext(
				frame_for_conversion_on_cpu->width, frame_for_conversion_on_cpu->height,
				(AVPixelFormat)frame_for_conversion_on_cpu->format, // Input format
				ctx.host_frame_bgr->width, ctx.host_frame_bgr->height,
				AV_PIX_FMT_BGR24, // Output BGR
				SWS_BILINEAR, nullptr, nullptr, nullptr);

			if (!ctx.sws_ctx_yuv_to_bgr) {
				printf("sws_getContext failed for client %d\n", ctx.client_id);
				break;
			}

			ctx.last_width = frame_for_conversion_on_cpu->width;
			ctx.last_height = frame_for_conversion_on_cpu->height;
		}

		// Perform the YUV to BGR conversion on CPU
		sws_scale(ctx.sws_ctx_yuv_to_bgr,
			frame_for_conversion_on_cpu->data, frame_for_conversion_on_cpu->linesize,
			0, frame_for_conversion_on_cpu->height,
			ctx.host_frame_bgr->data, ctx.host_frame_bgr->linesize);

		// --- Call bgrToBgra_export (from your DLL) with the CPU BGR data ---
		// This vector will hold the final BGRA data returned to CPU by the DLL
		std::vector<Npp8u> h_bgra_output(ctx.host_frame_bgr->width * ctx.host_frame_bgr->height * 4);
		Npp8u alpha_val = 255; // Fully opaque alpha

		int bgrToBgra_status = bgrToBgra_export(
			(const Npp8u*)ctx.host_frame_bgr->data[0], // Input: CPU BGR data
			h_bgra_output.data(),                       // Output: CPU BGRA buffer
			ctx.host_frame_bgr->width,
			ctx.host_frame_bgr->height,
			alpha_val
		);

		if (bgrToBgra_status != 0) {
			fprintf(stderr, "bgrToBgra_export failed with error code: %d. NDI frame will not be sent.\n", bgrToBgra_status);
			// Decide how to handle this error: break, continue, log, etc.
		}
		else {
			// --- Send the final host BGRA buffer to NDI ---
			NDIlib_video_frame_v2_t ndi_frame;
			ndi_frame.xres = ctx.host_frame_bgr->width;
			ndi_frame.yres = ctx.host_frame_bgr->height;
			ndi_frame.FourCC = NDIlib_FourCC_type_BGRA; // NDI understands BGRA
			ndi_frame.frame_rate_N = 240000; // Example framerate
			ndi_frame.frame_rate_D = 1001;
			ndi_frame.picture_aspect_ratio = (float)ndi_frame.xres / ndi_frame.yres;
			ndi_frame.timecode = NDIlib_send_timecode_synthesize;
			ndi_frame.p_data = h_bgra_output.data(); // This is a CPU pointer
			ndi_frame.line_stride_in_bytes = ctx.host_frame_bgr->width * 4; // Width * 4 bytes per pixel

			NDIlib_send_send_video_v2(ctx.ndi_sender, &ndi_frame);
		}

		av_frame_unref(frame); // Unreference the original decoded frame (GPU or CPU)
		av_frame_unref(decoded_frame_on_cpu); // Unreference the CPU transferred frame (if HW)
	}

	// Clean up resources allocated for this packet/frame processing loop
	av_frame_free(&frame);
	av_frame_free(&decoded_frame_on_cpu);
	av_packet_free(&pkt);
}
#endif

// Don't forget to add a cleanup function for ClientContext to free CUDA resources
void cleanup_client_context_cuda(ClientContext& ctx) {
	/*if (ctx.d_bgra_frame) {
		CHECK_CUDA(cudaFree(ctx.d_bgra_frame));
		ctx.d_bgra_frame = nullptr;
	}*/
	ctx.cuda_resources_initialized = false;
	// Also free AVFrame and SwsContext if they are kept for SW path
	if (ctx.rgb_frame) {
		av_frame_free(&ctx.rgb_frame);
		ctx.rgb_frame = nullptr;
	}
	if (ctx.sws_ctx) {
		sws_freeContext(ctx.sws_ctx);
		ctx.sws_ctx = nullptr;
	}
	// Don't free ctx.codec_ctx or ctx.ndi_sender here, as they are managed externally
}


#if 1
void handle_h264_data(ClientContext& ctx, struct imf::SspH264Data* h264) {
	std::lock_guard<std::mutex> lock(ctx.decoder_mutex);

	if (!ctx.codec_ctx) return;

	AVPacket* pkt = av_packet_alloc();
	pkt->data = h264->data;
	pkt->size = h264->len;

	if (avcodec_send_packet(ctx.codec_ctx, pkt) < 0) {
		av_packet_free(&pkt);
		return;
	}

	AVFrame* frame = av_frame_alloc();
	AVFrame* hw_frame = nullptr;

	while (true) {
		int ret = avcodec_receive_frame(ctx.codec_ctx, frame);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			break;
		if (ret < 0) break;

		AVFrame* use_frame = frame;
		printf("Decoded frame %d size %dx%d\n", h264->frm_no, frame->width, frame->height);

		// If hardware decode, transfer to system memory
		if (ctx.type == DecoderType::HW_CUDA) {
			if (!hw_frame) hw_frame = av_frame_alloc();
			if (av_hwframe_transfer_data(hw_frame, frame, 0) < 0) {
				printf("Failed to transfer HW frame to CPU\n");
				break;
			}
			use_frame = hw_frame;
		}

		// Prepare or reallocate RGB frame
		if (!ctx.rgb_frame || ctx.last_width != use_frame->width || ctx.last_height != use_frame->height) {
			if (ctx.rgb_frame) av_frame_free(&ctx.rgb_frame);
			if (ctx.sws_ctx) sws_freeContext(ctx.sws_ctx);

			ctx.rgb_frame = av_frame_alloc();
			ctx.rgb_frame->format = AV_PIX_FMT_BGRA;
			ctx.rgb_frame->width = use_frame->width;
			ctx.rgb_frame->height = use_frame->height;
			av_frame_get_buffer(ctx.rgb_frame, 32);

			ctx.sws_ctx = sws_getContext(
				use_frame->width, use_frame->height,
				(AVPixelFormat)use_frame->format,
				ctx.rgb_frame->width, ctx.rgb_frame->height,
				AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);

			if (!ctx.sws_ctx) {
				printf("sws_getContext failed for client %d\n", ctx.client_id);
				break;
			}

			ctx.last_width = use_frame->width;
			ctx.last_height = use_frame->height;
		}

		sws_scale(ctx.sws_ctx,
			use_frame->data, use_frame->linesize,
			0, use_frame->height,
			ctx.rgb_frame->data, ctx.rgb_frame->linesize);

		NDIlib_video_frame_v2_t ndi_frame;
		ndi_frame.xres = ctx.rgb_frame->width;
		ndi_frame.yres = ctx.rgb_frame->height;
		ndi_frame.FourCC = NDIlib_FourCC_type_BGRA;
		ndi_frame.frame_rate_N = 240000;
		ndi_frame.frame_rate_D = 1001;
		ndi_frame.picture_aspect_ratio = (float)ctx.rgb_frame->width / ctx.rgb_frame->height;
		ndi_frame.timecode = NDIlib_send_timecode_synthesize;
		ndi_frame.p_data = ctx.rgb_frame->data[0];
		ndi_frame.line_stride_in_bytes = ctx.rgb_frame->linesize[0];

		NDIlib_send_send_video_v2(ctx.ndi_sender, &ndi_frame);

		av_frame_unref(frame);
		if (hw_frame) av_frame_unref(hw_frame);
	}

	if (hw_frame) av_frame_free(&hw_frame);
	av_frame_free(&frame);
	av_packet_free(&pkt);
}
#endif

static void on_audio_data_1(struct imf::SspAudioData * audio)
{

}

static void on_audio_data_2(struct imf::SspAudioData * audio)
{

}

static void on_meta_1(struct imf::SspVideoMeta *v, struct imf::SspAudioMeta *a, struct imf::SspMeta * m)
{
	printf("on meta 1 wall clock %d", m->pts_is_wall_clock);
	printf("              video %dx%d %d/%d\n", v->width, v->height, v->unit, v->timescale);
	printf("              audio %d\n", a->sample_rate);
}

static void on_meta_2(struct imf::SspVideoMeta *v, struct imf::SspAudioMeta *a, struct imf::SspMeta * m)
{

}

static void on_disconnect()
{
	printf("on disconnect\n");
}

static std::vector<std::unique_ptr<ClientContext>> g_client_contexts;
static std::vector<std::unique_ptr<imf::SspClient>> g_ssp_clients;

bool is_zcam(const std::string& ip) {
	std::string url = "http://" + ip + "/info";
	CURL* curl = curl_easy_init();
	if (!curl) return false;

	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 300L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](char* ptr, size_t size, size_t nmemb, void* userdata) {
		std::string* response = static_cast<std::string*>(userdata);
		response->append(ptr, size * nmemb);
		return size * nmemb;
		});

	std::string response;
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

	CURLcode res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);

	if (res == CURLE_OK && response.find("\"model\"") != std::string::npos) {
		std::lock_guard<std::mutex> lock(out_mutex);
		std::cout << "Found Z CAM : " << ip << "\n";
		return true;
	}
	return false;
}

void scan_range(const std::string& base, int start, int end, std::vector<std::string>& cameras) {
	for (int i = start; i <= end; ++i) {
		std::string ip = base + std::to_string(i);
		if (is_zcam(ip)) {
			std::lock_guard<std::mutex> lock(out_mutex);
			cameras.push_back(ip);
		}
	}
}

class ZCamStreamBuilder {
public:
	ZCamStreamBuilder(const std::string& ip)
		: ip_(ip) {
		params_["index"] = "stream1"; // default
	}

	ZCamStreamBuilder& index(const std::string& index) {
		params_["index"] = index;
		return *this;
	}

	ZCamStreamBuilder& resolution(int w, int h) {
		params_["width"] = std::to_string(w);
		params_["height"] = std::to_string(h);
		return *this;
	}

	ZCamStreamBuilder& bitrate(int bps) {
		params_["bitrate"] = std::to_string(bps);
		return *this;
	}

	ZCamStreamBuilder& encoder(const std::string& enc) {
		params_["venc"] = enc;
		return *this;
	}

	ZCamStreamBuilder& fps(int value) {
		params_["fps"] = std::to_string(value);
		return *this;
	}

	bool apply() {
		std::string url = "http://" + ip_ + "/ctrl/stream_setting";
		bool first = true;
		for (const auto& pair : params_) {
			url += (first ? "?" : "&") + pair.first + "=" + pair.second;
			first = false;
		}

		CURL* curl = curl_easy_init();
		if (!curl) return false;

		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
		CURLcode res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);

		return res == CURLE_OK;
	}


private:
	std::string ip_;
	std::map<std::string, std::string> params_;
};


static void setup(imf::Loop* loop)
{
	// Define client inputs (IP + decoder type)
	struct {
		std::string ip;
		DecoderType decoder_type;
		HWCodecType hwCodecType;
		std::string ndi_name;
	} client_inputs[] = {
		{ "192.168.11.108", DecoderType::HW_CUDA, HWCodecType::H264_CUVID, "Khel_NDI_1" },
		{ "192.168.11.149", DecoderType::HW_CUDA, HWCodecType::H264_CUVID, "Khel_NDI_2" }
	};

	const int client_count = sizeof(client_inputs) / sizeof(client_inputs[0]);

	for (int i = 0; i < client_count; ++i) {
		const auto& input = client_inputs[i];

		// Create ClientContext
		auto ctx = std::make_unique<ClientContext>();
		ctx->client_id = i;
		ctx->type = input.decoder_type;
		ctx->name = input.ndi_name;
		ctx->hwCodecType = input.hwCodecType;

		// Initialize decoder
		if (!init_decoder(*ctx)) {
			printf("Decoder init failed for client %d\n", i);
			continue;
		}

		// Initialize NDI sender
		NDIlib_send_create_t NDI_send_create_desc;
		NDI_send_create_desc.p_ndi_name = ctx->name.c_str();
		ctx->ndi_sender = NDIlib_send_create(&NDI_send_create_desc);
		if (!ctx->ndi_sender) {
			printf("Failed to create NDI sender for client %d\n", i);
			continue;
		}

		// Save context pointer for lambda
		ClientContext* ctx_ptr = ctx.get();

		// Create SspClient
		auto client = std::make_unique<imf::SspClient>(input.ip, loop, 0x400000);
		client->init();
		client->setOnH264DataCallback([ctx_ptr](imf::SspH264Data* h264) {
			handle_h264_data(*ctx_ptr, h264);
			});
		client->setOnMetaCallback(on_meta_1);  // Optional: pass client ID
		client->setOnDisconnectedCallback(on_disconnect);
		client->setOnAudioDataCallback(on_audio_data_1);

		client->start();

		// Save for lifetime management
		g_client_contexts.push_back(std::move(ctx));
		g_ssp_clients.push_back(std::move(client));

		printf("Started client %d: %s [%s]\n", i, input.ip.c_str(),
			input.decoder_type == DecoderType::HW_CUDA ? "HW_CUDA" : "SW");
	}
}

void cleanup_clients() {
	for (auto& ctx : g_client_contexts) {
		if (ctx->ndi_sender)
			NDIlib_send_destroy(ctx->ndi_sender);
		if (ctx->codec_ctx)
			avcodec_free_context(&ctx->codec_ctx);
		if (ctx->hw_device_ctx)
			av_buffer_unref(&ctx->hw_device_ctx);
		if (ctx->sws_ctx)
			sws_freeContext(ctx->sws_ctx);
		if (ctx->rgb_frame)
			av_frame_free(&ctx->rgb_frame);
	}
	g_client_contexts.clear();
	g_ssp_clients.clear();
}

void handle_sigint(int) {
	running = false;
}


int DetectZCam() {
	std::vector<std::string> found;
	std::vector<std::thread> threads;
	std::string base = "192.168.11."; // Or your subnet

	for (int i = 1; i <= 254; i += 32)
		threads.emplace_back(scan_range, base, i, ((i + 31) < 254 ? (i + 31) : 254), std::ref(found));


	for (auto& t : threads) t.join();

	std::cout << "Z CAMs Found: " << found.size() << std::endl;

	for (int i = 0; i < found.size(); ++i) {
		ZCamStreamBuilder builder(found[i]);
		bool success = builder.index("stream0")
			.resolution(1920, 1080)
			.bitrate(10000000)
			.encoder("h265")
			.fps(30)
			.apply();

		if (success) {
			std::cout << "Stream settings applied successfully for: " << found[i] << std::endl;
		}
		else {
			std::cout << "Failed to apply stream settings for: " << found[i] << std::endl;
		}
	}

	return 0;
}

int main(int argc, char ** argv)
{
	signal(SIGINT, handle_sigint);

	DetectZCam();

	std::unique_ptr<imf::ThreadLoop> threadLooper(new imf::ThreadLoop(std::bind(setup, _1)));
	threadLooper->start();


	while (1) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	cleanup_clients();
	threadLooper->stop();
	return 0;
}

