#include "sender_output.hpp"

#include <obs-module.h>
#include <obs-encoder.h>
#include <util/platform.h>
#include <callback/proc.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <turbojpeg.h>
#include "protocol.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <cmath>
#include <climits>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {
using obs_duocast::protocol::PacketHeader;
using obs_duocast::protocol::StreamKind;
std::atomic<bool> g_senderOutputConnected{false};
std::atomic<std::uint64_t> g_videoPackets{0};
std::atomic<std::uint64_t> g_liveAudioPackets{0};
std::atomic<std::uint64_t> g_vodAudioPackets{0};
std::atomic<std::uint64_t> g_verticalVideoPackets{0};
std::atomic<std::uint64_t> g_videoBytes{0};
std::atomic<std::uint64_t> g_liveAudioBytes{0};
std::atomic<std::uint64_t> g_vodAudioBytes{0};
std::atomic<std::uint64_t> g_verticalVideoBytes{0};
std::mutex g_remoteStatsMutex;
SenderRemoteStats g_remoteStats{};

struct QueuePacket {
	std::vector<std::uint8_t> bytes;
	StreamKind kind = StreamKind::Heartbeat;
};
struct MjpegEncoder {
	tjhandle handle = nullptr;
	unsigned char *jpeg_buffer = nullptr;
	size_t jpeg_capacity = 0;
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t fps_num = 30;
	std::uint32_t fps_den = 1;
	std::uint64_t fps_accumulator = 0;
};
struct SenderState;
struct ObsMjpegEncoder {
	MjpegEncoder encoder;
	SenderState *sender = nullptr;
	StreamKind kind = StreamKind::Video;
	std::atomic<bool> first_frame_logged{false};
};
struct SenderState {
	obs_output_t *output = nullptr;
	obs_output_t *vertical_output = nullptr;
	obs_encoder_t *video_encoder = nullptr;
	obs_encoder_t *vertical_encoder = nullptr;
	// The Aitum Vertical video is owned by its canvas. Keep the canvas alive for
	// as long as the encoder is connected to that video output.
	obs_canvas_t *vertical_canvas = nullptr;
	obs_encoder_t *live = nullptr;
	obs_encoder_t *vod = nullptr;
	video_t *vertical_canvas_video = nullptr;
	bool send_vertical = false;
	bool output_resolution = false;
	std::uint32_t video_width = 0, video_height = 0, fps_num = 30, fps_den = 1;
	std::uint32_t vertical_width = 0, vertical_height = 0, vertical_fps_num = 30, vertical_fps_den = 1;
	video_colorspace video_cs = VIDEO_CS_709;
	video_range_type video_color_range = VIDEO_RANGE_PARTIAL;
	video_colorspace vertical_cs = VIDEO_CS_709;
	video_range_type vertical_color_range = VIDEO_RANGE_PARTIAL;
	std::atomic<SOCKET> socket{INVALID_SOCKET};
	std::mutex socket_mutex;
	std::string destination;
	int port = 17777;
	int video_quality = 85;
	int video_encoder_mode = 0; // 0 = TurboJPEG MJPEG, 1 = NVENC H.264, 2 = NVENC HEVC
	int nvenc_bitrate_mbps = 80;
	int max_bitrate_mbps = 100;
	int max_fps = 0;
	std::mutex mutex;
	std::condition_variable ready;
	// Audio shares the TCP transport with large MJPEG frames, but should never
	// wait behind a backlog of video frames. Keep FIFO order within each class
	// and let the writer drain pending audio before taking another video frame.
	std::deque<QueuePacket> audio_queue;
	std::deque<QueuePacket> video_queue;
	std::size_t queue_bytes = 0;
	std::thread writer;
	std::atomic<bool> stopping{false};
	bool active = false;
	std::atomic<bool> reconnect_requested{false};
	std::atomic<bool> connected{false};
	std::atomic<bool> vertical_packet_queued_logged{false};
	std::atomic<bool> vertical_packet_sent_logged{false};
	std::atomic<bool> vertical_encode_error_logged{false};
	bool capture_started = false;
	bool vertical_capture_started = false;
	std::atomic<std::uint32_t> sequence{0};
};

void clear_media_queues(SenderState *s)
{
	s->audio_queue.clear();
	s->video_queue.clear();
	s->queue_bytes = 0;
}

video_t *get_aitum_vertical_video()
{
	proc_handler_t *handler = obs_get_proc_handler();
	if (!handler) return nullptr;
	calldata_t params;
	calldata_init(&params);
	calldata_set_int(&params, "width", 0);
	calldata_set_int(&params, "height", 0);
	const bool called = proc_handler_call(handler, "aitum_vertical_get_video", &params);
	auto *video = called ? static_cast<video_t *>(calldata_ptr(&params, "video")) : nullptr;
	calldata_free(&params);
	return video;
}

void store16(std::vector<std::uint8_t> &b, std::uint16_t v)
{
	b.push_back(static_cast<std::uint8_t>(v >> 8)); b.push_back(static_cast<std::uint8_t>(v));
}
void store32(std::vector<std::uint8_t> &b, std::uint32_t v)
{
	for (int i = 3; i >= 0; --i) b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}

bool socket_send_all(SOCKET s, const std::uint8_t *data, std::size_t size)
{
	while (size) {
		const int part = static_cast<int>(std::min<std::size_t>(size, 1024 * 1024));
		const int sent = send(s, reinterpret_cast<const char *>(data), part, 0);
		if (sent <= 0) return false;
		data += sent;
		size -= static_cast<std::size_t>(sent);
	}
	return true;
}

bool socket_receive_all(SOCKET s, std::uint8_t *data, std::size_t size)
{
	while (size) {
		const int received = recv(s, reinterpret_cast<char *>(data),
			static_cast<int>(std::min<std::size_t>(size, 1024 * 1024)), 0);
		if (received <= 0) return false;
		data += received;
		size -= static_cast<std::size_t>(received);
	}
	return true;
}

void receive_receiver_stats(SenderState *state, SOCKET socket)
{
	while (!state->stopping.load()) {
		std::uint8_t wire[obs_duocast::protocol::kHeaderSize];
		if (!socket_receive_all(socket, wire, sizeof(wire))) return;
		PacketHeader header;
		if (!obs_duocast::protocol::decode_header(wire, header) ||
		    header.kind != StreamKind::ReceiverStats ||
		    header.payload_size != obs_duocast::protocol::kReceiverStatsPayloadSize)
			return;
		std::uint8_t payload[obs_duocast::protocol::kReceiverStatsPayloadSize];
		if (!socket_receive_all(socket, payload, sizeof(payload))) return;
		SenderRemoteStats snapshot;
		if (!obs_duocast::protocol::decode_receiver_stats(payload, sizeof(payload), snapshot.stats)) return;
		if (snapshot.stats.flags & (1U << 3)) {
			bool request_accepted = false;
			{
				std::lock_guard<std::mutex> lock(state->mutex);
				if (state->active && !state->stopping && !state->reconnect_requested) {
					state->reconnect_requested = true;
					request_accepted = true;
				}
			}
			if (request_accepted) {
				blog(LOG_WARNING, "DuoCast: receiver requested automatic feed recovery; reconnecting transport");
				state->ready.notify_one();
			}
		}
		snapshot.received_at_ns = os_gettime_ns();
		{
			std::lock_guard<std::mutex> lock(g_remoteStatsMutex);
			g_remoteStats = snapshot;
		}
	}
}

bool socket_send_all_capped(SenderState *state, SOCKET socket, const std::uint8_t *data,
			    std::size_t size, long double &tokens, std::chrono::steady_clock::time_point &last)
{
	// Auto mode (0 Mbps) sends as fast as the TCP socket accepts data.
	if (state->max_bitrate_mbps <= 0) {
		while (size) {
			if (state->stopping || state->reconnect_requested) return false;
			const int part = static_cast<int>(std::min<std::size_t>(size, 64 * 1024));
			const int sent = send(socket, reinterpret_cast<const char *>(data), part, 0);
			if (sent <= 0) return false;
			data += sent;
			size -= static_cast<std::size_t>(sent);
		}
		return true;
	}

	const long double bytes_per_second = static_cast<long double>(state->max_bitrate_mbps) * 125000.0L;
	const long double burst_bytes = std::clamp(bytes_per_second * 0.05L, 16384.0L, 512 * 1024.0L);
	while (size) {
		if (state->stopping || state->reconnect_requested) return false;
		const auto now = std::chrono::steady_clock::now();
		tokens = std::min(burst_bytes, tokens +
			std::chrono::duration<long double>(now - last).count() * bytes_per_second);
		last = now;
		if (tokens < 1.0L) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
		const auto allowed = static_cast<std::size_t>(tokens);
		const int part = static_cast<int>(std::min<std::size_t>({size, allowed, 64 * 1024}));
		const int sent = send(socket, reinterpret_cast<const char *>(data), part, 0);
		if (sent <= 0) return false;
		data += sent;
		size -= static_cast<std::size_t>(sent);
		tokens -= sent;
	}
	return true;
}

bool connect_with_timeout(SOCKET s, const sockaddr *address, int length, long timeout_ms)
{
	u_long nonblocking = 1;
	if (ioctlsocket(s, FIONBIO, &nonblocking) == SOCKET_ERROR) return false;
	const int result = connect(s, address, length);
	if (result == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK && WSAGetLastError() != WSAEINPROGRESS) {
		nonblocking = 0; ioctlsocket(s, FIONBIO, &nonblocking); return false;
	}
	fd_set writable; FD_ZERO(&writable); FD_SET(s, &writable);
	timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	const int selected = select(0, nullptr, &writable, nullptr, &timeout);
	int error = 0; int error_size = sizeof(error);
	const bool connected = selected > 0 && getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &error_size) == 0 && error == 0;
	nonblocking = 0;
	ioctlsocket(s, FIONBIO, &nonblocking);
	return connected;
}

bool send_packet(SenderState *s, SOCKET socket, StreamKind kind, std::uint64_t pts, bool keyframe,
		 const std::uint8_t *payload, std::size_t size)
{
	if (size > obs_duocast::protocol::kMaxPayload) return false;
	PacketHeader h;
	h.kind = kind;
	h.flags = keyframe ? 1 : 0;
	h.sequence = s->sequence.fetch_add(1);
	h.pts_ns = pts;
	h.payload_size = static_cast<std::uint32_t>(size);
	std::uint8_t header[obs_duocast::protocol::kHeaderSize];
	obs_duocast::protocol::encode_header(h, header);
	return socket_send_all(socket, header, sizeof(header)) && (!size || socket_send_all(socket, payload, size));
}

bool enqueue_media(SenderState *s, StreamKind kind, std::uint64_t pts, std::uint64_t dts,
		   bool keyframe, const std::uint8_t *payload, std::size_t size)
{
	if (size > obs_duocast::protocol::kMaxPayload) return false;
	PacketHeader h;
	h.kind = kind;
	h.flags = keyframe ? 1 : 0;
	h.sequence = s->sequence.fetch_add(1);
	h.pts_ns = pts;
	h.dts_ns = dts;
	h.payload_size = static_cast<std::uint32_t>(size);
	std::uint8_t encoded[obs_duocast::protocol::kHeaderSize];
	obs_duocast::protocol::encode_header(h, encoded);
	QueuePacket item;
	item.kind = kind;
	item.bytes.reserve(sizeof(encoded) + size);
	item.bytes.insert(item.bytes.end(), encoded, encoded + sizeof(encoded));
	if (size) item.bytes.insert(item.bytes.end(), payload, payload + size);
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		if (!s->active || s->stopping || s->reconnect_requested) return false;
		auto is_video = [](StreamKind packet_kind) {
			return packet_kind == StreamKind::Video || packet_kind == StreamKind::VerticalVideo;
		};
		if (!s->connected.load()) {
			return false;
		}
		const std::size_t kMaxQueuedBytes = s->max_bitrate_mbps <= 0
			? 32 * 1024 * 1024
			: std::clamp<std::size_t>(static_cast<std::size_t>(s->max_bitrate_mbps) * 125000ULL,
					  512 * 1024, 32 * 1024 * 1024);
		while (s->audio_queue.size() + s->video_queue.size() >= 256 ||
		       s->queue_bytes + item.bytes.size() > kMaxQueuedBytes) {
			if (!s->video_queue.empty()) {
				s->queue_bytes -= s->video_queue.front().bytes.size();
				s->video_queue.pop_front();
			} else if (is_video(kind)) {
				return false;
			}
			else {
				s->reconnect_requested = true;
				s->connected.store(false);
				clear_media_queues(s);
				s->ready.notify_one();
				return false;
			}
		}
		s->queue_bytes += item.bytes.size();
		if (is_video(kind)) s->video_queue.push_back(std::move(item));
		else s->audio_queue.push_back(std::move(item));
	}
	s->ready.notify_one();
	return true;
}

void store_float(std::vector<std::uint8_t> &b, float value)
{
	std::uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); store32(b, bits);
}

std::vector<std::uint8_t> make_video_config(std::uint32_t width, std::uint32_t height,
						   std::uint32_t fps_num, std::uint32_t fps_den,
						   video_colorspace colorspace, video_range_type range,
						   std::uint8_t codec_type = 4,
						   const std::uint8_t *extra_data = nullptr, std::size_t extra_size = 0)
{
	std::vector<std::uint8_t> p;
	if (!width || !height || !fps_num || !fps_den || extra_size > 65535 || (extra_size && !extra_data)) return {};
	store32(p, width); store32(p, height);
	store32(p, fps_num); store32(p, fps_den);
	p.push_back(codec_type);
	store16(p, static_cast<std::uint16_t>(extra_size));
	if (extra_size) p.insert(p.end(), extra_data, extra_data + extra_size);
	if (codec_type != 4) return p;
	p.push_back(static_cast<std::uint8_t>(range));
	float matrix[16]{}, range_min[3]{}, range_max[3]{};
	video_format_get_parameters(colorspace, range, matrix, range_min, range_max);
	for (float value : matrix) store_float(p, value);
	for (float value : range_min) store_float(p, value);
	for (float value : range_max) store_float(p, value);
	return p;
}

void destroy_mjpeg_encoder(MjpegEncoder &encoder)
{
	if (encoder.handle) tj3Destroy(encoder.handle);
	if (encoder.jpeg_buffer) tj3Free(encoder.jpeg_buffer);
	encoder = {};
}

bool initialize_jpeg_encoder(MjpegEncoder &encoder, std::uint32_t width, std::uint32_t height,
			     std::uint32_t fps_num, std::uint32_t fps_den, int quality,
			     video_colorspace colorspace, video_range_type input_range)
{
	(void)colorspace;
	(void)input_range;
	if (!width || !height || width > INT_MAX || height > INT_MAX) return false;
	encoder.handle = tj3Init(TJINIT_COMPRESS);
	if (!encoder.handle) return false;
	if (tj3Set(encoder.handle, TJPARAM_QUALITY, std::clamp(quality, 1, 100)) < 0 ||
	    tj3Set(encoder.handle, TJPARAM_SUBSAMP, TJSAMP_420) < 0 ||
	    tj3Set(encoder.handle, TJPARAM_NOREALLOC, 1) < 0) {
		destroy_mjpeg_encoder(encoder);
		return false;
	}
	encoder.jpeg_capacity = tj3JPEGBufSize(static_cast<int>(width), static_cast<int>(height), TJSAMP_420);
	if (!encoder.jpeg_capacity || !(encoder.jpeg_buffer = static_cast<unsigned char *>(tj3Alloc(encoder.jpeg_capacity)))) {
		destroy_mjpeg_encoder(encoder);
		return false;
	}
	encoder.width = width;
	encoder.height = height;
	encoder.fps_num = fps_num;
	encoder.fps_den = fps_den;
	encoder.fps_accumulator = 0;
	return true;
}

bool compress_yuv420(MjpegEncoder &encoder, const std::uint8_t *const planes[3], const int strides[3],
		    std::size_t &jpeg_size, int quality)
{
	if (!encoder.handle || !encoder.jpeg_buffer || !planes[0] || !planes[1] || !planes[2]) return false;
	if (tj3Set(encoder.handle, TJPARAM_QUALITY, std::clamp(quality, 1, 100)) < 0) return false;
	const unsigned char *source_planes[] = {planes[0], planes[1], planes[2]};
	const int source_strides[] = {strides[0], strides[1], strides[2]};
	jpeg_size = encoder.jpeg_capacity;
	return tj3CompressFromYUVPlanes8(encoder.handle, source_planes, static_cast<int>(encoder.width),
					 source_strides, static_cast<int>(encoder.height),
					 &encoder.jpeg_buffer, &jpeg_size) == 0;
}

bool mjpeg_should_encode_frame(MjpegEncoder &encoder, int max_fps)
{
	if (max_fps <= 0 || static_cast<std::uint64_t>(max_fps) * encoder.fps_den >= encoder.fps_num)
		return true;
	encoder.fps_accumulator += static_cast<std::uint64_t>(max_fps) * encoder.fps_den;
	if (encoder.fps_accumulator < encoder.fps_num) return false;
	encoder.fps_accumulator -= encoder.fps_num;
	return true;
}

bool initialize_vertical_canvas(SenderState *s)
{
	video_t *aitum_video = get_aitum_vertical_video();
	if (!aitum_video) {
		blog(LOG_ERROR, "DuoCast: vertical feed requested, but Aitum Vertical canvas is unavailable");
		return false;
	}
	struct FindCanvas { video_t *video; obs_canvas_t *canvas; } find{aitum_video, nullptr};
	obs_enum_canvases([](void *opaque, obs_canvas_t *canvas) {
		auto *find = static_cast<FindCanvas *>(opaque);
		if (obs_canvas_get_video(canvas) != find->video) return true;
		find->canvas = obs_canvas_get_ref(canvas);
		return false;
	}, &find);
	s->vertical_canvas = find.canvas;
	if (!s->vertical_canvas) {
		blog(LOG_ERROR, "DuoCast: could not retain the Aitum Vertical canvas for transport");
		return false;
	}
	s->vertical_canvas_video = obs_canvas_get_video(s->vertical_canvas);
	if (!s->vertical_canvas_video) {
		blog(LOG_ERROR, "DuoCast: retained Aitum Vertical canvas has no video output");
		obs_canvas_release(s->vertical_canvas);
		s->vertical_canvas = nullptr;
		return false;
	}
	obs_video_info info{};
	if (!obs_canvas_get_video_info(s->vertical_canvas, &info) ||
	    !(s->output_resolution ? info.output_width : info.base_width) ||
	    !(s->output_resolution ? info.output_height : info.base_height) || !info.fps_num || !info.fps_den) {
		blog(LOG_ERROR, "DuoCast: Aitum Vertical canvas has invalid video settings");
		obs_canvas_release(s->vertical_canvas);
		s->vertical_canvas = nullptr;
		s->vertical_canvas_video = nullptr;
		return false;
	}
	s->vertical_width = s->output_resolution ? info.output_width : info.base_width;
	s->vertical_height = s->output_resolution ? info.output_height : info.base_height;
	s->vertical_cs = info.colorspace;
	s->vertical_color_range = resolve_video_range(VIDEO_FORMAT_I420, info.range);
	s->vertical_fps_num = info.fps_num;
	s->vertical_fps_den = info.fps_den;
	blog(LOG_INFO, "DuoCast: Aitum Vertical feed ready at %ux%u (%u/%u fps)", s->vertical_width,
	     s->vertical_height, s->vertical_fps_num, s->vertical_fps_den);
	return true;
}

const char *vertical_encoder_name(void *) { return "DuoCast Vertical TurboJPEG"; }

void *mjpeg_encoder_create(obs_data_t *settings, obs_encoder_t *)
{
	auto *state = new ObsMjpegEncoder();
	const auto width = static_cast<std::uint32_t>(obs_data_get_int(settings, "width"));
	const auto height = static_cast<std::uint32_t>(obs_data_get_int(settings, "height"));
	const auto fps_num = static_cast<std::uint32_t>(obs_data_get_int(settings, "fps_num"));
	const auto fps_den = static_cast<std::uint32_t>(obs_data_get_int(settings, "fps_den"));
	const auto colorspace = static_cast<video_colorspace>(obs_data_get_int(settings, "colorspace"));
	const auto range = static_cast<video_range_type>(obs_data_get_int(settings, "range"));
	const int quality = std::clamp(static_cast<int>(obs_data_get_int(settings, "quality")), 1, 100);
	state->sender = reinterpret_cast<SenderState *>(static_cast<std::uintptr_t>(
		static_cast<std::uint64_t>(obs_data_get_int(settings, "sender_pointer"))));
	state->kind = obs_data_get_bool(settings, "vertical") ? StreamKind::VerticalVideo : StreamKind::Video;
	if (!initialize_jpeg_encoder(state->encoder, width, height, fps_num, fps_den, quality, colorspace, range)) {
		delete state;
		return nullptr;
	}
	return state;
}

void mjpeg_encoder_destroy(void *opaque)
{
	auto *state = static_cast<ObsMjpegEncoder *>(opaque);
	if (!state) return;
	destroy_mjpeg_encoder(state->encoder);
	delete state;
}

void mjpeg_encoder_video_info(void *, video_scale_info *info)
{
	info->format = VIDEO_FORMAT_I420;
}

bool mjpeg_encoder_encode(void *opaque, encoder_frame *input, encoder_packet *output, bool *received)
{
	auto *state = static_cast<ObsMjpegEncoder *>(opaque);
	if (!state || !input || !output || !received) return false;
	*received = false;
	if (!state->first_frame_logged.exchange(true))
		blog(LOG_INFO, "DuoCast: OBS delivered the first %s frame to TurboJPEG",
		     state->kind == StreamKind::VerticalVideo ? "Aitum Vertical" : "landscape");
	if (!state->sender || !state->sender->connected.load()) return true;
	if (!mjpeg_should_encode_frame(state->encoder, state->sender->max_fps)) return true;
	if (!input->data[0] || !input->data[1] || !input->data[2]) return false;
	const std::uint8_t *planes[] = {input->data[0], input->data[1], input->data[2]};
	const int strides[] = {static_cast<int>(input->linesize[0]), static_cast<int>(input->linesize[1]),
				static_cast<int>(input->linesize[2])};
	std::size_t jpeg_size = 0;
	const int quality = state->sender ? state->sender->video_quality : 85;
	if (!compress_yuv420(state->encoder, planes, strides, jpeg_size, quality)) return false;
	output->data = state->encoder.jpeg_buffer;
	output->size = jpeg_size;
	output->pts = input->pts;
	output->dts = input->pts;
	output->type = OBS_ENCODER_VIDEO;
	output->keyframe = true;
	*received = true;
	return true;
}

void *vertical_output_create(obs_data_t *settings, obs_output_t *)
{
	return reinterpret_cast<void *>(static_cast<std::uintptr_t>(obs_data_get_int(settings, "sender_pointer")));
}

bool vertical_output_start(void *opaque)
{
	return opaque != nullptr;
}

void vertical_output_destroy(void *)
{
}

void vertical_output_stop(void *, std::uint64_t)
{
}

void vertical_output_packet(void *opaque, encoder_packet *packet)
{
	auto *s = static_cast<SenderState *>(opaque);
	if (!s || !packet || packet->type != OBS_ENCODER_VIDEO || !packet->data || !packet->size ||
	    packet->sys_dts_usec <= 0)
		return;
	const auto dts_ns = static_cast<std::uint64_t>(packet->sys_dts_usec) * 1000ULL;
	const bool queued = enqueue_media(s, StreamKind::VerticalVideo, dts_ns, dts_ns, packet->keyframe,
					  packet->data, packet->size);
	if (queued && !s->vertical_packet_queued_logged.exchange(true))
		blog(LOG_INFO, "DuoCast: queued first Aitum Vertical video frame (%zu bytes)", packet->size);
}

std::uint32_t fps_divisor_for_cap(std::uint32_t fps_num, std::uint32_t fps_den, int max_fps)
{
	if (!fps_num || !fps_den || max_fps <= 0 ||
	    static_cast<std::uint64_t>(max_fps) * fps_den >= fps_num)
		return 1;
	const std::uint64_t denominator = static_cast<std::uint64_t>(max_fps) * fps_den;
	return static_cast<std::uint32_t>((fps_num + denominator - 1) / denominator);
}

obs_encoder_t *create_video_encoder(SenderState *s, bool vertical)
{
	const std::uint32_t width = vertical ? s->vertical_width : s->video_width;
	const std::uint32_t height = vertical ? s->vertical_height : s->video_height;
	std::uint32_t fps_num = vertical ? s->vertical_fps_num : s->fps_num;
	std::uint32_t fps_den = vertical ? s->vertical_fps_den : s->fps_den;
	video_t *video = vertical ? s->vertical_canvas_video : obs_get_video();
	if (!video || !width || !height) return nullptr;
	const bool use_nvenc = s->video_encoder_mode > 0;
	const std::uint32_t frame_rate_divisor = use_nvenc
		? fps_divisor_for_cap(fps_num, fps_den, s->max_fps) : 1;
	if (use_nvenc && frame_rate_divisor > 1) {
		fps_den *= frame_rate_divisor;
	} else if (!use_nvenc && s->max_fps > 0 && static_cast<std::uint64_t>(s->max_fps) * fps_den < fps_num) {
		fps_num = static_cast<std::uint32_t>(s->max_fps);
		fps_den = 1;
	}
	const char *encoder_id = !use_nvenc ? "duocast_turbojpeg" :
		(s->video_encoder_mode == 2 ? "obs_nvenc_hevc_tex" : "obs_nvenc_h264_tex");
	obs_data_t *settings = use_nvenc ? obs_encoder_defaults(encoder_id) : obs_data_create();
	if (!settings) return nullptr;
	obs_data_set_int(settings, "width", width);
	obs_data_set_int(settings, "height", height);
	obs_data_set_int(settings, "fps_num", fps_num);
	obs_data_set_int(settings, "fps_den", fps_den);
	obs_data_set_int(settings, "colorspace", vertical ? s->vertical_cs : s->video_cs);
	obs_data_set_int(settings, "range", vertical ? s->vertical_color_range : s->video_color_range);
	obs_data_set_int(settings, "quality", s->video_quality);
	obs_data_set_bool(settings, "vertical", vertical);
	obs_data_set_int(settings, "sender_pointer", static_cast<long long>(reinterpret_cast<std::uintptr_t>(s)));
	if (use_nvenc) {
		const int bitrate_kbps = std::clamp(s->nvenc_bitrate_mbps, 1, 1000) * 1000;
		// OBS NVENC expects this exact spelling for its CBR rate-control setup.
		obs_data_set_string(settings, "rate_control", "CBR");
		obs_data_set_int(settings, "bitrate", bitrate_kbps);
		obs_data_set_int(settings, "max_bitrate", bitrate_kbps);
		obs_data_set_int(settings, "keyint_sec", 2);
		obs_data_set_int(settings, "bf", 0);
		obs_data_set_string(settings, "preset", "p1");
		obs_data_set_string(settings, "tune", "ull");
		obs_data_set_string(settings, "multipass", "disabled");
		obs_data_set_bool(settings, "lookahead", false);
		obs_data_set_bool(settings, "adaptive_quantization", false);
		obs_data_set_bool(settings, "repeat_headers", true);
	}
	auto *encoder = obs_video_encoder_create(encoder_id, vertical ? "DuoCast Vertical Video" : "DuoCast Video",
						 settings, nullptr);
	obs_data_release(settings);
	if (!encoder) {
		blog(LOG_ERROR, "DuoCast: could not create %s encoder; check OBS NVENC codec support on this GPU",
		     !use_nvenc ? "TurboJPEG MJPEG" : (s->video_encoder_mode == 2 ? "OBS NVENC HEVC" : "OBS NVENC H.264"));
		return nullptr;
	}
	obs_encoder_set_video(encoder, video);
	obs_encoder_set_scaled_size(encoder, width, height);
	if (use_nvenc) {
		// Preserve NVENC's texture path when output dimensions differ and apply
		// the FPS cap inside libobs instead of doing frame-rate work in DuoCast.
		obs_encoder_set_gpu_scale_type(encoder, OBS_SCALE_BICUBIC);
		if (frame_rate_divisor > 1 && !obs_encoder_set_frame_rate_divisor(encoder, frame_rate_divisor))
			blog(LOG_WARNING, "DuoCast: could not apply NVENC frame-rate divisor %u", frame_rate_divisor);
	}
	return encoder;
}

bool create_vertical_transport(SenderState *s, int quality)
{
	(void)quality;
	s->vertical_encoder = create_video_encoder(s, true);
	if (!s->vertical_encoder) return false;

	obs_data_t *output_settings = obs_data_create();
	obs_data_set_int(output_settings, "sender_pointer",
			 static_cast<long long>(reinterpret_cast<std::uintptr_t>(s)));
	s->vertical_output = obs_output_create("duocast_vertical_transport", "DuoCast Vertical Transport",
						       output_settings, nullptr);
	obs_data_release(output_settings);
	if (!s->vertical_output) return false;
	obs_output_set_video_encoder(s->vertical_output, s->vertical_encoder);
	return true;
}

std::vector<std::uint8_t> make_audio_config(obs_encoder_t *encoder)
{
	std::vector<std::uint8_t> p;
	struct obs_audio_info info{};
	if (!obs_get_audio_info(&info)) return {};
	store32(p, info.samples_per_sec);
	audio_t *audio = obs_encoder_audio(encoder);
	const auto channels = audio ? audio_output_get_channels(audio) : (info.speakers == SPEAKERS_MONO ? 1U : 2U);
	if (channels < 1 || channels > 8) return {};
	p.push_back(static_cast<std::uint8_t>(channels));
	p.push_back(1); // AAC
	std::uint8_t *extra = nullptr; std::size_t extra_size = 0;
	if (!obs_encoder_get_extra_data(encoder, &extra, &extra_size) || extra_size > 65535) return {};
	store16(p, static_cast<std::uint16_t>(extra_size));
	p.insert(p.end(), extra, extra + extra_size);
	return p;
}

void writer_thread(SenderState *s)
{
	int retry_delay_seconds = 1;
	unsigned failed_attempts = 0;
	for (;;) {
		{
			std::lock_guard<std::mutex> lock(s->mutex);
			if (s->stopping) break;
		}

		SOCKET candidate = INVALID_SOCKET;
		addrinfo hints{};
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		addrinfo *addresses = nullptr;
		const std::string port_text = std::to_string(s->port);
		if (getaddrinfo(s->destination.c_str(), port_text.c_str(), &hints, &addresses) == 0) {
			for (auto *address = addresses; address; address = address->ai_next) {
				candidate = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (candidate == INVALID_SOCKET) continue;
				DWORD send_timeout_ms = 3000;
				setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO,
					   reinterpret_cast<const char *>(&send_timeout_ms), sizeof(send_timeout_ms));
				if (connect_with_timeout(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen), 3000))
					break;
				closesocket(candidate);
				candidate = INVALID_SOCKET;
			}
			freeaddrinfo(addresses);
		}

		if (candidate != INVALID_SOCKET) {
			BOOL no_delay = TRUE;
			if (setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
				       reinterpret_cast<const char *>(&no_delay), sizeof(no_delay)) == SOCKET_ERROR)
				blog(LOG_WARNING, "DuoCast: could not disable TCP packet coalescing (socket error %d)", WSAGetLastError());
			{
				std::lock_guard<std::mutex> socketLock(s->socket_mutex);
				s->socket.store(candidate);
				// Publish the active socket before any blocking writes so sender_stop
				// can interrupt a connect that completed at the same time as shutdown.
				if (s->stopping.load()) shutdown(candidate, SD_BOTH);
			}
			long double send_tokens = 0.0L;
			auto send_tokens_last = std::chrono::steady_clock::now();
			auto make_config_for_encoder = [s](obs_encoder_t *encoder, std::uint32_t width, std::uint32_t height,
							 std::uint32_t fps_num, std::uint32_t fps_den,
							 video_colorspace colorspace) {
				if (s->max_fps > 0 && static_cast<std::uint64_t>(s->max_fps) * fps_den < fps_num) {
					if (s->video_encoder_mode > 0) {
						fps_den *= fps_divisor_for_cap(fps_num, fps_den, s->max_fps);
					} else {
						fps_num = static_cast<std::uint32_t>(s->max_fps);
						fps_den = 1;
					}
				}
				if (s->video_encoder_mode > 0) {
					std::uint8_t *extra_data = nullptr;
					std::size_t extra_size = 0;
					if (!encoder || !obs_encoder_get_extra_data(encoder, &extra_data, &extra_size) || !extra_data || !extra_size) {
						blog(LOG_ERROR, "DuoCast: NVENC did not provide %s codec headers; cannot configure receiver",
						     s->video_encoder_mode == 2 ? "HEVC" : "H.264");
						return std::vector<std::uint8_t>{};
					}
					const std::uint8_t codec_type = s->video_encoder_mode == 2 ? 5 : 1;
					return make_video_config(width, height, fps_num, fps_den, colorspace, VIDEO_RANGE_FULL,
								 codec_type, extra_data, extra_size);
				}
				return make_video_config(width, height, fps_num, fps_den, colorspace, VIDEO_RANGE_FULL);
			};
			const auto video_cfg = make_config_for_encoder(s->video_encoder, s->video_width, s->video_height,
					 s->fps_num, s->fps_den, s->video_cs);
			const auto vertical_cfg = s->send_vertical
				? make_config_for_encoder(s->vertical_encoder, s->vertical_width, s->vertical_height,
						 s->vertical_fps_num, s->vertical_fps_den, s->vertical_cs)
				: std::vector<std::uint8_t>{};
			const auto live_cfg = make_audio_config(s->live);
			const auto vod_cfg = make_audio_config(s->vod);
			const bool configured = !video_cfg.empty() && (!s->send_vertical || !vertical_cfg.empty()) && !live_cfg.empty() && !vod_cfg.empty() &&
				!s->stopping.load() &&
				send_packet(s, candidate, StreamKind::VideoConfig, 0, false, video_cfg.data(), video_cfg.size()) &&
				(!s->send_vertical || send_packet(s, candidate, StreamKind::VerticalVideoConfig, 0, false, vertical_cfg.data(), vertical_cfg.size())) &&
				send_packet(s, candidate, StreamKind::LiveAudioConfig, 0, false, live_cfg.data(), live_cfg.size()) &&
				send_packet(s, candidate, StreamKind::VodAudioConfig, 0, false, vod_cfg.data(), vod_cfg.size());
			if (configured) {
				{
					std::lock_guard<std::mutex> lock(s->mutex);
					if (!s->stopping) {
						s->reconnect_requested = false;
						s->connected.store(true);
						g_senderOutputConnected.store(true);
						clear_media_queues(s);
					}
				}
				if (s->connected.load()) {
					blog(LOG_INFO, "DuoCast: connected to receiver %s:%d", s->destination.c_str(), s->port);
					retry_delay_seconds = 1;
					std::thread stats_reader([s, candidate] { receive_receiver_stats(s, candidate); });
					for (;;) {
						QueuePacket packet;
						{
							std::unique_lock<std::mutex> lock(s->mutex);
							s->ready.wait(lock, [&] {
								return s->stopping || s->reconnect_requested || !s->audio_queue.empty() ||
								       !s->video_queue.empty();
							});
							if (s->stopping || s->reconnect_requested) break;
							auto &next_queue = s->audio_queue.empty() ? s->video_queue : s->audio_queue;
							packet = std::move(next_queue.front());
							s->queue_bytes -= packet.bytes.size();
							next_queue.pop_front();
						}
						if (!socket_send_all_capped(s, candidate, packet.bytes.data(), packet.bytes.size(),
								    send_tokens, send_tokens_last)) break;
						const auto payload_bytes = packet.bytes.size() > obs_duocast::protocol::kHeaderSize
							? packet.bytes.size() - obs_duocast::protocol::kHeaderSize : 0;
						if (packet.kind == StreamKind::Video || packet.kind == StreamKind::VerticalVideo) {
							if (packet.kind == StreamKind::VerticalVideo) {
								g_verticalVideoPackets.fetch_add(1);
								g_verticalVideoBytes.fetch_add(payload_bytes);
								if (!s->vertical_packet_sent_logged.exchange(true))
									blog(LOG_INFO, "DuoCast: sent first Aitum Vertical video frame (%zu bytes)", payload_bytes);
							} else {
							g_videoPackets.fetch_add(1);
							g_videoBytes.fetch_add(payload_bytes);
							}
						} else if (packet.kind == StreamKind::LiveAudio) {
							g_liveAudioPackets.fetch_add(1);
							g_liveAudioBytes.fetch_add(payload_bytes);
						} else if (packet.kind == StreamKind::VodAudio) {
							g_vodAudioPackets.fetch_add(1);
							g_vodAudioBytes.fetch_add(payload_bytes);
						}
					}
					shutdown(candidate, SD_BOTH);
					if (stats_reader.joinable()) stats_reader.join();
				}
			}
			{
				std::lock_guard<std::mutex> lock(s->mutex);
				s->connected.store(false);
				g_senderOutputConnected.store(false);
					clear_media_queues(s);
				s->reconnect_requested = false;
				{
					std::lock_guard<std::mutex> socketLock(s->socket_mutex);
					const SOCKET active_socket = s->socket.exchange(INVALID_SOCKET);
					if (active_socket != INVALID_SOCKET) {
						shutdown(active_socket, SD_BOTH);
						closesocket(active_socket);
					}
				}
				if (s->stopping) break;
			}
			blog(LOG_WARNING, "DuoCast: connection to %s:%d lost; retrying",
			     s->destination.c_str(), s->port);
		} else {
			++failed_attempts;
			if (failed_attempts == 1 || failed_attempts % 5 == 0)
				blog(LOG_INFO, "DuoCast: waiting for receiver %s:%d (attempt %u)",
				     s->destination.c_str(), s->port, failed_attempts);
		}

		std::unique_lock<std::mutex> lock(s->mutex);
		if (s->ready.wait_for(lock, std::chrono::seconds(retry_delay_seconds), [&] { return s->stopping.load(); }))
			break;
		retry_delay_seconds = std::min(retry_delay_seconds * 2, 5);
	}
	g_senderOutputConnected.store(false);
}

const char *sender_name(void *) { return "OBS DuoCast Sender"; }

obs_properties_t *sender_properties(void *)
{
	auto *props = obs_properties_create();
	obs_properties_add_text(props, "destination", "Receiver PC address", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "port", "Receiver port", 1, 65535, 1);
	obs_properties_add_int(props, "live_mix", "Live mix (OBS track)", 1, 6, 1);
	obs_properties_add_int(props, "vod_mix", "VOD mix (OBS track)", 1, 6, 1);
	return props;
}

void sender_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "destination", "192.168.50.102");
	obs_data_set_default_int(settings, "port", 17777);
	obs_data_set_default_int(settings, "live_mix", 1);
	obs_data_set_default_int(settings, "vod_mix", 2);
	obs_data_set_default_int(settings, "video_quality", 85);
	obs_data_set_default_int(settings, "video_encoder_mode", 0);
	obs_data_set_default_int(settings, "nvenc_bitrate_mbps", 80);
	obs_data_set_default_int(settings, "max_bitrate_mbps", 100);
	obs_data_set_default_int(settings, "max_fps", 0);
	obs_data_set_default_bool(settings, "send_vertical", false);
	obs_data_set_default_int(settings, "resolution_mode", 0);
}

void *sender_create(obs_data_t *settings, obs_output_t *output)
{
	auto *s = new SenderState();
	s->output = output;
	s->send_vertical = obs_data_get_bool(settings, "send_vertical");
	s->output_resolution = obs_data_get_int(settings, "resolution_mode") != 0;
	s->video_encoder_mode = std::clamp(static_cast<int>(obs_data_get_int(settings, "video_encoder_mode")), 0, 2);
	s->nvenc_bitrate_mbps = std::clamp(static_cast<int>(obs_data_get_int(settings, "nvenc_bitrate_mbps")), 1, 1000);
	const int quality = std::clamp(static_cast<int>(obs_data_get_int(settings, "video_quality")), 1, 100);
	struct obs_video_info info{};
	if (!obs_get_video_info(&info) ||
	    !(s->output_resolution ? info.output_width : info.base_width) ||
	    !(s->output_resolution ? info.output_height : info.base_height) ||
	    !info.fps_num || !info.fps_den) { delete s; return nullptr; }
	s->video_width = s->output_resolution ? info.output_width : info.base_width;
	s->video_height = s->output_resolution ? info.output_height : info.base_height;
	s->fps_num = info.fps_num; s->fps_den = info.fps_den;
	s->video_cs = info.colorspace;
	s->video_color_range = resolve_video_range(VIDEO_FORMAT_I420, info.range);
	s->video_quality = quality;
	s->max_bitrate_mbps = std::clamp(static_cast<int>(obs_data_get_int(settings, "max_bitrate_mbps")), 0, 1000);
	s->max_fps = std::clamp(static_cast<int>(obs_data_get_int(settings, "max_fps")), 0, 120);
	blog(LOG_INFO, "DuoCast: video settings are %s, %ux%u at %u/%u fps, %s, bitrate cap %d Mbps",
	     s->output_resolution ? "OBS output resolution" : "OBS canvas resolution", s->video_width,
	     s->video_height, s->fps_num, s->fps_den,
	     s->video_encoder_mode == 2 ? "OBS texture-passed NVENC HEVC" :
	     (s->video_encoder_mode == 1 ? "OBS texture-passed NVENC H.264" : "TurboJPEG MJPEG quality fixed"), s->max_bitrate_mbps);
	if (s->send_vertical && !initialize_vertical_canvas(s)) {
		if (s->vertical_canvas) obs_canvas_release(s->vertical_canvas);
		delete s;
		return nullptr;
	}
	s->video_encoder = create_video_encoder(s, false);
	if (!s->video_encoder) {
		if (s->vertical_canvas) obs_canvas_release(s->vertical_canvas);
		delete s;
		return nullptr;
	}
	auto create_audio = [&](const char *name, const char *key) {
		obs_data_t *audio_settings = obs_encoder_defaults("ffmpeg_aac");
		if (!audio_settings) return static_cast<obs_encoder_t *>(nullptr);
		obs_data_set_int(audio_settings, "bitrate", 160);
		const int track = static_cast<int>(obs_data_get_int(settings, key));
		auto *encoder = obs_audio_encoder_create("ffmpeg_aac", name, audio_settings,
							 static_cast<size_t>(std::clamp(track - 1, 0, 5)), nullptr);
		obs_data_release(audio_settings);
		if (encoder) obs_encoder_set_audio(encoder, obs_get_audio());
		return encoder;
	};
	s->live = create_audio("DuoCast live audio", "live_mix");
	s->vod = create_audio("DuoCast VOD audio", "vod_mix");
	if (!s->live || !s->vod) {
		obs_encoder_release(s->video_encoder);
		if (s->live) obs_encoder_release(s->live);
		if (s->vod) obs_encoder_release(s->vod);
		if (s->vertical_canvas) obs_canvas_release(s->vertical_canvas);
		delete s;
		return nullptr;
	}
	if (s->send_vertical && !create_vertical_transport(s, quality)) {
		if (s->vertical_output) obs_output_release(s->vertical_output);
		if (s->vertical_encoder) obs_encoder_release(s->vertical_encoder);
		if (s->vertical_canvas) obs_canvas_release(s->vertical_canvas);
		obs_encoder_release(s->video_encoder);
		obs_encoder_release(s->live);
		obs_encoder_release(s->vod);
		delete s;
		return nullptr;
	}
	obs_output_set_video_encoder(output, s->video_encoder);
	obs_output_set_audio_encoder(output, s->live, 0);
	obs_output_set_audio_encoder(output, s->vod, 1);
	g_senderOutputConnected.store(false);
	return s;
}

void sender_stop(void *opaque, std::uint64_t)
{
	auto *s = static_cast<SenderState *>(opaque);
	if (!s) return;
	blog(LOG_INFO, "DuoCast: sender stop requested; interrupting network worker");
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		s->active = false;
		s->stopping = true;
		s->connected.store(false);
		clear_media_queues(s);
	}
	// Wake a writer blocked in send() before asking OBS to end capture or
	// joining the worker. The worker owns closing the handle after send returns.
	{
		std::lock_guard<std::mutex> socketLock(s->socket_mutex);
		const SOCKET active_socket = s->socket.load();
		if (active_socket != INVALID_SOCKET) shutdown(active_socket, SD_BOTH);
	}
	s->ready.notify_all();
	if (s->vertical_capture_started && s->vertical_output) {
		obs_output_end_data_capture(s->vertical_output);
		s->vertical_capture_started = false;
	}
	if (s->capture_started) {
		obs_output_end_data_capture(s->output);
		s->capture_started = false;
	}
	g_senderOutputConnected.store(false);
	if (s->writer.joinable()) {
		if (s->writer.get_id() == std::this_thread::get_id()) s->writer.detach();
		else s->writer.join();
	}
	blog(LOG_INFO, "DuoCast: sender worker stopped");
}

void sender_destroy(void *opaque)
{
	auto *s = static_cast<SenderState *>(opaque);
	if (!s) return;
	sender_stop(s, 0);
	if (s->live) obs_encoder_release(s->live);
	if (s->vod) obs_encoder_release(s->vod);
	if (s->video_encoder) obs_encoder_release(s->video_encoder);
	if (s->vertical_output) obs_output_release(s->vertical_output);
	if (s->vertical_encoder) obs_encoder_release(s->vertical_encoder);
	if (s->vertical_canvas) obs_canvas_release(s->vertical_canvas);
	delete s;
}

bool sender_start(void *opaque)
{
	auto *s = static_cast<SenderState *>(opaque);
	if (!obs_output_can_begin_data_capture(s->output, 0) || !obs_output_initialize_encoders(s->output, 0))
		return false;
	if (s->send_vertical && (!s->vertical_output ||
				 !obs_output_can_begin_data_capture(s->vertical_output, 0) ||
				 !obs_output_initialize_encoders(s->vertical_output, 0))) {
		blog(LOG_ERROR, "DuoCast: could not initialize OBS video processing for the Aitum Vertical canvas");
		return false;
	}
	obs_data_t *settings = obs_output_get_settings(s->output);
	s->destination = obs_data_get_string(settings, "destination");
	s->port = std::clamp(static_cast<int>(obs_data_get_int(settings, "port")), 1, 65535);
	obs_data_release(settings);
	if (s->destination.empty()) return false;

	WSADATA wsa{};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
	{
		std::lock_guard<std::mutex> lock(s->mutex);
		s->stopping = false;
		s->active = true;
		s->reconnect_requested = false;
		s->connected.store(false);
		clear_media_queues(s);
	}
	g_senderOutputConnected.store(false);
	g_videoPackets.store(0);
	g_liveAudioPackets.store(0);
	g_vodAudioPackets.store(0);
	g_verticalVideoPackets.store(0);
	g_videoBytes.store(0);
	g_liveAudioBytes.store(0);
	g_vodAudioBytes.store(0);
	g_verticalVideoBytes.store(0);
	s->writer = std::thread(writer_thread, s);
	if (!obs_output_begin_data_capture(s->output, 0)) {
		sender_stop(s, 0);
		return false;
	}
	s->capture_started = true;
	if (s->send_vertical) {
		if (!obs_output_begin_data_capture(s->vertical_output, 0)) {
			blog(LOG_ERROR, "DuoCast: could not activate OBS video processing for the Aitum Vertical canvas");
			sender_stop(s, 0);
			return false;
		}
		s->vertical_capture_started = true;
		blog(LOG_INFO, "DuoCast: OBS-managed Aitum Vertical video capture started");
	}
	return true;
}

void sender_packet(void *opaque, encoder_packet *packet)
{
	auto *s = static_cast<SenderState *>(opaque);
	if (!packet) return;
	StreamKind kind;
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (packet->encoder != s->video_encoder) return;
		kind = StreamKind::Video;
	} else if (packet->type == OBS_ENCODER_AUDIO) {
		if (packet->track_idx == 0) kind = StreamKind::LiveAudio;
		else if (packet->track_idx == 1) kind = StreamKind::VodAudio;
		else return;
	} else return;
	if (packet->size > obs_duocast::protocol::kMaxPayload) return;
	// OBS encoder PTS/DTS are stream-relative (normally starting near zero).
	// Use OBS's system decode timestamp so audio and video share one clock domain.
	if (packet->sys_dts_usec <= 0) return;
	const std::uint64_t dts_ns = static_cast<std::uint64_t>(packet->sys_dts_usec) * 1000ULL;
	const std::uint64_t pts_ns = dts_ns;
	enqueue_media(s, kind, pts_ns, dts_ns, packet->keyframe, packet->data, packet->size);
}
} // namespace

bool sender_vertical_canvas_available()
{
	obs_canvas_t *canvas = sender_vertical_canvas_ref();
	if (!canvas) return false;
	struct obs_video_info info{};
	const bool available = obs_canvas_get_video_info(canvas, &info) && info.base_width && info.base_height;
	obs_canvas_release(canvas);
	return available;
}

obs_canvas_t *sender_vertical_canvas_ref()
{
	video_t *video = get_aitum_vertical_video();
	if (!video) return nullptr;
	struct FindCanvas { video_t *video; obs_canvas_t *canvas; } find{video, nullptr};
	obs_enum_canvases([](void *opaque, obs_canvas_t *canvas) {
		auto *find = static_cast<FindCanvas *>(opaque);
		if (obs_canvas_get_video(canvas) != find->video) return true;
		find->canvas = obs_canvas_get_ref(canvas);
		return false;
	}, &find);
	return find.canvas;
}

bool sender_output_connected()
{
	return g_senderOutputConnected.load();
}

SenderMetrics sender_output_metrics()
{
	return {g_videoPackets.load(), g_liveAudioPackets.load(), g_vodAudioPackets.load(),
		g_videoBytes.load(), g_liveAudioBytes.load(), g_vodAudioBytes.load(),
		g_verticalVideoPackets.load(), g_verticalVideoBytes.load()};
}

SenderRemoteStats sender_remote_stats()
{
	std::lock_guard<std::mutex> lock(g_remoteStatsMutex);
	return g_remoteStats;
}

void sender_remote_stats_clear()
{
	std::lock_guard<std::mutex> lock(g_remoteStatsMutex);
	g_remoteStats = {};
}

void register_sender_output()
{
	static obs_encoder_info mjpeg_encoder{};
	mjpeg_encoder.id = "duocast_turbojpeg";
	mjpeg_encoder.type = OBS_ENCODER_VIDEO;
	mjpeg_encoder.codec = "mjpeg";
	mjpeg_encoder.get_name = [](void *) { return "DuoCast TurboJPEG MJPEG"; };
	mjpeg_encoder.create = mjpeg_encoder_create;
	mjpeg_encoder.destroy = mjpeg_encoder_destroy;
	mjpeg_encoder.encode = mjpeg_encoder_encode;
	mjpeg_encoder.get_video_info = mjpeg_encoder_video_info;
	mjpeg_encoder.caps = OBS_ENCODER_CAP_INTERNAL;
	obs_register_encoder(&mjpeg_encoder);

	static obs_output_info vertical_output{};
	vertical_output.id = "duocast_vertical_transport";
	vertical_output.flags = OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED;
	vertical_output.encoded_video_codecs = "h264;hevc;mjpeg";
	vertical_output.get_name = [](void *) { return "DuoCast Vertical Transport"; };
	vertical_output.create = vertical_output_create;
	vertical_output.destroy = vertical_output_destroy;
	vertical_output.start = vertical_output_start;
	vertical_output.stop = vertical_output_stop;
	vertical_output.encoded_packet = vertical_output_packet;
	obs_register_output(&vertical_output);

	static obs_output_info info{};
	info.id = "obs_duocast_sender";
	info.flags = OBS_OUTPUT_AUDIO | OBS_OUTPUT_VIDEO | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK;
	info.encoded_audio_codecs = "aac";
	info.encoded_video_codecs = "h264;hevc;mjpeg";
	info.get_name = sender_name;
	info.create = sender_create;
	info.destroy = sender_destroy;
	info.start = sender_start;
	info.stop = sender_stop;
	info.encoded_packet = sender_packet;
	info.get_properties = sender_properties;
	info.get_defaults = sender_defaults;
	obs_register_output(&info);
}
