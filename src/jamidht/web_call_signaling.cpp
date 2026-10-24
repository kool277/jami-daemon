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
#include "jamidht/web_call_signaling.h"

#include "call_factory.h"
#include "conference.h"
#include "jamidht/conversation_module.h"
#include "jamidht/jamiaccount.h"
#include "json_utils.h"
#include "logger.h"
#include "manager.h"
#include "media/media_attribute.h"
#include "sip/webrtccall.h"

#include <opendht/thread_pool.h>

#include <algorithm>
#include <array>
#include <chrono>

namespace jami {

namespace {

// A description is at most 64 KiB (webrtc::parse refuses more).
constexpr size_t MAX_MESSAGE_SIZE = 80 * 1024;
// Messages of one peer about one conversation waiting to be handled.
constexpr size_t MAX_QUEUED_MESSAGES = 64;
// Peer sessions remembered per conversation.
constexpr size_t MAX_RETIRED_SESSIONS = 16;
// Account messages wait for unreachable peers: an invite delivered later than
// this rings for a call long over. Clocks may disagree a little.
constexpr auto MAX_INVITE_AGE = std::chrono::minutes(2);
// Members a group call connects this device to: a mesh sends each one this
// device's media, so it does not grow far.
constexpr size_t MAX_GROUP_LEGS = 8;
constexpr size_t MAX_SESSION_ID_SIZE = 64;
constexpr std::string_view MIME_TYPE = "application/json";

constexpr std::array<std::string_view, 5> CALL_TYPES {
    "onCallInvite",
    "onCallJoin",
    "onCallExit",
    "onWebRtcDescription",
    "onWebRtcIceCandidate",
};

bool
isHex(std::string_view s, size_t minSize, size_t maxSize)
{
    return s.size() >= minSize and s.size() <= maxSize and std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' and c <= '9') or (c >= 'a' and c <= 'f') or (c >= 'A' and c <= 'F');
           });
}

// An invite delivered long after it was sent, as its sender says.
bool
isLate(const Json::Value& data)
{
    const auto& sentAt = data["sentAt"];
    if (not sentAt.isInt64())
        return false;
    auto sent = std::chrono::system_clock::time_point(std::chrono::milliseconds(sentAt.asInt64()));
    return std::chrono::system_clock::now() - sent > MAX_INVITE_AGE;
}

} // namespace

// A group call, in a conversation of more than two members. The web client
// makes it a mesh: whoever is in the call connects to each member who joins
// (sends it an offer), and the member who joins answers those offers. This
// device has a leg (a WebRtcCall) with each member it is connected to; the
// legs make a conference here, which sends each member this device's own media
// (see Call::isMeshLeg), as each gets the others' directly.
//
// Or this device hosts the conversation's call, as the native apps do: the
// browsers connect to this device alone, which sends each its mix, as to the
// native members who join by SIP.
class WebCallSignaling::Group : public WebCallGroup, public std::enable_shared_from_this<Group>
{
public:
    Group(std::weak_ptr<JamiAccount> account,
          std::string accountId,
          std::string conversationId,
          std::string hostedConfId = {},
          std::vector<libjami::MediaMap> hostMedia = {})
        : account_(std::move(account))
        , accountId_(std::move(accountId))
        , conversationId_(std::move(conversationId))
        , sessionId_(WebRtcCall::newSessionId())
        , hostedConfId_(std::move(hostedConfId))
    {
        if (not hostedConfId_.empty()) {
            state_ = State::Joined;
            confId_ = hostedConfId_;
            media_ = std::move(hostMedia);
        }
    }

    bool isMesh() const override { return hostedConfId_.empty(); }

    // This device hosts: the members' browsers ring (the native apps see the
    // conversation's active call and join by SIP).
    void inviteToHosted(bool withVideo)
    {
        Json::Value data(Json::objectValue);
        data["sessionId"] = sessionId_;
        data["withVideoOn"] = withVideo;
        data["native"] = true;
        data["hosted"] = true;
        if (auto account = account_.lock())
            account->webCallSignaling().broadcast(conversationId_, "onCallInvite", std::move(data));
    }

    // The hosted conference ended: the browsers leave.
    void endHosted()
    {
        std::vector<std::shared_ptr<WebRtcCall>> legs;
        {
            std::lock_guard lk(mutex_);
            if (state_ == State::Ended)
                return;
            state_ = State::Ended;
            for (const auto& member : members_) {
                if (auto leg = member.second.leg.lock())
                    legs.emplace_back(std::move(leg));
            }
        }
        for (const auto& leg : legs)
            leg->hangup(0);
        end(true);
    }

    bool isHosting(const std::string& confId) const { return not hostedConfId_.empty() and hostedConfId_ == confId; }

    // The first invite: this device rings, with a leg to the member who invited.
    void ring(const std::string& from, const Message& message, bool withVideo)
    {
        auto account = account_.lock();
        if (not account)
            return;
        auto media = MediaAttribute::mediaAttributesToMediaMaps(account->createDefaultMediaList(withVideo));
        auto leg = newLeg(Call::CallType::INCOMING, from, media);
        if (not leg) {
            end(false);
            return;
        }
        {
            std::lock_guard lk(mutex_);
            members_[from] = {message.sessionId, message.device, leg};
            ringing_ = leg;
        }
        leg->setState(Call::ConnectionState::PROGRESSING);
        leg->setState(Call::ConnectionState::TRYING);
        leg->setState(Call::ConnectionState::RINGING);
        Manager::instance().incomingCall(accountId_, *leg);
    }

    // Another member calls too: it is in the call.
    void onInvite(const std::string& from, const Message& message) { onJoin(from, message); }

    void onJoin(const std::string& from, const Message& message)
    {
        {
            std::lock_guard lk(mutex_);
            if (state_ == State::Ended)
                return;
            auto& member = members_[from];
            auto leg = member.leg.lock();
            if (leg and not leg->hasEnded() and member.session == message.sessionId)
                return;
            if (state_ == State::Ringing) {
                // It connects to this device once this one joins.
                member.session = message.sessionId;
                member.device = message.device;
                return;
            }
        }
        // This device is in the call: it connects to the member who joined.
        connect(from, message, true);
    }

    void onDescription(const std::string& from, const Message& message)
    {
        const auto& sdp = message.data["sdp"];
        auto type = sdp["type"].asString();
        std::shared_ptr<WebRtcCall> leg;
        State state;
        {
            std::lock_guard lk(mutex_);
            state = state_;
            auto it = members_.find(from);
            if (it != members_.end() and (it->second.session.empty() or it->second.session == message.sessionId))
                leg = it->second.leg.lock();
        }
        if (leg and not leg->hasEnded()) {
            leg->onRemoteDescription(type, sdp["sdp"].asString());
            return;
        }
        // A member in the call connects to this device, which joined.
        if (type == "offer" and state == State::Joined) {
            if (auto leg = connect(from, message, false))
                leg->onRemoteDescription(type, sdp["sdp"].asString());
        }
    }

    void onExit(const std::string& from, const Message& message)
    {
        std::shared_ptr<WebRtcCall> leg;
        {
            std::lock_guard lk(mutex_);
            auto it = members_.find(from);
            if (it == members_.end())
                return;
            // The exit of an earlier session of a member who came back since.
            if (not it->second.session.empty() and not message.sessionId.empty()
                and it->second.session != message.sessionId)
                return;
            leg = it->second.leg.lock();
            members_.erase(it);
            if (state_ == State::Ringing) {
                // It rings while anyone is in the call.
                leg = members_.empty() ? ringing_.lock() : nullptr;
            }
        }
        if (leg)
            leg->onPeerExit();
    }

    // Another device of this account joined: this one stops ringing.
    void onAnsweredElsewhere()
    {
        std::shared_ptr<WebRtcCall> ringing;
        {
            std::lock_guard lk(mutex_);
            if (state_ != State::Ringing)
                return;
            ringing = ringing_.lock();
        }
        if (ringing)
            ringing->onAnsweredElsewhere();
    }

    // Where a member's session is, for messages to it alone.
    std::string deviceOf(const std::string& peer)
    {
        std::lock_guard lk(mutex_);
        auto it = members_.find(peer);
        return it == members_.end() ? std::string {} : it->second.device;
    }

    // WebCallGroup
    void onLocalJoin(WebRtcCall& leg) override
    {
        {
            std::lock_guard lk(mutex_);
            if (state_ != State::Ringing)
                return;
            state_ = State::Joined;
            // What the user answered with goes to every member.
            media_ = MediaAttribute::mediaAttributesToMediaMaps(leg.getMediaAttributeList());
        }
        JAMI_LOG("[Account {}] Joining the group call in {}", accountId_, conversationId_);
        Json::Value data(Json::objectValue);
        data["sessionId"] = sessionId_;
        if (auto account = account_.lock())
            account->webCallSignaling().broadcast(conversationId_, "onCallJoin", std::move(data));
    }

    void onLegEnded(WebRtcCall& leg, bool byPeer) override
    {
        bool leave = false;
        bool tell = false;
        {
            std::lock_guard lk(mutex_);
            if (state_ == State::Ended)
                return;
            for (auto it = members_.begin(); it != members_.end(); ++it) {
                if (it->second.leg.lock().get() == &leg) {
                    if (byPeer)
                        members_.erase(it);
                    else
                        it->second.leg.reset();
                    break;
                }
            }
            if (not hostedConfId_.empty()) {
                // The hosted call goes on, with the native members.
            } else if (state_ == State::Ringing) {
                // Declined (told to the members), answered elsewhere, or over.
                if (ringing_.lock().get() == &leg or ringing_.expired()) {
                    leave = true;
                    tell = not byPeer;
                }
            } else {
                // This device left the call when it has no leg left.
                leave = std::none_of(members_.begin(), members_.end(), [&](const auto& member) {
                    auto other = member.second.leg.lock();
                    return other and other.get() != &leg and not other->hasEnded();
                });
                tell = leave;
            }
            if (leave)
                state_ = State::Ended;
        }
        if (leave)
            end(tell);
    }

private:
    enum class State { Ringing, Joined, Ended };
    struct Member
    {
        std::string session;
        std::string device;
        std::weak_ptr<WebRtcCall> leg;
    };

    std::shared_ptr<WebRtcCall> newLeg(Call::CallType type,
                                       const std::string& peer,
                                       const std::vector<libjami::MediaMap>& media)
    {
        auto account = account_.lock();
        if (not account)
            return {};
        auto sendSignal = [w = account_, conversationId = conversationId_, peer, g = weak_from_this()](
                              const std::string& type, Json::Value&& data) {
            auto account = w.lock();
            auto group = g.lock();
            if (account and group)
                account->webCallSignaling().send(peer, conversationId, type, std::move(data), group->deviceOf(peer));
        };
        auto leg = Manager::instance().callFactory.newCall<WebRtcCall>(std::static_pointer_cast<SIPAccountBase>(account),
                                                                       type,
                                                                       media,
                                                                       conversationId_,
                                                                       peer,
                                                                       std::move(sendSignal),
                                                                       shared_from_this(),
                                                                       sessionId_);
        if (not leg)
            return {};
        leg->setPeerUri("jami:" + peer);
        leg->setPeerNumber(peer);
        return leg;
    }

    // A leg with a member in the call, which this device offers to (it
    // joined) or answers (this device joined): connected without ringing, in
    // the conference.
    std::shared_ptr<WebRtcCall> connect(const std::string& peer, const Message& message, bool offering)
    {
        std::vector<libjami::MediaMap> media;
        {
            std::lock_guard lk(mutex_);
            if (state_ != State::Joined)
                return {};
            auto legs = std::count_if(members_.begin(), members_.end(), [](const auto& member) {
                auto leg = member.second.leg.lock();
                return leg and not leg->hasEnded();
            });
            if (static_cast<size_t>(legs) >= MAX_GROUP_LEGS) {
                JAMI_WARNING("[Account {}] Group call in {} already connects {} members: not {}",
                             accountId_,
                             conversationId_,
                             legs,
                             peer);
                return {};
            }
            media = media_;
        }
        auto leg = newLeg(offering ? Call::CallType::OUTGOING : Call::CallType::INCOMING, peer, media);
        if (not leg)
            return {};
        std::shared_ptr<WebRtcCall> previous;
        {
            std::lock_guard lk(mutex_);
            auto& member = members_[peer];
            previous = member.leg.lock();
            member = {message.sessionId, message.device, leg};
        }
        // An earlier session of the member, which came back (a reload).
        if (previous)
            previous->onPeerExit();
        leg->setState(Call::CallState::ACTIVE, Call::ConnectionState::CONNECTED);
        attach(leg);
        if (offering)
            leg->offer();
        return leg;
    }

    // Into the conference of the call's legs, made with the second one.
    void attach(const std::shared_ptr<WebRtcCall>& leg)
    {
        runOnMainThread([g = weak_from_this(), w = std::weak_ptr<WebRtcCall>(leg)] {
            auto group = g.lock();
            auto leg = w.lock();
            auto account = group ? group->account_.lock() : nullptr;
            if (not group or not leg or not account or leg->hasEnded())
                return;
            std::string confId;
            std::string other;
            {
                std::lock_guard lk(group->mutex_);
                confId = group->confId_;
                // A conference down to one leg was dissolved.
                if (not confId.empty() and not account->getConference(confId))
                    confId.clear();
                for (const auto& member : group->members_) {
                    if (not confId.empty())
                        break;
                    auto candidate = member.second.leg.lock();
                    if (candidate and candidate != leg and not candidate->hasEnded()
                        and candidate->getConnectionState() == Call::ConnectionState::CONNECTED) {
                        other = candidate->getCallId();
                        break;
                    }
                }
            }
            if (not confId.empty()) {
                Manager::instance().addSubCall(group->accountId_, leg->getCallId(), group->accountId_, confId);
                return;
            }
            // The hosted conference ended.
            if (not group->hostedConfId_.empty()) {
                leg->hangup(0);
                return;
            }
            // The only leg: a call with one member, until another connects.
            if (other.empty())
                return;
            if (Manager::instance().joinParticipant(group->accountId_, other, group->accountId_, leg->getCallId())) {
                if (auto conference = leg->getConference()) {
                    std::lock_guard lk(group->mutex_);
                    group->confId_ = conference->getConfId();
                }
            }
        });
    }

    // This device is out of the call: the members hear it (unless it never
    // joined), and a next call in the conversation is a new group.
    void end(bool tellMembers)
    {
        auto account = account_.lock();
        if (not account)
            return;
        if (tellMembers) {
            Json::Value data(Json::objectValue);
            data["sessionId"] = sessionId_;
            account->webCallSignaling().broadcast(conversationId_, "onCallExit", std::move(data));
        }
        account->webCallSignaling().endGroup(conversationId_, this);
    }

    std::weak_ptr<JamiAccount> account_;
    const std::string accountId_;
    const std::string conversationId_;
    // This device's session in the call, for every leg.
    const std::string sessionId_;
    // The conference this device hosts, if it does.
    const std::string hostedConfId_;

    std::mutex mutex_;
    State state_ {State::Ringing};
    // The members in the call, and this device's leg with each.
    std::map<std::string, Member> members_;
    std::weak_ptr<WebRtcCall> ringing_;
    std::vector<libjami::MediaMap> media_;
    std::string confId_;
};

WebCallSignaling::WebCallSignaling(JamiAccount& account)
    : account_(std::static_pointer_cast<JamiAccount>(account.shared_from_this()))
    , accountId_(account.getAccountID())
{}

WebCallSignaling::~WebCallSignaling() = default;

std::optional<WebCallSignaling::Message>
WebCallSignaling::parse(const std::string& from, std::string_view payload, bool& isCall)
{
    isCall = false;
    if (payload.size() > MAX_MESSAGE_SIZE)
        return std::nullopt;

    Json::Value root;
    std::string error;
    auto reader = std::unique_ptr<Json::CharReader>(json::rbuilder.newCharReader());
    if (not reader->parse(payload.data(), payload.data() + payload.size(), &root, &error) or not root.isObject())
        return std::nullopt;
    const auto& type = root["type"];
    if (not type.isString()
        or std::find(CALL_TYPES.begin(), CALL_TYPES.end(), type.asString()) == CALL_TYPES.end())
        return std::nullopt;
    isCall = true;

    const auto& data = root["data"];
    if (not data.isObject())
        return std::nullopt;
    // The message names its sender: only the account that sent it.
    const auto& sender = data["senderId"];
    if (not sender.isString() or sender.asString() != from)
        return std::nullopt;
    const auto& conversationId = data["conversationId"];
    if (not conversationId.isString() or not isHex(conversationId.asString(), 1, 64))
        return std::nullopt;
    const auto& sessionId = data["sessionId"];
    if (not sessionId.isNull() and (not sessionId.isString() or not isHex(sessionId.asString(), 1, MAX_SESSION_ID_SIZE)))
        return std::nullopt;

    if (type.asString() == "onWebRtcDescription") {
        const auto& sdp = data["sdp"];
        if (not sdp.isObject() or not sdp["type"].isString() or not sdp["sdp"].isString())
            return std::nullopt;
        auto sdpType = sdp["type"].asString();
        if (sdpType != "offer" and sdpType != "answer")
            return std::nullopt;
    }

    return Message {type.asString(), conversationId.asString(), sessionId.isString() ? sessionId.asString() : "", data};
}

bool
WebCallSignaling::onMessage(const std::string& from, const std::string& device, std::string_view payload)
{
    // Such as the daemon of the web client's server: its app handles these
    // messages.
    if (not Manager::instance().takesWebCalls())
        return false;

    bool isCall = false;
    auto message = parse(from, payload, isCall);
    if (not isCall)
        return false;
    if (not message) {
        JAMI_WARNING("[Account {}] Dropping a malformed call message from {}", accountId_, from);
        return true;
    }
    message->device = device;

    // Off the thread of the message channel, in order: an exit must not
    // overtake the invite it cancels.
    auto key = message->conversationId + '/' + from;
    {
        std::lock_guard lk(queueMutex_);
        auto& queue = queues_[key];
        if (queue.messages.size() >= MAX_QUEUED_MESSAGES) {
            JAMI_WARNING("[Account {}] Too many call messages from {}, dropping one", accountId_, from);
            return true;
        }
        queue.messages.emplace_back(std::move(*message));
        if (queue.running)
            return true;
        queue.running = true;
    }
    dht::ThreadPool::io().run([w = account_, key, from] {
        if (auto account = w.lock())
            account->webCallSignaling().drain(key, from);
    });
    return true;
}

void
WebCallSignaling::drain(const std::string& key, const std::string& from)
{
    while (true) {
        Message message;
        {
            std::lock_guard lk(queueMutex_);
            auto it = queues_.find(key);
            if (it == queues_.end())
                return;
            if (it->second.messages.empty()) {
                queues_.erase(it);
                return;
            }
            message = std::move(it->second.messages.front());
            it->second.messages.pop_front();
        }
        handle(from, std::move(message));
    }
}

void
WebCallSignaling::handle(const std::string& from, Message&& message)
{
    size_t members = 0;
    if (not isMember(message.conversationId, from, &members)) {
        JAMI_WARNING("[Account {}] Call message from {}, not a member of conversation {}",
                     accountId_,
                     from,
                     message.conversationId);
        return;
    }
    if (members > 2) {
        handleGroup(from, std::move(message));
        return;
    }

    auto account = account_.lock();
    if (not account)
        return;
    if (from == account->getUsername()) {
        // Another device of this account answered: this one stops ringing.
        if (message.type == "onCallJoin") {
            std::shared_ptr<WebRtcCall> call;
            {
                std::lock_guard lk(mutex_);
                call = currentCall(message.conversationId);
            }
            if (call)
                call->onAnsweredElsewhere();
        }
        return;
    }

    if (message.type == "onCallInvite") {
        onInvite(from, message);
        return;
    }

    std::shared_ptr<WebRtcCall> call;
    {
        std::lock_guard lk(mutex_);
        call = currentCall(message.conversationId);
        if (not call or call->getPeerAccountUri() != from)
            return;
        if (not message.sessionId.empty()) {
            // A late message of an earlier call, or of another session of the
            // peer (another tab).
            const auto& retired = retired_[message.conversationId];
            if (std::find(retired.begin(), retired.end(), message.sessionId) != retired.end())
                return;
            auto& entry = calls_[message.conversationId];
            if (not entry.peerSession.empty() and entry.peerSession != message.sessionId)
                return;
            if (entry.peerSession.empty()) {
                entry.peerSession = message.sessionId;
                entry.peerDevice = message.device;
            }
        }
    }

    if (message.type == "onCallJoin") {
        call->onPeerJoin();
    } else if (message.type == "onCallExit") {
        call->onPeerExit();
    } else if (message.type == "onWebRtcDescription") {
        const auto& sdp = message.data["sdp"];
        call->onRemoteDescription(sdp["type"].asString(), sdp["sdp"].asString());
    }
    // onWebRtcIceCandidate: this side reads the candidates in the descriptions.
}

void
WebCallSignaling::onInvite(const std::string& from, const Message& message)
{
    // A native caller also invites the browsers of the account; this device
    // rings with the caller's SIP call instead.
    if (message.data["native"].isBool() and message.data["native"].asBool())
        return;
    if (isLate(message.data)) {
        JAMI_LOG("[Account {}] Ignoring a late call invite from {}", accountId_, from);
        return;
    }

    auto account = account_.lock();
    if (not account)
        return;
    {
        std::lock_guard lk(mutex_);
        if (currentCall(message.conversationId))
            return;
        // A late invite of an earlier call.
        const auto& retired = retired_[message.conversationId];
        if (std::find(retired.begin(), retired.end(), message.sessionId) != retired.end())
            return;
    }

    bool withVideo = message.data["withVideoOn"].isBool() and message.data["withVideoOn"].asBool()
                     and account->isVideoEnabled();
    auto mediaList = MediaAttribute::mediaAttributesToMediaMaps(account->createDefaultMediaList(withVideo));

    auto conversationId = message.conversationId;
    auto sendSignal = [w = account_, conversationId, peer = from](const std::string& type, Json::Value&& data) {
        if (auto account = w.lock())
            account->webCallSignaling().sendToCall(peer, conversationId, type, std::move(data));
    };
    auto call = Manager::instance().callFactory.newCall<WebRtcCall>(std::static_pointer_cast<SIPAccountBase>(account),
                                                                    Call::CallType::INCOMING,
                                                                    mediaList,
                                                                    conversationId,
                                                                    from,
                                                                    std::move(sendSignal));
    if (not call)
        return;
    call->setPeerUri("jami:" + from);
    call->setPeerNumber(from);
    {
        std::lock_guard lk(mutex_);
        setCall(conversationId, call, message.sessionId, message.device);
    }

    call->setState(Call::ConnectionState::PROGRESSING);
    call->setState(Call::ConnectionState::TRYING);
    call->setState(Call::ConnectionState::RINGING);
    Manager::instance().incomingCall(accountId_, *call);
}

std::shared_ptr<WebRtcCall>
WebCallSignaling::newOutgoingCall(const std::string& peer, const std::vector<libjami::MediaMap>& mediaList)
{
    if (not Manager::instance().takesWebCalls() or not isHex(peer, 40, 40))
        return {};
    auto account = account_.lock();
    if (not account)
        return {};
    auto* convModule = account->convModule(true);
    if (not convModule)
        return {};
    auto conversationId = convModule->getOneToOneConversation(peer);
    size_t members = 0;
    if (conversationId.empty() or not isMember(conversationId, peer, &members) or members != 2)
        return {};
    {
        std::lock_guard lk(mutex_);
        if (currentCall(conversationId))
            return {};
    }

    auto sendSignal = [w = account_, conversationId, peer](const std::string& type, Json::Value&& data) {
        if (auto account = w.lock())
            account->webCallSignaling().sendToCall(peer, conversationId, type, std::move(data));
    };
    auto call = Manager::instance().callFactory.newCall<WebRtcCall>(std::static_pointer_cast<SIPAccountBase>(account),
                                                                    Call::CallType::OUTGOING,
                                                                    mediaList,
                                                                    conversationId,
                                                                    peer,
                                                                    std::move(sendSignal));
    if (not call)
        return {};
    call->setPeerUri("jami:" + peer);
    call->setPeerNumber(peer);
    {
        std::lock_guard lk(mutex_);
        setCall(conversationId, call, {}, {});
    }
    call->invite();
    return call;
}

void
WebCallSignaling::handleGroup(const std::string& from, Message&& message)
{
    auto account = account_.lock();
    if (not account)
        return;
    std::shared_ptr<Group> group;
    {
        std::lock_guard lk(mutex_);
        if (auto it = groups_.find(message.conversationId); it != groups_.end())
            group = it->second;
    }

    if (from == account->getUsername()) {
        // Another device of this account joined: this one stops ringing.
        if (group and message.type == "onCallJoin")
            group->onAnsweredElsewhere();
        return;
    }

    if (message.type == "onCallInvite") {
        // Group calls of the native apps (hosted by a device) are not the web
        // client's mesh: not taken here yet.
        if (message.data["native"].isBool() and message.data["native"].asBool())
            return;
        if (isLate(message.data)) {
            JAMI_LOG("[Account {}] Ignoring a late call invite from {}", accountId_, from);
            return;
        }
        if (not group) {
            bool created = false;
            {
                std::lock_guard lk(mutex_);
                auto& slot = groups_[message.conversationId];
                if (not slot) {
                    slot = std::make_shared<Group>(account_, accountId_, message.conversationId);
                    created = true;
                }
                group = slot;
            }
            if (created) {
                bool withVideo = message.data["withVideoOn"].isBool() and message.data["withVideoOn"].asBool()
                                 and account->isVideoEnabled();
                group->ring(from, message, withVideo);
                return;
            }
        }
        group->onInvite(from, message);
        return;
    }

    if (not group)
        return;
    if (message.type == "onCallJoin")
        group->onJoin(from, message);
    else if (message.type == "onCallExit")
        group->onExit(from, message);
    else if (message.type == "onWebRtcDescription")
        group->onDescription(from, message);
}

void
WebCallSignaling::onHosting(const std::string& conversationId, const std::string& confId, bool withVideo)
{
    auto account = account_.lock();
    if (not Manager::instance().takesWebCalls() or not account)
        return;
    size_t members = 0;
    if (not isMember(conversationId, account->getUsername(), &members) or members <= 2)
        return;
    auto media = MediaAttribute::mediaAttributesToMediaMaps(account->createDefaultMediaList(withVideo));
    auto group = std::make_shared<Group>(account_, accountId_, conversationId, confId, std::move(media));
    {
        std::lock_guard lk(mutex_);
        auto& slot = groups_[conversationId];
        // A call already going on in the conversation stays.
        if (slot)
            return;
        slot = group;
    }
    JAMI_LOG("[Account {}] Hosting conference {}: inviting the browsers of conversation {}",
             accountId_,
             confId,
             conversationId);
    group->inviteToHosted(withVideo);
}

void
WebCallSignaling::onHostingEnded(const std::string& conversationId, const std::string& confId)
{
    std::shared_ptr<Group> group;
    {
        std::lock_guard lk(mutex_);
        if (auto it = groups_.find(conversationId); it != groups_.end() and it->second->isHosting(confId))
            group = it->second;
    }
    if (group)
        group->endHosted();
}

void
WebCallSignaling::endGroup(const std::string& conversationId, const Group* group)
{
    std::lock_guard lk(mutex_);
    auto it = groups_.find(conversationId);
    if (it != groups_.end() and it->second.get() == group)
        groups_.erase(it);
}

std::shared_ptr<WebRtcCall>
WebCallSignaling::currentCall(const std::string& conversationId)
{
    auto it = calls_.find(conversationId);
    if (it == calls_.end())
        return {};
    // Not the call's state, which takes the call's lock: this lock and calls'
    // are never held together.
    auto call = it->second.call.lock();
    if (not call or call->hasEnded()) {
        retire(conversationId, std::move(it->second.peerSession));
        calls_.erase(it);
        return {};
    }
    return call;
}

void
WebCallSignaling::setCall(const std::string& conversationId,
                          const std::shared_ptr<WebRtcCall>& call,
                          std::string peerSession,
                          std::string peerDevice)
{
    auto& entry = calls_[conversationId];
    retire(conversationId, std::move(entry.peerSession));
    entry = {call, std::move(peerSession), std::move(peerDevice)};
}

void
WebCallSignaling::retire(const std::string& conversationId, std::string&& session)
{
    if (session.empty())
        return;
    auto& retired = retired_[conversationId];
    retired.emplace_back(std::move(session));
    if (retired.size() > MAX_RETIRED_SESSIONS)
        retired.pop_front();
}

bool
WebCallSignaling::isMember(const std::string& conversationId, const std::string& uri, size_t* memberCount)
{
    auto account = account_.lock();
    if (not account)
        return false;
    auto* convModule = account->convModule(true);
    if (not convModule)
        return false;
    bool found = false;
    size_t count = 0;
    for (const auto& member : convModule->getConversationMembers(conversationId)) {
        auto role = member.find("role");
        auto memberUri = member.find("uri");
        if (role == member.end() or memberUri == member.end())
            continue;
        if (role->second != "member" and role->second != "admin")
            continue;
        ++count;
        if (memberUri->second == uri)
            found = true;
    }
    if (memberCount)
        *memberCount = count;
    return found;
}

void
WebCallSignaling::sendToCall(const std::string& to,
                             const std::string& conversationId,
                             const std::string& type,
                             Json::Value&& data)
{
    std::string peerDevice;
    {
        std::lock_guard lk(mutex_);
        if (auto it = calls_.find(conversationId); it != calls_.end())
            peerDevice = it->second.peerDevice;
    }
    // An answer also tells this account's other devices, which stop ringing.
    if (type == "onCallJoin") {
        if (auto account = account_.lock()) {
            Json::Value copy = data;
            send(account->getUsername(), conversationId, type, std::move(copy));
        }
    }
    send(to, conversationId, type, std::move(data), peerDevice);
}

void
WebCallSignaling::send(const std::string& to,
                       const std::string& conversationId,
                       const std::string& type,
                       Json::Value&& data,
                       const std::string& device)
{
    auto account = account_.lock();
    if (not account)
        return;
    data["conversationId"] = conversationId;
    data["senderId"] = account->getUsername();
    // Descriptions from this side hold all their candidates, and it wants
    // the same from the browser.
    data["trickleIce"] = false;
    if (type == "onCallInvite")
        data["sentAt"] = static_cast<Json::Int64>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                       std::chrono::system_clock::now().time_since_epoch())
                                                       .count());
    Json::Value root(Json::objectValue);
    root["type"] = type;
    root["data"] = std::move(data);
    std::map<std::string, std::string> payloads {{std::string(MIME_TYPE), json::toString(root)}};

    if (type == "onCallInvite" or type == "onCallJoin" or type == "onCallExit") {
        // To each device of the account: the message engine counts a message
        // to an account sent once one device has it, so a device that connects
        // later, such as the web client's server, would not get it. Every
        // browser of the account rings, stops, or hears of the member.
        account->forEachDevice(dht::InfoHash(to),
                               [w = account_, to, payloads, self = account->currentDeviceId()](
                                   const std::shared_ptr<dht::crypto::PublicKey>& dev) {
                                   auto deviceId = dev->getLongId().toString();
                                   if (deviceId == self)
                                       return;
                                   if (auto account = w.lock())
                                       account->sendTextMessage(to, deviceId, payloads);
                               });
        return;
    }
    // A description is for the peer's session, on the device it came from.
    account->sendTextMessage(to, device, payloads);
}

void
WebCallSignaling::broadcast(const std::string& conversationId, const std::string& type, Json::Value&& data)
{
    auto account = account_.lock();
    if (not account)
        return;
    auto* convModule = account->convModule(true);
    if (not convModule)
        return;
    // This account included: its other devices stop ringing when this one joins.
    for (const auto& member : convModule->getConversationMembers(conversationId)) {
        auto uri = member.find("uri");
        auto role = member.find("role");
        if (uri == member.end() or role == member.end() or (role->second != "member" and role->second != "admin"))
            continue;
        Json::Value copy = data;
        send(uri->second, conversationId, type, std::move(copy));
    }
}

} // namespace jami
