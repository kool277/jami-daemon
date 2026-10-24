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
#include "sip/webrtccall.h"

#include "jamidht/jamiaccount.h"
#include "client/jami_signal.h"
#include "jami/call_const.h"
#include "logger.h"
#include "manager.h"
#include "media/audio/audio_rtp_session.h"
#include "media/media_attribute.h"
#include "sip/sipaccountbase.h"
#ifdef ENABLE_VIDEO
#include "media/video/video_rtp_session.h"
#endif

#include <dhtnet/ice_socket.h>
#include <dhtnet/ice_transport.h>
#include <dhtnet/ice_transport_factory.h>
#include <dhtnet/ip_utils.h>
#include <opendht/thread_pool.h>

#include <asio/ip/address.hpp>
#include <asio/steady_timer.hpp>

#include <algorithm>
#include <cctype>
#include <random>
#include <utility>

namespace jami {

namespace {

using namespace std::chrono_literals;

// The web client sends its offer as soon as it hears that this side joined,
// and its answer once it has its candidates.
constexpr auto NEGOTIATION_TIMEOUT = 30s;
constexpr auto ICE_INIT_TIMEOUT = 15s;
constexpr auto KEYFRAME_REQUEST_INTERVAL = 1s;
// A browser that leaves without a word (its tab closed, its network gone):
// it sends nothing more, while even a muted one sends RTCP every few seconds.
// As long as ICE consent freshness allows (RFC 7675).
constexpr auto PEER_SILENCE_TIMEOUT = 30s;
constexpr auto PEER_SILENCE_CHECK = 5s;

bool
equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

// Whether a browser's candidate can be used: UDP (browsers' TCP candidates are
// active-only, port 9), component 1 (RTP and RTCP are multiplexed), and an IP
// address: names, such as mDNS ones ("<uuid>.local"), are not resolved here.
bool
isUsableCandidate(std::string_view candidate)
{
    std::vector<std::string_view> fields;
    while (not candidate.empty() and fields.size() < 8) {
        auto pos = candidate.find(' ');
        auto field = candidate.substr(0, pos);
        if (not field.empty())
            fields.emplace_back(field);
        candidate = pos == std::string_view::npos ? std::string_view {} : candidate.substr(pos + 1);
    }
    if (fields.size() < 8 or fields[6] != "typ")
        return false;
    if (fields[1] != "1" or not equalsIgnoreCase(fields[2], "udp"))
        return false;
    std::error_code ec;
    asio::ip::make_address(std::string(fields[4]), ec);
    return not ec;
}

// The stream a media of an app's list is about: the one with its label, or
// when the app gave none, the only one of its type.
template<typename Streams>
auto
findStream(Streams& streams, const MediaAttribute& attr)
{
    auto it = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
        return not attr.label_.empty() and stream.mediaAttribute_->label_ == attr.label_;
    });
    if (it == streams.end() and attr.label_.empty()
        and std::count_if(streams.begin(), streams.end(), [&](const auto& stream) {
                return stream.mediaAttribute_->type_ == attr.type_;
            }) == 1)
        it = std::find_if(streams.begin(), streams.end(), [&](const auto& stream) {
            return stream.mediaAttribute_->type_ == attr.type_;
        });
    if (it != streams.end() and (*it).mediaAttribute_->type_ != attr.type_)
        it = streams.end();
    return it;
}

// Whether an m-line has what a WebRTC transport needs.
bool
hasTransport(const webrtc::MediaSection& section)
{
    return not section.iceUfrag.empty() and not section.icePwd.empty() and not section.fingerprint.empty()
           and equalsIgnoreCase(section.fingerprintAlgorithm, "sha-256") and section.rtcpMux;
}

// Key frame requests are the only feedback this side handles.
void
setFeedback(webrtc::RtpCodec& codec, MediaType type)
{
    codec.rtcpFeedback.clear();
    if (type == MediaType::MEDIA_VIDEO)
        codec.rtcpFeedback = {"nack pli", "ccm fir"};
}

} // namespace

std::string
WebRtcCall::newSessionId()
{
    std::random_device rd;
    std::uniform_int_distribution<uint64_t> dist;
    return fmt::format("{:016x}", dist(rd));
}

WebRtcCall::WebRtcCall(const std::shared_ptr<SIPAccountBase>& account,
                       const std::string& id,
                       Call::CallType type,
                       const std::vector<libjami::MediaMap>& mediaList,
                       std::string conversationId,
                       std::string peerUri,
                       SendSignal sendSignal,
                       std::shared_ptr<WebCallGroup> group,
                       std::string sessionId)
    : SIPCall(account, id, type, mediaList)
    , conversationId_(std::move(conversationId))
    , peerUri_(std::move(peerUri))
    , sessionId_(sessionId.empty() ? newSessionId() : std::move(sessionId))
    , sendSignal_(std::move(sendSignal))
    , group_(std::move(group))
    , leg_(type == Call::CallType::INCOMING or group_ ? Leg::Web : Leg::Pending)
    , invited_(type == Call::CallType::INCOMING or group_)
{
    JAMI_LOG("[call:{}] Web call with {} in conversation {}{}",
             getCallId(),
             peerUri_,
             conversationId_,
             group_ ? " (group)" : "");
    // As a SIP call's, from its To header: the apps attach an incoming call
    // to its conversation only when it names their account.
    if (type == Call::CallType::INCOMING)
        toUsername(account->getUsername());
#ifdef ENABLE_VIDEO
    // Each member gets the others' video directly, not this device's mix.
    if (group_ and group_->isMesh()) {
        for (const auto& session : getRtpSessionList(MediaType::MEDIA_VIDEO))
            std::static_pointer_cast<video::VideoRtpSession>(session)->setLocalSenderInConference(true);
    }
#endif
}

WebRtcCall::~WebRtcCall()
{
    stopTransports();
}

std::map<std::string, std::string>
WebRtcCall::getDetails() const
{
    auto details = SIPCall::getDetails();
    // An incoming call names its peer as a SIP call's From header does. The
    // apps take a bare URI for a SIP peer, without a name of its own:
    // jami-client-qt named the caller's contact after the URI, and saved it.
    if (getCallType() == Call::CallType::INCOMING and not getPeerNumber().empty())
        details[libjami::Call::Details::PEER_NUMBER] = getPeerNumber() + "@ring.dht";
    return details;
}

std::shared_ptr<WebRtcCall>
WebRtcCall::shared()
{
    return std::static_pointer_cast<WebRtcCall>(shared_from_this());
}

std::weak_ptr<WebRtcCall>
WebRtcCall::weakFromThis()
{
    return shared();
}

bool
WebRtcCall::isWebLeg() const
{
    std::lock_guard lk {callMutex_};
    return leg_ == Leg::Web;
}

bool
WebRtcCall::isPolite() const
{
    auto account = getSIPAccount();
    return account and account->getUsername() < peerUri_;
}

void
WebRtcCall::signal(const std::string& type, Json::Value&& data)
{
    data["sessionId"] = sessionId_;
    sendSignal_(type, std::move(data));
}

void
WebRtcCall::exitWeb()
{
    {
        std::lock_guard lk {callMutex_};
        if (not invited_ or exited_)
            return;
        exited_ = true;
        // A group's exit is the group's, once this device leaves the call.
        if (group_)
            return;
    }
    signal("onCallExit", Json::Value(Json::objectValue));
}

std::vector<std::string>
WebRtcCall::codecPreference(MediaType type) const
{
    // The account's order, among what browsers and the daemon both do well.
    std::vector<std::string> names;
    auto account = getSIPAccount();
    if (not account)
        return names;
    for (const auto& codec : account->getActiveAccountCodecInfoList(type)) {
        if (equalsIgnoreCase(codec->name, "opus") or equalsIgnoreCase(codec->name, "H264")
            or equalsIgnoreCase(codec->name, "VP8"))
            names.emplace_back(codec->name);
    }
    return names;
}

void
WebRtcCall::invite()
{
    Json::Value data(Json::objectValue);
    data["withVideoOn"] = hasVideo() and not isCaptureDeviceMuted(MediaType::MEDIA_VIDEO);
    // The peer's devices ring with the SIP call: they leave this to browsers.
    data["native"] = true;
    {
        std::lock_guard lk {callMutex_};
        if (isIncoming() or invited_ or exited_)
            return;
        invited_ = true;
    }
    signal("onCallInvite", std::move(data));
}

void
WebRtcCall::onPeerJoin()
{
    // On the main thread, where the answers of the peer's devices are
    // handled: the first to answer wins.
    runOnMainThread([w = weakFromThis()] {
        if (auto call = w.lock(); call and call->becomeWebCall()) {
            dht::ThreadPool::io().run([w] {
                if (auto call = w.lock())
                    call->makeOffer();
            });
        }
    });
}

bool
WebRtcCall::becomeWebCall()
{
    {
        std::lock_guard lk {callMutex_};
        // Neither answered by a device nor ended: while the devices ring, the
        // call may be ACTIVE already.
        auto state = getState();
        if (leg_ != Leg::Pending or exited_ or getConnectionState() == ConnectionState::CONNECTED
            or (state != CallState::INACTIVE and state != CallState::ACTIVE))
            return false;
        leg_ = Leg::Web;
    }
    JAMI_LOG("[call:{}] Answered in a browser", getCallId());
    // Connected first: from then on no device call joins (Call::addSubCall
    // refuses a connected call) and the setup does not ring again; those that
    // joined before are hung up. The peer's devices stop ringing, and the SIP
    // media's transport is not used.
    setState(CallState::ACTIVE, ConnectionState::CONNECTED);
    hangupSubcalls();
    stopIceMedia();
    Manager::instance().peerAnsweredCall(*this);
    return true;
}

void
WebRtcCall::merge(Call& call)
{
    {
        std::lock_guard lk {callMutex_};
        if (leg_ == Leg::Pending)
            leg_ = Leg::Sip;
    }
    SIPCall::merge(call);
    // A device answered: the browsers stop ringing.
    exitWeb();
}

void
WebRtcCall::answer(const std::vector<libjami::MediaMap>& mediaList)
{
    std::unique_lock lk {callMutex_};
    if (not isIncoming() or getConnectionState() == ConnectionState::CONNECTED or exited_)
        return;

    // The apps may answer with less than offered: no video, or the camera off.
    if (not mediaList.empty()) {
        auto answered = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());
        std::vector<bool> found(rtpStreams_.size(), false);
        for (const auto& a : answered) {
            auto it = findStream(rtpStreams_, a);
            if (it == rtpStreams_.end())
                continue;
            found[it - rtpStreams_.begin()] = true;
            auto& attr = *it->mediaAttribute_;
            attr.enabled_ = a.enabled_;
            attr.muted_ = a.muted_;
            if (not a.sourceUri_.empty())
                attr.sourceUri_ = a.sourceUri_;
        }
        for (size_t i = 0; i < rtpStreams_.size(); ++i) {
            if (not found[i])
                rtpStreams_[i].mediaAttribute_->enabled_ = false;
        }
    }

    armTimeout("The peer sent no offer");
    // Signals outside the lock: sending reads the signaling's state, whose
    // lock is taken before calls' locks.
    lk.unlock();
    setState(CallState::ACTIVE, ConnectionState::CONNECTED);
    // Those in the call connect to the one who joins: their offers follow.
    if (group_)
        group_->onLocalJoin(*this);
    else
        signal("onCallJoin", Json::Value(Json::objectValue));
}

void
WebRtcCall::offer()
{
    dht::ThreadPool::io().run([w = weakFromThis()] {
        if (auto call = w.lock())
            call->makeOffer();
    });
}

void
WebRtcCall::armTimeout(const char* reason)
{
    if (not timer_)
        timer_ = std::make_unique<asio::steady_timer>(*Manager::instance().ioContext());
    timer_->expires_after(NEGOTIATION_TIMEOUT);
    timer_->async_wait([w = weakFromThis(), reason](const std::error_code& ec) {
        if (ec)
            return;
        if (auto call = w.lock()) {
            std::unique_lock lk {call->callMutex_};
            if (call->negotiation_ == Negotiation::Done or call->exited_)
                return;
            lk.unlock();
            call->fail(reason);
        }
    });
}

void
WebRtcCall::hangup(int code)
{
    exitWeb();
    SIPCall::hangup(code);
}

void
WebRtcCall::refuse()
{
    if (not isIncoming() or getConnectionState() == ConnectionState::CONNECTED)
        return;
    {
        std::lock_guard lk {callMutex_};
        if (exited_)
            return;
    }
    // The web client takes an exit while it rings for a decline.
    exitWeb();
    stopAllMedia();
    setState(ConnectionState::DISCONNECTED, PJSIP_SC_DECLINE);
    removeCall();
}

void
WebRtcCall::onPeerExit()
{
    {
        std::lock_guard lk {callMutex_};
        // Once a device answered, the browsers have no say.
        if (exited_ or leg_ == Leg::Sip)
            return;
        // The peer's browser knows. The peer's other browsers may still ring
        // for an outgoing call: they hear that it ended.
        if (isIncoming())
            exited_ = true;
        peerLeft_ = true;
    }
    runOnMainThread([w = weakFromThis()] {
        auto call = w.lock();
        if (not call)
            return;
        {
            // A device may have answered meanwhile (on this thread too).
            std::lock_guard lk {call->callMutex_};
            if (call->leg_ == Leg::Sip)
                return;
        }
        Manager::instance().peerHungupCall(*call);
        call->removeCall();
    });
}

void
WebRtcCall::armSilenceCheck()
{
    if (not silenceTimer_)
        silenceTimer_ = std::make_unique<asio::steady_timer>(*Manager::instance().ioContext());
    silenceTimer_->expires_after(PEER_SILENCE_CHECK);
    silenceTimer_->async_wait([w = weakFromThis()](const std::error_code& ec) {
        if (ec)
            return;
        auto call = w.lock();
        if (not call)
            return;
        std::unique_lock lk {call->callMutex_};
        if (call->exited_)
            return;
        std::chrono::steady_clock::time_point last {};
        for (const auto& m : call->media_) {
            if (m.dtls)
                last = std::max(last, m.dtls->lastReceived());
        }
        if (last != std::chrono::steady_clock::time_point {}
            and std::chrono::steady_clock::now() - last > PEER_SILENCE_TIMEOUT) {
            lk.unlock();
            call->onPeerGone("Nothing from the peer for 30 s");
            return;
        }
        call->armSilenceCheck();
    });
}

void
WebRtcCall::onPeerGone(const std::string& reason)
{
    JAMI_WARNING("[call:{}] {}: the call ends", getCallId(), reason);
    onPeerExit();
}

void
WebRtcCall::onAnsweredElsewhere()
{
    {
        std::lock_guard lk {callMutex_};
        // Only while it rings here; the browser goes on with the other device,
        // so it is not told.
        if (not isIncoming() or exited_ or getConnectionState() == ConnectionState::CONNECTED)
            return;
        exited_ = true;
        // Not this device's leaving: a group does not tell its members.
        peerLeft_ = true;
    }
    JAMI_LOG("[call:{}] Answered on another device", getCallId());
    runOnMainThread([w = weakFromThis()] {
        if (auto call = w.lock()) {
            Manager::instance().peerHungupCall(*call);
            call->removeCall();
        }
    });
}

void
WebRtcCall::fail(const std::string& reason)
{
    JAMI_WARNING("[call:{}] Web call failed: {}", getCallId(), reason);
    exitWeb();
    onFailure(PJSIP_SC_NOT_ACCEPTABLE_HERE);
}

void
WebRtcCall::removeCall(int code)
{
    ended_ = true;
    exitWeb();
    // Media first: its sockets use the transports.
    if (isWebLeg())
        stopAllMedia();
    stopTransports();
    SIPCall::removeCall(code);
    if (group_ and not groupTold_.exchange(true)) {
        bool byPeer;
        {
            std::lock_guard lk {callMutex_};
            byPeer = peerLeft_;
        }
        group_->onLegEnded(*this, byPeer);
    }
}

void
WebRtcCall::stopTransports()
{
    std::vector<Media> media;
    {
        std::lock_guard lk {callMutex_};
        media = std::move(media_);
        media_.clear();
        if (timer_)
            timer_->cancel();
        if (silenceTimer_)
            silenceTimer_->cancel();
    }
    // Destroying a transport waits for its thread: done elsewhere, as SIPCall
    // does, and not on the main thread, which may be the caller. DTLS first,
    // as it sends over ICE.
    for (auto& m : media) {
        if (m.dtls or m.socket or m.ice)
            dht::ThreadPool::io().run(
                [dtls = std::move(m.dtls),
                 socket = std::shared_ptr<DatagramSocket>(std::move(m.socket)),
                 ice = std::move(m.ice)]() mutable {
                    dtls.reset();
                    socket.reset();
                    ice.reset();
                });
    }
}

void
WebRtcCall::onRemoteDescription(const std::string& type, const std::string& sdp)
{
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or leg_ == Leg::Sip)
            return;
    }
    auto description = webrtc::parse(sdp);
    if (not description) {
        fail("The peer's description is unreadable");
        return;
    }
    if (type == "answer") {
        applyAnswer(*description);
        return;
    }

    bool crossed = false;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or leg_ == Leg::Sip)
            return;
        if (leg_ == Leg::Pending) {
            // The peer's browser was calling too: it offers, which answers
            // this call.
            runOnMainThread([w = weakFromThis(), offer = std::move(*description)]() mutable {
                auto call = w.lock();
                if (not call or not call->becomeWebCall())
                    return;
                {
                    // Answered from here on, as below.
                    std::lock_guard lk {call->callMutex_};
                    if (call->negotiation_ != Negotiation::None)
                        return;
                    call->negotiation_ = Negotiation::Done;
                }
                dht::ThreadPool::io().run([w, offer = std::move(offer)] {
                    if (auto call = w.lock())
                        call->negotiate(offer);
                });
            });
            return;
        }
        if (getConnectionState() != ConnectionState::CONNECTED)
            return;
        if (negotiation_ == Negotiation::Done) {
            // The browser renegotiates, for example to add a screen share.
        } else if (negotiation_ == Negotiation::Offered) {
            // Both offered at once: the polite side answers, the other waits
            // for its answer (perfect negotiation, as the web client).
            if (not isPolite()) {
                JAMI_LOG("[call:{}] Offers crossed: waiting for the peer's answer", getCallId());
                return;
            }
            crossed = true;
        }
    }
    if (crossed) {
        JAMI_LOG("[call:{}] Offers crossed: answering the peer's", getCallId());
        stopTransports();
        std::lock_guard lk {callMutex_};
        negotiation_ = Negotiation::None;
    }

    std::unique_lock lk {callMutex_};
    if (negotiation_ == Negotiation::Done) {
        lk.unlock();
        answerAgain(*description);
        return;
    }
    // Answered from here on: a second offer is a renegotiation.
    negotiation_ = Negotiation::Done;
    if (timer_)
        timer_->cancel();
    lk.unlock();
    negotiate(*description);
}

bool
WebRtcCall::createTransports(std::vector<Media>& media, bool controlling)
{
    auto account = getSIPAccount();
    if (not account) {
        fail("No account");
        return false;
    }
    auto factory = Manager::instance().getIceTransportFactory();
    for (size_t i = 0; i < media.size(); ++i) {
        auto& m = media[i];
        m.ice = factory->createTransport(fmt::format("{}:{}", getCallId(), i));
        if (not m.ice) {
            fail("Unable to create an ICE transport");
            return false;
        }
        auto options = account->getIceOptions();
        // As SIP calls do: the address the DHT published, for a server
        // reflexive candidate (Jami configures no STUN server).
        options.accountPublicAddr = account->getPublishedIpAddress();
        if (options.accountPublicAddr) {
            options.accountLocalAddr = dhtnet::ip_utils::getInterfaceAddr(account->getLocalInterface(),
                                                                          options.accountPublicAddr.getFamily());
        } else {
            options.accountLocalAddr = dhtnet::ip_utils::getInterfaceAddr(account->getLocalInterface(), AF_INET);
            if (not options.accountLocalAddr)
                options.accountLocalAddr = dhtnet::ip_utils::getInterfaceAddr(account->getLocalInterface(), AF_INET6);
            options.accountPublicAddr = options.accountLocalAddr;
        }
        options.onInitDone = {};
        options.onNegoDone = [w = weakFromThis(), i](bool ok) {
            dht::ThreadPool::io().run([w, i, ok] {
                if (auto call = w.lock())
                    call->onIceDone(i, ok);
            });
        };
        // The offerer controls (RFC 8445).
        options.master = controlling;
        options.streamsCount = 1;
        options.compCountPerStream = 1;
        options.qosType = {m.type == MediaType::MEDIA_AUDIO ? dhtnet::QosType::VOICE : dhtnet::QosType::VIDEO};
        m.ice->initIceInstance(options);
    }
    for (auto& m : media) {
        if (not m.ice->waitForInitialization(ICE_INIT_TIMEOUT)) {
            fail("ICE gathering failed");
            return false;
        }
        // Listening before ICE starts: the peer's first DTLS message may come
        // before this side learns that ICE is done.
        m.socket = makeIceDatagramSocket(std::make_unique<dhtnet::IceSocket>(m.ice, 1));
    }
    return true;
}

std::vector<IceCandidate>
WebRtcCall::usableCandidates(dhtnet::IceTransport& ice, const webrtc::MediaSection& section)
{
    std::vector<IceCandidate> candidates;
    for (const auto& line : section.candidates) {
        IceCandidate candidate;
        if (isUsableCandidate(line) and ice.parseIceAttributeLine(0, line, candidate))
            candidates.emplace_back(candidate);
    }
    return candidates;
}

void
WebRtcCall::sendDescription(const char* type, const webrtc::SessionDescription& description)
{
    uint64_t version;
    {
        std::lock_guard lk {callMutex_};
        localDescription_ = description;
        version = ++descriptionVersion_;
    }
    Json::Value data(Json::objectValue);
    data["sdp"]["type"] = type;
    data["sdp"]["sdp"] = webrtc::serialize(description, std::hash<std::string> {}(sessionId_), version);
    signal("onWebRtcDescription", std::move(data));
}

void
WebRtcCall::makeOffer()
{
    std::vector<Media> media;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or leg_ != Leg::Web or negotiation_ != Negotiation::None)
            return;
        if (not identity_)
            identity_ = DtlsIdentity::generate();
        for (size_t stream = 0; stream < rtpStreams_.size(); ++stream) {
            const auto& attr = *rtpStreams_[stream].mediaAttribute_;
            if (not attr.enabled_)
                continue;
            auto codecs = webrtc::localCodecs(attr.type_, codecPreference(attr.type_));
            if (codecs.empty())
                continue;
            auto& m = media.emplace_back();
            m.stream = stream;
            m.type = attr.type_;
            m.mid = std::to_string(media.size() - 1);
            m.offeredCodecs = std::move(codecs);
        }
    }
    if (media.empty()) {
        fail("Nothing to offer the peer");
        return;
    }
    if (not createTransports(media, true))
        return;

    webrtc::SessionDescription offer;
    for (auto& m : media) {
        auto& section = offer.media.emplace_back();
        section.type = m.type;
        section.mid = m.mid;
        section.codecs = m.offeredCodecs;
        section.direction = m.localDirection;
        auto attributes = m.ice->getLocalAttributes();
        section.iceUfrag = attributes.ufrag;
        section.icePwd = attributes.pwd;
        section.candidates = m.ice->getLocalCandidates(1);
        section.fingerprintAlgorithm = "sha-256";
        // The answering browser picks its DTLS role.
        section.setup = "actpass";
        section.rtcpMux = true;
    }
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or negotiation_ != Negotiation::None)
            return;
        for (size_t i = 0; i < media.size(); ++i) {
            offer.media[i].fingerprint = identity_->fingerprint;
            // One stream for audio and video, so the browser plays them together.
            offer.media[i].msid = getCallId() + " " + rtpStreams_[media[i].stream].mediaAttribute_->label_;
        }
        media_ = std::move(media);
        negotiation_ = Negotiation::Offered;
        armTimeout("The peer sent no answer");
    }
    sendDescription("offer", offer);
}

void
WebRtcCall::applyAnswer(const webrtc::SessionDescription& answer)
{
    auto account = getSIPAccount();
    if (not account) {
        fail("No account");
        return;
    }

    // The ICE transport of each m-line the browser took, and its peer's.
    struct Start
    {
        std::shared_ptr<dhtnet::IceTransport> ice;
        const webrtc::MediaSection* section;
        std::vector<IceCandidate> candidates;
    };
    std::vector<Start> starts;
    std::vector<std::shared_ptr<dhtnet::IceTransport>> unused;
    std::string error;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or negotiation_ != Negotiation::Offered) {
            JAMI_WARNING("[call:{}] Unexpected answer from the peer", getCallId());
            return;
        }
        negotiation_ = Negotiation::Done;
        if (timer_)
            timer_->cancel();

        for (size_t i = 0; i < media_.size() and error.empty(); ++i) {
            auto& m = media_[i];
            auto section = std::find_if(answer.media.begin(), answer.media.end(), [&](const auto& s) {
                return s.mid == m.mid;
            });
            if (section == answer.media.end() or section->rejected) {
                m.rejected = true;
                unused.emplace_back(std::move(m.ice));
                continue;
            }
            if (not hasTransport(*section)) {
                error = "The peer's answer lacks ICE, a SHA-256 fingerprint or rtcp-mux";
                break;
            }
            // One of the codecs offered, the account's favorite.
            auto codec = webrtc::chooseCodec(*section, codecPreference(m.type));
            if (codec
                and std::none_of(m.offeredCodecs.begin(), m.offeredCodecs.end(), [&](const webrtc::RtpCodec& c) {
                        return c.payloadType == codec->payloadType and equalsIgnoreCase(c.name, codec->name);
                    }))
                codec.reset();
            std::shared_ptr<SystemCodecInfo> codecInfo;
            if (codec) {
                for (const auto& info : account->getActiveAccountCodecInfoList(m.type)) {
                    if (equalsIgnoreCase(info->name, codec->name)) {
                        codecInfo = info;
                        break;
                    }
                }
            }
            if (not codecInfo) {
                error = "No codec in common with the peer";
                break;
            }
            m.codec = *codec;
            setFeedback(m.codec, m.type);
            m.codecInfo = codecInfo;
            m.remoteDirection = section->direction;
            m.remoteUfrag = section->iceUfrag;
            m.remoteFingerprint = section->fingerprint;
            // The answerer chose: a passive browser waits for this side.
            m.dtlsClient = section->setup == "passive";
            starts.push_back({m.ice, &*section, {}});
        }
    }
    for (auto& ice : unused) {
        if (ice)
            dht::ThreadPool::io().run([ice = std::move(ice)]() mutable { ice.reset(); });
    }
    if (error.empty() and starts.empty())
        error = "The peer takes none of the media";
    for (auto& start : starts) {
        if (not error.empty())
            break;
        start.candidates = usableCandidates(*start.ice, *start.section);
        if (start.candidates.empty())
            error = "The peer gave no usable ICE candidate: its browser needs a STUN or TURN server";
    }
    if (not error.empty()) {
        fail(error);
        return;
    }

    for (auto& start : starts) {
        if (not start.ice->startIce({start.section->iceUfrag, start.section->icePwd}, std::move(start.candidates))) {
            fail("ICE did not start");
            return;
        }
    }
    reportMediaNegotiationStatus();
}

void
WebRtcCall::negotiate(const webrtc::SessionDescription& offer)
{
    auto account = getSIPAccount();
    if (not account) {
        fail("No account");
        return;
    }

    webrtc::SessionDescription answer;
    std::vector<Media> media;
    // The m-line of each media, in the offer and the answer alike.
    std::vector<size_t> sections;
    {
        std::lock_guard lk {callMutex_};
        if (not identity_)
            identity_ = DtlsIdentity::generate();
        std::vector<bool> used(rtpStreams_.size(), false);
        for (const auto& section : offer.media) {
            auto& answered = answer.media.emplace_back();
            answered.type = section.type;
            answered.media = section.media;
            answered.protocol = section.protocol;
            answered.formats = section.formats;
            answered.mid = section.mid;
            answered.codecs = section.codecs;
            answered.rejected = true;

            if (section.rejected or section.type == MediaType::MEDIA_NONE)
                continue;
            if (not hasTransport(section)) {
                JAMI_WARNING("[call:{}] m-line {} lacks ICE, a SHA-256 fingerprint or rtcp-mux", getCallId(), section.mid);
                continue;
            }
            size_t stream = 0;
            while (stream < rtpStreams_.size()
                   and (used[stream] or rtpStreams_[stream].mediaAttribute_->type_ != section.type
                        or not rtpStreams_[stream].mediaAttribute_->enabled_))
                ++stream;
            if (stream == rtpStreams_.size())
                continue;
            auto codec = webrtc::chooseCodec(section, codecPreference(section.type));
            if (not codec) {
                JAMI_WARNING("[call:{}] No codec in common for m-line {}", getCallId(), section.mid);
                continue;
            }
            std::shared_ptr<SystemCodecInfo> codecInfo;
            for (const auto& info : account->getActiveAccountCodecInfoList(section.type)) {
                if (equalsIgnoreCase(info->name, codec->name)) {
                    codecInfo = info;
                    break;
                }
            }
            if (not codecInfo)
                continue;
            used[stream] = true;

            Media m;
            m.stream = stream;
            m.type = section.type;
            m.mid = section.mid;
            m.codec = *codec;
            setFeedback(m.codec, m.type);
            m.codecInfo = codecInfo;
            m.remoteDirection = section.direction;
            m.localDirection = webrtc::answerDirection(section.direction);
            m.remoteUfrag = section.iceUfrag;
            m.remoteFingerprint = section.fingerprint;
            // The answerer chooses the DTLS role unless the offerer insists.
            m.dtlsClient = section.setup != "active";

            answered.rejected = false;
            answered.codecs = {m.codec};
            answered.direction = m.localDirection;
            answered.setup = m.dtlsClient ? "active" : "passive";
            answered.fingerprintAlgorithm = "sha-256";
            answered.fingerprint = identity_->fingerprint;
            answered.rtcpMux = true;
            // One stream for audio and video, so the browser plays them together.
            answered.msid = getCallId() + " " + rtpStreams_[stream].mediaAttribute_->label_;

            media.emplace_back(std::move(m));
            sections.emplace_back(answer.media.size() - 1);
        }
    }
    if (media.empty()) {
        fail("Nothing in the peer's offer is usable");
        return;
    }

    // One ICE transport per m-line, controlled: this side answers.
    if (not createTransports(media, false))
        return;

    for (size_t i = 0; i < media.size(); ++i) {
        auto& section = answer.media[sections[i]];
        auto attributes = media[i].ice->getLocalAttributes();
        section.iceUfrag = attributes.ufrag;
        section.icePwd = attributes.pwd;
        section.candidates = media[i].ice->getLocalCandidates(1);
    }

    // The peer's candidates, before the transports become reachable.
    std::vector<std::vector<IceCandidate>> remoteCandidates(media.size());
    for (size_t i = 0; i < media.size(); ++i) {
        remoteCandidates[i] = usableCandidates(*media[i].ice, offer.media[sections[i]]);
        if (remoteCandidates[i].empty()) {
            fail("The peer gave no usable ICE candidate: its browser needs a STUN or TURN server");
            return;
        }
    }

    {
        std::lock_guard lk {callMutex_};
        if (exited_)
            return;
        media_ = std::move(media);
    }
    sendDescription("answer", answer);

    std::vector<std::shared_ptr<dhtnet::IceTransport>> transports;
    {
        std::lock_guard lk {callMutex_};
        for (const auto& m : media_)
            transports.emplace_back(m.ice);
    }
    for (size_t i = 0; i < transports.size(); ++i) {
        const auto& section = offer.media[sections[i]];
        if (not transports[i]
            or not transports[i]->startIce({section.iceUfrag, section.icePwd}, std::move(remoteCandidates[i]))) {
            fail("ICE did not start");
            return;
        }
    }
    reportMediaNegotiationStatus();
}

void
WebRtcCall::answerAgain(const webrtc::SessionDescription& offer)
{
    // The browser offers again, for example to add a screen share: the media
    // in use go on as they are, on their transports, and new media are
    // declined, so that the browser's negotiation completes.
    webrtc::SessionDescription answer;
    {
        std::lock_guard lk {callMutex_};
        if (exited_)
            return;
        for (const auto& section : offer.media) {
            auto& answered = answer.media.emplace_back();
            answered.type = section.type;
            answered.media = section.media;
            answered.protocol = section.protocol;
            answered.formats = section.formats;
            answered.mid = section.mid;
            answered.codecs = section.codecs;
            answered.rejected = true;
            if (section.rejected)
                continue;

            auto m = std::find_if(media_.begin(), media_.end(), [&](const Media& m) {
                return m.mid == section.mid and not m.rejected;
            });
            auto local = std::find_if(localDescription_.media.begin(),
                                      localDescription_.media.end(),
                                      [&](const webrtc::MediaSection& s) {
                                          return s.mid == section.mid and not s.rejected;
                                      });
            if (m == media_.end() or local == localDescription_.media.end())
                continue;
            if (std::none_of(section.codecs.begin(), section.codecs.end(), [&](const webrtc::RtpCodec& c) {
                    return c.payloadType == m->codec.payloadType and equalsIgnoreCase(c.name, m->codec.name);
                })) {
                JAMI_WARNING("[call:{}] The peer's new offer drops the codec of m-line {}", getCallId(), section.mid);
                continue;
            }
            if (section.iceUfrag != m->remoteUfrag)
                JAMI_WARNING("[call:{}] The peer restarts ICE for m-line {}, which web calls do not support yet",
                             getCallId(),
                             section.mid);

            answered = *local;
            answered.codecs = {m->codec};
            answered.direction = webrtc::answerDirection(section.direction);
            // The roles the handshake took.
            answered.setup = m->dtlsClient ? "active" : "passive";
        }
    }
    JAMI_LOG("[call:{}] Answering the peer's new offer with the media in use", getCallId());
    sendDescription("answer", answer);
}

void
WebRtcCall::onIceDone(size_t index, bool ok)
{
    if (not ok) {
        fail("ICE failed");
        return;
    }

    auto account = std::dynamic_pointer_cast<JamiAccount>(getAccount().lock());
    if (not account) {
        fail("No account");
        return;
    }

    DtlsSrtpTransport::Config config;
    std::unique_ptr<DatagramSocket> socket;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or index >= media_.size() or media_[index].rejected or media_[index].dtls
            or not media_[index].socket)
            return;
        socket = std::move(media_[index].socket);
        config.identity = *identity_;
        config.peerFingerprint = media_[index].remoteFingerprint;
        config.client = media_[index].dtlsClient;
    }
    config.certStore = &account->certStore();
    config.ioContext = Manager::instance().ioContext();
    config.logger = Logger::dhtLogger(fmt::format("Call {}", getCallId()));
    config.onClosed = [w = weakFromThis()] {
        dht::ThreadPool::io().run([w] {
            if (auto call = w.lock())
                call->onPeerGone("The peer closed its DTLS session");
        });
    };

    std::shared_ptr<DtlsSrtpTransport> dtls;
    try {
        dtls = std::make_shared<DtlsSrtpTransport>(std::move(socket),
                                                   std::move(config),
                                                   [w = weakFromThis(), index](std::optional<SrtpKeys> keys) {
                                                       dht::ThreadPool::io().run(
                                                           [w, index, keys = std::move(keys)]() mutable {
                                                               if (auto call = w.lock())
                                                                   call->onDtlsKeys(index, std::move(keys));
                                                           });
                                                   });
    } catch (const std::exception& e) {
        fail(fmt::format("Unable to start DTLS: {}", e.what()));
        return;
    }

    std::optional<SrtpKeys> keys;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or index >= media_.size())
            return;
        media_[index].dtls = std::move(dtls);
        // The handshake may have ended while the transport was being stored.
        keys = std::exchange(media_[index].pendingKeys, std::nullopt);
    }
    if (keys)
        startMedia(index, *keys);
}

void
WebRtcCall::onDtlsKeys(size_t index, std::optional<SrtpKeys> keys)
{
    if (not keys) {
        fail("DTLS failed, or the peer is not who its description says");
        return;
    }
    {
        std::lock_guard lk {callMutex_};
        if (index < media_.size() and not media_[index].dtls) {
            // onIceDone stores the transport next, then starts the media.
            media_[index].pendingKeys = std::move(keys);
            return;
        }
    }
    startMedia(index, *keys);
}

void
WebRtcCall::startMedia(size_t index, const SrtpKeys& keys)
{
    std::shared_ptr<RtpSession> session;
    std::unique_ptr<SocketPair> socketPair;
    int streamIdx = -1;
    bool isVideo = false;
    {
        std::lock_guard lk {callMutex_};
        if (exited_ or index >= media_.size() or not media_[index].dtls)
            return;
        auto& m = media_[index];
        auto& stream = rtpStreams_[m.stream];
        auto type = stream.mediaAttribute_->type_;

        // What the browser sends and receives, from this side: the remote
        // description is the sending side, the local one the receiving side.
        MediaDescription local, remote;
        for (auto* description : {&local, &remote}) {
            description->type = type;
            description->enabled = true;
            description->codec = m.codecInfo;
            description->payload_type = m.codec.payloadType;
            description->rtp_clockrate = m.codec.clockRate;
            if (type == MediaType::MEDIA_VIDEO)
                description->parameters = m.codec.fmtp;
        }
        local.direction_ = m.localDirection;
        remote.direction_ = m.remoteDirection;
        // Not sending to a peer that does not receive, and the reverse.
        remote.hold = m.remoteDirection == MediaDirection::SENDONLY or m.remoteDirection == MediaDirection::INACTIVE;
        local.hold = m.localDirection == MediaDirection::SENDONLY or m.localDirection == MediaDirection::INACTIVE;
        local.receiving_sdp = webrtc::receivingSdp(type, m.codec);

        configureRtpSession(stream.rtpSession_, stream.mediaAttribute_, local, remote);
        // configureRtpSession mutes by the media's own state: a leg on hold,
        // or a mesh leg whose conference host is muted or detached, sends
        // nothing either.
        stream.rtpSession_->setMuted(sendMuted(stream), RtpSession::Direction::SEND);
        if (stream.mediaAttribute_->hold_ and type == MediaType::MEDIA_VIDEO)
            stream.rtpSession_->setMuted(true, RtpSession::Direction::RECV);
        stream.remoteMediaAttribute_ = std::make_shared<MediaAttribute>(type,
                                                                        false,
                                                                        true,
                                                                        true,
                                                                        std::string_view {},
                                                                        stream.mediaAttribute_->label_);
        socketPair = std::make_unique<SocketPair>(m.dtls);
        try {
            socketPair->createSRTP(keys.suite.c_str(), keys.outgoing.c_str(), keys.suite.c_str(), keys.incoming.c_str());
        } catch (const std::exception& e) {
            JAMI_ERROR("[call:{}] SRTP setup failed: {}", getCallId(), e.what());
            socketPair.reset();
        }
        session = stream.rtpSession_;
        streamIdx = static_cast<int>(m.stream);
        isVideo = type == MediaType::MEDIA_VIDEO;
    }
    if (not socketPair) {
        fail("SRTP setup failed");
        return;
    }

    session->start(std::move(socketPair));
    JAMI_LOG("[call:{}] Media {} with the browser started", getCallId(), streamIdx);
    {
        std::lock_guard lk {callMutex_};
        if (not silenceTimer_)
            armSilenceCheck();
    }
    if (isVideo)
        requestKeyframe(streamIdx);
}

void
WebRtcCall::requestKeyframe(int streamIdx)
{
    if (not isWebLeg()) {
        SIPCall::requestKeyframe(streamIdx);
        return;
    }
#ifdef ENABLE_VIDEO
    std::vector<std::shared_ptr<RtpSession>> sessions;
    {
        std::lock_guard lk {callMutex_};
        auto now = std::chrono::steady_clock::now();
        if (now - lastKeyFrameRequest_ < KEYFRAME_REQUEST_INTERVAL)
            return;
        lastKeyFrameRequest_ = now;
        if (streamIdx >= 0 and static_cast<size_t>(streamIdx) < rtpStreams_.size())
            sessions.emplace_back(rtpStreams_[streamIdx].rtpSession_);
        else
            sessions = getRtpSessionList(MediaType::MEDIA_VIDEO);
    }
    for (const auto& session : sessions) {
        if (session and session->getMediaType() == MediaType::MEDIA_VIDEO)
            std::static_pointer_cast<video::VideoRtpSession>(session)->requestKeyFrameWithRtcp();
    }
#else
    (void) streamIdx;
#endif
}

void
WebRtcCall::sendSIPInfo(std::string_view body, std::string_view subtype)
{
    if (not isWebLeg()) {
        SIPCall::sendSIPInfo(body, subtype);
        return;
    }
    // Mute state, voice activity, orientation and recording state go to
    // daemons by SIP INFO; a browser has no channel for them.
    JAMI_DEBUG("[call:{}] Not sending {} to a browser", getCallId(), subtype);
}

void
WebRtcCall::transfer(const std::string& to)
{
    if (not isWebLeg()) {
        SIPCall::transfer(to);
        return;
    }
    JAMI_WARNING("[call:{}] Web calls cannot be transferred", getCallId());
}

bool
WebRtcCall::attendedTransfer(const std::string& to)
{
    if (not isWebLeg())
        return SIPCall::attendedTransfer(to);
    JAMI_WARNING("[call:{}] Web calls cannot be transferred", getCallId());
    return false;
}

bool
WebRtcCall::hold(OnReadyCb&& cb)
{
    if (not isWebLeg())
        return SIPCall::hold(std::move(cb));

    // A browser has no hold: this side stops sending (silence, no video) and
    // stops receiving video; with the callback, the apps stop playing its
    // audio (Manager unbinds it).
    bool held = getConnectionState() == ConnectionState::CONNECTED and setState(CallState::HOLD);
    if (held) {
        std::lock_guard lk {callMutex_};
        for (auto& stream : rtpStreams_) {
            stream.mediaAttribute_->hold_ = true;
            stream.rtpSession_->setMuted(true, RtpSession::Direction::SEND);
            if (stream.mediaAttribute_->type_ == MediaType::MEDIA_VIDEO)
                stream.rtpSession_->setMuted(true, RtpSession::Direction::RECV);
        }
    } else {
        JAMI_WARNING("[call:{}] Unable to hold the web call", getCallId());
    }
    if (cb)
        cb(held);
    return held;
}

bool
WebRtcCall::resume(OnReadyCb&& cb)
{
    if (not isWebLeg())
        return SIPCall::resume(std::move(cb));

    bool resumed = getState() == CallState::HOLD and setState(CallState::ACTIVE);
    if (resumed) {
        std::lock_guard lk {callMutex_};
        for (auto& stream : rtpStreams_) {
            stream.mediaAttribute_->hold_ = false;
            stream.rtpSession_->setMuted(sendMuted(stream), RtpSession::Direction::SEND);
            if (stream.mediaAttribute_->type_ == MediaType::MEDIA_VIDEO)
                stream.rtpSession_->setMuted(false, RtpSession::Direction::RECV);
        }
    }
    if (cb)
        cb(resumed);
    // Members who connected while it was on hold join the call now.
    if (resumed and group_)
        group_->onLegResumed(*this);
    return resumed;
}

bool
WebRtcCall::requestMediaChange(const std::vector<libjami::MediaMap>& mediaList)
{
    if (not isWebLeg())
        return SIPCall::requestMediaChange(mediaList);

    // Muting and changing a source need no renegotiation with a browser;
    // adding or removing media would, which web calls do not do yet.
    auto requested = MediaAttribute::buildMediaAttributesList(mediaList, isSrtpEnabled());
    {
        std::lock_guard lk {callMutex_};
        for (const auto& attr : requested) {
            auto it = findStream(rtpStreams_, attr);
            if (it == rtpStreams_.end()) {
                // Such as the screen share's sound: ignored, the others apply.
                JAMI_WARNING("[call:{}] Web calls cannot add media yet: {}", getCallId(), attr.label_);
                continue;
            }
            auto& current = *it->mediaAttribute_;
            if (not attr.sourceUri_.empty() and attr.sourceUri_ != current.sourceUri_) {
                current.sourceUri_ = attr.sourceUri_;
                it->rtpSession_->setMediaSource(attr.sourceUri_);
                it->rtpSession_->restartSender();
            }
            if (attr.muted_ != current.muted_) {
                current.muted_ = attr.muted_;
                it->rtpSession_->setMuted(sendMuted(*it), RtpSession::Direction::SEND);
                if (current.type_ == MediaType::MEDIA_AUDIO)
                    emitSignal<libjami::CallSignal::AudioMuted>(getCallId(), attr.muted_);
                else
                    emitSignal<libjami::CallSignal::VideoMuted>(getCallId(), attr.muted_);
            }
        }
    }
    reportMediaNegotiationStatus();
    return true;
}

void
WebRtcCall::switchInput(const std::string& source)
{
    if (not isWebLeg()) {
        SIPCall::switchInput(source);
        return;
    }
    // Without a re-INVITE: the video senders restart on the new source, such
    // as the camera when a conference ends.
    std::lock_guard lk {callMutex_};
    for (auto& stream : rtpStreams_) {
        if (stream.mediaAttribute_->type_ != MediaType::MEDIA_VIDEO)
            continue;
        stream.mediaAttribute_->sourceUri_ = source;
        stream.rtpSession_->setMediaSource(source);
        stream.rtpSession_->restartSender();
    }
}

bool
WebRtcCall::sendMuted(const RtpStream& stream) const
{
    const auto& media = *stream.mediaAttribute_;
    if (media.muted_ or media.hold_ or hostDetached_)
        return true;
    return media.type_ == MediaType::MEDIA_VIDEO ? hostVideoMuted_ : hostAudioMuted_;
}

void
WebRtcCall::applyHostMedia(bool audioMuted, bool videoMuted, bool detached, const std::string& videoSource)
{
    if (not isMeshLeg())
        return;
    std::lock_guard lk {callMutex_};
    hostAudioMuted_ = audioMuted;
    hostVideoMuted_ = videoMuted;
    hostDetached_ = detached;
    for (auto& stream : rtpStreams_) {
        auto muted = sendMuted(stream);
        if (stream.mediaAttribute_->type_ != MediaType::MEDIA_VIDEO) {
            // The microphone reaches the legs through the conference, which
            // unbinds it; other sources, such as a file, do not.
            stream.rtpSession_->setMuted(muted, RtpSession::Direction::SEND);
            continue;
        }
        if (not videoSource.empty() and videoSource != stream.mediaAttribute_->sourceUri_) {
            stream.mediaAttribute_->sourceUri_ = videoSource;
            stream.rtpSession_->setMediaSource(videoSource);
            if (not muted)
                stream.rtpSession_->restartSender();
        }
        stream.rtpSession_->setMuted(muted, RtpSession::Direction::SEND);
    }
}

void
WebRtcCall::exitConference()
{
    bool audioMuted = false;
    bool videoMuted = false;
    bool detached = false;
    {
        std::lock_guard lk {callMutex_};
        if (isMeshLeg()) {
            audioMuted = hostAudioMuted_;
            videoMuted = hostVideoMuted_;
            detached = hostDetached_;
            // Alone with this member again, what the user muted in the
            // conference stays muted (SIPCall::exitConference then leaves
            // the microphone unbound).
            for (auto& stream : rtpStreams_) {
                auto& media = *stream.mediaAttribute_;
                if (media.type_ == MediaType::MEDIA_VIDEO ? videoMuted : audioMuted)
                    media.muted_ = true;
            }
        }
        hostAudioMuted_ = false;
        hostVideoMuted_ = false;
        hostDetached_ = false;
    }
    if (audioMuted)
        emitSignal<libjami::CallSignal::AudioMuted>(getCallId(), true);
    if (videoMuted)
        emitSignal<libjami::CallSignal::VideoMuted>(getCallId(), true);
    SIPCall::exitConference();
    // A conference on hold leaves this call on hold, not live again (with
    // the microphone SIPCall::exitConference just bound).
    if (detached and getState() == CallState::ACTIVE and hold({}))
        Manager::instance().removeAudio(*this);
}

void
WebRtcCall::answerMediaChangeRequest(const std::vector<libjami::MediaMap>& mediaList, bool isRemote)
{
    // Browsers renegotiate with an offer, never with a media change request.
    if (not isWebLeg())
        SIPCall::answerMediaChangeRequest(mediaList, isRemote);
}

bool
WebRtcCall::checkMediaChangeRequest(const std::vector<libjami::MediaMap>& remoteMediaList)
{
    return not isWebLeg() and SIPCall::checkMediaChangeRequest(remoteMediaList);
}

void
WebRtcCall::handleMediaChangeRequest(const std::vector<libjami::MediaMap>& remoteMediaList)
{
    if (not isWebLeg())
        SIPCall::handleMediaChangeRequest(remoteMediaList);
}

void
WebRtcCall::sendTextMessage(const std::map<std::string, std::string>& messages, const std::string& from)
{
    if (not isWebLeg()) {
        SIPCall::sendTextMessage(messages, from);
        return;
    }
    JAMI_DEBUG("[call:{}] In-call messages do not reach a browser", getCallId());
}

} // namespace jami
