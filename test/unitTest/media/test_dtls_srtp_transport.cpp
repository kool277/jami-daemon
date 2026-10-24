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

#include "logger.h"
#include "media/dtls_srtp_transport.h"
#include "media/media_io_handle.h"
#include "media/socket_pair.h"
#include "test_runner.h"

#include <asio/io_context.hpp>
#include <dhtnet/certstore.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <vector>

namespace jami {
namespace test {
namespace {

using namespace std::chrono_literals;
using Packet = std::vector<uint8_t>;

// Two datagram sockets joined back to back, standing for an ICE pair.
struct Wire
{
    std::mutex mutex;
    DatagramSocket::OnRecv ends[2];
};

class WireSocket : public DatagramSocket
{
public:
    WireSocket(std::shared_ptr<Wire> wire, int side)
        : wire_(std::move(wire))
        , side_(side)
    {}

    ~WireSocket() override { setOnRecv(nullptr); }

    ssize_t send(const uint8_t* buf, size_t len) override
    {
        std::lock_guard lk(wire_->mutex);
        if (auto& peer = wire_->ends[1 - side_])
            peer(buf, len);
        return static_cast<ssize_t>(len);
    }

    void setOnRecv(OnRecv&& cb) override
    {
        std::lock_guard lk(wire_->mutex);
        wire_->ends[side_] = std::move(cb);
    }

    uint16_t getTransportOverhead() const override { return 28; }

private:
    std::shared_ptr<Wire> wire_;
    int side_;
};

struct Side
{
    DtlsIdentity identity = DtlsIdentity::generate();
    std::promise<std::optional<SrtpKeys>> keys;
    std::unique_ptr<DtlsSrtpTransport> transport;

    std::optional<SrtpKeys> waitForKeys()
    {
        auto future = keys.get_future();
        CPPUNIT_ASSERT(future.wait_for(20s) == std::future_status::ready);
        return future.get();
    }
};

class Fixture
{
public:
    Fixture()
        : storeDir_(std::filesystem::temp_directory_path() / ("dtls-srtp-test-" + std::to_string(std::random_device {}())))
        , certStore_(storeDir_, Logger::dhtLogger())
    {
        // TEST_LOG=1 shows the DTLS session's log.
        if (getenv("TEST_LOG")) {
            Logger::setConsoleLog(true);
            Logger::setDebugMode(true);
        }
    }

    ~Fixture()
    {
        client.transport.reset();
        server.transport.reset();
        std::error_code ec;
        std::filesystem::remove_all(storeDir_, ec);
    }

    // Starts both sides; each expects the other's fingerprint unless told
    // otherwise.
    void start(const std::string& clientExpects = {}, const std::string& serverExpects = {})
    {
        auto wire = std::make_shared<Wire>();
        server.transport = std::make_unique<DtlsSrtpTransport>(
            std::make_unique<WireSocket>(wire, 1),
            config(server.identity, serverExpects.empty() ? client.identity.fingerprint : serverExpects, false),
            [this](std::optional<SrtpKeys> keys) { server.keys.set_value(std::move(keys)); });
        client.transport = std::make_unique<DtlsSrtpTransport>(
            std::make_unique<WireSocket>(wire, 0),
            config(client.identity, clientExpects.empty() ? server.identity.fingerprint : clientExpects, true),
            [this](std::optional<SrtpKeys> keys) { client.keys.set_value(std::move(keys)); });
    }

    Side client;
    Side server;

private:
    DtlsSrtpTransport::Config config(const DtlsIdentity& identity, const std::string& peerFingerprint, bool isClient)
    {
        DtlsSrtpTransport::Config config;
        config.identity = identity;
        config.peerFingerprint = peerFingerprint;
        config.client = isClient;
        config.certStore = &certStore_;
        config.ioContext = ioContext_;
        config.logger = Logger::dhtLogger();
        config.timeout = 5s;
        return config;
    }

    std::filesystem::path storeDir_;
    dhtnet::tls::CertificateStore certStore_;
    std::shared_ptr<asio::io_context> ioContext_ = std::make_shared<asio::io_context>();
};

Packet
rtp(uint16_t seq, const std::string& payload)
{
    Packet packet(12, 0);
    packet[0] = 0x80;
    packet[1] = 96;
    packet[2] = seq >> 8;
    packet[3] = seq & 0xff;
    packet[11] = 1;
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

// A SocketPair keyed from one side's DTLS keys, and its FFmpeg I/O.
struct Media
{
    std::unique_ptr<SocketPair> socketPair;
    std::unique_ptr<MediaIOHandle> io;

    Media(std::shared_ptr<RtpMuxTransport> transport, const SrtpKeys& keys)
    {
        socketPair = std::make_unique<SocketPair>(std::move(transport));
        socketPair->createSRTP(keys.suite.c_str(), keys.outgoing.c_str(), keys.suite.c_str(), keys.incoming.c_str());
        io.reset(socketPair->createIOContext(1232));
    }

    void write(Packet packet)
    {
        auto* ctx = io->getContext();
        ctx->write_packet(ctx->opaque, packet.data(), static_cast<int>(packet.size()));
    }

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
};

// Lets a SocketPair use a transport owned elsewhere.
std::shared_ptr<RtpMuxTransport>
borrow(DtlsSrtpTransport& transport)
{
    return std::shared_ptr<RtpMuxTransport>(&transport, [](RtpMuxTransport*) {});
}

} // namespace

class DtlsSrtpTransportTest : public CppUnit::TestFixture
{
public:
    static std::string name() { return "dtls_srtp_transport"; }

private:
    void fingerprintIsSha256OfTheCertificate();
    void handshakeKeysBothDirections();
    void wrongFingerprintIsRefused();
    void onlyRtpAndRtcpReachTheMedia();
    void packetsBeforeTheMediaAreKept();
    void srtpFlowsBetweenTheSides();

    CPPUNIT_TEST_SUITE(DtlsSrtpTransportTest);
    CPPUNIT_TEST(fingerprintIsSha256OfTheCertificate);
    CPPUNIT_TEST(handshakeKeysBothDirections);
    CPPUNIT_TEST(wrongFingerprintIsRefused);
    CPPUNIT_TEST(onlyRtpAndRtcpReachTheMedia);
    CPPUNIT_TEST(packetsBeforeTheMediaAreKept);
    CPPUNIT_TEST(srtpFlowsBetweenTheSides);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(DtlsSrtpTransportTest, DtlsSrtpTransportTest::name());

void
DtlsSrtpTransportTest::fingerprintIsSha256OfTheCertificate()
{
    auto identity = DtlsIdentity::generate();
    // 32 bytes in upper-case hex, colon separated.
    CPPUNIT_ASSERT_EQUAL(size_t(32 * 3 - 1), identity.fingerprint.size());
    CPPUNIT_ASSERT(identity.fingerprint.find_first_not_of("0123456789ABCDEF:") == std::string::npos);
    // Of the DER form, as the peer receives it in the handshake.
    std::vector<uint8_t> der(8192);
    size_t size = der.size();
    CPPUNIT_ASSERT_EQUAL(0, gnutls_x509_crt_export(identity.certificate->cert, GNUTLS_X509_FMT_DER, der.data(), &size));
    CPPUNIT_ASSERT_EQUAL(identity.fingerprint, dtlsFingerprint(der.data(), size));
    // Fresh for each call leg.
    CPPUNIT_ASSERT(identity.fingerprint != DtlsIdentity::generate().fingerprint);
}

void
DtlsSrtpTransportTest::handshakeKeysBothDirections()
{
    Fixture fixture;
    fixture.start();

    auto clientKeys = fixture.client.waitForKeys();
    auto serverKeys = fixture.server.waitForKeys();
    CPPUNIT_ASSERT(clientKeys);
    CPPUNIT_ASSERT(serverKeys);
    CPPUNIT_ASSERT_EQUAL(std::string("SRTP_AES128_CM_HMAC_SHA1_80"), clientKeys->suite);
    CPPUNIT_ASSERT_EQUAL(clientKeys->outgoing, serverKeys->incoming);
    CPPUNIT_ASSERT_EQUAL(clientKeys->incoming, serverKeys->outgoing);
    CPPUNIT_ASSERT(clientKeys->outgoing != clientKeys->incoming);
}

void
DtlsSrtpTransportTest::wrongFingerprintIsRefused()
{
    Fixture fixture;
    // The server expects another certificate than the client's.
    fixture.start({}, DtlsIdentity::generate().fingerprint);

    CPPUNIT_ASSERT(not fixture.server.waitForKeys());
    CPPUNIT_ASSERT(not fixture.client.waitForKeys());
}

void
DtlsSrtpTransportTest::onlyRtpAndRtcpReachTheMedia()
{
    Fixture fixture;
    fixture.start();
    CPPUNIT_ASSERT(fixture.client.waitForKeys());
    CPPUNIT_ASSERT(fixture.server.waitForKeys());

    std::vector<Packet> received;
    std::mutex mutex;
    fixture.server.transport->setOnPacket([&](const uint8_t* buf, size_t len) {
        std::lock_guard lk(mutex);
        received.emplace_back(buf, buf + len);
    });

    Packet stun {0x00, 0x01, 0x00, 0x00};
    Packet unknown {0xF0, 0x00};
    auto media = rtp(1, "media");
    fixture.client.transport->send(stun.data(), stun.size());
    fixture.client.transport->send(unknown.data(), unknown.size());
    fixture.client.transport->send(media.data(), media.size());

    std::lock_guard lk(mutex);
    CPPUNIT_ASSERT_EQUAL(size_t(1), received.size());
    CPPUNIT_ASSERT(received[0] == media);
}

void
DtlsSrtpTransportTest::packetsBeforeTheMediaAreKept()
{
    Fixture fixture;
    fixture.start();
    CPPUNIT_ASSERT(fixture.client.waitForKeys());
    CPPUNIT_ASSERT(fixture.server.waitForKeys());

    // The peer sends once its handshake is done, before this side's media is
    // set up: its first key frame is in there.
    std::vector<Packet> early;
    for (uint16_t seq = 0; seq < 300; ++seq) {
        early.emplace_back(rtp(seq, "early"));
        fixture.client.transport->send(early.back().data(), early.back().size());
    }

    std::vector<Packet> received;
    std::mutex mutex;
    fixture.server.transport->setOnPacket([&](const uint8_t* buf, size_t len) {
        std::lock_guard lk(mutex);
        received.emplace_back(buf, buf + len);
    });
    auto late = rtp(300, "late");
    fixture.client.transport->send(late.data(), late.size());

    // The first ones, as many as the bound keeps, in order; then the rest as
    // it comes.
    std::lock_guard lk(mutex);
    CPPUNIT_ASSERT_EQUAL(size_t(257), received.size());
    for (size_t i = 0; i < 256; ++i)
        CPPUNIT_ASSERT(received[i] == early[i]);
    CPPUNIT_ASSERT(received.back() == late);
}

void
DtlsSrtpTransportTest::srtpFlowsBetweenTheSides()
{
    Fixture fixture;
    fixture.start();
    auto clientKeys = fixture.client.waitForKeys();
    auto serverKeys = fixture.server.waitForKeys();
    CPPUNIT_ASSERT(clientKeys and serverKeys);

    Media clientMedia(borrow(*fixture.client.transport), *clientKeys);
    Media serverMedia(borrow(*fixture.server.transport), *serverKeys);

    auto fromClient = rtp(1, "from the client");
    auto fromServer = rtp(1, "from the server");
    clientMedia.write(fromClient);
    serverMedia.write(fromServer);

    CPPUNIT_ASSERT(serverMedia.read() == fromClient);
    CPPUNIT_ASSERT(clientMedia.read() == fromServer);
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::DtlsSrtpTransportTest::name())
