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
#include "dtls_srtp_transport.h"

#include "base64.h"
#include "logger.h"

#include <dhtnet/certstore.h>
#include <dhtnet/generic_io.h>
#include <dhtnet/ice_socket.h>
#include <dhtnet/tls_session.h>

#include <gnutls/crypto.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <future>

namespace jami {

namespace {

constexpr const char* SRTP_PROFILE = "SRTP_AES128_CM_HMAC_SHA1_80";
// DTLS records stay under what every path carries, TURN and IPv6 included.
constexpr int DTLS_MTU = 1200;
// RTP and RTCP kept until the media attaches: the peer starts sending, with a
// key frame, as soon as its side of the handshake is done.
constexpr size_t MAX_EARLY_PACKETS = 256;
// Datagrams kept until the DTLS transport listens: the peer starts its
// handshake as soon as its own ICE checks succeed, possibly before this side
// knows that ICE is done.
constexpr size_t MAX_PENDING_DATAGRAMS = 64;

std::string
upper(std::string_view s)
{
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::toupper(c); });
    return result;
}

// Whether the TLS session would ask an OCSP responder about this certificate:
// the same search as dhtnet's, failing closed.
bool
hasOcspResponder(const gnutls_datum_t& der)
{
    gnutls_x509_crt_t cert;
    if (gnutls_x509_crt_init(&cert) != GNUTLS_E_SUCCESS)
        return true;
    bool found = true;
    if (gnutls_x509_crt_import(cert, &der, GNUTLS_X509_FMT_DER) == GNUTLS_E_SUCCESS) {
        gnutls_datum_t uri;
        unsigned seq = 0;
        int ret;
        do {
            ret = gnutls_x509_crt_get_authority_info_access(cert, seq++, GNUTLS_IA_OCSP_URI, &uri, nullptr);
        } while (ret < 0 and ret != GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE);
        found = ret >= 0;
        if (found)
            gnutls_free(uri.data);
    }
    gnutls_x509_crt_deinit(cert);
    return found;
}

std::shared_future<dhtnet::tls::DhParams>
noDhParams()
{
    // Browsers only offer ECDHE: there are no Diffie-Hellman parameters to
    // compute, but the DTLS server reads them.
    std::promise<dhtnet::tls::DhParams> params;
    params.set_value({});
    return params.get_future().share();
}

std::string
keyAndSalt(const std::vector<uint8_t>& key, const std::vector<uint8_t>& salt)
{
    std::vector<uint8_t> both;
    both.reserve(key.size() + salt.size());
    both.insert(both.end(), key.begin(), key.end());
    both.insert(both.end(), salt.begin(), salt.end());
    return base64::encode(both);
}

class IceDatagramSocket : public DatagramSocket
{
public:
    explicit IceDatagramSocket(std::unique_ptr<dhtnet::IceSocket> socket)
        : socket_(std::move(socket))
    {
        // The callback goes through onRecv_, under mutex_, so that once
        // setOnRecv(nullptr) returns no call is running or will run. Without
        // a callback, the ICE transport would keep the data in a byte stream
        // that no later callback gets.
        socket_->setOnRecv([this](unsigned char* buf, size_t len) {
            std::lock_guard lk(mutex_);
            if (onRecv_)
                onRecv_(buf, len);
            else if (not closed_ and pending_.size() < MAX_PENDING_DATAGRAMS)
                pending_.emplace_back(buf, buf + len);
            return static_cast<ssize_t>(len);
        });
    }

    ~IceDatagramSocket() override { socket_->setOnRecv(nullptr); }

    ssize_t send(const uint8_t* buf, size_t len) override
    {
        auto ret = socket_->send(buf, len);
        if (ret < 0)
            return -static_cast<ssize_t>(errno ? errno : EIO);
        return ret;
    }

    void setOnRecv(OnRecv&& cb) override
    {
        std::lock_guard lk(mutex_);
        onRecv_ = std::move(cb);
        if (not onRecv_) {
            closed_ = true;
            pending_.clear();
            return;
        }
        for (const auto& datagram : pending_)
            onRecv_(datagram.data(), datagram.size());
        pending_.clear();
    }

    uint16_t getTransportOverhead() const override { return socket_->getTransportOverhead(); }

private:
    std::unique_ptr<dhtnet::IceSocket> socket_;
    std::mutex mutex_;
    OnRecv onRecv_;
    std::deque<std::vector<uint8_t>> pending_;
    bool closed_ {false};
};

} // namespace

DtlsIdentity
DtlsIdentity::generate()
{
    DtlsIdentity identity;
    identity.key = std::make_shared<dht::crypto::PrivateKey>(
        dht::crypto::PrivateKey::generateEC(GNUTLS_ECC_CURVE_SECP256R1));
    identity.certificate = std::make_shared<dht::crypto::Certificate>(
        dht::crypto::Certificate::generate(*identity.key, "jami"));
    identity.fingerprint = dtlsFingerprint(*identity.certificate);
    return identity;
}

namespace {

std::string
formatFingerprint(const uint8_t (&digest)[32])
{
    static constexpr char HEX[] = "0123456789ABCDEF";
    std::string fingerprint;
    fingerprint.reserve(sizeof(digest) * 3);
    for (size_t i = 0; i < sizeof(digest); ++i) {
        if (i)
            fingerprint += ':';
        fingerprint += HEX[digest[i] >> 4];
        fingerprint += HEX[digest[i] & 0xF];
    }
    return fingerprint;
}

} // namespace

std::string
dtlsFingerprint(const uint8_t* der, size_t size)
{
    uint8_t digest[32];
    if (gnutls_hash_fast(GNUTLS_DIG_SHA256, der, size, digest) < 0)
        return {};
    return formatFingerprint(digest);
}

std::string
dtlsFingerprint(const dht::crypto::Certificate& certificate)
{
    // Certificate::getPacked() is PEM; the fingerprint is of the DER form.
    uint8_t digest[32];
    size_t size = sizeof(digest);
    if (gnutls_x509_crt_get_fingerprint(certificate.cert, GNUTLS_DIG_SHA256, digest, &size) < 0 or size != sizeof(digest))
        return {};
    return formatFingerprint(digest);
}

std::unique_ptr<DatagramSocket>
makeIceDatagramSocket(std::unique_ptr<dhtnet::IceSocket> socket)
{
    return std::make_unique<IceDatagramSocket>(std::move(socket));
}

// What the TLS session reads and writes: the DTLS packets of the socket.
class DtlsSrtpTransport::DtlsChannel : public dhtnet::GenericSocket<uint8_t>
{
public:
    explicit DtlsChannel(DtlsSrtpTransport& transport)
        : transport_(transport)
    {}

    void setOnRecv(RecvCb&& cb) override
    {
        std::lock_guard lk(transport_.mutex_);
        transport_.onDtls_ = std::move(cb);
    }

    bool isReliable() const override { return false; }
    // The TLS session is a client when its transport is the initiator.
    bool isInitiator() const override { return transport_.client_; }
    int maxPayload() const override { return DTLS_MTU; }
    int waitForData(std::chrono::milliseconds, std::error_code&) const override { return 0; }

    std::size_t write(const ValueType* buf, std::size_t len, std::error_code& ec) override
    {
        auto ret = transport_.socket_->send(buf, len);
        if (ret < 0) {
            ec = std::error_code(static_cast<int>(-ret), std::generic_category());
            return 0;
        }
        ec.clear();
        return static_cast<std::size_t>(ret);
    }

    std::size_t read(ValueType*, std::size_t, std::error_code& ec) override
    {
        // Unreliable transports deliver through setOnRecv only.
        ec = std::make_error_code(std::errc::operation_not_supported);
        return 0;
    }

private:
    DtlsSrtpTransport& transport_;
};

DtlsSrtpTransport::DtlsSrtpTransport(std::unique_ptr<DatagramSocket> socket, Config config, OnKeys onKeys)
    : socket_(std::move(socket))
    , peerFingerprint_(upper(config.peerFingerprint))
    , client_(config.client)
    , lastReceived_(std::chrono::steady_clock::now().time_since_epoch().count())
    , onKeys_(std::move(onKeys))
    , onClosed_(std::move(config.onClosed))
{
    socket_->setOnRecv([this](const uint8_t* buf, size_t len) {
        if (len == 0)
            return;
        lastReceived_ = std::chrono::steady_clock::now().time_since_epoch().count();
        std::lock_guard lk(mutex_);
        // RFC 7983 section 7: the first byte tells DTLS (20-63) from RTP
        // and RTCP (128-191). STUN stays inside ICE; anything else is dropped.
        if (buf[0] >= 20 and buf[0] <= 63) {
            if (onDtls_)
                onDtls_(buf, len);
        } else if (buf[0] >= 128 and buf[0] <= 191) {
            if (onPacket_)
                onPacket_(buf, len);
            else if (not attached_ and early_.size() < MAX_EARLY_PACKETS)
                early_.emplace_back(buf, buf + len);
        }
    });

    dhtnet::tls::TlsParams params {
        /*.ca_list = */ "",
        /*.peer_ca = */ nullptr,
        /*.cert = */ config.identity.certificate,
        /*.cert_key = */ config.identity.key,
        /*.dh_params = */ noDhParams(),
        /*.certStore = */ *config.certStore,
        /*.timeout = */ std::chrono::duration_cast<dhtnet::tls::duration>(config.timeout),
        /*.cert_check = */ nullptr,
        /*.io_context = */ config.ioContext,
        /*.logger = */ config.logger,
        /*.srtp_profiles = */ SRTP_PROFILE,
    };
    dhtnet::tls::TlsSession::TlsSessionCallbacks callbacks {
        /*.onStateChange = */ [this](dhtnet::tls::TlsSessionState state) { onStateChange(static_cast<int>(state)); },
        /*.onRxData = */ [](std::vector<uint8_t>&&) {},
        /*.onCertificatesUpdate = */ [](const gnutls_datum_t*, const gnutls_datum_t*, unsigned int) {},
        /*.verifyCertificate = */ [this](gnutls_session_t session) { return verifyPeer(session); },
    };

    // Certificates, not anonymous credentials: each side proves the
    // certificate its description named.
    auto tls = std::make_unique<dhtnet::tls::TlsSession>(std::make_unique<DtlsChannel>(*this),
                                                         params,
                                                         callbacks,
                                                         false);
    std::unique_lock lk(tlsMutex_);
    tls_ = std::move(tls);
    // The handshake may have ended while the session was being stored.
    if (establishedEarly_) {
        establishedEarly_ = false;
        lk.unlock();
        onStateChange(static_cast<int>(dhtnet::tls::TlsSessionState::ESTABLISHED));
    }
}

DtlsSrtpTransport::~DtlsSrtpTransport()
{
    std::unique_ptr<dhtnet::tls::TlsSession> tls;
    {
        std::lock_guard lk(tlsMutex_);
        stopped_ = true;
        tls = std::move(tls_);
    }
    // Joins the DTLS thread, which runs the callbacks above.
    tls.reset();
    socket_->setOnRecv(nullptr);
}

void
DtlsSrtpTransport::onStateChange(int s)
{
    auto state = static_cast<dhtnet::tls::TlsSessionState>(s);
    if (state != dhtnet::tls::TlsSessionState::ESTABLISHED and state != dhtnet::tls::TlsSessionState::SHUTDOWN)
        return;

    std::optional<SrtpKeys> keys;
    OnKeys onKeys;
    {
        std::unique_lock lk(tlsMutex_);
        if (stopped_)
            return;
        if (keysReported_) {
            // The peer closed the session, or it broke, after the handshake.
            if (state == dhtnet::tls::TlsSessionState::SHUTDOWN and onClosed_) {
                auto onClosed = std::move(onClosed_);
                onClosed_ = {};
                lk.unlock();
                onClosed();
            }
            return;
        }
        if (state == dhtnet::tls::TlsSessionState::ESTABLISHED) {
            if (not tls_) {
                establishedEarly_ = true;
                return;
            }
            if (auto material = tls_->srtpKeyMaterial()) {
                auto local = client_ ? keyAndSalt(material->client_key, material->client_salt)
                                     : keyAndSalt(material->server_key, material->server_salt);
                auto remote = client_ ? keyAndSalt(material->server_key, material->server_salt)
                                      : keyAndSalt(material->client_key, material->client_salt);
                keys = SrtpKeys {SRTP_PROFILE, std::move(local), std::move(remote)};
            } else {
                JAMI_WARNING("[DTLS] Handshake done without SRTP keys");
            }
        }
        keysReported_ = true;
        onKeys = std::move(onKeys_);
    }
    if (onKeys)
        onKeys(std::move(keys));
}

int
DtlsSrtpTransport::verifyPeer(void* s)
{
    auto session = static_cast<gnutls_session_t>(s);
    unsigned count = 0;
    const gnutls_datum_t* certificates = gnutls_certificate_get_peers(session, &count);
    if (not certificates or count == 0) {
        JAMI_WARNING("[DTLS] Peer sent no certificate");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    auto fingerprint = dtlsFingerprint(certificates[0].data, certificates[0].size);
    if (fingerprint.empty() or fingerprint != peerFingerprint_) {
        JAMI_WARNING("[DTLS] Peer certificate {} does not match the fingerprint of its description {}",
                     fingerprint,
                     peerFingerprint_);
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    // The fingerprint is the whole authentication. After this callback the TLS
    // session would check the certificate's OCSP responder, waiting for a
    // request to a URL the peer wrote: browsers' certificates name none.
    if (hasOcspResponder(certificates[0])) {
        JAMI_WARNING("[DTLS] Peer certificate names an OCSP responder, which a WebRTC peer's does not");
        return GNUTLS_E_CERTIFICATE_ERROR;
    }
    return GNUTLS_E_SUCCESS;
}

ssize_t
DtlsSrtpTransport::send(const uint8_t* buf, size_t len)
{
    return socket_->send(buf, len);
}

void
DtlsSrtpTransport::setOnPacket(OnPacket&& cb)
{
    std::lock_guard lk(mutex_);
    onPacket_ = std::move(cb);
    if (not onPacket_)
        return;
    attached_ = true;
    for (const auto& packet : early_)
        onPacket_(packet.data(), packet.size());
    early_.clear();
}

std::chrono::steady_clock::time_point
DtlsSrtpTransport::lastReceived() const
{
    return std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(lastReceived_.load()));
}

uint16_t
DtlsSrtpTransport::getTransportOverhead() const
{
    return socket_->getTransportOverhead();
}

} // namespace jami
