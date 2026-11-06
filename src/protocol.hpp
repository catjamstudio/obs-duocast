#pragma once

#include <cstddef>
#include <cstdint>

namespace obs_duocast::protocol {

inline constexpr std::uint32_t kMagic = 0x44554353; // DUCS
inline constexpr std::uint16_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 32;
// High-resolution JPEG frames can be much larger than compressed H.264 packets.
inline constexpr std::uint32_t kMaxPayload = 64 * 1024 * 1024;

enum class StreamKind : std::uint8_t {
	VideoConfig = 1,
	LiveAudioConfig = 2,
	VodAudioConfig = 3,
	Video = 4,
	LiveAudio = 5,
	VodAudio = 6,
	Heartbeat = 7,
	VerticalVideoConfig = 8,
	VerticalVideo = 9,
	ReceiverStats = 10,
};

// Lightweight receiver-side OBS statistics returned over the existing TCP
// connection. Integer scaling keeps the wire format stable and compact.
struct ReceiverStats {
	std::uint32_t flags = 0; // bit 0: streaming, bit 1: recording, bit 2: reconnecting, bit 3: request sender feed recovery
	std::uint32_t fps_milli = 0;
	std::uint32_t cpu_tenths_percent = 0;
	std::uint32_t memory_mb = 0;
	std::uint32_t render_time_tenths_ms = 0;
	std::uint32_t render_lagged = 0;
	std::uint32_t render_total = 0;
	std::uint32_t encode_skipped = 0;
	std::uint32_t encode_total = 0;
	std::uint32_t stream_dropped = 0;
	std::uint32_t stream_total = 0;
	std::uint32_t stream_bitrate_kbps = 0;
	std::uint32_t disk_free_tenths_gb = 0;
	std::uint64_t stream_bytes = 0;
	std::uint64_t recording_bytes = 0;
};
inline constexpr std::size_t kReceiverStatsPayloadSize = 68;
void encode_receiver_stats(const ReceiverStats &stats, std::uint8_t (&bytes)[kReceiverStatsPayloadSize]);
bool decode_receiver_stats(const std::uint8_t *bytes, std::size_t size, ReceiverStats &stats);

// Wire fields are serialized explicitly in network byte order; this struct is
// the logical header and must never be written directly as a C++ object.
struct PacketHeader {
	std::uint32_t magic = kMagic;
	std::uint16_t version = kVersion;
	StreamKind kind = StreamKind::Heartbeat;
	std::uint8_t flags = 0;
	std::uint32_t sequence = 0;
	std::uint64_t pts_ns = 0;
	std::uint64_t dts_ns = 0;
	std::uint32_t payload_size = 0;
};

void encode_header(const PacketHeader &header, std::uint8_t (&bytes)[kHeaderSize]);
bool decode_header(const std::uint8_t (&bytes)[kHeaderSize], PacketHeader &header);

} // namespace obs_duocast::protocol
