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

#include "account_const.h"
#include "call_const.h"
#include "jami.h"
#include "manager.h"
#include "media/media_attribute.h"
#include "sip/sipaccount.h"
#include "sip/webrtccall.h"
#include "test_runner.h"

#include <string>

namespace jami {
namespace test {
namespace {

const std::string PEER = "1a6c18e6943def8426b497ed38c99bc77696f32b";
const std::string CONVERSATION = "517691ba799e93e2471febf3f872a7804ba8b082";

} // namespace

class WebRtcCallTest : public CppUnit::TestFixture
{
public:
    WebRtcCallTest()
    {
        libjami::init(libjami::InitFlag(libjami::LIBJAMI_FLAG_DEBUG | libjami::LIBJAMI_FLAG_CONSOLE_LOG));
        if (not Manager::instance().initialized)
            CPPUNIT_ASSERT(libjami::start("jami-sample.yml"));
    }
    ~WebRtcCallTest() { libjami::fini(); }
    static std::string name() { return "webrtc_call"; }
    void setUp();
    void tearDown();

private:
    void incomingCallsNameTheirAccount();

    CPPUNIT_TEST_SUITE(WebRtcCallTest);
    CPPUNIT_TEST(incomingCallsNameTheirAccount);
    CPPUNIT_TEST_SUITE_END();

    std::string accountId_;
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(WebRtcCallTest, WebRtcCallTest::name());

void
WebRtcCallTest::setUp()
{
    // A SIP account on this host: a call's own state needs no more, where a
    // Jami account would need the DHT.
    auto details = libjami::getAccountTemplate("SIP");
    details[libjami::Account::ConfProperties::TYPE] = "SIP";
    details[libjami::Account::ConfProperties::USERNAME] = "erin";
    details[libjami::Account::ConfProperties::ALIAS] = "erin";
    details[libjami::Account::ConfProperties::LOCAL_PORT] = "5092";
    details[libjami::Account::ConfProperties::UPNP_ENABLED] = "false";
    accountId_ = Manager::instance().addAccount(details);
    CPPUNIT_ASSERT(not accountId_.empty());
}

void
WebRtcCallTest::tearDown()
{
    Manager::instance().removeAccount(accountId_, true);
}

void
WebRtcCallTest::incomingCallsNameTheirAccount()
{
    // The apps attach an incoming call to its conversation only when its
    // TO_USERNAME names their account, as a SIP call's does (jami-client-qt,
    // ConversationModel::slotNewCall): the desktop rang for a call from a
    // browser without ever showing it.
    auto account = Manager::instance().getAccount<SIPAccount>(accountId_);
    CPPUNIT_ASSERT(account);
    auto media = MediaAttribute::mediaAttributesToMediaMaps(account->createDefaultMediaList(true));
    auto call = Manager::instance().callFactory.newCall<WebRtcCall>(account,
                                                                    Call::CallType::INCOMING,
                                                                    media,
                                                                    CONVERSATION,
                                                                    PEER,
                                                                    [](const std::string&, Json::Value&&) {});
    CPPUNIT_ASSERT(call);
    CPPUNIT_ASSERT_EQUAL(account->getUsername(), call->getDetails()[libjami::Call::Details::TO_USERNAME]);
    Manager::instance().callFactory.removeCall(*call);
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::WebRtcCallTest::name())
