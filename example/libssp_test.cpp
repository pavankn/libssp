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
#include <curl/curl.h>
#include <algorithm>  // for std::min
#include <map>
#include <cuda_runtime.h> // For CUDA API calls
#include <nppi.h>         // For NPP functions
#include <nppi_color_conversion.h> // Specific NPP color conversion functions
#include "nv12_to_bgra.cuh" // Custom header for NV12 to BGRA conversion
#include <nlohmann/json.hpp>
#include <queue>

using namespace std::placeholders;

#ifdef _DEBUG
#pragma comment (lib, "libsspd.lib")
#else
#pragma comment (lib, "libssp.lib")
#endif

using json = nlohmann::json;


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

#if 0
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
#else

struct ClientContext {
	int client_id = -1;

	// Decoder config
	DecoderType type = DecoderType::SOFTWARE;
	HWCodecType hwCodecType = HWCodecType::H264_CUVID;

	const AVCodec* codec = nullptr;
	AVCodecContext* codec_ctx = nullptr;
	AVBufferRef* hw_device_ctx = nullptr;
	std::string name;  // NDI sender name

	// Output frame conversion
	SwsContext* sws_ctx = nullptr;
	AVFrame* rgb_frame = nullptr;
	int last_width = 0;
	int last_height = 0;

	// NDI
	NDIlib_send_instance_t ndi_sender = nullptr;

	// Mutex for decoder
	std::mutex decoder_mutex;

	// Threading for NDI
	std::thread ndi_thread;
	std::mutex rgb_mutex;
	std::condition_variable rgb_cv;
	std::atomic<bool> new_rgb_ready{ false };
	std::atomic<bool> shutdown{ false };

	std::queue<AVFrame*> frame_queue;
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	bool running = true; // for clean exit
};
#endif


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

// Don't forget to add a cleanup function for ClientContext to free CUDA resources
void cleanup_client_context_cuda(ClientContext& ctx) {
	/*if (ctx.d_bgra_frame) {
		CHECK_CUDA(cudaFree(ctx.d_bgra_frame));
		ctx.d_bgra_frame = nullptr;
	}*/
	//ctx.cuda_resources_initialized = false;
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


void send_to_ndi(ClientContext* ctx, AVFrame* frame) {
	if (!ctx || !frame) return;

	NDIlib_video_frame_v2_t ndi_frame = {};
	ndi_frame.xres = frame->width;
	ndi_frame.yres = frame->height;
	ndi_frame.FourCC = NDIlib_FourCC_type_BGRA;
	ndi_frame.frame_rate_N = 240000;
	ndi_frame.frame_rate_D = 1001;
	ndi_frame.picture_aspect_ratio = (float)frame->width / frame->height;
	ndi_frame.timecode = NDIlib_send_timecode_synthesize;
	ndi_frame.p_data = frame->data[0];
	ndi_frame.line_stride_in_bytes = frame->linesize[0];

	NDIlib_send_send_video_v2(ctx->ndi_sender, &ndi_frame);
}


void ndi_sender_thread(ClientContext* ctx) {
	while (ctx->running) {
		std::unique_lock<std::mutex> lock(ctx->queue_mutex);
		ctx->queue_cv.wait(lock, [&] {
			return !ctx->frame_queue.empty() || !ctx->running;
			});

		while (!ctx->frame_queue.empty()) {
			AVFrame* frame = ctx->frame_queue.front();
			ctx->frame_queue.pop();
			lock.unlock(); // unlock early

			printf("[NDI %d] Sending frame to NDI...\n", ctx->client_id);

			send_to_ndi(ctx, frame);  // Use the frame popped from queue

			av_frame_free(&frame);
			lock.lock();
		}
	}
}

#if 1
void handle_h264_data(ClientContext& ctx, struct imf::SspH264Data* h264) {
	std::lock_guard<std::mutex> decoder_lock(ctx.decoder_mutex);

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

			if (av_frame_get_buffer(ctx.rgb_frame, 32) < 0) {
				fprintf(stderr, "Failed to allocate buffer for RGB frame\n");
				av_frame_free(&ctx.rgb_frame);
				break;
			}


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

		printf("[Decoder %d] Queuing frame for NDI\n", ctx.client_id);

		AVFrame* queued_frame = av_frame_alloc();
		if (av_frame_ref(queued_frame, ctx.rgb_frame) < 0) {
			fprintf(stderr, "Failed to copy frame for queue\n");
			av_frame_free(&queued_frame);
			break;
		}

		{
			std::lock_guard<std::mutex> qlock(ctx.queue_mutex);
			ctx.frame_queue.push(queued_frame);
			printf("[Decoder %d] Queue size: %zu\n", ctx.client_id, ctx.frame_queue.size());
		}
		ctx.queue_cv.notify_one();

		/*std::lock_guard<std::mutex> rgb_lock(ctx.rgb_mutex);
		ctx.new_rgb_ready = true;
		ctx.rgb_cv.notify_one();*/

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
		params_["send_stream"] = "Stream0"; // default
	}

	ZCamStreamBuilder& index(const std::string& index) {
		params_["send_stream"] = index;
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
		CURL* curl = curl_easy_init();
		if (!curl) return false;

		std::string url = "http://" + ip_ + "/ctrl/set?";
		bool first = true;

		for (const auto& pair : params_) {
			if (!first) url += "&";
			char* key = curl_easy_escape(curl, pair.first.c_str(), 0);
			char* value = curl_easy_escape(curl, pair.second.c_str(), 0);
			url += std::string(key) + "=" + value;
			curl_free(key);
			curl_free(value);
			first = false;
		}

		std::string response_data;
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
		curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 500L);
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);

		CURLcode res = curl_easy_perform(curl);
		curl_easy_cleanup(curl);

		if (res != CURLE_OK) {
			fprintf(stderr, "CURL error: %s\n", curl_easy_strerror(res));
			return false;
		}

		try {
			auto j = json::parse(response_data);
			return j.contains("code") && j["code"] == 0;
		}
		catch (const std::exception& e) {
			fprintf(stderr, "JSON parse error: %s\n", e.what());
			return false;
		}
	}

private:
	std::string ip_;
	std::map<std::string, std::string> params_;

	static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
		((std::string*)userp)->append((char*)contents, size * nmemb);
		return size * nmemb;
	}
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
		//{ "192.168.11.108", DecoderType::HW_CUDA, HWCodecType::HEVC_CUVID, "Khel_NDI_1" },
		{ "192.168.11.149", DecoderType::SOFTWARE, HWCodecType::HEVC_CUVID, "Khel_NDI_2" }
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
		ctx->ndi_thread = std::thread(ndi_sender_thread, ctx.get());


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
		ctx->running = false;
		ctx->queue_cv.notify_all();
		if (ctx->ndi_thread.joinable()) {
			ctx->ndi_thread.join();
		}

		if (ctx->ndi_sender) {
			NDIlib_send_destroy(ctx->ndi_sender);
		}
		if (ctx->codec_ctx) {
			avcodec_free_context(&ctx->codec_ctx);
		}
		if (ctx->hw_device_ctx) {
			av_buffer_unref(&ctx->hw_device_ctx);
		}
		if (ctx->sws_ctx) {
			sws_freeContext(ctx->sws_ctx);
		}
		if (ctx->rgb_frame) {
			av_frame_free(&ctx->rgb_frame);
		}
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
		bool success = builder.index("Stream0")
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

