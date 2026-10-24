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

#include <cppunit/TestAssert.h>
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include "jamidht/web_call_signaling.h"
#include "test_runner.h"

#include <string>

namespace jami {
namespace test {
namespace {

const std::string ALICE = "9b778022bd1bc9a7a1b2c3d4e5f60718293a4b5c";
const std::string MALLORY = "0123456789abcdef0123456789abcdef01234567";
const std::string CONVERSATION = "429e9b9579bfd594dd4207a00bec6d86fca3553e";

// A message as the web client's server sends it.
std::string
message(const std::string& type, const std::string& data)
{
    return R"({"type":")" + type + R"(","data":)" + data + "}";
}

std::string
data(const std::string& extra = {}, const std::string& sender = ALICE)
{
    return R"({"conversationId":")" + CONVERSATION + R"(","senderId":")" + sender + R"(","sessionId":"5f0e3b2a9c1d4e7f")"
           + extra + "}";
}

std::optional<WebCallSignaling::Message>
parse(const std::string& payload, bool& isCall)
{
    return WebCallSignaling::parse(ALICE, payload, isCall);
}

} // namespace

class WebCallSignalingTest : public CppUnit::TestFixture
{
public:
    static std::string name() { return "web_call_signaling"; }

private:
    void readsTheWebClientsMessages();
    void dropsMessagesNamingAnotherSender();
    void leavesOtherMessagesToTheApps();
    void dropsMalformedCallMessages();

    CPPUNIT_TEST_SUITE(WebCallSignalingTest);
    CPPUNIT_TEST(readsTheWebClientsMessages);
    CPPUNIT_TEST(dropsMessagesNamingAnotherSender);
    CPPUNIT_TEST(leavesOtherMessagesToTheApps);
    CPPUNIT_TEST(dropsMalformedCallMessages);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(WebCallSignalingTest, WebCallSignalingTest::name());

void
WebCallSignalingTest::readsTheWebClientsMessages()
{
    bool isCall = false;
    auto invite = parse(message("onCallInvite", data(R"(,"withVideoOn":true)")), isCall);
    CPPUNIT_ASSERT(isCall);
    CPPUNIT_ASSERT(invite);
    CPPUNIT_ASSERT_EQUAL(std::string("onCallInvite"), invite->type);
    CPPUNIT_ASSERT_EQUAL(CONVERSATION, invite->conversationId);
    CPPUNIT_ASSERT_EQUAL(std::string("5f0e3b2a9c1d4e7f"), invite->sessionId);
    CPPUNIT_ASSERT(invite->data["withVideoOn"].asBool());

    for (const auto* type : {"onCallJoin", "onCallExit", "onWebRtcIceCandidate"}) {
        CPPUNIT_ASSERT(parse(message(type, data()), isCall));
        CPPUNIT_ASSERT(isCall);
    }

    auto offer = parse(message("onWebRtcDescription", data(R"(,"sdp":{"type":"offer","sdp":"v=0\r\n"})")), isCall);
    CPPUNIT_ASSERT(offer);
    CPPUNIT_ASSERT_EQUAL(std::string("offer"), offer->data["sdp"]["type"].asString());

    // Clients older than sessions leave the field out.
    auto withoutSession = parse(message("onCallExit",
                                        R"({"conversationId":")" + CONVERSATION + R"(","senderId":")" + ALICE + "\"}"),
                                isCall);
    CPPUNIT_ASSERT(withoutSession);
    CPPUNIT_ASSERT(withoutSession->sessionId.empty());
}

void
WebCallSignalingTest::dropsMessagesNamingAnotherSender()
{
    // Sent by ALICE's account, claiming to come from MALLORY: dropped, as a
    // call message, so it reaches nothing.
    bool isCall = false;
    CPPUNIT_ASSERT(not parse(message("onWebRtcDescription",
                                     data(R"(,"sdp":{"type":"offer","sdp":"v=0\r\n"})", MALLORY)),
                             isCall));
    CPPUNIT_ASSERT(isCall);
    CPPUNIT_ASSERT(not parse(message("onCallInvite", R"({"conversationId":")" + CONVERSATION + "\"}"), isCall));
    CPPUNIT_ASSERT(isCall);
}

void
WebCallSignalingTest::leavesOtherMessagesToTheApps()
{
    bool isCall = true;
    CPPUNIT_ASSERT(not parse("not json", isCall));
    CPPUNIT_ASSERT(not isCall);
    CPPUNIT_ASSERT(not parse("[1, 2]", isCall));
    CPPUNIT_ASSERT(not isCall);
    CPPUNIT_ASSERT(not parse(message("somethingElse", data()), isCall));
    CPPUNIT_ASSERT(not isCall);
    CPPUNIT_ASSERT(not parse(R"({"data":{}})", isCall));
    CPPUNIT_ASSERT(not isCall);
    // Too large to be a call message.
    CPPUNIT_ASSERT(not parse(message("onCallInvite", data(R"(,"pad":")" + std::string(90 * 1024, 'x') + "\"")),
                             isCall));
    CPPUNIT_ASSERT(not isCall);
}

void
WebCallSignalingTest::dropsMalformedCallMessages()
{
    bool isCall = false;
    const std::vector<std::string> malformed {
        message("onCallInvite", "[]"),
        message("onCallInvite", R"({"conversationId":"not hex!","senderId":")" + ALICE + "\"}"),
        message("onCallInvite", R"({"senderId":")" + ALICE + "\"}"),
        message("onCallInvite", R"({"conversationId":")" + CONVERSATION + R"(","senderId":")" + ALICE
                                    + R"(","sessionId":42})"),
        message("onWebRtcDescription", data()),
        message("onWebRtcDescription", data(R"(,"sdp":{"type":"rollback","sdp":""})")),
        message("onWebRtcDescription", data(R"(,"sdp":{"type":"offer"})")),
    };
    for (const auto& payload : malformed) {
        CPPUNIT_ASSERT_MESSAGE(payload, not parse(payload, isCall));
        CPPUNIT_ASSERT_MESSAGE(payload, isCall);
    }
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::WebCallSignalingTest::name())
