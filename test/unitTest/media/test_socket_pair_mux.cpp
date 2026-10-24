/*
 *  Copyright (C) 2026 Savoir-faire Linux Inc.
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include <dhtnet/ip_utils.h> // MUST BE INCLUDED FIRST

#include "media/libav_deps.h" // THEN THIS ONE AFTER

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include "media/media_io_handle.h"
#include "media/socket_pair.h"

extern "C" {
#include "media/srtp.h"
}

#include "test_runner.h"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace jami {
namespace test {
namespace {

// Keys of each direction, base64 of a 16-byte master key and a 14-byte salt.
constexpr const char* SUITE = "SRTP_AES128_CM_HMAC_SHA1_80";
constexpr const char* KEY_TO_US = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn";
constexpr const char* KEY_FROM_US = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMN";
constexpr uint32_t PEER_SSRC = 0x11223344;
constexpr uint32_t OUR_SSRC = 0x55667788;

using Packet = std::vector<uint8_t>;

// Stands for the ICE component of a WebRTC m-line.
class FakeTransport : public RtpMuxTransport
{
public:
    ssize_t send(const uint8_t* buf, size_t len) override
    {
        std::lock_guard lk(mutex_);
        sent_.emplace_back(buf, buf + len);
        return static_cast<ssize_t>(len);
    }

    void setOnPacket(OnPacket&& cb) override
    {
        std::lock_guard lk(mutex_);
        onPacket_ = std::move(cb);
    }

    uint16_t getTransportOverhead() const override { return 28; }

    void receive(const Packet& packet)
    {
        std::lock_guard lk(mutex_);
        if (onPacket_)
            onPacket_(packet.data(), packet.size());
    }

    std::vector<Packet> sent()
    {
        std::lock_guard lk(mutex_);
        return sent_;
    }

private:
    std::mutex mutex_;
    OnPacket onPacket_;
    std::vector<Packet> sent_;
};

void
putUint32(Packet& packet, size_t offset, uint32_t value)
{
    packet[offset] = value >> 24;
    packet[offset + 1] = value >> 16;
    packet[offset + 2] = value >> 8;
    packet[offset + 3] = value;
}

uint32_t
getUint32(const Packet& packet, size_t offset)
{
    return static_cast<uint32_t>(packet[offset]) << 24 | static_cast<uint32_t>(packet[offset + 1]) << 16
           | static_cast<uint32_t>(packet[offset + 2]) << 8 | packet[offset + 3];
}

Packet
rtp(uint16_t seq, uint32_t ssrc, const std::string& payload = "media")
{
    Packet packet(12, 0);
    packet[0] = 0x80;
    packet[1] = 111;
    packet[2] = seq >> 8;
    packet[3] = seq & 0xff;
    putUint32(packet, 8, ssrc);
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

// Sender report with one reception report block.
Packet
senderReport(uint8_t fractionLost)
{
    Packet packet(52, 0);
    packet[0] = 0x81;
    packet[1] = 200;
    packet[3] = 12;
    putUint32(packet, 4, PEER_SSRC);
    putUint32(packet, 28, OUR_SSRC);
    packet[32] = fractionLost;
    return packet;
}

// Source description with an empty CNAME, as browsers add to compounds.
Packet
sourceDescription()
{
    Packet packet(12, 0);
    packet[0] = 0x81;
    packet[1] = 202;
    packet[3] = 2;
    putUint32(packet, 4, PEER_SSRC);
    packet[8] = 1;
    return packet;
}

// Payload-specific feedback: 1 for a PLI, 4 for a FIR.
Packet
keyFrameRequest(uint8_t format)
{
    Packet packet(format == 4 ? 20 : 12, 0);
    packet[0] = 0x80 | format;
    packet[1] = 206;
    packet[3] = format == 4 ? 4 : 2;
    putUint32(packet, 4, PEER_SSRC);
    putUint32(packet, 8, format == 4 ? 0 : OUR_SSRC);
    if (format == 4)
        putUint32(packet, 12, OUR_SSRC);
    return packet;
}

Packet
compound(std::initializer_list<Packet> packets)
{
    Packet result;
    for (const auto& packet : packets)
        result.insert(result.end(), packet.begin(), packet.end());
    return result;
}

// The peer's SRTP contexts: it encrypts with the key of our input and
// decrypts with the key of our output.
struct Peer
{
    SRTPContext out {};
    SRTPContext in {};
    Peer()
    {
        CPPUNIT_ASSERT_EQUAL(0, ff_srtp_set_crypto(&out, SUITE, KEY_TO_US));
        CPPUNIT_ASSERT_EQUAL(0, ff_srtp_set_crypto(&in, SUITE, KEY_FROM_US));
    }
    ~Peer()
    {
        ff_srtp_free(&out);
        ff_srtp_free(&in);
    }

    Packet protect(const Packet& plain)
    {
        Packet result(plain.size() + 32);
        int len = ff_srtp_encrypt(&out,
                                  plain.data(),
                                  static_cast<int>(plain.size()),
                                  result.data(),
                                  static_cast<int>(result.size()));
        CPPUNIT_ASSERT(len > 0);
        result.resize(len);
        return result;
    }

    // Empty when the packet does not authenticate.
    Packet unprotect(Packet packet)
    {
        int len = static_cast<int>(packet.size());
        if (ff_srtp_decrypt(&in, packet.data(), &len) < 0)
            return {};
        packet.resize(len);
        return packet;
    }
};

struct Endpoint
{
    std::shared_ptr<FakeTransport> transport = std::make_shared<FakeTransport>();
    std::unique_ptr<SocketPair> socketPair;
    std::unique_ptr<MediaIOHandle> io;

    explicit Endpoint(bool withKeys = true)
    {
        socketPair = std::make_unique<SocketPair>(transport);
        if (withKeys)
            socketPair->createSRTP(SUITE, KEY_FROM_US, SUITE, KEY_TO_US);
        io.reset(socketPair->createIOContext(1232));
    }

    // What FFmpeg's RTP demuxer would get next: empty when nothing is left.
    Packet read()
    {
        auto* ctx = io->getContext();
        Packet buf(2048);
        int len = ctx->read_packet(ctx->opaque, buf.data(), static_cast<int>(buf.size()));
        if (len <= 0)
            return {};
        buf.resize(len);
        return buf;
    }

    // What FFmpeg's RTP muxer writes.
    void write(Packet packet)
    {
        auto* ctx = io->getContext();
        ctx->write_packet(ctx->opaque, packet.data(), static_cast<int>(packet.size()));
    }
};

} // namespace

class SocketPairMuxTest : public CppUnit::TestFixture
{
public:
    static std::string name() { return "socket_pair_mux"; }

private:
    void rtpAndRtcpAreDecrypted();
    void unauthenticatedPacketsAreDropped();
    void rtpAndRtcpAreSentEncrypted();
    void nothingIsSentWithoutKeys();
    void keyFrameRequestsInCompoundRtcpAreReported();
    void pictureLossIndicationNamesTheReceivedStream();
    void senderReportsCountAsReceiverReports();

    CPPUNIT_TEST_SUITE(SocketPairMuxTest);
    CPPUNIT_TEST(rtpAndRtcpAreDecrypted);
    CPPUNIT_TEST(unauthenticatedPacketsAreDropped);
    CPPUNIT_TEST(rtpAndRtcpAreSentEncrypted);
    CPPUNIT_TEST(nothingIsSentWithoutKeys);
    CPPUNIT_TEST(keyFrameRequestsInCompoundRtcpAreReported);
    CPPUNIT_TEST(pictureLossIndicationNamesTheReceivedStream);
    CPPUNIT_TEST(senderReportsCountAsReceiverReports);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(SocketPairMuxTest, SocketPairMuxTest::name());

void
SocketPairMuxTest::rtpAndRtcpAreDecrypted()
{
    Peer peer;
    Endpoint endpoint;
    auto media = rtp(1, PEER_SSRC);
    auto report = senderReport(0);

    endpoint.transport->receive(peer.protect(media));
    endpoint.transport->receive(peer.protect(report));

    // RTCP is read first, as with separate sockets.
    CPPUNIT_ASSERT(endpoint.read() == report);
    CPPUNIT_ASSERT(endpoint.read() == media);
    CPPUNIT_ASSERT(endpoint.read().empty());
}

void
SocketPairMuxTest::unauthenticatedPacketsAreDropped()
{
    Peer peer;
    Endpoint endpoint;
    auto tampered = peer.protect(rtp(1, PEER_SSRC, "forged"));
    tampered[14] ^= 0x01;
    auto genuine = rtp(2, PEER_SSRC, "genuine");

    endpoint.transport->receive(tampered);
    endpoint.transport->receive(rtp(3, PEER_SSRC, "clear"));
    endpoint.transport->receive(peer.protect(genuine));

    CPPUNIT_ASSERT(endpoint.read() == genuine);
    CPPUNIT_ASSERT(endpoint.read().empty());
}

void
SocketPairMuxTest::rtpAndRtcpAreSentEncrypted()
{
    Peer peer;
    Endpoint endpoint;
    auto media = rtp(7, OUR_SSRC);
    auto report = senderReport(0);

    endpoint.write(media);
    endpoint.write(report);

    auto sent = endpoint.transport->sent();
    CPPUNIT_ASSERT_EQUAL(size_t(2), sent.size());
    CPPUNIT_ASSERT(sent[0] != media);
    CPPUNIT_ASSERT(peer.unprotect(sent[0]) == media);
    CPPUNIT_ASSERT(sent[1] != report);
    CPPUNIT_ASSERT(peer.unprotect(sent[1]) == report);
}

void
SocketPairMuxTest::nothingIsSentWithoutKeys()
{
    Endpoint endpoint(false);

    endpoint.write(rtp(1, OUR_SSRC));
    endpoint.write(senderReport(0));

    CPPUNIT_ASSERT(endpoint.transport->sent().empty());
}

void
SocketPairMuxTest::keyFrameRequestsInCompoundRtcpAreReported()
{
    Peer peer;
    Endpoint endpoint;
    int requests = 0;
    endpoint.socketPair->setKeyFrameRequestCallback([&] { ++requests; });

    endpoint.transport->receive(peer.protect(compound({senderReport(0), sourceDescription(), keyFrameRequest(1)})));
    endpoint.transport->receive(peer.protect(compound({senderReport(0), keyFrameRequest(4)})));
    endpoint.read();
    endpoint.read();

    CPPUNIT_ASSERT_EQUAL(2, requests);
}

void
SocketPairMuxTest::pictureLossIndicationNamesTheReceivedStream()
{
    Peer peer;
    Endpoint endpoint;

    // Nothing received yet: the request waits for a packet to name its stream.
    CPPUNIT_ASSERT(not endpoint.socketPair->sendPictureLossIndication());
    CPPUNIT_ASSERT(endpoint.transport->sent().empty());

    endpoint.transport->receive(peer.protect(rtp(1, PEER_SSRC)));
    endpoint.read();
    CPPUNIT_ASSERT_EQUAL(size_t(1), endpoint.transport->sent().size());
    CPPUNIT_ASSERT(endpoint.socketPair->sendPictureLossIndication());

    auto sent = endpoint.transport->sent();
    CPPUNIT_ASSERT_EQUAL(size_t(2), sent.size());
    for (const auto& packet : sent) {
        auto pli = peer.unprotect(packet);
        CPPUNIT_ASSERT_EQUAL(size_t(12), pli.size());
        CPPUNIT_ASSERT_EQUAL(uint8_t(0x81), pli[0]);
        CPPUNIT_ASSERT_EQUAL(uint8_t(206), pli[1]);
        CPPUNIT_ASSERT_EQUAL(PEER_SSRC, getUint32(pli, 8));
    }
}

void
SocketPairMuxTest::senderReportsCountAsReceiverReports()
{
    Peer peer;
    Endpoint endpoint;

    endpoint.transport->receive(peer.protect(compound({senderReport(64), sourceDescription()})));
    endpoint.read();

    auto reports = endpoint.socketPair->getRtcpRR();
    CPPUNIT_ASSERT_EQUAL(size_t(1), reports.size());
    CPPUNIT_ASSERT_EQUAL(uint32_t(64), static_cast<uint32_t>(reports.front().fraction_lost));
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::SocketPairMuxTest::name())
