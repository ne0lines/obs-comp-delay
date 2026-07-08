#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace comp_delay {

enum class EncodedPacketKind {
	Video,
	Audio,
};

struct EncodedPacket {
	EncodedPacketKind kind = EncodedPacketKind::Video;
	uint64_t timestampNs = 0;
	uint64_t durationNs = 0;
	bool keyframe = false;
	std::vector<uint8_t> data;
	std::vector<uint8_t> codecConfig;
};

struct EncodedDecodeBatch {
	uint64_t targetTimestampNs = 0;
	uint64_t decodeStartTimestampNs = 0;
	std::vector<EncodedPacket> packets;
};

class EncodedRingBuffer {
public:
	EncodedRingBuffer(uint64_t maxDurationNs, size_t byteCap);

	void push(EncodedPacket packet);
	void clear();
	void setLimits(uint64_t maxDurationNs, size_t byteCap);
	void dropBefore(uint64_t timestampNs);

	uint64_t durationNs() const;
	uint32_t durationSecondsFloor() const;
	bool hasDepth(uint32_t seconds) const;
	std::optional<uint64_t> targetTimestampForDelay(uint32_t seconds) const;
	std::optional<uint64_t> videoKeyframeAtOrBefore(uint64_t timestampNs) const;
	std::optional<EncodedDecodeBatch> decodeBatchForDelay(uint32_t seconds, uint64_t maxDurationNs) const;
	std::vector<EncodedPacket> packetsReadySince(EncodedPacketKind kind, uint32_t delaySeconds,
						     uint64_t lastTimestampNs, bool haveLastTimestamp,
						     size_t maxPackets) const;
	size_t byteSize() const;
	size_t packetCount() const;
	size_t keyframeCount() const;
	size_t byteCap() const;

private:
	void evictLocked();
	void dropBeforeLocked(uint64_t timestampNs);
	uint64_t durationNsLocked() const;

	mutable std::mutex mutex_;
	std::deque<EncodedPacket> packets_;
	uint64_t maxDurationNs_ = 0;
	size_t byteCap_ = 0;
	size_t byteSize_ = 0;
};

} // namespace comp_delay
