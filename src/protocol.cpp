#include "protocol.hpp"

#include <cstring>

namespace obs_duocast::protocol {
namespace {
void put16(std::uint8_t *out, std::uint16_t v)
{
	out[0] = static_cast<std::uint8_t>(v >> 8);
	out[1] = static_cast<std::uint8_t>(v);
}
void put32(std::uint8_t *out, std::uint32_t v)
{
	for (int i = 0; i < 4; ++i)
		out[i] = static_cast<std::uint8_t>(v >> (24 - i * 8));
}
void put64(std::uint8_t *out, std::uint64_t v)
{
	for (int i = 0; i < 8; ++i)
		out[i] = static_cast<std::uint8_t>(v >> (56 - i * 8));
}
std::uint16_t get16(const std::uint8_t *p) { return (std::uint16_t(p[0]) << 8) | p[1]; }
std::uint32_t get32(const std::uint8_t *p)
{
	return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}
std::uint64_t get64(const std::uint8_t *p)
{
	return (std::uint64_t(get32(p)) << 32) | get32(p + 4);
}
} // namespace

void encode_header(const PacketHeader &h, std::uint8_t (&out)[kHeaderSize])
{
	put32(out, h.magic);
	put16(out + 4, h.version);
	out[6] = static_cast<std::uint8_t>(h.kind);
	out[7] = h.flags;
	put32(out + 8, h.sequence);
	put64(out + 12, h.pts_ns);
	put64(out + 20, h.dts_ns);
	put32(out + 28, h.payload_size);
}

bool decode_header(const std::uint8_t (&in)[kHeaderSize], PacketHeader &h)
{
	h.magic = get32(in);
	h.version = get16(in + 4);
	h.kind = static_cast<StreamKind>(in[6]);
	h.flags = in[7];
	h.sequence = get32(in + 8);
	h.pts_ns = get64(in + 12);
	h.dts_ns = get64(in + 20);
	h.payload_size = get32(in + 28);
	return h.magic == kMagic && h.version == kVersion && h.payload_size <= kMaxPayload &&
	       static_cast<std::uint8_t>(h.kind) >= static_cast<std::uint8_t>(StreamKind::VideoConfig) &&
	       static_cast<std::uint8_t>(h.kind) <= static_cast<std::uint8_t>(StreamKind::ReceiverStats);
}

void encode_receiver_stats(const ReceiverStats &s, std::uint8_t (&out)[kReceiverStatsPayloadSize])
{
	const std::uint32_t fields[] = {s.flags, s.fps_milli, s.cpu_tenths_percent, s.memory_mb,
		s.render_time_tenths_ms, s.render_lagged, s.render_total, s.encode_skipped, s.encode_total,
		s.stream_dropped, s.stream_total, s.stream_bitrate_kbps, s.disk_free_tenths_gb};
	for (std::size_t i = 0; i < 13; ++i) put32(out + i * 4, fields[i]);
	put64(out + 52, s.stream_bytes);
	put64(out + 60, s.recording_bytes);
}

bool decode_receiver_stats(const std::uint8_t *in, std::size_t size, ReceiverStats &s)
{
	if (!in || size != kReceiverStatsPayloadSize) return false;
	std::uint32_t *fields[] = {&s.flags, &s.fps_milli, &s.cpu_tenths_percent, &s.memory_mb,
		&s.render_time_tenths_ms, &s.render_lagged, &s.render_total, &s.encode_skipped, &s.encode_total,
		&s.stream_dropped, &s.stream_total, &s.stream_bitrate_kbps, &s.disk_free_tenths_gb};
	for (std::size_t i = 0; i < 13; ++i) *fields[i] = get32(in + i * 4);
	s.stream_bytes = get64(in + 52);
	s.recording_bytes = get64(in + 60);
	return true;
}
} // namespace obs_duocast::protocol
