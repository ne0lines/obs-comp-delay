#include "core/delay-settings.hpp"
#include "core/delay-state-machine.hpp"
#include "core/encoded-ring-buffer.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

using namespace comp_delay;

static EncodedPacket packet(uint64_t timestampNs, size_t bytes)
{
	EncodedPacket pkt;
	pkt.kind = EncodedPacketKind::Video;
	pkt.timestampNs = timestampNs;
	pkt.durationNs = 1000000000ULL;
	pkt.keyframe = true;
	pkt.data = std::vector<uint8_t>(bytes, 0x1);
	return pkt;
}

static EncodedPacket videoPacket(uint64_t timestampNs, bool keyframe)
{
	EncodedPacket pkt = packet(timestampNs, 10);
	pkt.keyframe = keyframe;
	return pkt;
}

static EncodedPacket audioPacket(uint64_t timestampNs)
{
	EncodedPacket pkt;
	pkt.kind = EncodedPacketKind::Audio;
	pkt.timestampNs = timestampNs;
	pkt.durationNs = 20000000ULL;
	pkt.data = std::vector<uint8_t>(4, 0x2);
	return pkt;
}

static void testDelayClamp()
{
	assert(clampDelaySeconds(0) == 0);
	assert(clampDelaySeconds(60) == 60);
	assert(clampDelaySeconds(301) == kMaxDelaySeconds);
}

static void testEncodedRetentionIncludesPreroll()
{
	DelaySettings settings;
	settings.targetDelaySeconds = 300;
	settings.keyframeIntervalSeconds = 2;
	assert(encodedRetentionSeconds(settings) == 304);

	settings.targetDelaySeconds = 999;
	settings.keyframeIntervalSeconds = 0;
	assert(encodedRetentionSeconds(settings) == 303);
}

static void testStateMachine()
{
	DelayStateMachine machine;
	assert(machine.state() == RuntimeState::Off);

	machine.requestDelay(60);
	assert(machine.state() == RuntimeState::Filling);
	assert(machine.targetDelaySeconds() == 60);

	machine.updateBufferDepth(59);
	assert(machine.state() == RuntimeState::Filling);

	machine.updateBufferDepth(60);
	assert(machine.state() == RuntimeState::Delayed);
	assert(machine.activeDelaySeconds() == 60);

	machine.goLive();
	assert(machine.state() == RuntimeState::Off);
	assert(machine.targetDelaySeconds() == 0);
}

static void testRingBufferEvictsByTime()
{
	EncodedRingBuffer buffer(3ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(packet(0, 10));
	buffer.push(packet(1000000000ULL, 10));
	buffer.push(packet(2000000000ULL, 10));
	buffer.push(packet(3000000000ULL, 10));
	assert(buffer.durationSecondsFloor() <= 3);
}

static void testRingBufferLimitsCanGrow()
{
	EncodedRingBuffer buffer(3ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(packet(0, 10));
	buffer.push(packet(1000000000ULL, 10));
	buffer.push(packet(2000000000ULL, 10));
	buffer.push(packet(3000000000ULL, 10));
	assert(buffer.durationSecondsFloor() <= 3);

	buffer.setLimits(6ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(packet(4000000000ULL, 10));
	buffer.push(packet(5000000000ULL, 10));
	buffer.push(packet(6000000000ULL, 10));
	assert(buffer.durationSecondsFloor() > 3);
}

static void testRingBufferEvictsByBytes()
{
	EncodedRingBuffer buffer(300ULL * 1000000000ULL, 100);
	buffer.push(packet(0, 60));
	buffer.push(packet(1000000000ULL, 60));
	assert(buffer.byteSize() <= 100);
}

static void testRingBufferFindsDecodeKeyframeForDelay()
{
	EncodedRingBuffer buffer(300ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(videoPacket(0, true));
	buffer.push(audioPacket(10000000ULL));
	buffer.push(videoPacket(1000000000ULL, false));
	buffer.push(videoPacket(2000000000ULL, true));
	buffer.push(audioPacket(2010000000ULL));
	buffer.push(videoPacket(3000000000ULL, false));
	buffer.push(videoPacket(4000000000ULL, true));

	const auto target = buffer.targetTimestampForDelay(2);
	assert(target);
	assert(*target == 3000000000ULL);

	const auto keyframe = buffer.videoKeyframeAtOrBefore(*target);
	assert(keyframe);
	assert(*keyframe == 2000000000ULL);

	const auto batch = buffer.decodeBatchForDelay(2, 1000000000ULL);
	assert(batch);
	assert(batch->targetTimestampNs == 3000000000ULL);
	assert(batch->decodeStartTimestampNs == 2000000000ULL);
	assert(!batch->packets.empty());
	assert(batch->packets.front().timestampNs == 2000000000ULL);
}

static void testRingBufferDropBefore()
{
	EncodedRingBuffer buffer(300ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(videoPacket(0, true));
	buffer.push(audioPacket(10000000ULL));
	buffer.push(videoPacket(1000000000ULL, false));
	buffer.push(videoPacket(2000000000ULL, true));

	buffer.dropBefore(1000000000ULL);
	assert(buffer.packetCount() == 2);
	assert(buffer.keyframeCount() == 1);
}

static void testRingBufferRequiresVideoKeyframeForDecodeBatch()
{
	EncodedRingBuffer buffer(300ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(videoPacket(0, false));
	buffer.push(audioPacket(10000000ULL));
	buffer.push(videoPacket(1000000000ULL, false));
	buffer.push(videoPacket(2000000000ULL, false));

	assert(buffer.durationSecondsFloor() >= 2);
	assert(!buffer.decodeBatchForDelay(1, 0));

	buffer.push(videoPacket(3000000000ULL, true));
	buffer.push(videoPacket(4000000000ULL, false));
	assert(buffer.decodeBatchForDelay(1, 0));
}

static void testRingBufferKeepsPacketsSortedByTimestamp()
{
	EncodedRingBuffer buffer(300ULL * 1000000000ULL, 1024 * 1024);
	buffer.push(videoPacket(3000000000ULL, false));
	buffer.push(audioPacket(10000000ULL));
	buffer.push(videoPacket(0, true));
	buffer.push(videoPacket(1000000000ULL, false));
	buffer.push(videoPacket(2000000000ULL, true));
	buffer.push(audioPacket(2010000000ULL));
	buffer.push(videoPacket(4000000000ULL, false));

	const auto target = buffer.targetTimestampForDelay(2);
	assert(target);
	assert(*target == 3000000000ULL);

	const auto batch = buffer.decodeBatchForDelay(2, 0);
	assert(batch);
	assert(batch->decodeStartTimestampNs == 2000000000ULL);
	assert(!batch->packets.empty());
	assert(batch->packets.front().timestampNs == 2000000000ULL);
}

int main()
{
	testDelayClamp();
	testEncodedRetentionIncludesPreroll();
	testStateMachine();
	testRingBufferEvictsByTime();
	testRingBufferLimitsCanGrow();
	testRingBufferEvictsByBytes();
	testRingBufferFindsDecodeKeyframeForDelay();
	testRingBufferDropBefore();
	testRingBufferRequiresVideoKeyframeForDecodeBatch();
	testRingBufferKeepsPacketsSortedByTimestamp();
	return 0;
}
