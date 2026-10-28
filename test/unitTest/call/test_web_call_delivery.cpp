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
#include "common.h"
#include "configurationmanager_interface.h"
#include "jami.h"
#include "jamidht/jamiaccount.h"
#include "jamidht/web_call_signaling.h"
#include "manager.h"
#include "test_runner.h"

#include <opendht/dhtrunner.h>

#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>

using namespace std::literals::chrono_literals;

namespace jami {
namespace test {
namespace {

const std::string CONVERSATION = "429e9b9579bfd594dd4207a00bec6d86fca3553e";

} // namespace

// Call messages of the web client's protocol between two Jami accounts, alice
// and bob, on a DHT of this process: a node that both bootstrap from, so that
// the test needs no network.
class WebCallDeliveryTest : public CppUnit::TestFixture
{
public:
    WebCallDeliveryTest()
    {
        libjami::init(libjami::InitFlag(libjami::LIBJAMI_FLAG_DEBUG | libjami::LIBJAMI_FLAG_CONSOLE_LOG));
        if (not Manager::instance().initialized)
            CPPUNIT_ASSERT(libjami::start("jami-sample.yml"));
    }
    ~WebCallDeliveryTest() { libjami::fini(); }
    static std::string name() { return "web_call_delivery"; }
    void setUp();
    void tearDown();

private:
    std::unique_ptr<dht::DhtRunner> bootstrap_;
    std::string aliceId;
    std::string bobId;

    void reachesConnectedDevicesWithoutTheDht();

    CPPUNIT_TEST_SUITE(WebCallDeliveryTest);
    CPPUNIT_TEST(reachesConnectedDevicesWithoutTheDht);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(WebCallDeliveryTest, WebCallDeliveryTest::name());

void
WebCallDeliveryTest::setUp()
{
    bootstrap_ = std::make_unique<dht::DhtRunner>();
    bootstrap_->run(0);
    auto details = libjami::getAccountTemplate("RING");
    details[libjami::Account::ConfProperties::HOSTNAME] = "127.0.0.1:" + std::to_string(bootstrap_->getBoundPort());
    details[libjami::Account::ConfProperties::UPNP_ENABLED] = "false";
    details[libjami::Account::ConfProperties::TURN::ENABLED] = "false";
    details[libjami::Account::ConfProperties::ALIAS] = "ALICE";
    aliceId = Manager::instance().addAccount(details);
    details[libjami::Account::ConfProperties::ALIAS] = "BOB";
    bobId = Manager::instance().addAccount(details);
    wait_for_announcement_of({aliceId, bobId});
}

void
WebCallDeliveryTest::tearDown()
{
    libjami::unregisterSignalHandlers();
    wait_for_removal_of({aliceId, bobId});
    bootstrap_->join();
}

// While the DHT is out, it finds no device of the peer's; a device that a
// message channel connects to still gets the call messages. An answer lost
// this way failed the call after 30 s.
void
WebCallDeliveryTest::reachesConnectedDevicesWithoutTheDht()
{
    auto aliceAccount = Manager::instance().getAccount<JamiAccount>(aliceId);
    auto bobUri = Manager::instance().getAccount<JamiAccount>(bobId)->getUsername();
    add_confirmed_contact(aliceId, bobId);

    std::mutex mtx;
    std::condition_variable cv;
    size_t sentToBob = 0;
    std::map<std::string, std::shared_ptr<libjami::CallbackWrapperBase>> handlers;
    handlers.insert(libjami::exportable_callback<libjami::ConfigurationSignal::AccountMessageStatusChanged>(
        [&](const std::string& accountId, const std::string&, const std::string& peer, const std::string&, int state) {
            if (accountId != aliceId or peer != bobUri
                or state != static_cast<int>(libjami::Account::MessageStates::SENT))
                return;
            std::lock_guard lk(mtx);
            sentToBob++;
            cv.notify_one();
        }));
    libjami::registerSignalHandlers(handlers);

    // An account message opens a message channel to bob's device.
    libjami::sendAccountTextMessage(aliceId, bobUri, {{"application/json", R"({"type":"ping"})"}}, 0);
    {
        std::unique_lock lk(mtx);
        CPPUNIT_ASSERT(cv.wait_for(lk, 60s, [&] { return sentToBob > 0; }));
        sentToBob = 0;
    }
    CPPUNIT_ASSERT(not aliceAccount->messageChannelDevices(bobUri).empty());

    // The outage: alice's DHT finds no device of bob's any more.
    aliceAccount->dht()->shutdown({}, false);
    std::promise<bool> found;
    aliceAccount->forEachDevice(dht::InfoHash(bobUri), [](const auto&) {}, [&](bool ok) { found.set_value(ok); });
    auto lookup = found.get_future();
    CPPUNIT_ASSERT(lookup.wait_for(10s) == std::future_status::ready);
    CPPUNIT_ASSERT(not lookup.get());

    Json::Value data(Json::objectValue);
    data["sessionId"] = "5f0e3b2a9c1d4e7f";
    aliceAccount->webCallSignaling().send(bobUri, CONVERSATION, "onCallExit", std::move(data));
    std::unique_lock lk(mtx);
    CPPUNIT_ASSERT(cv.wait_for(lk, 30s, [&] { return sentToBob > 0; }));
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::WebCallDeliveryTest::name())
