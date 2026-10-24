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
#pragma once

#include "socket_pair.h"

#include <opendht/crypto.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace asio {
class io_context;
}

namespace dht::log {
struct Logger;
}

namespace dhtnet {
class IceSocket;
namespace tls {
class CertificateStore;
class TlsSession;
} // namespace tls
} // namespace dhtnet

namespace jami {

// The DTLS identity of one side of a WebRTC call leg: a fresh ECDSA P-256
// certificate, as browsers use, and its fingerprint.
struct DtlsIdentity
{
    std::shared_ptr<dht::crypto::PrivateKey> key;
    std::shared_ptr<dht::crypto::Certificate> certificate;
    std::string fingerprint; // SHA-256, "AB:CD:..." as in SDP (RFC 8122)

    static DtlsIdentity generate();
};

// The SHA-256 fingerprint of a certificate, "AB:CD:...": of its DER form.
std::string dtlsFingerprint(const uint8_t* der, size_t size);
std::string dtlsFingerprint(const dht::crypto::Certificate& certificate);

// SRTP keys for both directions, as SocketPair::createSRTP takes them.
struct SrtpKeys
{
    std::string suite;
    std::string outgoing; // base64 of master key and salt
    std::string incoming;
};

// The datagram socket under a DtlsSrtpTransport, such as an ICE component.
// What arrives before a callback is set is kept for it, within a bound.
class DatagramSocket
{
public:
    using OnRecv = std::function<void(const uint8_t* buf, size_t len)>;

    virtual ~DatagramSocket() = default;
    // The size sent, or a negative errno.
    virtual ssize_t send(const uint8_t* buf, size_t len) = 0;
    // nullptr stops; no call runs after it returns.
    virtual void setOnRecv(OnRecv&& cb) = 0;
    virtual uint16_t getTransportOverhead() const = 0;
};

std::unique_ptr<DatagramSocket> makeIceDatagramSocket(std::unique_ptr<dhtnet::IceSocket> socket);

// A WebRTC m-line's transport over one datagram socket, which DTLS shares
// with RTP and RTCP (RFC 7983). The DTLS handshake authenticates the peer by
// the fingerprint its description gave, and keys SRTP (RFC 5764). Then the
// transport carries the RTP and RTCP of a SocketPair; what arrives before the
// SocketPair attaches is kept for it, within a bound.
class DtlsSrtpTransport : public RtpMuxTransport
{
public:
    // The keys once the handshake is done, or nothing when it failed or the
    // peer's certificate does not match its fingerprint. Called once, from
    // the DTLS thread: it must not block.
    using OnKeys = std::function<void(std::optional<SrtpKeys>)>;

    struct Config
    {
        DtlsIdentity identity;
        // From the peer's authenticated description.
        std::string peerFingerprint;
        // DTLS client (a=setup:active) or server (a=setup:passive).
        bool client {true};
        dhtnet::tls::CertificateStore* certStore {nullptr};
        std::shared_ptr<asio::io_context> ioContext;
        std::shared_ptr<dht::log::Logger> logger;
        std::chrono::milliseconds timeout {std::chrono::seconds(15)};
        // The peer ended the session after the handshake (close_notify or a
        // fatal alert). Called once, from the DTLS thread.
        std::function<void()> onClosed;
    };

    DtlsSrtpTransport(std::unique_ptr<DatagramSocket> socket, Config config, OnKeys onKeys);
    ~DtlsSrtpTransport();

    ssize_t send(const uint8_t* buf, size_t len) override;
    void setOnPacket(OnPacket&& cb) override;
    uint16_t getTransportOverhead() const override;

    // When the peer last sent anything: DTLS, RTP or RTCP.
    std::chrono::steady_clock::time_point lastReceived() const;

private:
    class DtlsChannel;

    void onStateChange(int state);
    int verifyPeer(void* session);

    std::unique_ptr<DatagramSocket> socket_;
    const std::string peerFingerprint_;
    const bool client_;
    std::atomic<std::chrono::steady_clock::rep> lastReceived_;

    // Guards what the socket's receive callback uses.
    std::mutex mutex_;
    std::function<ssize_t(const uint8_t*, size_t)> onDtls_;
    OnPacket onPacket_;
    std::deque<std::vector<uint8_t>> early_;
    bool attached_ {false};

    // Guards the TLS session and the reporting of its outcome.
    std::mutex tlsMutex_;
    std::unique_ptr<dhtnet::tls::TlsSession> tls_;
    OnKeys onKeys_;
    std::function<void()> onClosed_;
    bool keysReported_ {false};
    bool establishedEarly_ {false};
    bool stopped_ {false};
};

} // namespace jami
