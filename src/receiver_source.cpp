#include "receiver_source.hpp"

#include <obs-module.h>
#include <obs-encoder.h>
#include <util/platform.h>
#include <media-io/video-io.h>
#include <turbojpeg.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswresample/swresample.h>
}

#include <winsock2.h>
#include <ws2tcpip.h>

#include "protocol.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <string>
#include <unordered_map>
#include <vector>

namespace {
using obs_duocast::protocol::PacketHeader;
using obs_duocast::protocol::StreamKind;

enum class SinkKind { Video, Vertical, Vod, PreviewVideo, PreviewVertical };

std::uint16_t read16(const std::uint8_t *p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t read32(const std::uint8_t *p)
{
	return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

bool read_exact(SOCKET socket, std::uint8_t *data, std::size_t size)
{
	while (size) {
		const int n = recv(socket, reinterpret_cast<char *>(data), static_cast<int>(std::min<std::size_t>(size, 1024 * 1024)), 0);
		if (n <= 0) return false;
		data += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

std::once_flag g_wsaOnce;
std::unordered_map<int, std::weak_ptr<class ReceiverSession>> g_sessions;
std::mutex g_sessionsMutex;
std::atomic<bool> g_receiverEnabled{false};
std::atomic<int> g_receiverSyncBufferMs{1000};
std::atomic<bool> g_autoRecoverLowFps{true};
std::atomic<bool> g_requestSenderRecovery{false};
std::mutex g_remoteStatsMutex;
obs_duocast::protocol::ReceiverStats g_remoteStats{};

bool write_exact(SOCKET socket, const std::uint8_t *data, std::size_t size)
{
	while (size) {
		const int n = send(socket, reinterpret_cast<const char *>(data),
			static_cast<int>(std::min<std::size_t>(size, 1024 * 1024)), 0);
		if (n <= 0) return false;
		data += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

const char *video_codec_label(AVCodecID id)
{
	switch (id) {
	case AV_CODEC_ID_H264: return "H.264";
	case AV_CODEC_ID_HEVC: return "HEVC";
	case AV_CODEC_ID_MJPEG: return "MJPEG";
	default: return "unknown";
	}
}

void send_receiver_stats(SOCKET socket, std::uint32_t sequence)
{
	obs_duocast::protocol::ReceiverStats stats;
	{
		std::lock_guard<std::mutex> lock(g_remoteStatsMutex);
		stats = g_remoteStats;
	}
	if (g_requestSenderRecovery.exchange(false)) stats.flags |= 1U << 3;
	std::uint8_t payload[obs_duocast::protocol::kReceiverStatsPayloadSize]{};
	obs_duocast::protocol::encode_receiver_stats(stats, payload);
	PacketHeader header;
	header.kind = StreamKind::ReceiverStats;
	header.sequence = sequence;
	header.pts_ns = os_gettime_ns();
	header.payload_size = static_cast<std::uint32_t>(sizeof(payload));
	std::uint8_t wire[obs_duocast::protocol::kHeaderSize];
	obs_duocast::protocol::encode_header(header, wire);
	(void)(write_exact(socket, wire, sizeof(wire)) && write_exact(socket, payload, sizeof(payload)));
}

class ReceiverSession : public std::enable_shared_from_this<ReceiverSession> {
public:
	explicit ReceiverSession(int port) : port_(port) { start(); }
	~ReceiverSession() { stop(); clear_decoder(video_); clear_decoder(vertical_video_); clear_decoder(live_); clear_decoder(vod_); }
	void set_enabled(bool enabled) { if (enabled) start(); else stop(); }

	bool add_source(SinkKind kind, obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(sinksMutex_);
		auto &list = sinks_[index(kind)];
		if (std::find(list.begin(), list.end(), source) == list.end()) list.push_back(source);
		return true;
	}
	void remove_source(SinkKind kind, obs_source_t *source)
	{
		std::lock_guard<std::mutex> lock(sinksMutex_);
		auto &list = sinks_[index(kind)];
		list.erase(std::remove(list.begin(), list.end(), source), list.end());
	}
	std::uint32_t width() const { return width_.load(); }
	std::uint32_t height() const { return height_.load(); }
	std::uint32_t width(SinkKind kind) const { return (kind == SinkKind::Vertical || kind == SinkKind::PreviewVertical) ? vertical_width_.load() : width_.load(); }
	std::uint32_t height(SinkKind kind) const { return (kind == SinkKind::Vertical || kind == SinkKind::PreviewVertical) ? vertical_height_.load() : height_.load(); }
	bool connection_seen() const { return connection_started_ns_.load() != 0; }
	bool vertical_feed_configured() const { return vertical_feed_configured_.load(); }
	bool vertical_video_healthy() const
	{
		const auto last = last_vertical_video_ns_.load();
		const auto now = os_gettime_ns();
		return last && now >= last && now - last < 2500000000ULL;
	}
	bool landscape_video_healthy() const
	{
		const auto last = last_video_ns_.load();
		const auto connected_at = connection_started_ns_.load();
		const auto reference = last ? last : connected_at;
		const auto now = os_gettime_ns();
		return reference && now >= reference && now - reference < 2500000000ULL;
	}
	std::string active_peer_address() const
	{
		if (client_.load() == INVALID_SOCKET) return {};
		std::lock_guard<std::mutex> lock(peer_mutex_);
		return peer_address_;
	}
	bool video_healthy() const
	{
		const auto connected_at = connection_started_ns_.load();
		if (!connected_at) return false;
		const auto last = std::max(last_video_ns_.load(), last_vertical_video_ns_.load());
		const auto reference = last ? last : connected_at;
		const auto now = os_gettime_ns();
		return now >= reference && now - reference < 2500000000ULL;
	}

private:
	static constexpr std::size_t index(SinkKind kind)
	{
		return kind == SinkKind::Video ? 0 : kind == SinkKind::Vertical ? 1 :
			kind == SinkKind::Vod ? 2 : kind == SinkKind::PreviewVideo ? 3 : 4;
	}
	struct Decoder {
		AVCodecContext *ctx = nullptr;
		AVFrame *frame = nullptr;
		AVFrame *transfer_frame = nullptr;
		AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
		SwrContext *resampler = nullptr;
		tjhandle jpeg = nullptr;
		std::array<std::vector<std::uint8_t>, 3> jpeg_planes;
		std::uint32_t width = 0, height = 0;
		std::uint32_t expected_fps_milli = 0;
		float jpeg_matrix[16]{}, jpeg_range_min[3]{}, jpeg_range_max[3]{};
		bool has_jpeg_color_params = false;
		video_range_type jpeg_range = VIDEO_RANGE_FULL;
		AVCodecID codec_id = AV_CODEC_ID_NONE;
		bool first_frame_logged = false;
	};
	struct QueuedMedia {
		StreamKind kind = StreamKind::Heartbeat;
		std::uint64_t pts_ns = 0;
		std::uint64_t dts_ns = 0;
		bool keyframe = false;
		std::vector<std::uint8_t> payload;
	};

	void start_playout()
	{
		g_requestSenderRecovery.store(false);
		{
			std::lock_guard<std::mutex> lock(playout_mutex_);
			playout_queue_.clear();
			playout_queue_bytes_ = 0;
			playout_stopping_ = false;
			active_playout_delay_ms_ = g_receiverSyncBufferMs.load();
		}
		blog(LOG_INFO, "DuoCast: receiver A/V sync buffer set to %d ms", active_playout_delay_ms_);
		playout_worker_ = std::thread(&ReceiverSession::playout_loop, this);
	}

	void stop_playout()
	{
		{
			std::lock_guard<std::mutex> lock(playout_mutex_);
			playout_stopping_ = true;
			playout_queue_.clear();
			playout_queue_bytes_ = 0;
		}
		playout_ready_.notify_all();
		if (playout_worker_.joinable()) playout_worker_.join();
	}

	void queue_media(StreamKind kind, std::uint64_t pts_ns, std::uint64_t dts_ns,
			  bool keyframe, std::vector<std::uint8_t> &&payload)
	{
		constexpr std::size_t kMaxPlayoutQueueBytes = 256ULL * 1024ULL * 1024ULL;
		QueuedMedia item;
		item.kind = kind;
		item.pts_ns = pts_ns;
		item.dts_ns = dts_ns;
		item.keyframe = keyframe;
		item.payload = std::move(payload);
		const auto itemBytes = item.payload.size();
		if (itemBytes > kMaxPlayoutQueueBytes) {
			blog(LOG_WARNING, "DuoCast: receiver sync buffer packet exceeds the 256 MiB queue limit; dropping packet");
			return;
		}
		{
			std::lock_guard<std::mutex> lock(playout_mutex_);
			if (playout_stopping_) return;
			while (playout_queue_bytes_ + itemBytes > kMaxPlayoutQueueBytes) {
				auto oldestVideo = std::find_if(playout_queue_.begin(), playout_queue_.end(), [](const auto &entry) {
					return entry.second.kind == StreamKind::Video || entry.second.kind == StreamKind::VerticalVideo;
				});
				if (oldestVideo == playout_queue_.end()) {
					blog(LOG_WARNING, "DuoCast: receiver sync buffer full; dropping incoming media packet");
					return;
				}
				playout_queue_bytes_ -= oldestVideo->second.payload.size();
				playout_queue_.erase(oldestVideo);
			}
			playout_queue_bytes_ += itemBytes;
			const auto pts = item.pts_ns;
			playout_queue_.emplace(pts, std::move(item));
		}
		playout_ready_.notify_one();
	}

	void playout_loop()
	{
		constexpr std::uint64_t kDecodeLeadNs = 2000000ULL;
		for (;;) {
			QueuedMedia item;
			int delayMs = 0;
			{
				std::unique_lock<std::mutex> lock(playout_mutex_);
				for (;;) {
					if (playout_stopping_) return;
					if (playout_queue_.empty()) {
						playout_ready_.wait(lock, [this] { return playout_stopping_ || !playout_queue_.empty(); });
						continue;
					}
					delayMs = active_playout_delay_ms_;
					const auto delayNs = static_cast<std::uint64_t>(delayMs) * 1000000ULL;
					const auto scheduledNs = playout_queue_.begin()->second.pts_ns + delayNs;
					const auto decodeAtNs = scheduledNs > kDecodeLeadNs ? scheduledNs - kDecodeLeadNs : 0;
					const auto nowNs = os_gettime_ns();
					if (decodeAtNs > nowNs) {
						playout_ready_.wait_for(lock, std::chrono::nanoseconds(decodeAtNs - nowNs));
						continue;
					}
					auto first = playout_queue_.begin();
					item = std::move(first->second);
					playout_queue_bytes_ -= item.payload.size();
					playout_queue_.erase(first);
					break;
				}
			}
			const auto delayNs = static_cast<std::uint64_t>(delayMs) * 1000000ULL;
			const auto outputPts = item.pts_ns + delayNs;
			const auto outputDts = item.dts_ns + delayNs;
			switch (item.kind) {
			case StreamKind::Video:
			case StreamKind::VerticalVideo:
			case StreamKind::LiveAudio:
			case StreamKind::VodAudio: {
				std::lock_guard<std::mutex> decoderLock(decoder_mutex_);
				if (item.kind == StreamKind::Video) decode(video_, item.kind, item.payload, outputPts, outputDts, item.keyframe);
				else if (item.kind == StreamKind::VerticalVideo) decode(vertical_video_, item.kind, item.payload, outputPts, outputDts, item.keyframe);
				else if (item.kind == StreamKind::LiveAudio) decode(live_, item.kind, item.payload, outputPts, outputDts, false);
				else decode(vod_, item.kind, item.payload, outputPts, outputDts, false);
				break;
			}
			default: break;
			}
		}
	}

	void start()
	{
		std::lock_guard<std::mutex> lifecycleLock(lifecycle_mutex_);
		if (!g_receiverEnabled.load() || worker_.joinable()) return;
		stopping_.store(false);
		std::call_once(g_wsaOnce, [] { WSADATA data{}; WSAStartup(MAKEWORD(2, 2), &data); });
		listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (listener_ == INVALID_SOCKET) { blog(LOG_ERROR, "DuoCast: TCP listener socket creation failed"); return; }
		BOOL reuse = TRUE;
		setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse));
		sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(static_cast<u_short>(port_)); addr.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(listener_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR || listen(listener_, 1) == SOCKET_ERROR) {
			blog(LOG_ERROR, "DuoCast: could not listen on TCP port %d", port_);
			closesocket(listener_); listener_ = INVALID_SOCKET; return;
		}
		worker_ = std::thread(&ReceiverSession::accept_loop, this);
		blog(LOG_INFO, "DuoCast: receiver listening on TCP port %d", port_);
	}

	void stop()
	{
		std::lock_guard<std::mutex> lifecycleLock(lifecycle_mutex_);
		stopping_.store(true);
		const SOCKET listener = listener_.exchange(INVALID_SOCKET);
		if (listener != INVALID_SOCKET) { shutdown(listener, SD_BOTH); closesocket(listener); }
		const SOCKET client = client_.exchange(INVALID_SOCKET);
		if (client != INVALID_SOCKET) { shutdown(client, SD_BOTH); closesocket(client); }
		if (worker_.joinable()) worker_.join();
	}

	void accept_loop()
	{
		while (!stopping_.load()) {
			SOCKET accepted = accept(listener_.load(), nullptr, nullptr);
			if (accepted == INVALID_SOCKET) { if (stopping_.load()) break; continue; }
			BOOL no_delay = TRUE;
			if (setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
				       reinterpret_cast<const char *>(&no_delay), sizeof(no_delay)) == SOCKET_ERROR)
				blog(LOG_WARNING, "DuoCast: could not disable TCP packet coalescing (socket error %d)", WSAGetLastError());
			DWORD receive_timeout_ms = 5000;
			if (setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
				       reinterpret_cast<const char *>(&receive_timeout_ms), sizeof(receive_timeout_ms)) == SOCKET_ERROR)
				blog(LOG_WARNING, "DuoCast: could not set receiver read timeout (socket error %d)", WSAGetLastError());
			client_.store(accepted);
			sockaddr_storage peer{};
			int peer_size = sizeof(peer);
			char host[NI_MAXHOST]{};
			char service[NI_MAXSERV]{};
			std::string peer_address;
			if (getpeername(accepted, reinterpret_cast<sockaddr *>(&peer), &peer_size) == 0 &&
			    getnameinfo(reinterpret_cast<sockaddr *>(&peer), peer_size, host, sizeof(host), service,
					 sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) == 0)
				peer_address = std::string(host) + ":" + service;
			{
				std::lock_guard<std::mutex> lock(peer_mutex_);
				peer_address_ = std::move(peer_address);
			}
			connection_started_ns_.store(os_gettime_ns());
			last_video_ns_.store(0);
			last_vertical_video_ns_.store(0);
			vertical_feed_configured_.store(false);
			blog(LOG_INFO, "DuoCast: sender connected on port %d", port_);
			start_playout();
			stream_loop(accepted);
			stop_playout();
			SOCKET expected = accepted;
			if (client_.compare_exchange_strong(expected, INVALID_SOCKET)) closesocket(accepted);
			{
				std::lock_guard<std::mutex> lock(peer_mutex_);
				peer_address_.clear();
			}
			clear_decoder(video_); clear_decoder(vertical_video_); clear_decoder(live_); clear_decoder(vod_);
			// Keep the last negotiated source dimensions after disconnect. OBS keeps
			// rendering the last decoded frame; resetting its reported size here
			// changes the source transform underneath that frame and makes it look
			// zoomed/cropped until the next sender connects.
			blog(LOG_INFO, "DuoCast: sender disconnected on port %d; waiting for reconnect", port_);
		}
	}

	void stream_loop(SOCKET socket)
	{
		// Sender OBS timestamps use its monotonic clock. Rebase the shared
		// sender timeline onto this OBS process's clock so all three feeds use
		// the same clock domain across machines with different uptime clocks.
		bool have_clock_offset = false;
		std::int64_t clock_offset_ns = 0;
		std::uint64_t video_packet_count = 0;
		std::uint64_t vertical_video_packet_count = 0;
		std::uint64_t next_stats_at_ns = os_gettime_ns() + 1000000000ULL;
		std::uint64_t previous_health_sample_ns = os_gettime_ns();
		std::uint64_t previous_landscape_frames = delivered_landscape_frames_.load();
		std::uint64_t previous_vertical_frames = delivered_vertical_frames_.load();
		unsigned low_fps_windows = 0;
		unsigned healthy_windows = 0;
		std::uint32_t stats_sequence = 0;
		auto rebase_timestamp = [&](std::uint64_t sender_ns) -> std::uint64_t {
			if (sender_ns > static_cast<std::uint64_t>(INT64_MAX)) return 0;
			const auto adjusted = static_cast<std::int64_t>(sender_ns) + clock_offset_ns;
			return adjusted > 0 ? static_cast<std::uint64_t>(adjusted) : 0;
		};
		while (!stopping_.load()) {
			std::uint8_t wire[obs_duocast::protocol::kHeaderSize];
			if (!read_exact(socket, wire, sizeof(wire))) return;
			PacketHeader h;
			if (!obs_duocast::protocol::decode_header(wire, h)) { blog(LOG_WARNING, "DuoCast: invalid packet header"); return; }
			std::vector<std::uint8_t> payload(h.payload_size);
			if (h.payload_size && !read_exact(socket, payload.data(), payload.size())) return;
			if (!have_clock_offset && (h.kind == StreamKind::Video || h.kind == StreamKind::VerticalVideo || h.kind == StreamKind::LiveAudio ||
						   h.kind == StreamKind::VodAudio)) {
				if (h.pts_ns > static_cast<std::uint64_t>(INT64_MAX)) return;
			clock_offset_ns = static_cast<std::int64_t>(os_gettime_ns()) -
						  static_cast<std::int64_t>(h.pts_ns);
				have_clock_offset = true;
			}
			switch (h.kind) {
			case StreamKind::VideoConfig: { std::lock_guard<std::mutex> lock(decoder_mutex_); setup_video(video_, payload, false); break; }
			case StreamKind::VerticalVideoConfig:
				vertical_feed_configured_.store(true);
				last_vertical_video_ns_.store(os_gettime_ns());
				{ std::lock_guard<std::mutex> lock(decoder_mutex_); setup_video(vertical_video_, payload, true); }
				break;
			case StreamKind::LiveAudioConfig: { std::lock_guard<std::mutex> lock(decoder_mutex_); setup_audio(live_, payload); break; }
			case StreamKind::VodAudioConfig: { std::lock_guard<std::mutex> lock(decoder_mutex_); setup_audio(vod_, payload); break; }
			case StreamKind::Video:
				last_video_ns_.store(os_gettime_ns());
				if (video_packet_count++ == 0)
					blog(LOG_INFO, "DuoCast: received first video packet (%zu bytes)", payload.size());
				queue_media(StreamKind::Video, rebase_timestamp(h.pts_ns), rebase_timestamp(h.dts_ns),
					    h.flags & 1, std::move(payload));
				break;
			case StreamKind::VerticalVideo:
				last_vertical_video_ns_.store(os_gettime_ns());
				if (vertical_video_packet_count++ == 0)
					blog(LOG_INFO, "DuoCast: received first vertical video packet (%zu bytes)", payload.size());
				queue_media(StreamKind::VerticalVideo, rebase_timestamp(h.pts_ns), rebase_timestamp(h.dts_ns),
					    h.flags & 1, std::move(payload));
				break;
			case StreamKind::LiveAudio:
				queue_media(StreamKind::LiveAudio, rebase_timestamp(h.pts_ns), rebase_timestamp(h.dts_ns),
					    false, std::move(payload));
				break;
			case StreamKind::VodAudio:
				queue_media(StreamKind::VodAudio, rebase_timestamp(h.pts_ns), rebase_timestamp(h.dts_ns),
					    false, std::move(payload));
				break;
			case StreamKind::Heartbeat: break;
			}
			const auto now_ns = os_gettime_ns();
			if (now_ns >= next_stats_at_ns) {
				const auto sample_ns = std::max<std::uint64_t>(1, now_ns - previous_health_sample_ns);
				const auto landscape_frames = delivered_landscape_frames_.load();
				const auto vertical_frames = delivered_vertical_frames_.load();
				const auto landscape_fps_milli = static_cast<std::uint64_t>(landscape_frames - previous_landscape_frames) * 1000000000000ULL / sample_ns;
				const auto vertical_fps_milli = static_cast<std::uint64_t>(vertical_frames - previous_vertical_frames) * 1000000000000ULL / sample_ns;
				previous_health_sample_ns = now_ns;
				previous_landscape_frames = landscape_frames;
				previous_vertical_frames = vertical_frames;
				const auto expected_landscape = video_.expected_fps_milli;
				const auto expected_vertical = vertical_video_.expected_fps_milli;
				const bool warmed_up = connection_started_ns_.load() && now_ns - connection_started_ns_.load() >= 7000000000ULL;
				const bool landscape_bad = expected_landscape && landscape_fps_milli < static_cast<std::uint64_t>(expected_landscape) * 60 / 100;
				const bool vertical_bad = expected_vertical && vertical_fps_milli < static_cast<std::uint64_t>(expected_vertical) * 60 / 100;
				const bool any_bad = landscape_bad || vertical_bad;
				if (g_autoRecoverLowFps.load() && warmed_up) {
					if (any_bad) {
						++low_fps_windows;
						healthy_windows = 0;
						if (low_fps_windows >= 5 && auto_recovery_armed_) {
						auto_recovery_armed_ = false;
						low_fps_windows = 0;
						g_requestSenderRecovery.store(true);
						if (expected_vertical) {
							blog(LOG_WARNING,
							     "DuoCast: low receiver video rate detected (landscape %.1f/%.1f fps, vertical %.1f/%.1f fps); requesting sender transport recovery",
							     landscape_fps_milli / 1000.0, expected_landscape / 1000.0,
							     vertical_fps_milli / 1000.0, expected_vertical / 1000.0);
						} else {
							blog(LOG_WARNING,
							     "DuoCast: low receiver video rate detected (landscape %.1f/%.1f fps); requesting sender transport recovery",
							     landscape_fps_milli / 1000.0, expected_landscape / 1000.0);
						}
					}
					} else {
						low_fps_windows = 0;
						if (++healthy_windows >= 10) auto_recovery_armed_ = true;
					}
				} else {
					low_fps_windows = 0;
					healthy_windows = 0;
				}
				send_receiver_stats(socket, stats_sequence++);
				next_stats_at_ns = now_ns + 1000000000ULL;
			}
		}
	}

	void clear_decoder(Decoder &d)
	{
		if (d.resampler) swr_free(&d.resampler);
		if (d.frame) av_frame_free(&d.frame);
		if (d.transfer_frame) av_frame_free(&d.transfer_frame);
		if (d.ctx) avcodec_free_context(&d.ctx);
		if (d.jpeg) {
			tj3Destroy(d.jpeg);
			d.jpeg = nullptr;
		}
		for (auto &plane : d.jpeg_planes) plane.clear();
		d.width = d.height = 0;
		d.expected_fps_milli = 0;
		d.first_frame_logged = false;
		d.hw_pix_fmt = AV_PIX_FMT_NONE;
		d.codec_id = AV_CODEC_ID_NONE;
		d.has_jpeg_color_params = false;
	}

	bool open_decoder(Decoder &d, AVCodecID id, int width, int height, int sample_rate, int channels,
			  const std::uint8_t *extra, std::size_t extra_size)
	{
		clear_decoder(d);
		const AVCodec *codec = avcodec_find_decoder(id);
		if (!codec || extra_size > 65535) return false;
		auto initialize = [&](bool use_d3d11va) {
			d.ctx = avcodec_alloc_context3(codec);
			if (!d.ctx) return false;
			d.ctx->time_base = AVRational{1, 1000000000};
			d.ctx->pkt_timebase = d.ctx->time_base;
			d.ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
			if (id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC) {
				d.ctx->width = width;
				d.ctx->height = height;
				d.codec_id = id;
			} else {
				d.ctx->sample_rate = sample_rate;
				av_channel_layout_default(&d.ctx->ch_layout, channels);
			}
			if (extra_size) {
				d.ctx->extradata = static_cast<std::uint8_t *>(av_mallocz(extra_size + AV_INPUT_BUFFER_PADDING_SIZE));
				if (!d.ctx->extradata) return false;
				std::memcpy(d.ctx->extradata, extra, extra_size);
				d.ctx->extradata_size = static_cast<int>(extra_size);
			}
			if (use_d3d11va) {
				const AVCodecHWConfig *hw_config = nullptr;
				for (int i = 0; (hw_config = avcodec_get_hw_config(codec, i)); ++i) {
					if (hw_config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
					    (hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
						break;
					hw_config = nullptr;
				}
				AVBufferRef *device = nullptr;
				if (!hw_config || av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0)
					return false;
				d.ctx->hw_device_ctx = av_buffer_ref(device);
				av_buffer_unref(&device);
				if (!d.ctx->hw_device_ctx) return false;
				d.hw_pix_fmt = hw_config->pix_fmt;
				d.ctx->opaque = &d;
				d.ctx->get_format = [](AVCodecContext *ctx, const AVPixelFormat *formats) {
					auto *decoder = static_cast<Decoder *>(ctx->opaque);
					if (decoder && decoder->hw_pix_fmt != AV_PIX_FMT_NONE) {
						for (const AVPixelFormat *format = formats; *format != AV_PIX_FMT_NONE; ++format)
							if (*format == decoder->hw_pix_fmt) return *format;
					}
					return formats[0];
				};
			}
			if (avcodec_open2(d.ctx, codec, nullptr) < 0) return false;
			d.frame = av_frame_alloc();
			if (use_d3d11va) d.transfer_frame = av_frame_alloc();
			return d.frame && (!use_d3d11va || d.transfer_frame);
		};

		if ((id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC) && initialize(true)) {
			blog(LOG_INFO, "DuoCast: using D3D11 hardware decoding for %s", video_codec_label(id));
			return true;
		}
		if (id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC) {
			blog(LOG_WARNING, "DuoCast: D3D11 %s decoding unavailable; falling back to software decoding",
			     video_codec_label(id));
			clear_decoder(d);
		}
		if (initialize(false)) return true;
		clear_decoder(d);
		return false;
	}

	void setup_video(Decoder &decoder, const std::vector<std::uint8_t> &p, bool vertical)
	{
		if (p.size() < 19) return;
		const auto w = read32(p.data()), h = read32(p.data() + 4);
		const auto extra_size = read16(p.data() + 17);
		const auto codec_type = p[16];
		const AVCodecID codec_id = codec_type == 1 ? AV_CODEC_ID_H264 :
			codec_type == 5 ? AV_CODEC_ID_HEVC :
			(codec_type == 2 || codec_type == 4) ? AV_CODEC_ID_MJPEG : AV_CODEC_ID_NONE;
		const std::size_t color_bytes = codec_type == 4 ? 89 : 0;
		if (codec_id == AV_CODEC_ID_NONE || !w || !h || w > 8192 || h > 8192 ||
		    p.size() != 19 + extra_size + color_bytes) return;
		blog(LOG_INFO, "DuoCast: received %s video config (%s, %ux%u)", vertical ? "vertical" : "landscape",
		     codec_id == AV_CODEC_ID_MJPEG ? "TurboJPEG MJPEG" : video_codec_label(codec_id), w, h);
		bool opened = false;
		if (codec_id == AV_CODEC_ID_MJPEG && !extra_size && w <= INT_MAX && h <= INT_MAX && !(w & 1) && !(h & 1)) {
			clear_decoder(decoder);
			decoder.jpeg = tj3Init(TJINIT_DECOMPRESS);
			if (decoder.jpeg && tj3Set(decoder.jpeg, TJPARAM_MAXPIXELS, static_cast<int>(w * h)) == 0) {
				decoder.width = w;
				decoder.height = h;
				decoder.codec_id = AV_CODEC_ID_MJPEG;
				opened = true;
			} else clear_decoder(decoder);
		} else if (codec_id == AV_CODEC_ID_H264 || codec_id == AV_CODEC_ID_HEVC) {
			opened = open_decoder(decoder, codec_id, static_cast<int>(w), static_cast<int>(h), 0, 0,
					       p.data() + 19, extra_size);
		}
		if (opened) {
			const auto fps_num = read32(p.data() + 8);
			const auto fps_den = read32(p.data() + 12);
			decoder.expected_fps_milli = fps_den
				? static_cast<std::uint32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(fps_num) * 1000 / fps_den, 240000))
				: 0;
			decoder.has_jpeg_color_params = codec_type == 4;
			if (decoder.has_jpeg_color_params) {
				std::size_t offset = 19 + extra_size;
				decoder.jpeg_range = static_cast<video_range_type>(p[offset++]);
				if (decoder.jpeg_range != VIDEO_RANGE_FULL && decoder.jpeg_range != VIDEO_RANGE_PARTIAL)
					decoder.jpeg_range = VIDEO_RANGE_PARTIAL;
				auto read_float = [&](float &value) {
					const auto bits = read32(p.data() + offset); offset += 4;
					std::memcpy(&value, &bits, sizeof(value));
				};
				for (auto &value : decoder.jpeg_matrix) read_float(value);
				for (auto &value : decoder.jpeg_range_min) read_float(value);
				for (auto &value : decoder.jpeg_range_max) read_float(value);
			}
			if (vertical) { vertical_width_.store(w); vertical_height_.store(h); }
			else { width_.store(w); height_.store(h); }
		}
		else blog(LOG_ERROR, "DuoCast: could not initialize %s decoder",
			  codec_id == AV_CODEC_ID_MJPEG ? "TurboJPEG MJPEG" : video_codec_label(codec_id));
	}

	void setup_audio(Decoder &d, const std::vector<std::uint8_t> &p)
	{
		if (p.size() < 8) return;
		const auto rate = read32(p.data());
		const int channels = p[4];
		const auto extra_size = read16(p.data() + 6);
		if (p[5] != 1 || channels < 1 || channels > 8 || rate < 8000 || rate > 192000 || p.size() != 8 + extra_size) return;
		if (!open_decoder(d, AV_CODEC_ID_AAC, 0, 0, static_cast<int>(rate), channels, p.data() + 8, extra_size))
			blog(LOG_ERROR, "DuoCast: could not initialize AAC decoder");
	}

	void decode(Decoder &d, StreamKind kind, const std::vector<std::uint8_t> &payload, std::uint64_t pts,
		    std::uint64_t dts, bool keyframe)
	{
		if (d.codec_id == AV_CODEC_ID_MJPEG) {
			decode_jpeg(d, kind, payload, pts);
			return;
		}
		if (!d.ctx || payload.empty() || payload.size() > obs_duocast::protocol::kMaxPayload) return;
		AVPacket *packet = av_packet_alloc();
		if (!packet) return;
		if (av_new_packet(packet, static_cast<int>(payload.size())) < 0) { av_packet_free(&packet); return; }
		std::memcpy(packet->data, payload.data(), payload.size());
		packet->pts = static_cast<std::int64_t>(pts);
		packet->dts = static_cast<std::int64_t>(dts);
		if (keyframe) packet->flags |= AV_PKT_FLAG_KEY;
		const bool is_video = kind == StreamKind::Video || kind == StreamKind::VerticalVideo;
		const int send_result = avcodec_send_packet(d.ctx, packet);
		if (send_result < 0) {
			char error[AV_ERROR_MAX_STRING_SIZE]{};
			av_strerror(send_result, error, sizeof(error));
			blog(LOG_WARNING, "DuoCast: %s packet decode failed: %s",
			     is_video ? video_codec_label(d.codec_id) : "AAC", error);
		} else {
			int receive_result = 0;
			while ((receive_result = avcodec_receive_frame(d.ctx, d.frame)) == 0) {
				AVFrame *decoded = d.frame;
				if (d.hw_pix_fmt != AV_PIX_FMT_NONE && d.frame->format == d.hw_pix_fmt) {
					av_frame_unref(d.transfer_frame);
					const int transfer_result = av_hwframe_transfer_data(d.transfer_frame, d.frame, 0);
					if (transfer_result < 0) {
						char error[AV_ERROR_MAX_STRING_SIZE]{};
						av_strerror(transfer_result, error, sizeof(error));
						blog(LOG_WARNING, "DuoCast: D3D11 decoded frame transfer failed: %s", error);
						av_frame_unref(d.frame);
						continue;
					}
					av_frame_copy_props(d.transfer_frame, d.frame);
					decoded = d.transfer_frame;
				}
				const auto timestamp = decoded->pts != AV_NOPTS_VALUE && decoded->pts >= 0
					? static_cast<std::uint64_t>(decoded->pts) : pts;
				if (!d.first_frame_logged) {
					if (is_video && decoded->data[0]) {
						const char *pixel_format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(decoded->format));
						blog(LOG_INFO, "DuoCast: decoded first %s frame (%dx%d, format %s)",
						     video_codec_label(d.codec_id), decoded->width, decoded->height,
						     pixel_format ? pixel_format : "unknown");
						d.first_frame_logged = true;
					} else if (!is_video && decoded->nb_samples > 0 && decoded->data[0]) {
						blog(LOG_INFO, "DuoCast: decoded first %s AAC frame (%d samples)",
						     kind == StreamKind::LiveAudio ? "live" : "VOD", decoded->nb_samples);
						d.first_frame_logged = true;
					}
				}
				if (is_video) deliver_video(d, decoded, timestamp, kind);
				else deliver_audio(d, decoded, kind, timestamp);
				if (decoded == d.transfer_frame) av_frame_unref(d.transfer_frame);
				av_frame_unref(d.frame);
			}
			if (receive_result != AVERROR(EAGAIN) && receive_result != AVERROR_EOF) {
				char error[AV_ERROR_MAX_STRING_SIZE]{};
				av_strerror(receive_result, error, sizeof(error));
				blog(LOG_WARNING, "DuoCast: %s decoder receive failed: %s",
				     is_video ? video_codec_label(d.codec_id) : "AAC", error);
			}
		}
		av_packet_free(&packet);
	}

	void decode_jpeg(Decoder &d, StreamKind kind, const std::vector<std::uint8_t> &payload, std::uint64_t timestamp)
	{
		if (!d.jpeg || payload.empty() || payload.size() > obs_duocast::protocol::kMaxPayload) return;
		if (tj3DecompressHeader(d.jpeg, payload.data(), payload.size()) < 0) {
			blog(LOG_WARNING, "DuoCast: TurboJPEG rejected a video frame header: %s", tj3GetErrorStr(d.jpeg));
			return;
		}
		const int width = tj3Get(d.jpeg, TJPARAM_JPEGWIDTH);
		const int height = tj3Get(d.jpeg, TJPARAM_JPEGHEIGHT);
		const int subsamp = tj3Get(d.jpeg, TJPARAM_SUBSAMP);
		if (width <= 0 || height <= 0 || static_cast<std::uint32_t>(width) != d.width ||
		    static_cast<std::uint32_t>(height) != d.height || subsamp != TJSAMP_420)
			return;
		int strides[] = {width, width / 2, width / 2};
		for (int plane = 0; plane < 3; ++plane) {
			const auto size = tj3YUVPlaneSize(plane, width, strides[plane], height, subsamp);
			if (!size) return;
			d.jpeg_planes[plane].resize(size);
		}
		unsigned char *planes[] = {d.jpeg_planes[0].data(), d.jpeg_planes[1].data(), d.jpeg_planes[2].data()};
		if (tj3DecompressToYUVPlanes8(d.jpeg, payload.data(), payload.size(), planes, strides) < 0) {
			blog(LOG_WARNING, "DuoCast: TurboJPEG frame decode failed: %s", tj3GetErrorStr(d.jpeg));
			return;
		}
		if (!d.first_frame_logged) {
			blog(LOG_INFO, "DuoCast: TurboJPEG decoded first %s frame (%dx%d)",
			     kind == StreamKind::VerticalVideo ? "vertical" : "landscape", width, height);
			d.first_frame_logged = true;
		}
		deliver_video_planes(d.jpeg_planes[0].data(), d.jpeg_planes[1].data(), d.jpeg_planes[2].data(),
				    strides, d.width, d.height, timestamp, kind,
				    d.has_jpeg_color_params ? d.jpeg_range : VIDEO_RANGE_FULL,
				    VIDEO_CS_709,
				    d.has_jpeg_color_params ? d.jpeg_matrix : nullptr,
				    d.has_jpeg_color_params ? d.jpeg_range_min : nullptr,
				    d.has_jpeg_color_params ? d.jpeg_range_max : nullptr);
	}

	void deliver_video(Decoder &d, AVFrame *decoded, std::uint64_t timestamp, StreamKind kind)
	{
		if (!decoded->data[0] || !decoded->data[1]) return;
		// Follow Teleport's source handoff model: pass decoded planes through
		// OBS's frame2 API with complete color metadata. The legacy frame API
		// otherwise receives zeroed conversion parameters and renders black.
		const auto range = decoded->format == AV_PIX_FMT_YUVJ420P || decoded->color_range == AVCOL_RANGE_JPEG
			? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		auto colorspace = VIDEO_CS_709;
		if (decoded->color_trc == AVCOL_TRC_SMPTE2084) colorspace = VIDEO_CS_2100_PQ;
		else if (decoded->color_trc == AVCOL_TRC_ARIB_STD_B67) colorspace = VIDEO_CS_2100_HLG;
		else if (decoded->colorspace == AVCOL_SPC_BT470BG || decoded->colorspace == AVCOL_SPC_SMPTE170M)
			colorspace = VIDEO_CS_601;
		else if (decoded->colorspace == AVCOL_SPC_BT709) colorspace = VIDEO_CS_709;
		if (decoded->format == AV_PIX_FMT_NV12 && !(decoded->width & 1) && !(decoded->height & 1)) {
			std::vector<std::uint8_t> u(static_cast<std::size_t>(decoded->width) * decoded->height / 4);
			std::vector<std::uint8_t> v(u.size());
			for (std::uint32_t row = 0; row < decoded->height / 2; ++row) {
				const auto *uv = decoded->data[1] + static_cast<std::ptrdiff_t>(row) * decoded->linesize[1];
				auto *u_row = u.data() + static_cast<std::size_t>(row) * decoded->width / 2;
				auto *v_row = v.data() + static_cast<std::size_t>(row) * decoded->width / 2;
				for (std::uint32_t col = 0; col < decoded->width / 2; ++col) {
					u_row[col] = uv[col * 2];
					v_row[col] = uv[col * 2 + 1];
				}
			}
			const int strides[] = {decoded->linesize[0], static_cast<int>(decoded->width / 2),
					       static_cast<int>(decoded->width / 2)};
			deliver_video_planes(decoded->data[0], u.data(), v.data(), strides, decoded->width,
					    decoded->height, timestamp, kind, range, colorspace);
			return;
		}
		if ((decoded->format != AV_PIX_FMT_YUV420P && decoded->format != AV_PIX_FMT_YUVJ420P) || !decoded->data[2])
			return;
		deliver_video_planes(decoded->data[0], decoded->data[1], decoded->data[2], decoded->linesize,
				    decoded->width, decoded->height, timestamp, kind,
				    d.has_jpeg_color_params ? d.jpeg_range : range, colorspace,
				    d.has_jpeg_color_params ? d.jpeg_matrix : nullptr,
				    d.has_jpeg_color_params ? d.jpeg_range_min : nullptr,
				    d.has_jpeg_color_params ? d.jpeg_range_max : nullptr);
	}

	void deliver_video_planes(std::uint8_t *y, std::uint8_t *u, std::uint8_t *v,
			   const int *strides, std::uint32_t width, std::uint32_t height,
			   std::uint64_t timestamp, StreamKind kind, video_range_type range,
			   video_colorspace colorspace = VIDEO_CS_709, const float *matrix = nullptr,
			   const float *range_min = nullptr, const float *range_max = nullptr)
	{
		if (kind == StreamKind::VerticalVideo) delivered_vertical_frames_.fetch_add(1);
		else delivered_landscape_frames_.fetch_add(1);
		struct obs_source_frame2 frame{};
		frame.data[0] = y; frame.data[1] = u; frame.data[2] = v;
		frame.linesize[0] = static_cast<std::uint32_t>(strides[0]);
		frame.linesize[1] = static_cast<std::uint32_t>(strides[1]);
		frame.linesize[2] = static_cast<std::uint32_t>(strides[2]);
		frame.width = width; frame.height = height;
		frame.timestamp = timestamp; frame.format = VIDEO_FORMAT_I420; frame.range = range;
		if (matrix && range_min && range_max) {
			std::copy_n(matrix, 16, frame.color_matrix);
			std::copy_n(range_min, 3, frame.color_range_min);
			std::copy_n(range_max, 3, frame.color_range_max);
		} else {
			video_format_get_parameters_for_format(colorspace, range, VIDEO_FORMAT_I420,
						       frame.color_matrix, frame.color_range_min, frame.color_range_max);
		}
		std::lock_guard<std::mutex> lock(sinksMutex_);
		const auto sink = kind == StreamKind::VerticalVideo ? SinkKind::Vertical : SinkKind::Video;
		for (auto *source : sinks_[index(sink)]) obs_source_output_video2(source, &frame);
		const auto previewSink = kind == StreamKind::VerticalVideo ? SinkKind::PreviewVertical : SinkKind::PreviewVideo;
		for (auto *source : sinks_[index(previewSink)]) obs_source_output_video2(source, &frame);
	}

	void deliver_audio(Decoder &decoder, AVFrame *decoded, StreamKind kind, std::uint64_t timestamp)
	{
		if (decoded->sample_rate == 0 || decoded->nb_samples <= 0 || decoded->ch_layout.nb_channels < 1) return;
		struct obs_source_audio audio{};
		std::vector<float> downmixed;
		const std::uint8_t *planes[MAX_AV_PLANES]{};
		int channels = decoded->ch_layout.nb_channels;
		if (decoded->format == AV_SAMPLE_FMT_FLTP && channels <= 2) {
			for (int c = 0; c < channels; ++c) planes[c] = decoded->extended_data[c];
		} else {
			AVChannelLayout target{};
			av_channel_layout_default(&target, channels == 1 ? 1 : 2);
			if (!decoder.resampler) {
				const int setup = swr_alloc_set_opts2(&decoder.resampler, &target, AV_SAMPLE_FMT_FLTP,
								      decoded->sample_rate, &decoded->ch_layout,
								      static_cast<AVSampleFormat>(decoded->format),
								      decoded->sample_rate, 0, nullptr);
				if (setup < 0 || swr_init(decoder.resampler) < 0) {
					av_channel_layout_uninit(&target);
					return;
				}
			}
			if (!decoder.resampler) {
				av_channel_layout_uninit(&target);
				return;
			}
			channels = channels == 1 ? 1 : 2;
			const int capacity = static_cast<int>(decoded->nb_samples + swr_get_delay(decoder.resampler, decoded->sample_rate) + 64);
			downmixed.resize(static_cast<std::size_t>(capacity) * channels);
			std::uint8_t *out[MAX_AV_PLANES]{};
			for (int c = 0; c < channels; ++c)
				out[c] = reinterpret_cast<std::uint8_t *>(downmixed.data() + static_cast<std::size_t>(c) * capacity);
			const int converted = swr_convert(decoder.resampler, out, capacity,
							  const_cast<const std::uint8_t **>(decoded->extended_data), decoded->nb_samples);
			av_channel_layout_uninit(&target);
			if (converted <= 0) return;
			audio.frames = static_cast<std::uint32_t>(converted);
			for (int c = 0; c < channels; ++c) planes[c] = out[c];
		}
		if (!audio.frames) audio.frames = static_cast<std::uint32_t>(decoded->nb_samples);
		audio.timestamp = timestamp;
		audio.samples_per_sec = static_cast<std::uint32_t>(decoded->sample_rate);
		audio.speakers = channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
		audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
		for (int c = 0; c < channels; ++c) audio.data[c] = planes[c];
		std::lock_guard<std::mutex> lock(sinksMutex_);
		if (kind == StreamKind::LiveAudio) {
			for (auto *source : sinks_[index(SinkKind::Video)]) obs_source_output_audio(source, &audio);
			for (auto *source : sinks_[index(SinkKind::Vertical)]) obs_source_output_audio(source, &audio);
		} else {
			for (auto *source : sinks_[index(SinkKind::Vod)]) obs_source_output_audio(source, &audio);
		}
	}

	int port_;
	std::atomic<SOCKET> listener_{INVALID_SOCKET};
	std::atomic<SOCKET> client_{INVALID_SOCKET};
	std::atomic<bool> stopping_{false};
	std::mutex playout_mutex_;
	std::condition_variable playout_ready_;
	std::multimap<std::uint64_t, QueuedMedia> playout_queue_;
	std::size_t playout_queue_bytes_ = 0;
	bool playout_stopping_ = false;
	int active_playout_delay_ms_ = 1000;
	std::thread playout_worker_;
	std::mutex decoder_mutex_;
	std::mutex lifecycle_mutex_;
	std::thread worker_;
	mutable std::mutex peer_mutex_;
	std::string peer_address_;
	mutable std::mutex sinksMutex_;
		std::array<std::vector<obs_source_t *>, 5> sinks_;
	std::atomic<std::uint32_t> width_{1280}, height_{720};
	std::atomic<std::uint32_t> vertical_width_{720}, vertical_height_{1280};
	std::atomic<std::uint64_t> connection_started_ns_{0}, last_video_ns_{0}, last_vertical_video_ns_{0};
	std::atomic<bool> vertical_feed_configured_{false};
	std::atomic<std::uint64_t> delivered_landscape_frames_{0};
	std::atomic<std::uint64_t> delivered_vertical_frames_{0};
	bool auto_recovery_armed_ = true;
	Decoder video_, vertical_video_, live_, vod_;
};

std::shared_ptr<ReceiverSession> get_session(int port)
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	if (auto existing = g_sessions[port].lock()) return existing;
	auto session = std::make_shared<ReceiverSession>(port);
	g_sessions[port] = session;
	return session;
}

struct ReceiverState { std::shared_ptr<ReceiverSession> session; obs_source_t *source = nullptr; SinkKind kind; };

const char *video_name(void *) { return "DuoCast Video Source"; }
const char *vertical_video_name(void *) { return "DuoCast Vertical Video Source"; }
const char *preview_video_name(void *) { return "DuoCast Receiver Preview - Landscape"; }
const char *preview_vertical_name(void *) { return "DuoCast Receiver Preview - Vertical"; }
const char *vod_name(void *) { return "DuoCast Audio VOD Track Mix Source"; }

obs_properties_t *receiver_properties(void *)
{
	auto *props = obs_properties_create();
	obs_properties_add_int(props, "port", "Listen port", 1, 65535, 1);
	return props;
}
void receiver_defaults(obs_data_t *settings) { obs_data_set_default_int(settings, "port", 17777); }
void *receiver_create_kind(obs_data_t *settings, obs_source_t *source, SinkKind kind)
{
	auto *state = new ReceiverState();
	state->source = source; state->kind = kind;
	const int port = std::clamp(static_cast<int>(obs_data_get_int(settings, "port")), 1, 65535);
	state->session = get_session(port);
	state->session->add_source(kind, source);
	return state;
}
void destroy_receiver(void *opaque)
{
	auto *state = static_cast<ReceiverState *>(opaque);
	if (!state) return;
	state->session->remove_source(state->kind, state->source);
	delete state;
}
void *create_video(obs_data_t *s, obs_source_t *src) { return receiver_create_kind(s, src, SinkKind::Video); }
void *create_vertical_video(obs_data_t *s, obs_source_t *src) { return receiver_create_kind(s, src, SinkKind::Vertical); }
void *create_preview_video(obs_data_t *s, obs_source_t *src) { return receiver_create_kind(s, src, SinkKind::PreviewVideo); }
void *create_preview_vertical(obs_data_t *s, obs_source_t *src) { return receiver_create_kind(s, src, SinkKind::PreviewVertical); }
void *create_vod(obs_data_t *s, obs_source_t *src) { return receiver_create_kind(s, src, SinkKind::Vod); }
uint32_t receiver_width(void *opaque) { const auto *s = static_cast<ReceiverState *>(opaque); return s->session->width(s->kind); }
uint32_t receiver_height(void *opaque) { const auto *s = static_cast<ReceiverState *>(opaque); return s->session->height(s->kind); }

void register_receiver(const char *id, const char *(*name)(void *), void *(*create)(obs_data_t *, obs_source_t *),
		      std::uint32_t flags)
{
	static obs_source_info infos[6]{};
	static size_t idx = 0;
	auto &info = infos[idx++];
	info.id = id; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = flags;
	info.get_name = name; info.create = create; info.destroy = destroy_receiver;
	info.get_properties = receiver_properties; info.get_defaults = receiver_defaults;
	if (flags & OBS_SOURCE_ASYNC_VIDEO) { info.get_width = receiver_width; info.get_height = receiver_height; }
	obs_register_source(&info);
}
} // namespace

void register_receiver_source()
{
	register_receiver("obs_duocast_video", video_name, create_video, OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO);
	register_receiver("obs_duocast_vertical_video", vertical_video_name, create_vertical_video, OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO);
	register_receiver("obs_duocast_preview_video", preview_video_name, create_preview_video, OBS_SOURCE_ASYNC_VIDEO);
	register_receiver("obs_duocast_preview_vertical", preview_vertical_name, create_preview_vertical, OBS_SOURCE_ASYNC_VIDEO);
	register_receiver("obs_duocast_vod_mix", vod_name, create_vod, OBS_SOURCE_AUDIO);
}

obs_source_t *receiver_preview_source(int port, bool vertical)
{
	auto *settings = obs_data_create();
	obs_data_set_int(settings, "port", std::clamp(port, 1, 65535));
	auto *source = obs_source_create_private(vertical ? "obs_duocast_preview_vertical" : "obs_duocast_preview_video",
						 vertical ? "DuoCast Receiver Preview - Vertical" : "DuoCast Receiver Preview - Landscape",
						 settings);
	obs_data_release(settings);
	return source;
}

void receiver_set_enabled(bool enabled)
{
	g_receiverEnabled.store(enabled);
	std::vector<std::shared_ptr<ReceiverSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(g_sessionsMutex);
		for (auto it = g_sessions.begin(); it != g_sessions.end();) {
			if (auto session = it->second.lock()) {
				sessions.push_back(std::move(session));
				++it;
			} else {
				it = g_sessions.erase(it);
			}
		}
	}
	for (const auto &session : sessions) {
		session->set_enabled(enabled);
	}
}

void receiver_set_sync_buffer_ms(int delay_ms)
{
	g_receiverSyncBufferMs.store(std::clamp(delay_ms, 0, 3000));
}

void receiver_set_auto_recovery_enabled(bool enabled)
{
	g_autoRecoverLowFps.store(enabled);
	if (!enabled) g_requestSenderRecovery.store(false);
}

void receiver_update_remote_stats(const obs_duocast::protocol::ReceiverStats &stats)
{
	std::lock_guard<std::mutex> lock(g_remoteStatsMutex);
	g_remoteStats = stats;
}

bool receiver_video_connection_seen()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			if (session->connection_seen()) return true;
			++it;
		} else it = g_sessions.erase(it);
	}
	return false;
}

bool receiver_video_feed_healthy()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			if (session->video_healthy()) return true;
			++it;
		} else it = g_sessions.erase(it);
	}
	return false;
}

bool receiver_vertical_feed_configured()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			if (session->vertical_feed_configured()) return true;
			++it;
		} else it = g_sessions.erase(it);
	}
	return false;
}

bool receiver_vertical_feed_healthy()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			if (session->vertical_video_healthy()) return true;
			++it;
		} else it = g_sessions.erase(it);
	}
	return false;
}

bool receiver_landscape_feed_healthy()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			if (session->landscape_video_healthy()) return true;
			++it;
		} else it = g_sessions.erase(it);
	}
	return false;
}

std::string receiver_active_peer_address()
{
	std::lock_guard<std::mutex> lock(g_sessionsMutex);
	for (auto it = g_sessions.begin(); it != g_sessions.end();) {
		if (auto session = it->second.lock()) {
			const auto peer = session->active_peer_address();
			if (!peer.empty()) return peer;
			++it;
		} else {
			it = g_sessions.erase(it);
		}
	}
	return {};
}
