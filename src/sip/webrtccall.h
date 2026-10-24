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

#include "sip/sipcall.h"
#include "sip/webrtc_sdp.h"
#include "media/dtls_srtp_transport.h"

#include <asio/steady_timer.hpp>
#include <json/json.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace dhtnet {
class IceTransport;
}

namespace jami {

class WebRtcCall;

// The group call that legs belong to (see WebCallSignaling): in a mesh, this
// device has a leg with each member in the call, and the group speaks for the
// call as a whole.
class WebCallGroup
{
public:
    virtual ~WebCallGroup() = default;
    // A mesh, where each member gets each other's media directly; else this
    // device hosts the call and sends each its mix.
    virtual bool isMesh() const { return true; }
    // The leg that rang was answered: this device joins the call.
    virtual void onLocalJoin(WebRtcCall& leg) = 0;
    // A leg ended, as its member left (byPeer) or as this device hung up.
    virtual void onLegEnded(WebRtcCall& leg, bool byPeer) = 0;
    // The user resumed a leg that was on hold.
    virtual void onLegResumed(WebRtcCall&) {}
};

// A call with a user of the web client, whose browser speaks WebRTC. The call
// is signaled with the web client's account messages instead of SIP; each
// m-line has its own ICE transport, which carries RTP, RTCP and DTLS; DTLS
// authenticates the browser by the fingerprint of its description and keys
// SRTP. The rest is a SIPCall, so the apps see an ordinary call.
//
// An incoming one is a browser's call. An outgoing one rings the peer's
// browsers (an invite) as well as its devices, which it calls by SIP as any
// call: when a browser answers first, the call goes on with it and the
// devices stop ringing; when a device does, it is an ordinary SIP call and
// the browsers stop ringing.
class WebRtcCall : public SIPCall
{
public:
    // Sends a message of the web client's protocol to the peer, such as
    // ("onWebRtcDescription", {"sdp": ...}): the sender adds the
    // conversation and this account's ID.
    using SendSignal = std::function<void(const std::string& type, Json::Value&& data)>;

    // A leg of a group call takes its group and the group's session.
    WebRtcCall(const std::shared_ptr<SIPAccountBase>& account,
               const std::string& id,
               Call::CallType type,
               const std::vector<libjami::MediaMap>& mediaList,
               std::string conversationId,
               std::string peerUri,
               SendSignal sendSignal,
               std::shared_ptr<WebCallGroup> group = {},
               std::string sessionId = {});
    ~WebRtcCall();

    // A session ID of the web client's protocol (CallAction.sessionId).
    static std::string newSessionId();

    const std::string& getConversationId() const { return conversationId_; }
    const std::string& getPeerAccountUri() const { return peerUri_; }
    // This side's session, new for each call (CallAction.sessionId).
    const std::string& getSessionId() const { return sessionId_; }
    // Whether the call was removed, without taking the call's lock.
    bool hasEnded() const { return ended_; }

    // An outgoing call rings the peer's browsers.
    void invite();
    // A browser of the peer answered an outgoing call: this side offers.
    void onPeerJoin();
    // A leg of a group call connects to its member, who joined.
    void offer();
    // A description from the peer, whose sender and membership were checked.
    void onRemoteDescription(const std::string& type, const std::string& sdp);
    // The peer left the call or declined it.
    void onPeerExit();
    // Another device of this account answered the browser's call.
    void onAnsweredElsewhere();

    // Call
    bool isMeshLeg() const override { return group_ and group_->isMesh(); }
    void applyHostMedia(bool audioMuted, bool videoMuted, bool detached, const std::string& videoSource) override;
    bool writesCallHistory() const override { return not group_; }
    void exitConference() override;
    void answer(const std::vector<libjami::MediaMap>& mediaList) override;
    void hangup(int code) override;
    void refuse() override;
    void transfer(const std::string& to) override;
    bool attendedTransfer(const std::string& to) override;
    bool hold(OnReadyCb&& cb) override;
    bool resume(OnReadyCb&& cb) override;
    bool requestMediaChange(const std::vector<libjami::MediaMap>& mediaList) override;
    void switchInput(const std::string& source = {}) override;
    void answerMediaChangeRequest(const std::vector<libjami::MediaMap>& mediaList, bool isRemote = false) override;
    bool checkMediaChangeRequest(const std::vector<libjami::MediaMap>& remoteMediaList) override;
    void handleMediaChangeRequest(const std::vector<libjami::MediaMap>& remoteMediaList) override;
    void sendTextMessage(const std::map<std::string, std::string>& messages, const std::string& from) override;
    void removeCall(int code = 0) override;

    // SIPCall
    void requestKeyframe(int streamIdx = -1) override;
    void sendSIPInfo(std::string_view body, std::string_view subtype) override;

protected:
    // A device of the peer answered an outgoing call.
    void merge(Call& call) override;

private:
    // Who carries an outgoing call: until the peer answers, both its
    // browsers and its devices ring. An incoming call is a browser's.
    enum class Leg { Pending, Web, Sip };
    enum class Negotiation { None, Offered, Done };

    // One m-line.
    struct Media
    {
        size_t stream {0}; // in rtpStreams_
        MediaType type {MediaType::MEDIA_NONE};
        std::string mid;
        // The browser did not take it.
        bool rejected {false};
        // When this side offered.
        std::vector<webrtc::RtpCodec> offeredCodecs;
        webrtc::RtpCodec codec;
        std::shared_ptr<SystemCodecInfo> codecInfo;
        // What each side does with it.
        MediaDirection localDirection {MediaDirection::SENDRECV};
        MediaDirection remoteDirection {MediaDirection::SENDRECV};
        std::string remoteUfrag;
        std::string remoteFingerprint;
        bool dtlsClient {true};
        std::shared_ptr<dhtnet::IceTransport> ice;
        // ICE component 1, until DTLS takes it.
        std::unique_ptr<DatagramSocket> socket;
        std::shared_ptr<DtlsSrtpTransport> dtls;
        // Keys of a handshake that ended before its transport was stored.
        std::optional<SrtpKeys> pendingKeys;
    };

    std::shared_ptr<WebRtcCall> shared();
    std::weak_ptr<WebRtcCall> weakFromThis();

    bool isWebLeg() const;
    // Whether this side gives up its offer when offers cross: as in the web
    // client, the side with the smaller account URI.
    bool isPolite() const;
    // Sends a message of the protocol, with this side's session.
    void signal(const std::string& type, Json::Value&& data);
    // Tells the peer's browsers the call ended, once, if they know of it.
    void exitWeb();
    // The peer answered in a browser: the call goes on with it.
    bool becomeWebCall();
    void makeOffer();
    void applyAnswer(const webrtc::SessionDescription& answer);
    void negotiate(const webrtc::SessionDescription& offer);
    void answerAgain(const webrtc::SessionDescription& offer);
    bool createTransports(std::vector<Media>& media, bool controlling);
    std::vector<IceCandidate> usableCandidates(dhtnet::IceTransport& ice, const webrtc::MediaSection& section);
    void sendDescription(const char* type, const webrtc::SessionDescription& description);
    void onIceDone(size_t media, bool ok);
    void onDtlsKeys(size_t media, std::optional<SrtpKeys> keys);
    void startMedia(size_t media, const SrtpKeys& keys);
    void fail(const std::string& reason);
    // The peer left without saying so.
    void onPeerGone(const std::string& reason);
    // Checks now and then that the peer still sends. Called with callMutex_ held.
    void armSilenceCheck();
    void stopTransports();
    // Fails the call unless a negotiation completes in time. Called with
    // callMutex_ held.
    void armTimeout(const char* reason);
    // Whether a stream sends nothing: muted, on hold, or, for a mesh leg, as
    // the conference's host is. Called with callMutex_ held.
    bool sendMuted(const RtpStream& stream) const;
    std::vector<std::string> codecPreference(MediaType type) const;

    const std::string conversationId_;
    const std::string peerUri_;
    const std::string sessionId_;
    SendSignal sendSignal_;
    const std::shared_ptr<WebCallGroup> group_;
    std::atomic_bool groupTold_ {false};

    std::atomic_bool ended_ {false};

    // Guarded by callMutex_.
    Leg leg_;
    Negotiation negotiation_ {Negotiation::None};
    // Whether the peer's browsers know of the call, and that it ended.
    bool invited_;
    bool exited_ {false};
    // The peer left (a group leg tells its group).
    bool peerLeft_ {false};
    // A mesh leg in a conference: the host's audio or video is muted, or the
    // host is detached (see applyHostMedia).
    bool hostAudioMuted_ {false};
    bool hostVideoMuted_ {false};
    bool hostDetached_ {false};
    std::optional<DtlsIdentity> identity_;
    std::vector<Media> media_;
    // The last description sent, to answer a new offer alike.
    webrtc::SessionDescription localDescription_;
    uint64_t descriptionVersion_ {0};
    std::unique_ptr<asio::steady_timer> timer_;
    std::unique_ptr<asio::steady_timer> silenceTimer_;
    std::chrono::steady_clock::time_point lastKeyFrameRequest_ {};
};

} // namespace jami
