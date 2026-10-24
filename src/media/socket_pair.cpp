/*
 *  Copyright (C) 2004-2026 Savoir-faire Linux Inc.
 *  Copyright (c) 2007 The FFmpeg Project
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

#include "libav_deps.h" // THEN THIS ONE AFTER

#include "socket_pair.h"
#include "logger.h"
#include "connectivity/security/memory.h"

#include <dhtnet/ice_socket.h>

#include <string>
#include <algorithm>

extern "C" {
#include "srtp.h"
}

#include <cstring>
#include <stdexcept>
#include <unistd.h>
#include <sys/types.h>

#ifdef _WIN32
#define SOCK_NONBLOCK FIONBIO
#define poll          WSAPoll
#define close(x)      closesocket(x)
#endif

#ifdef __ANDROID__
#include <asm-generic/fcntl.h>
#define SOCK_NONBLOCK O_NONBLOCK
#endif

#ifdef __APPLE__
#include <fcntl.h>
#endif

// Swap 2 byte, 16 bit values:
#define Swap2Bytes(val) ((((val) >> 8) & 0x00FF) | (((val) << 8) & 0xFF00))

// Swap 4 byte, 32 bit values:
#define Swap4Bytes(val) \
    ((((val) >> 24) & 0x000000FF) | (((val) >> 8) & 0x0000FF00) | (((val) << 8) & 0x00FF0000) \
     | (((val) << 24) & 0xFF000000))

// Swap 8 byte, 64 bit values:
#define Swap8Bytes(val) \
    ((((val) >> 56) & 0x00000000000000FF) | (((val) >> 40) & 0x000000000000FF00) \
     | (((val) >> 24) & 0x0000000000FF0000) | (((val) >> 8) & 0x00000000FF000000) \
     | (((val) << 8) & 0x000000FF00000000) | (((val) << 24) & 0x0000FF0000000000) \
     | (((val) << 40) & 0x00FF000000000000) | (((val) << 56) & 0xFF00000000000000))

namespace jami {

static constexpr int NET_POLL_TIMEOUT = 100; /* poll() timeout in ms */
static constexpr int RTP_MAX_PACKET_LENGTH = 2048;
static constexpr auto UDP_HEADER_SIZE = 8;
static constexpr auto SRTP_OVERHEAD = 10;
static constexpr uint32_t RTCP_RR_FRACTION_MASK = 0xFF000000;
static constexpr unsigned MINIMUM_RTP_HEADER_SIZE = 16;
// Packets waiting for the receiver thread on a multiplexed transport; older
// ones are dropped beyond this, so a stalled receiver cannot grow memory.
static constexpr size_t MAX_QUEUED_PACKETS = 1024;

static uint32_t
readUint32(const uint8_t* buf)
{
    return static_cast<uint32_t>(buf[0]) << 24 | static_cast<uint32_t>(buf[1]) << 16
           | static_cast<uint32_t>(buf[2]) << 8 | buf[3];
}

static void
writeUint32(uint8_t* buf, uint32_t value)
{
    buf[0] = value >> 24;
    buf[1] = value >> 16;
    buf[2] = value >> 8;
    buf[3] = value;
}

enum class DataType : uint8_t { RTP = 1 << 0, RTCP = 1 << 1 };

class SRTPProtoContext
{
public:
    SRTPProtoContext(const char* out_suite, const char* out_key, const char* in_suite, const char* in_key)
    {
        jami_secure_memzero(&srtp_out, sizeof(srtp_out));
        jami_secure_memzero(&srtp_in, sizeof(srtp_in));
        if (out_suite && out_key) {
            // XXX: see srtp_open from libavformat/srtpproto.c
            if (ff_srtp_set_crypto(&srtp_out, out_suite, out_key) < 0) {
                srtp_close();
                throw std::runtime_error("Unable to set crypto on output");
            }
        }

        if (in_suite && in_key) {
            if (ff_srtp_set_crypto(&srtp_in, in_suite, in_key) < 0) {
                srtp_close();
                throw std::runtime_error("Unable to set crypto on input");
            }
        }
    }

    ~SRTPProtoContext() { srtp_close(); }

    SRTPContext srtp_out {};
    SRTPContext srtp_in {};
    uint8_t encryptbuf[RTP_MAX_PACKET_LENGTH];

private:
    void srtp_close() noexcept
    {
        ff_srtp_free(&srtp_out);
        ff_srtp_free(&srtp_in);
    }
};

static int
ff_network_wait_fd(int fd)
{
    struct pollfd p = {fd, POLLOUT, 0};
    auto ret = poll(&p, 1, NET_POLL_TIMEOUT);
    return ret < 0 ? errno : p.revents & (POLLOUT | POLLERR | POLLHUP) ? 0 : -EAGAIN;
}

static int
udp_socket_create(int family, int port)
{
    int udp_fd = -1;

#ifdef __APPLE__
    udp_fd = socket(family, SOCK_DGRAM, 0);
    if (udp_fd >= 0 && fcntl(udp_fd, F_SETFL, O_NONBLOCK) < 0) {
        close(udp_fd);
        udp_fd = -1;
    }
#elif defined _WIN32
    udp_fd = socket(family, SOCK_DGRAM, 0);
    u_long block = 1;
    if (udp_fd >= 0 && ioctlsocket(udp_fd, FIONBIO, &block) < 0) {
        close(udp_fd);
        udp_fd = -1;
    }
#else
    udp_fd = socket(family, SOCK_DGRAM | SOCK_NONBLOCK, 0);
#endif

    if (udp_fd < 0) {
        JAMI_ERROR("socket() failed");
        strErr();
        return -1;
    }

    auto bind_addr = dhtnet::ip_utils::getAnyHostAddr(family);
    if (not bind_addr.isIpv4() and not bind_addr.isIpv6()) {
        JAMI_ERROR("No IPv4/IPv6 host found for family {}", family);
        close(udp_fd);
        return -1;
    }

    bind_addr.setPort(port);
    JAMI_LOG("use local address: {}", bind_addr.toString(true, true));
    if (::bind(udp_fd, bind_addr, bind_addr.getLength()) < 0) {
        JAMI_ERROR("bind() failed");
        strErr();
        close(udp_fd);
        udp_fd = -1;
    }

    return udp_fd;
}

SocketPair::SocketPair(const char* uri, int localPort)
{
    openSockets(uri, localPort);
}

SocketPair::SocketPair(std::unique_ptr<dhtnet::IceSocket> rtp_sock, std::unique_ptr<dhtnet::IceSocket> rtcp_sock)
    : rtp_sock_(std::move(rtp_sock))
    , rtcp_sock_(std::move(rtcp_sock))
{
    JAMI_LOG("[{}] Creating instance using ICE sockets for comp {} and {}",
             fmt::ptr(this),
             rtp_sock_->getCompId(),
             rtcp_sock_->getCompId());

    rtp_sock_->setOnRecv([this](uint8_t* buf, size_t len) {
        std::lock_guard l(dataBuffMutex_);
        rtpDataBuff_.emplace_back(buf, buf + len);
        cv_.notify_one();
        return len;
    });
    rtcp_sock_->setOnRecv([this](uint8_t* buf, size_t len) {
        std::lock_guard l(dataBuffMutex_);
        rtcpDataBuff_.emplace_back(buf, buf + len);
        cv_.notify_one();
        return len;
    });
}

SocketPair::SocketPair(std::shared_ptr<RtpMuxTransport> transport)
    : muxTransport_(std::move(transport))
{
    JAMI_LOG("[{}] Creating instance with RTP and RTCP multiplexed", fmt::ptr(this));

    muxTransport_->setOnPacket([this](const uint8_t* buf, size_t len) {
        // RFC 5761 section 4: RTCP packet types are 192-223, a range RTP
        // payload types avoid when multiplexed with RTCP.
        bool isRtcp = len >= 2 and buf[1] >= 192 and buf[1] <= 223;
        std::lock_guard l(dataBuffMutex_);
        auto& queue = isRtcp ? rtcpDataBuff_ : rtpDataBuff_;
        if (queue.size() >= MAX_QUEUED_PACKETS)
            queue.pop_front();
        queue.emplace_back(buf, buf + len);
        cv_.notify_one();
    });
}

SocketPair::~SocketPair()
{
    interrupt();
    closeSockets();
    JAMI_LOG("[{}] Instance destroyed", fmt::ptr(this));
}

bool
SocketPair::waitForRTCP(std::chrono::seconds interval)
{
    std::unique_lock lock(rtcpInfo_mutex_);
    return cvRtcpPacketReadyToRead_.wait_for(lock, interval, [this] {
        return interrupted_ or not listRtcpRRHeader_.empty() or not listRtcpREMBHeader_.empty();
    });
}

void
SocketPair::saveRtcpRRPacket(uint8_t* buf, size_t len)
{
    if (len < sizeof(rtcpRRHeader))
        return;

    auto* header = reinterpret_cast<rtcpRRHeader*>(buf);
    if (header->pt != 201) // 201 = RR PT
        return;

    std::lock_guard lock(rtcpInfo_mutex_);

    if (listRtcpRRHeader_.size() >= MAX_LIST_SIZE) {
        listRtcpRRHeader_.pop_front();
    }

    listRtcpRRHeader_.emplace_back(*header);

    cvRtcpPacketReadyToRead_.notify_one();
}

void
SocketPair::saveRtcpREMBPacket(uint8_t* buf, size_t len)
{
    if (len < sizeof(rtcpREMBHeader))
        return;

    auto* header = reinterpret_cast<rtcpREMBHeader*>(buf);
    if (header->pt != 206) // 206 = REMB PT
        return;

    if (header->uid != 0x424D4552) // uid must be "REMB"
        return;

    std::lock_guard lock(rtcpInfo_mutex_);

    if (listRtcpREMBHeader_.size() >= MAX_LIST_SIZE) {
        listRtcpREMBHeader_.pop_front();
    }

    listRtcpREMBHeader_.push_back(*header);

    cvRtcpPacketReadyToRead_.notify_one();
}

std::list<rtcpRRHeader>
SocketPair::getRtcpRR()
{
    std::lock_guard lock(rtcpInfo_mutex_);
    return std::move(listRtcpRRHeader_);
}

std::list<rtcpREMBHeader>
SocketPair::getRtcpREMB()
{
    std::lock_guard lock(rtcpInfo_mutex_);
    return std::move(listRtcpREMBHeader_);
}

void
SocketPair::createSRTP(const char* out_suite, const char* out_key, const char* in_suite, const char* in_key)
{
    srtpContext_.reset(new SRTPProtoContext(out_suite, out_key, in_suite, in_key));
}

void
SocketPair::interrupt()
{
    JAMI_WARNING("[{}] Interrupting RTP sockets", fmt::ptr(this));
    interrupted_ = true;
    if (rtp_sock_)
        rtp_sock_->setOnRecv(nullptr);
    if (rtcp_sock_)
        rtcp_sock_->setOnRecv(nullptr);
    if (muxTransport_)
        muxTransport_->setOnPacket(nullptr);
    cv_.notify_all();
    cvRtcpPacketReadyToRead_.notify_all();
}

void
SocketPair::setReadBlockingMode(bool block)
{
    JAMI_LOG("[{}] Read operations in blocking mode [{}]", fmt::ptr(this), block ? "YES" : "NO");
    readBlockingMode_ = block;
    cv_.notify_all();
    cvRtcpPacketReadyToRead_.notify_all();
}

void
SocketPair::stopSendOp(bool state)
{
    noWrite_ = state;
}

void
SocketPair::closeSockets()
{
    if (rtcpHandle_ > 0 and close(rtcpHandle_))
        strErr();
    if (rtpHandle_ > 0 and close(rtpHandle_))
        strErr();
}

void
SocketPair::openSockets(const char* uri, int local_rtp_port)
{
    JAMI_LOG("[{}] Creating rtp socket for uri {} on port {}", fmt::ptr(this), uri, local_rtp_port);

    char hostname[256];
    char path[1024];
    int dst_rtp_port;

    av_url_split(NULL, 0, NULL, 0, hostname, sizeof(hostname), &dst_rtp_port, path, sizeof(path), uri);

    const int local_rtcp_port = local_rtp_port + 1;
    const int dst_rtcp_port = dst_rtp_port + 1;

    rtpDestAddr_ = dhtnet::IpAddr {hostname};
    rtpDestAddr_.setPort(dst_rtp_port);
    rtcpDestAddr_ = dhtnet::IpAddr {hostname};
    rtcpDestAddr_.setPort(dst_rtcp_port);

    // Open local sockets (RTP/RTCP)
    if ((rtpHandle_ = udp_socket_create(rtpDestAddr_.getFamily(), local_rtp_port)) == -1
        or (rtcpHandle_ = udp_socket_create(rtcpDestAddr_.getFamily(), local_rtcp_port)) == -1) {
        closeSockets();
        JAMI_ERROR("[{}] Sockets creation failed", fmt::ptr(this));
        throw std::runtime_error("Sockets creation failed");
    }

    JAMI_WARNING("SocketPair: local({},{}) / {}({},{})",
                 local_rtp_port,
                 local_rtcp_port,
                 hostname,
                 dst_rtp_port,
                 dst_rtcp_port);
}

MediaIOHandle*
SocketPair::createIOContext(const uint16_t mtu)
{
    unsigned ip_header_size;
    if (rtp_sock_)
        ip_header_size = rtp_sock_->getTransportOverhead();
    else if (muxTransport_)
        ip_header_size = muxTransport_->getTransportOverhead();
    else if (rtpDestAddr_.getFamily() == AF_INET6)
        ip_header_size = 40;
    else
        ip_header_size = 20;
    return new MediaIOHandle(
        mtu - (srtpContext_ ? SRTP_OVERHEAD : 0) - UDP_HEADER_SIZE - ip_header_size,
        true,
        [](void* sp, uint8_t* buf, int len) { return static_cast<SocketPair*>(sp)->readCallback(buf, len); },
        [](void* sp, uint8_t* buf, int len) { return static_cast<SocketPair*>(sp)->writeCallback(buf, len); },
        0,
        reinterpret_cast<void*>(this));
}

int
SocketPair::waitForData()
{
    // System sockets
    if (rtpHandle_ >= 0) {
        int ret;
        do {
            if (interrupted_) {
                errno = EINTR;
                return -1;
            }

            if (not readBlockingMode_) {
                return 0;
            }

            // work with system socket
            struct pollfd p[2] = {{rtpHandle_, POLLIN, 0}, {rtcpHandle_, POLLIN, 0}};
            ret = poll(p, 2, NET_POLL_TIMEOUT);
            if (ret > 0) {
                ret = 0;
                if (p[0].revents & POLLIN)
                    ret |= static_cast<int>(DataType::RTP);
                if (p[1].revents & POLLIN)
                    ret |= static_cast<int>(DataType::RTCP);
            }
        } while (!ret or (ret < 0 and errno == EAGAIN));

        return ret;
    }

    // work with IceSocket
    {
        std::unique_lock lk(dataBuffMutex_);
        cv_.wait(lk, [this] {
            return interrupted_ or not rtpDataBuff_.empty() or not rtcpDataBuff_.empty() or not readBlockingMode_;
        });
    }

    if (interrupted_) {
        errno = EINTR;
        return -1;
    }

    return static_cast<int>(DataType::RTP) | static_cast<int>(DataType::RTCP);
}

int
SocketPair::readRtpData(void* buf, int buf_size)
{
    // handle system socket
    if (rtpHandle_ >= 0) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        return static_cast<int>(recvfrom(rtpHandle_,
                                         static_cast<char*>(buf),
                                         buf_size,
                                         0,
                                         reinterpret_cast<struct sockaddr*>(&from),
                                         &from_len));
    }

    // handle ICE
    std::unique_lock lk(dataBuffMutex_);
    if (not rtpDataBuff_.empty()) {
        auto pkt = std::move(rtpDataBuff_.front());
        rtpDataBuff_.pop_front();
        lk.unlock(); // to not block our ICE callbacks
        int pkt_size = static_cast<int>(pkt.size());
        int len = std::min(pkt_size, buf_size);
        std::copy_n(pkt.begin(), len, static_cast<char*>(buf));
        return len;
    }

    return 0;
}

int
SocketPair::readRtcpData(void* buf, int buf_size)
{
    // handle system socket
    if (rtcpHandle_ >= 0) {
        struct sockaddr_storage from;
        socklen_t from_len = sizeof(from);
        return static_cast<int>(recvfrom(rtcpHandle_,
                                         static_cast<char*>(buf),
                                         buf_size,
                                         0,
                                         reinterpret_cast<struct sockaddr*>(&from),
                                         &from_len));
    }

    // handle ICE
    std::unique_lock lk(dataBuffMutex_);
    if (not rtcpDataBuff_.empty()) {
        auto pkt = std::move(rtcpDataBuff_.front());
        rtcpDataBuff_.pop_front();
        lk.unlock();
        int pkt_size = static_cast<int>(pkt.size());
        int len = std::min(pkt_size, buf_size);
        std::copy_n(pkt.begin(), len, static_cast<char*>(buf));
        return len;
    }

    return 0;
}

int
SocketPair::readMuxed(uint8_t* buf, int buf_size)
{
    while (true) {
        auto datatype = waitForData();
        if (datatype < 0)
            return datatype;

        int len = readRtcpData(buf, buf_size);
        bool isRtcp = len > 0;
        if (not isRtcp)
            len = readRtpData(buf, buf_size);
        if (len <= 0)
            return len;

        // Without keys nothing can be authenticated, so nothing is used.
        if (not srtpContext_ or not srtpContext_->srtp_in.aes)
            continue;

        if (isRtcp) {
            if (ff_srtp_decrypt(&srtpContext_->srtp_in, buf, &len) < 0 or len <= 0)
                continue;
            handleMuxedRtcp(buf, len);
            return len;
        }

        if (len < static_cast<int>(MINIMUM_RTP_HEADER_SIZE))
            continue;

        if (ff_srtp_decrypt(&srtpContext_->srtp_in, buf, &len) < 0 or len <= 0)
            continue;

        // The RTP header, extensions included, is authenticated, not encrypted.
        float abs = 0.0f;
        int32_t gradient = 0;
        int32_t deltaT = 0;
        if (rtpDelayCallback_ and parse_RTP_ext(buf, &abs)
            and getOneWayDelayGradient(abs, buf[1] & 0x80, &gradient, &deltaT))
            rtpDelayCallback_(gradient, deltaT);

        uint16_t seq = buf[2] << 8 | buf[3];
        if (packetLossCallback_ and seq != static_cast<uint16_t>(lastSeqNumIn_ + 1))
            packetLossCallback_();
        lastSeqNumIn_ = seq;
        lastSsrcIn_ = readUint32(buf + 8);
        if (pliPending_.exchange(false))
            sendPictureLossIndication();
        return len;
    }
}

// Browsers send compound RTCP packets, for example a sender report followed
// by SDES and a key frame request: read every packet of it.
void
SocketPair::handleMuxedRtcp(uint8_t* buf, size_t len)
{
    size_t offset = 0;
    while (offset + 4 <= len) {
        uint8_t* packet = buf + offset;
        if ((packet[0] >> 6) != 2)
            return;
        size_t size = (static_cast<size_t>(packet[2] << 8 | packet[3]) + 1) * 4;
        if (offset + size > len)
            return;
        handleRtcpPacket(packet, size);
        offset += size;
    }
}

void
SocketPair::handleRtcpPacket(uint8_t* packet, size_t size)
{
    uint8_t count = packet[0] & 0x1F;
    switch (packet[1]) {
    case 200: {
        // A sender report carries the reception reports a peer that also
        // sends would otherwise put in a receiver report: read the first
        // one like a receiver report.
        constexpr size_t SR_HEADER_SIZE = 28;
        constexpr size_t REPORT_BLOCK_SIZE = 24;
        if (count == 0 or size < SR_HEADER_SIZE + REPORT_BLOCK_SIZE)
            return;
        uint8_t rr[sizeof(rtcpRRHeader)];
        std::memcpy(rr, packet, 8);
        rr[1] = 201;
        std::memcpy(rr + 8, packet + SR_HEADER_SIZE, REPORT_BLOCK_SIZE);
        auto* header = reinterpret_cast<rtcpRRHeader*>(rr);
        lastDLSR_ = Swap4Bytes(header->dlsr);
        lastRR_time = std::chrono::steady_clock::now();
        saveRtcpRRPacket(rr, sizeof(rr));
        break;
    }
    case 201:
        if (count == 0 or size < sizeof(rtcpRRHeader))
            return;
        lastDLSR_ = Swap4Bytes(reinterpret_cast<rtcpRRHeader*>(packet)->dlsr);
        lastRR_time = std::chrono::steady_clock::now();
        saveRtcpRRPacket(packet, size);
        break;
    case 206:
        // Payload-specific feedback (RFC 4585, RFC 5104): 1 is a Picture
        // Loss Indication, 4 a Full Intra Request, 15 an application
        // message such as REMB.
        if (count == 1 or count == 4) {
            if (keyFrameRequestCallback_)
                keyFrameRequestCallback_();
        } else if (count == 15) {
            saveRtcpREMBPacket(packet, size);
        }
        break;
    default:
        break;
    }
}

int
SocketPair::readCallback(uint8_t* buf, int buf_size)
{
    if (muxTransport_)
        return readMuxed(buf, buf_size);

    while (true) {
        auto datatype = waitForData();
        if (datatype < 0)
            return datatype;

        int len = 0;
        bool fromRTCP = false;

        if (datatype & static_cast<int>(DataType::RTCP)) {
            len = readRtcpData(buf, buf_size);
            if (len > 0) {
                auto* header = reinterpret_cast<rtcpRRHeader*>(buf);
                // 201 = RR PT
                if (header->pt == 201) {
                    lastDLSR_ = Swap4Bytes(header->dlsr);
                    // JAMI_WARN("Read RR, lastDLSR : %d", lastDLSR_);
                    lastRR_time = std::chrono::steady_clock::now();
                    saveRtcpRRPacket(buf, len);
                }
                // 206 = REMB PT
                else if (header->pt == 206)
                    saveRtcpREMBPacket(buf, len);
                // 200 = SR PT
                else if (header->pt == 200) {
                    // not used yet
                } else {
                    unsigned pt = header->pt;
                    JAMI_LOG("Unable to read RTCP: unknown packet type {}", pt);
                }
                fromRTCP = true;
            }
        }

        // No RTCP… attempt RTP
        if (!len and (datatype & static_cast<int>(DataType::RTP))) {
            len = readRtpData(buf, buf_size);
            fromRTCP = false;
        }

        if (len <= 0)
            return len;

        if (not fromRTCP && (buf_size < static_cast<int>(MINIMUM_RTP_HEADER_SIZE)))
            return len;

        // SRTP decrypt
        if (not fromRTCP and srtpContext_ and srtpContext_->srtp_in.aes) {
            int32_t gradient = 0;
            int32_t deltaT = 0;
            float abs = 0.0f;
            bool res_parse = false;
            bool res_delay = false;

            res_parse = parse_RTP_ext(buf, &abs);
            bool marker = (buf[1] & 0x80) >> 7;

            if (res_parse)
                res_delay = getOneWayDelayGradient(abs, marker, &gradient, &deltaT);

            // rtpDelayCallback_ is not set for audio
            if (rtpDelayCallback_ and res_delay)
                rtpDelayCallback_(gradient, deltaT);

            auto err = ff_srtp_decrypt(&srtpContext_->srtp_in, buf, &len);
            if (packetLossCallback_ and (buf[2] << 8 | buf[3]) != lastSeqNumIn_ + 1)
                packetLossCallback_();
            lastSeqNumIn_ = buf[2] << 8 | buf[3];
            if (err < 0) {
                // Dropped, not taken for the end of the stream: a duplicate, a
                // late or a stray packet would stop the receiver.
                JAMI_WARNING("decrypt error {}", err);
                continue;
            }
        }

        if (len != 0)
            return len;
        else
            return AVERROR_EOF;
    }
}

int
SocketPair::writeData(uint8_t* buf, int buf_size)
{
    bool isRTCP = RTP_PT_IS_RTCP(buf[1]);

    // System sockets?
    if (rtpHandle_ >= 0) {
        int fd;
        dhtnet::IpAddr* dest_addr;

        if (isRTCP) {
            fd = rtcpHandle_;
            dest_addr = &rtcpDestAddr_;
        } else {
            fd = rtpHandle_;
            dest_addr = &rtpDestAddr_;
        }

        auto ret = ff_network_wait_fd(fd);
        if (ret < 0)
            return ret;

        if (noWrite_)
            return buf_size;
        return static_cast<int>(
            ::sendto(fd, reinterpret_cast<const char*>(buf), buf_size, 0, *dest_addr, dest_addr->getLength()));
    }

    if (noWrite_)
        return buf_size;

    if (muxTransport_) {
        auto ret = muxTransport_->send(buf, buf_size);
        if (ret < 0) {
            errno = static_cast<int>(-ret);
            return -1;
        }
        return static_cast<int>(ret);
    }

    // IceSocket
    if (isRTCP)
        return static_cast<int>(rtcp_sock_->send(buf, buf_size));
    else
        return static_cast<int>(rtp_sock_->send(buf, buf_size));
}

// WebRTC peers require SRTCP: RTCP is encrypted like RTP, and nothing is sent
// in clear, even before the keys are set.
int
SocketPair::encryptAndWrite(uint8_t* buf, int buf_size)
{
    bool isRTCP = RTP_PT_IS_RTCP(buf[1]);
    if (isRTCP and static_cast<unsigned>(buf_size) >= sizeof(rtcpRRHeader)) {
        auto* header = reinterpret_cast<rtcpRRHeader*>(buf);
        rtcpPacketLoss_ = (header->pt == 201 && ntohl(header->fraction_lost) & RTCP_RR_FRACTION_MASK);
    }
    if (isRTCP and buf[1] == 200 and static_cast<unsigned>(buf_size) >= sizeof(rtcpSRHeader))
        noteSentSenderReport(buf);
    if (not isRTCP and buf_size >= 12)
        lastSsrcOut_ = readUint32(buf + 8);

    std::lock_guard lk(writeMutex_);
    if (not srtpContext_ or not srtpContext_->srtp_out.aes)
        return buf_size;

    int size = ff_srtp_encrypt(&srtpContext_->srtp_out,
                               buf,
                               buf_size,
                               srtpContext_->encryptbuf,
                               sizeof(srtpContext_->encryptbuf));
    if (size < 0) {
        JAMI_WARNING("encrypt error {}", size);
        return size;
    }

    if (interrupted_)
        return -EINTR;
    // A datagram the transport has no room for is dropped, as the network
    // would: waiting would only make the media late (and hold writeMutex_).
    int ret = writeData(srtpContext_->encryptbuf, size);
    if (ret < 0 and errno == EAGAIN)
        return buf_size;
    return ret < 0 ? -errno : buf_size;
}

int
SocketPair::writeRtcp(uint8_t* buf, int buf_size)
{
    // A WebRTC peer takes SRTCP only.
    if (muxTransport_)
        return encryptAndWrite(buf, buf_size);
    return writeData(buf, buf_size);
}

void
SocketPair::setKeyFrameRequestCallback(std::function<void()> cb)
{
    keyFrameRequestCallback_ = std::move(cb);
}

bool
SocketPair::sendPictureLossIndication()
{
    if (not muxTransport_ or noWrite_)
        return false;
    uint32_t mediaSsrc = lastSsrcIn_;
    if (not mediaSsrc) {
        // Sent when the first packet names the stream.
        pliPending_ = true;
        return false;
    }

    // RFC 4585 section 6.3.1: V=2, FMT=1, PT=206 (payload-specific
    // feedback), length 2 (in 32-bit words, minus one), sender SSRC, media
    // SSRC. It is sent alone, as reduced-size RTCP (RFC 5506).
    uint8_t pli[12] = {0x81, 206, 0, 2};
    uint32_t senderSsrc = lastSsrcOut_;
    writeUint32(pli + 4, senderSsrc ? senderSsrc : 1);
    writeUint32(pli + 8, mediaSsrc);
    return encryptAndWrite(pli, sizeof(pli)) > 0;
}

int
SocketPair::writeCallback(uint8_t* buf, int buf_size)
{
    if (noWrite_)
        return 0;

    if (muxTransport_)
        return encryptAndWrite(buf, buf_size);

    int ret;
    bool isRTCP = RTP_PT_IS_RTCP(buf[1]);

    // Encrypt?
    if (not isRTCP and srtpContext_ and srtpContext_->srtp_out.aes) {
        buf_size = ff_srtp_encrypt(&srtpContext_->srtp_out,
                                   buf,
                                   buf_size,
                                   srtpContext_->encryptbuf,
                                   sizeof(srtpContext_->encryptbuf));
        if (buf_size < 0) {
            JAMI_WARNING("encrypt error {}", buf_size);
            return buf_size;
        }

        buf = srtpContext_->encryptbuf;
    }

    // check if we're sending an RR, if so, detect packet loss
    // buf_size gives length of buffer, not just header
    if (isRTCP && static_cast<unsigned>(buf_size) >= sizeof(rtcpRRHeader)) {
        auto* header = reinterpret_cast<rtcpRRHeader*>(buf);
        rtcpPacketLoss_ = (header->pt == 201 && ntohl(header->fraction_lost) & RTCP_RR_FRACTION_MASK);
    }

    do {
        if (interrupted_)
            return -EINTR;
        ret = writeData(buf, buf_size);
    } while (ret < 0 and errno == EAGAIN);

    if (buf[1] == 200) // Sender Report
        noteSentSenderReport(buf);

    return ret < 0 ? -errno : ret;
}

void
SocketPair::noteSentSenderReport(const uint8_t* buf)
{
    const auto* header = reinterpret_cast<const rtcpSRHeader*>(buf);
    unsigned int ts_LSB = Swap4Bytes(header->timestampLSB);
    unsigned int ts_MSB = Swap4Bytes(header->timestampMSB);

    double currentSRTS = ts_MSB + (ts_LSB / pow(2, 32));

    if (lastSRTS_ != 0 && lastDLSR_ != 0) {
        if (histoLatency_.size() >= MAX_LIST_SIZE)
            histoLatency_.pop_front();

        histoLatency_.push_back((currentSRTS - lastSRTS_) / 2);
    }

    lastSRTS_ = currentSRTS;
}

double
SocketPair::getLastLatency()
{
    if (not histoLatency_.empty())
        return histoLatency_.back();
    else
        return -1;
}

void
SocketPair::setRtpDelayCallback(std::function<void(int, int)> cb)
{
    rtpDelayCallback_ = std::move(cb);
}

bool
SocketPair::getOneWayDelayGradient(float sendTS, bool marker, int32_t* gradient, int32_t* deltaT)
{
    // Keep only last packet of each frame
    if (not marker) {
        return false;
    }

    // 1st frame
    if (lastSendTS_ == 0.0f) {
        lastSendTS_ = sendTS;
        lastReceiveTS_ = std::chrono::steady_clock::now();
        return false;
    }

    int32_t deltaS = static_cast<int32_t>((sendTS - lastSendTS_) * 1000); // milliseconds
    if (deltaS < 0)
        deltaS += 64000;
    lastSendTS_ = sendTS;

    std::chrono::steady_clock::time_point arrival_TS = std::chrono::steady_clock::now();
    auto deltaR = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(arrival_TS - lastReceiveTS_).count());
    lastReceiveTS_ = arrival_TS;

    *gradient = deltaR - deltaS;
    *deltaT = deltaR;

    return true;
}

bool
SocketPair::parse_RTP_ext(uint8_t* buf, float* abs)
{
    if (not(buf[0] & 0x10))
        return false;

    // The fixed offsets below hold for the daemon's own packets only: no
    // CSRC, and a first extension element of ID 3 with 3 bytes of
    // abs-send-time. A browser's packets may carry other extensions.
    if (buf[0] & 0x0F)
        return false;

    uint16_t magic_word = (buf[12] << 8) + buf[13];
    if (magic_word != 0xBEDE)
        return false;

    if (buf[16] != ((3 << 4) | 2))
        return false;

    uint8_t sec = buf[17] >> 2;
    uint32_t fract = ((buf[17] & 0x3) << 16 | (buf[18] << 8) | buf[19]) << 14;
    float milli = static_cast<float>(fract / pow(2, 32));

    *abs = static_cast<float>(sec) + (milli);
    return true;
}

uint16_t
SocketPair::lastSeqValOut()
{
    if (srtpContext_)
        return srtpContext_->srtp_out.seq_largest;
    JAMI_ERROR("SRTP context not found.");
    return 0;
}

} // namespace jami
