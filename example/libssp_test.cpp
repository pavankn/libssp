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
#include <nlohmann/json.hpp>
#include <curl/curl.h>
#include <queue>
#include "nv12_to_bgra.cuh"

using namespace std::placeholders;
using json = nlohmann::json;

#ifdef _DEBUG
#pragma comment (lib, "libsspd.lib")
#else
#pragma comment (lib, "libssp.lib")
#endif
#define MAX_QUEUE_SIZE 10


extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libavcodec/bsf.h>
#include <libavutil/error.h> // For av_err2str
}

// fix temporary array error in c++1x
#ifdef av_err2str
#undef av_err2str
av_always_inline char* av_err2str(int errnum)
{
	// static char str[AV_ERROR_MAX_STRING_SIZE];
	// thread_local may be better than static in multi-thread circumstance
	thread_local char str[AV_ERROR_MAX_STRING_SIZE];
	memset(str, 0, sizeof(str));
	return av_make_error_string(str, AV_ERROR_MAX_STRING_SIZE, errnum);
}
#endif


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

	std::thread ndi_thread;

	// Async decoding additions
	std::queue<AVPacket*> packet_queue;
	std::mutex packet_queue_mutex;
	std::condition_variable packet_queue_cv;
	std::thread decode_thread;

	bool running = true;

	std::queue<AVFrame*> frame_queue;
	std::mutex queue_mutex;
	std::condition_variable queue_cv;

	bool seen_keyframe = false;  // Track if we've seen a keyframe

};


class ZCamStreamBuilder {
public:
	ZCamStreamBuilder(const std::string& ip) : ip_(ip) {
		params_["send_stream"] = "Stream0";
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
	ZCamStreamBuilder& movfmt(const std::string& movfmt) { 
		params_["k=movfmt"] = movfmt; 
		return *this; 
	}
	ZCamStreamBuilder& movvfr(const std::string& movvfr) { 
		params_["k=movvfr"] = movvfr; 
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

int DetectZCam() {
	std::vector<std::string> found;
	std::vector<std::thread> threads;
	std::string base = "192.168.11.";
	for (int i = 1; i <= 254; i += 32)
		threads.emplace_back(scan_range, base, i, ((i + 31) < 254 ? (i + 31) : 254), std::ref(found));
	for (auto& t : threads) t.join();
	std::cout << "Z CAMs Found: " << found.size() << std::endl;
	for (int i = 0; i < found.size(); ++i) {
		ZCamStreamBuilder builder(found[i]);
		bool success = builder.index("Stream0")
			.fps(30)
			.bitrate(10000000)
			.apply();
		if (success) {
			std::cout << "Stream settings applied successfully for: " << found[i] << std::endl;
		}
		else {
			std::cout << "Failed to apply stream settings for: " << found[i] << std::endl;
			return -1;
		}
	}
	return 0;
}


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
		}else if (ctx.hwCodecType == HWCodecType::HEVC_CUVID) {
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

	printf("before avcodec_open2");

	int ret = avcodec_open2(ctx.codec_ctx, ctx.codec, nullptr);
	if (ret < 0) {
		printf("Client[%d] Could not open codec: %s\n", ctx.client_id, av_err2str(ret));
		return false;
	}

	printf("Client[%d] decoder (%s) initialized.\n", ctx.client_id,
		(ctx.type == DecoderType::HW_CUDA ? "HW_CUDA" : "SOFTWARE"));
	return true;
}

void handle_h264_data(ClientContext& ctx, struct imf::SspH264Data* h264) {
	if (!h264 || !h264->data || h264->len == 0) {
		printf("[Decoder %d] Invalid H264 data received\n", ctx.client_id);
		return;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		printf("[Decoder %d] Failed to allocate AVPacket\n", ctx.client_id);
		return;
	}

	pkt->dts = h264->pts;
	pkt->pts = h264->pts;
	pkt->flags = (h264->type == 0) ? AV_PKT_FLAG_KEY : 0; // 0 for I-frame, non-zero for P-frame

	pkt->data = (uint8_t*)av_memdup(h264->data, h264->len);
	pkt->size = (int)h264->len;	

	std::lock_guard<std::mutex> lock(ctx.packet_queue_mutex);
	ctx.packet_queue.push(pkt);

	ctx.packet_queue_cv.notify_one();
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
	NDIlib_send_send_video_async_v2(ctx->ndi_sender, &ndi_frame);
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
			lock.unlock();
			printf("[NDI %d] Sending frame to NDI...\n", ctx->client_id);
			auto t1 = std::chrono::steady_clock::now();

			send_to_ndi(ctx, frame);
			auto t2 = std::chrono::steady_clock::now();
			printf("[NDI %d] Send time: %lld ms\n", ctx->client_id,
				std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count());
			av_frame_free(&frame);
			lock.lock();
		}
	}
}

void decode_thread_func(ClientContext* ctx)
{
	while (ctx->running) {
		std::unique_lock<std::mutex> lock(ctx->packet_queue_mutex);
		ctx->packet_queue_cv.wait(lock, [&] {
			return !ctx->packet_queue.empty() || !ctx->running;
			});
		if (!ctx->running && ctx->packet_queue.empty()) {
			lock.unlock();
			break;
		}
		AVPacket* pkt = ctx->packet_queue.front();
		ctx->packet_queue.pop();
		lock.unlock();

		std::lock_guard<std::mutex> decoder_lock(ctx->decoder_mutex);
		if (!ctx->codec_ctx) {
			av_packet_free(&pkt);
			continue;
		}
		int ret = avcodec_send_packet(ctx->codec_ctx, pkt);
		if (ret < 0) {
			printf("[Decoder %d] Failed to send packet : %s\n", ctx->client_id, av_err2str(ret));
			av_packet_free(&pkt);
			continue;
		}		

		AVFrame* frame = av_frame_alloc();
		AVFrame* hw_frame = nullptr;

		while (true) {

			auto t1 = std::chrono::steady_clock::now();

			int ret = avcodec_receive_frame(ctx->codec_ctx, frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
				break;
			if (ret < 0) break;

			AVFrame* use_frame = frame;
			printf("Decoded frame size %dx%d\n", frame->width, frame->height);

			// If hardware decode, transfer to system memory
			if (ctx->type == DecoderType::HW_CUDA) {
				if (!hw_frame) {
					hw_frame = av_frame_alloc();
				}
				ret = av_hwframe_transfer_data(hw_frame, frame, 0);
				if (ret < 0) {
					printf("Failed to transfer HW frame to CPU: %s\n", av_err2str(ret));
					break;
				}
				use_frame = hw_frame;
			}

			// Prepare or reallocate BGRA frame if resolution changed
			if (!ctx->rgb_frame || ctx->last_width != use_frame->width || ctx->last_height != use_frame->height) {
				if (ctx->rgb_frame) av_frame_free(&ctx->rgb_frame);

				ctx->rgb_frame = av_frame_alloc();
				ctx->rgb_frame->format = AV_PIX_FMT_BGRA;
				ctx->rgb_frame->width = use_frame->width;
				ctx->rgb_frame->height = use_frame->height;

				if (av_frame_get_buffer(ctx->rgb_frame, 32) < 0) {
					fprintf(stderr, "[Decoder %d] Failed to allocate rgb_frame\n", ctx->client_id);
					break;
				}

				ctx->last_width = use_frame->width;
				ctx->last_height = use_frame->height;
			}

			// === GPU-Accelerated NV12 → BGRA ===
			bool ok = ConvertAVFrameToBGRA(
				use_frame->data[0], use_frame->linesize[0],  // Y plane + pitch
				use_frame->data[1], use_frame->linesize[1],  // UV plane + pitch (though pitch not used internally)
				use_frame->width, use_frame->height,
				ctx->rgb_frame->data[0], ctx->rgb_frame->linesize[0]  // output BGRA CPU buffer
			);

			if (!ok) {
				fprintf(stderr, "[Decoder %d] GPU color conversion failed\n", ctx->client_id);
				break;
			}
			

			// === Queue the output frame as before ===
			AVFrame* queued_frame = av_frame_alloc();
			if (av_frame_ref(queued_frame, ctx->rgb_frame) < 0) {
				fprintf(stderr, "[Decoder %d] Failed to copy frame for queue\n", ctx->client_id);
				av_frame_free(&queued_frame);
				break;
			}

			send_to_ndi(ctx, ctx->rgb_frame);


			/*{
				std::lock_guard<std::mutex> qlock(ctx->queue_mutex);
				ctx->frame_queue.push(queued_frame);
				printf("[Decoder %d] Queue size: %zu\n", ctx->client_id, ctx->frame_queue.size());
			}
			ctx->queue_cv.notify_one();*/

			auto t2 = std::chrono::steady_clock::now();
			std::cout << "Decode time: " << std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1).count() << "ms\n";

			av_frame_unref(frame);
			if (hw_frame) av_frame_unref(hw_frame);			
		}

		if (hw_frame) av_frame_free(&hw_frame);
		av_frame_free(&frame);
		av_packet_free(&pkt);
	}
}



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

static void setup(imf::Loop* loop)
{
	// Define client inputs (IP + decoder type)
	struct {
		std::string ip;
		DecoderType decoder_type;
		HWCodecType hwCodecType;
		std::string ndi_name;
	} client_inputs[] = {
		{ "192.168.11.108", DecoderType::HW_CUDA, HWCodecType::HEVC_CUVID, "Khel_NDI_1" },
		{ "192.168.11.149", DecoderType::HW_CUDA, HWCodecType::HEVC_CUVID, "Khel_NDI_2" },
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
		ctx->decode_thread = std::thread(decode_thread_func, ctx.get());

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

int main(int argc, char ** argv)
{
	signal(SIGINT, handle_sigint);

	int ret = DetectZCam();
	if (ret < 0) {
		std::cerr << "Failed to Apply Settings" << std::endl;
		return ret;
	}

	std::unique_ptr<imf::ThreadLoop> threadLooper(new imf::ThreadLoop(std::bind(setup, _1)));
	threadLooper->start();


	while (running) {
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	cleanup_clients();
	threadLooper->stop();
	return 0;
}

