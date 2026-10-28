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

#include "jami/jami.h"

#include <json/json.h>

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jami {

namespace test {
class WebCallDeliveryTest;
}
class JamiAccount;
class WebRtcCall;

// The web client's call protocol, for one account: calls with browsers (see
// WebRtcCall). Each message is an account message of type application/json,
// {"type": "on...", "data": {"conversationId", "senderId", "sessionId", ...}},
// sent to every member of the conversation or to one of them.
//
// In a conversation of two, a call is one WebRtcCall. In a larger one, the web
// client's calls are a mesh: whoever is in the call connects to each member
// who joins. This device then has a leg (a WebRtcCall) with each member in the
// call, in a conference here (see Group).
class WebCallSignaling
{
public:
    // A call message once checked: who sent it and in which conversation.
    struct Message
    {
        std::string type;
        std::string conversationId;
        std::string sessionId;
        Json::Value data;
        // The sender's device, such as the web client's server for a browser.
        std::string device;
    };

    explicit WebCallSignaling(JamiAccount& account);
    ~WebCallSignaling();

    // An application/json account message from `from`'s `device`, which the
    // account authenticated. True when it was a call message, handled or
    // dropped; other messages are left to the apps.
    bool onMessage(const std::string& from, const std::string& device, std::string_view payload);

    // A call to `peer`, who has a one-to-one conversation with this account:
    // it rings the peer's browsers as well as its devices (see WebRtcCall).
    // Nothing for another peer, or when a web call is already going on in
    // the conversation: the caller makes a SIP call.
    std::shared_ptr<WebRtcCall> newOutgoingCall(const std::string& peer,
                                                const std::vector<libjami::MediaMap>& mediaList);

    // This device hosts the call of a group conversation (as the native apps
    // do): the members' browsers are invited, and join the conference.
    void onHosting(const std::string& conversationId, const std::string& confId, bool withVideo);
    void onHostingEnded(const std::string& conversationId, const std::string& confId);

    // Parses and checks a message: its type is one of the protocol's, its
    // sender is the account that sent it, its fields have the right shape.
    // Nothing for a message of another kind or a malformed one; `isCall`
    // tells which.
    static std::optional<Message> parse(const std::string& from, std::string_view payload, bool& isCall);

private:
    class Group;

    // A web call, and the peer's session in it: the peer's messages from
    // another session are left out.
    struct Entry
    {
        std::weak_ptr<WebRtcCall> call;
        std::string peerSession;
        // Where the peer's session is, for messages to it alone.
        std::string peerDevice;
    };
    // The messages of one peer about one conversation, handled one at a time
    // in the order they came.
    struct Queue
    {
        std::deque<Message> messages;
        bool running {false};
    };

    void drain(const std::string& key, const std::string& from);
    void handle(const std::string& from, Message&& message);
    void handleGroup(const std::string& from, Message&& message);
    // The group is over: a new call in its conversation is a new group.
    void endGroup(const std::string& conversationId, const Group* group);
    void onInvite(const std::string& from, const Message& message);
    // The call in the conversation, unless it ended. Called with mutex_ held.
    std::shared_ptr<WebRtcCall> currentCall(const std::string& conversationId);
    // Called with mutex_ held.
    void setCall(const std::string& conversationId,
                 const std::shared_ptr<WebRtcCall>& call,
                 std::string peerSession,
                 std::string peerDevice);
    void retire(const std::string& conversationId, std::string&& session);
    // retire, for a group call: its member's session is over.
    void retireSession(const std::string& conversationId, std::string session);
    bool isMember(const std::string& conversationId, const std::string& uri, size_t* memberCount = nullptr);
    // To the peer of the conversation's call, on the device of its session.
    void sendToCall(const std::string& to, const std::string& conversationId, const std::string& type, Json::Value&& data);
    // To `to` on `device` (any when empty); invites and exits to each device.
    void send(const std::string& to,
              const std::string& conversationId,
              const std::string& type,
              Json::Value&& data,
              const std::string& device = {});
    // To each member of the conversation, this account included, on each device.
    void broadcast(const std::string& conversationId, const std::string& type, Json::Value&& data);

    friend class test::WebCallDeliveryTest;

    std::weak_ptr<JamiAccount> account_;
    const std::string accountId_;

    std::mutex mutex_;
    // One web call per conversation.
    std::map<std::string, Entry> calls_;
    // The peer sessions of earlier calls in each conversation: their late
    // messages, such as an exit, do not reach a new call.
    std::map<std::string, std::deque<std::string>> retired_;
    // Group calls, by conversation.
    std::map<std::string, std::shared_ptr<Group>> groups_;

    std::mutex queueMutex_;
    std::map<std::string, Queue> queues_;
};

} // namespace jami
