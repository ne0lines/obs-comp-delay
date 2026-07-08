#include "encoded-ring-buffer.hpp"

#include <algorithm>
#include <utility>

namespace comp_delay {

EncodedRingBuffer::EncodedRingBuffer(uint64_t maxDurationNs, size_t byteCap) : maxDurationNs_(maxDurationNs), byteCap_(byteCap) {}

void EncodedRingBuffer::push(EncodedPacket packet)
{
	std::lock_guard<std::mutex> lock(mutex_);
	byteSize_ += packet.data.size() + packet.codecConfig.size();
	if (packets_.empty() || packet.timestampNs >= packets_.back().timestampNs) {
		packets_.push_back(std::move(packet));
	} else {
		auto insertAfter = std::find_if(packets_.rbegin(), packets_.rend(), [&packet](const EncodedPacket &existing) {
			return existing.timestampNs <= packet.timestampNs;
		});
		if (insertAfter == packets_.rend())
			packets_.push_front(std::move(packet));
		else
			packets_.insert(insertAfter.base(), std::move(packet));
	}
	evictLocked();
}

void EncodedRingBuffer::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	packets_.clear();
	byteSize_ = 0;
}

void EncodedRingBuffer::setLimits(uint64_t maxDurationNs, size_t byteCap)
{
	std::lock_guard<std::mutex> lock(mutex_);
	maxDurationNs_ = maxDurationNs;
	byteCap_ = byteCap;
	evictLocked();
}

void EncodedRingBuffer::dropBefore(uint64_t timestampNs)
{
	std::lock_guard<std::mutex> lock(mutex_);
	dropBeforeLocked(timestampNs);
}

uint64_t EncodedRingBuffer::durationNs() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return durationNsLocked();
}

uint32_t EncodedRingBuffer::durationSecondsFloor() const
{
	return static_cast<uint32_t>(durationNs() / 1000000000ULL);
}

bool EncodedRingBuffer::hasDepth(uint32_t seconds) const
{
	return durationNs() >= static_cast<uint64_t>(seconds) * 1000000000ULL;
}

std::optional<uint64_t> EncodedRingBuffer::targetTimestampForDelay(uint32_t seconds) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (packets_.empty())
		return std::nullopt;

	const auto &back = packets_.back();
	const uint64_t latestEnd = back.timestampNs + back.durationNs;
	const uint64_t delayNs = static_cast<uint64_t>(seconds) * 1000000000ULL;
	if (latestEnd <= delayNs)
		return packets_.front().timestampNs;

	return latestEnd - delayNs;
}

std::optional<uint64_t> EncodedRingBuffer::videoKeyframeAtOrBefore(uint64_t timestampNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::optional<uint64_t> keyframeTimestamp;
	for (const auto &packet : packets_) {
		if (packet.timestampNs > timestampNs)
			break;
		if (packet.kind == EncodedPacketKind::Video && packet.keyframe)
			keyframeTimestamp = packet.timestampNs;
	}

	return keyframeTimestamp;
}

std::optional<EncodedDecodeBatch> EncodedRingBuffer::decodeBatchForDelay(uint32_t seconds, uint64_t maxDurationNs) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (packets_.empty())
		return std::nullopt;

	const auto &back = packets_.back();
	const uint64_t latestEnd = back.timestampNs + back.durationNs;
	const uint64_t delayNs = static_cast<uint64_t>(seconds) * 1000000000ULL;
	const uint64_t targetTimestamp = latestEnd > delayNs ? latestEnd - delayNs : packets_.front().timestampNs;

	std::optional<uint64_t> decodeStart;
	for (const auto &packet : packets_) {
		if (packet.timestampNs > targetTimestamp)
			break;
		if (packet.kind == EncodedPacketKind::Video && packet.keyframe)
			decodeStart = packet.timestampNs;
	}
	if (!decodeStart)
		return std::nullopt;

	EncodedDecodeBatch batch;
	batch.targetTimestampNs = targetTimestamp;
	batch.decodeStartTimestampNs = *decodeStart;

	const uint64_t decodeEnd = maxDurationNs > 0 ? targetTimestamp + maxDurationNs : targetTimestamp;
	for (const auto &packet : packets_) {
		if (packet.timestampNs < *decodeStart)
			continue;
		if (packet.timestampNs > decodeEnd)
			break;
		batch.packets.push_back(packet);
	}

	if (batch.packets.empty())
		return std::nullopt;

	return batch;
}

std::vector<EncodedPacket> EncodedRingBuffer::packetsReadySince(EncodedPacketKind kind, uint32_t delaySeconds,
								uint64_t lastTimestampNs,
								bool haveLastTimestamp, size_t maxPackets) const
{
	std::vector<EncodedPacket> ready;
	if (maxPackets == 0)
		return ready;

	std::lock_guard<std::mutex> lock(mutex_);
	if (packets_.empty())
		return ready;

	const auto &back = packets_.back();
	const uint64_t latestEnd = back.timestampNs + back.durationNs;
	const uint64_t delayNs = static_cast<uint64_t>(delaySeconds) * 1000000000ULL;
	const uint64_t targetTimestamp = latestEnd > delayNs ? latestEnd - delayNs : packets_.front().timestampNs;

	for (const auto &packet : packets_) {
		if (packet.timestampNs > targetTimestamp)
			break;
		if (packet.kind != kind)
			continue;
		if (haveLastTimestamp && packet.timestampNs <= lastTimestampNs)
			continue;

		ready.push_back(packet);
		if (ready.size() >= maxPackets)
			break;
	}

	return ready;
}

size_t EncodedRingBuffer::byteSize() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return byteSize_;
}

size_t EncodedRingBuffer::packetCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return packets_.size();
}

size_t EncodedRingBuffer::keyframeCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	size_t count = 0;
	for (const auto &packet : packets_) {
		if (packet.kind == EncodedPacketKind::Video && packet.keyframe)
			++count;
	}
	return count;
}

size_t EncodedRingBuffer::byteCap() const
{
	return byteCap_;
}

void EncodedRingBuffer::evictLocked()
{
	while (!packets_.empty() && (byteSize_ > byteCap_ || durationNsLocked() > maxDurationNs_)) {
		byteSize_ -= std::min(byteSize_, packets_.front().data.size() + packets_.front().codecConfig.size());
		packets_.pop_front();
	}
}

void EncodedRingBuffer::dropBeforeLocked(uint64_t timestampNs)
{
	while (!packets_.empty()) {
		const auto &front = packets_.front();
		const uint64_t packetEnd = front.timestampNs + front.durationNs;
		if (packetEnd > timestampNs)
			break;

		byteSize_ -= std::min(byteSize_, front.data.size() + front.codecConfig.size());
		packets_.pop_front();
	}
}

uint64_t EncodedRingBuffer::durationNsLocked() const
{
	if (packets_.size() < 2)
		return 0;

	const auto &front = packets_.front();
	const auto &back = packets_.back();
	const uint64_t backEnd = back.timestampNs + back.durationNs;
	return backEnd > front.timestampNs ? backEnd - front.timestampNs : 0;
}

} // namespace comp_delay
