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

#include "sip/webrtc_sdp.h"
#include "test_runner.h"
#include "webrtc_sdp_samples.h"

#include <string>

namespace jami {
namespace test {
namespace {

using namespace webrtc;

std::string
withCrlf(std::string_view sdp)
{
    std::string result;
    for (char c : sdp) {
        if (c == '\n')
            result += '\r';
        result += c;
    }
    return result;
}

SessionDescription
parseSample(const char* sample)
{
    auto session = parse(withCrlf(sample));
    CPPUNIT_ASSERT(session);
    return *session;
}

MediaSection
localSection(MediaType type, const std::string& mid)
{
    MediaSection media;
    media.type = type;
    media.mid = mid;
    media.iceUfrag = "AbCd1234";
    media.icePwd = "0123456789abcdefghijklmn";
    media.fingerprintAlgorithm = "sha-256";
    media.fingerprint = "01:02:03:04:05:06:07:08:09:0A:0B:0C:0D:0E:0F:10:11:12:13:14:15:16:17:18:19:1A:1B:1C:1D:1E:1F:20";
    media.setup = "active";
    media.rtcpMux = true;
    media.msid = "stream track-" + mid;
    media.candidates = {"1 1 UDP 2130706431 192.0.2.10 40000 typ host",
                        "2 1 UDP 1694498815 198.51.100.7 40000 typ srflx raddr 192.0.2.10 rport 40000"};
    return media;
}

} // namespace

class WebRtcSdpTest : public CppUnit::TestFixture
{
public:
    static std::string name() { return "webrtc_sdp"; }

private:
    void parsesChromeOffer();
    void parsesFirefoxOfferWithSessionLevelFingerprint();
    void parsesAnswers();
    void choosesOpusAndUsableH264ElseVp8();
    void rejectsMalformedOrOversizedInput();
    void rejectsIceCredentialsPjnathCannotTake();
    void answerRoundTrips();
    void offerListsTheAccountCodecs();
    void rejectedSectionsKeepTheirMid();
    void answerDirectionComplementsTheOffer();
    void receivingSdpDescribesOneStream();

    CPPUNIT_TEST_SUITE(WebRtcSdpTest);
    CPPUNIT_TEST(parsesChromeOffer);
    CPPUNIT_TEST(parsesFirefoxOfferWithSessionLevelFingerprint);
    CPPUNIT_TEST(parsesAnswers);
    CPPUNIT_TEST(choosesOpusAndUsableH264ElseVp8);
    CPPUNIT_TEST(rejectsMalformedOrOversizedInput);
    CPPUNIT_TEST(rejectsIceCredentialsPjnathCannotTake);
    CPPUNIT_TEST(answerRoundTrips);
    CPPUNIT_TEST(offerListsTheAccountCodecs);
    CPPUNIT_TEST(rejectedSectionsKeepTheirMid);
    CPPUNIT_TEST(answerDirectionComplementsTheOffer);
    CPPUNIT_TEST(receivingSdpDescribesOneStream);
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_NAMED_REGISTRATION(WebRtcSdpTest, WebRtcSdpTest::name());

void
WebRtcSdpTest::parsesChromeOffer()
{
    auto offer = parseSample(webrtc_samples::CHROMIUM_OFFER);

    CPPUNIT_ASSERT_EQUAL(size_t(2), offer.media.size());
    const auto& audio = offer.media[0];
    const auto& video = offer.media[1];
    CPPUNIT_ASSERT_EQUAL(MediaType::MEDIA_AUDIO, audio.type);
    CPPUNIT_ASSERT_EQUAL(MediaType::MEDIA_VIDEO, video.type);
    CPPUNIT_ASSERT_EQUAL(std::string("0"), audio.mid);
    CPPUNIT_ASSERT_EQUAL(std::string("1"), video.mid);
    CPPUNIT_ASSERT(not audio.rejected and not video.rejected);
    for (const auto* media : {&audio, &video}) {
        CPPUNIT_ASSERT_EQUAL(std::string("HtjM"), media->iceUfrag);
        CPPUNIT_ASSERT_EQUAL(std::string("UEXhgRX6v996GRSPbybIJ1wa"), media->icePwd);
        CPPUNIT_ASSERT_EQUAL(std::string("sha-256"), media->fingerprintAlgorithm);
        CPPUNIT_ASSERT(media->fingerprint.starts_with("0B:C5:B5:5B"));
        CPPUNIT_ASSERT_EQUAL(std::string("actpass"), media->setup);
        CPPUNIT_ASSERT(media->rtcpMux);
        CPPUNIT_ASSERT(media->direction == MediaDirection::SENDRECV);
        CPPUNIT_ASSERT_EQUAL(size_t(6), media->candidates.size());
    }
    CPPUNIT_ASSERT(audio.candidates[0].starts_with("4147514935 1 udp 2122260223 172.18.0.1 55689 typ host"));
    // Each m-line gathered its own transport: the ports differ.
    CPPUNIT_ASSERT(video.candidates[0].find(" 36891 ") != std::string::npos);
}

void
WebRtcSdpTest::parsesFirefoxOfferWithSessionLevelFingerprint()
{
    auto offer = parseSample(webrtc_samples::FIREFOX_OFFER);

    CPPUNIT_ASSERT_EQUAL(size_t(2), offer.media.size());
    for (const auto& media : offer.media) {
        CPPUNIT_ASSERT(media.fingerprint.starts_with("9E:C3:89:EA"));
        CPPUNIT_ASSERT_EQUAL(std::string("291ca461"), media.iceUfrag);
        CPPUNIT_ASSERT_EQUAL(std::string("actpass"), media.setup);
        CPPUNIT_ASSERT(media.rtcpMux);
    }
    // Firefox also gathers component 2 (RTCP) in case rtcp-mux is refused.
    CPPUNIT_ASSERT_EQUAL(size_t(4), offer.media[0].candidates.size());
}

void
WebRtcSdpTest::parsesAnswers()
{
    for (const auto* sample : {webrtc_samples::CHROMIUM_ANSWER, webrtc_samples::FIREFOX_ANSWER}) {
        auto answer = parseSample(sample);
        CPPUNIT_ASSERT_EQUAL(size_t(2), answer.media.size());
        for (const auto& media : answer.media) {
            CPPUNIT_ASSERT_EQUAL(std::string("active"), media.setup);
            CPPUNIT_ASSERT(not media.fingerprint.empty());
            CPPUNIT_ASSERT(not media.iceUfrag.empty() and not media.icePwd.empty());
            CPPUNIT_ASSERT(not media.codecs.empty());
        }
    }
}

void
WebRtcSdpTest::choosesOpusAndUsableH264ElseVp8()
{
    auto chrome = parseSample(webrtc_samples::CHROMIUM_OFFER);
    auto firefox = parseSample(webrtc_samples::FIREFOX_OFFER);
    const std::vector<std::string> videoCodecs {"H264", "VP8"};

    auto opus = chooseCodec(chrome.media[0], {"opus"});
    CPPUNIT_ASSERT(opus);
    CPPUNIT_ASSERT_EQUAL(111u, opus->payloadType);
    CPPUNIT_ASSERT_EQUAL(48000u, opus->clockRate);
    CPPUNIT_ASSERT_EQUAL(2u, opus->channels);
    CPPUNIT_ASSERT_EQUAL(std::string("minptime=10;useinbandfec=1"), opus->fmtp);

    // The first H264 in packetization mode 1 with a baseline profile, in
    // the browser's order: not mode 0 (104), not main profile (116).
    auto h264 = chooseCodec(chrome.media[1], videoCodecs);
    CPPUNIT_ASSERT(h264);
    CPPUNIT_ASSERT_EQUAL(102u, h264->payloadType);

    // VP8 first when the account prefers it.
    auto vp8 = chooseCodec(chrome.media[1], {"VP8", "H264"});
    CPPUNIT_ASSERT(vp8);
    CPPUNIT_ASSERT_EQUAL(96u, vp8->payloadType);

    // This Firefox offers no H264.
    auto firefoxVideo = chooseCodec(firefox.media[1], videoCodecs);
    CPPUNIT_ASSERT(firefoxVideo);
    CPPUNIT_ASSERT_EQUAL(std::string("VP8"), firefoxVideo->name);
    CPPUNIT_ASSERT_EQUAL(120u, firefoxVideo->payloadType);
    auto firefoxAudio = chooseCodec(firefox.media[0], {"opus"});
    CPPUNIT_ASSERT(firefoxAudio);
    CPPUNIT_ASSERT_EQUAL(109u, firefoxAudio->payloadType);

    CPPUNIT_ASSERT(not chooseCodec(chrome.media[1], {"H265"}));
}

void
WebRtcSdpTest::rejectsMalformedOrOversizedInput()
{
    CPPUNIT_ASSERT(not parse(""));
    CPPUNIT_ASSERT(not parse("hello"));
    CPPUNIT_ASSERT(not parse("v=0\r\nm=audio x UDP/TLS/RTP/SAVPF 111\r\n"));
    CPPUNIT_ASSERT(not parse("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 300\r\n"));
    CPPUNIT_ASSERT(not parse("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:x opus/48000/2\r\n"));
    CPPUNIT_ASSERT(not parse("v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus\r\n"));

    std::string manyMedia = "v=0\r\n";
    for (int i = 0; i < 9; ++i)
        manyMedia += "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
    CPPUNIT_ASSERT(not parse(manyMedia));

    std::string huge = "v=0\r\n" + std::string(70 * 1024, 'a');
    CPPUNIT_ASSERT(not parse(huge));

    std::string longLine = "v=0\r\na=" + std::string(5000, 'x') + "\r\n";
    CPPUNIT_ASSERT(not parse(longLine));

    // Plain RTP, without DTLS, is not usable with a browser.
    auto plain = parse("v=0\r\nm=audio 9 RTP/AVP 111\r\na=rtpmap:111 opus/48000/2\r\n");
    CPPUNIT_ASSERT(plain);
    CPPUNIT_ASSERT(plain->media[0].rejected);
}

void
WebRtcSdpTest::rejectsIceCredentialsPjnathCannotTake()
{
    auto withCredentials = [](const std::string& ufrag, const std::string& pwd) {
        return parse("v=0\r\n"
                     "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
                     "a=ice-ufrag:"
                     + ufrag
                     + "\r\n"
                       "a=ice-pwd:"
                     + pwd + "\r\n");
    };
    const std::string pwd(24, 'p');
    // What browsers send: 4 characters (Chrome), 8 (Firefox).
    CPPUNIT_ASSERT(withCredentials("a+/B", pwd));
    CPPUNIT_ASSERT(withCredentials("4f2b1d3e", std::string(32, 'f')));
    // pjnath copies the peer's ufrag into a 128-byte buffer without checking.
    CPPUNIT_ASSERT(withCredentials(std::string(64, 'u'), pwd));
    CPPUNIT_ASSERT(not withCredentials(std::string(65, 'u'), pwd));
    CPPUNIT_ASSERT(not withCredentials(std::string(4000, 'u'), pwd));
    CPPUNIT_ASSERT(not withCredentials("abc", pwd));
    CPPUNIT_ASSERT(not withCredentials("ab:cd", pwd));
    CPPUNIT_ASSERT(not withCredentials("ab cd", pwd));
    CPPUNIT_ASSERT(not withCredentials("abcd", std::string(21, 'p')));
    CPPUNIT_ASSERT(not withCredentials("abcd", std::string(257, 'p')));
}

void
WebRtcSdpTest::answerRoundTrips()
{
    SessionDescription answer;
    auto audio = localSection(MediaType::MEDIA_AUDIO, "0");
    audio.codecs.push_back({111, "opus", 48000, 2, "minptime=10;useinbandfec=1", {}});
    auto video = localSection(MediaType::MEDIA_VIDEO, "1");
    video.direction = MediaDirection::RECVONLY;
    video.codecs.push_back({102,
                            "H264",
                            90000,
                            1,
                            "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f",
                            {"nack pli", "ccm fir"}});
    answer.media = {audio, video};

    auto text = serialize(answer, UINT64_MAX, 1);
    // Firefox refuses a session ID beyond a signed 64-bit integer.
    CPPUNIT_ASSERT(text.find("o=- 9223372036854775807 1 IN IP4 127.0.0.1\r\n") != std::string::npos);
    CPPUNIT_ASSERT(text.find("a=rtcp-fb:102 nack pli\r\n") != std::string::npos);
    CPPUNIT_ASSERT(text.find("a=end-of-candidates\r\n") != std::string::npos);
    CPPUNIT_ASSERT(text.find("m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n") != std::string::npos);

    auto parsed = parse(text);
    CPPUNIT_ASSERT(parsed);
    CPPUNIT_ASSERT_EQUAL(size_t(2), parsed->media.size());
    for (size_t i = 0; i < 2; ++i) {
        const auto& in = answer.media[i];
        const auto& out = parsed->media[i];
        CPPUNIT_ASSERT_EQUAL(in.type, out.type);
        CPPUNIT_ASSERT_EQUAL(in.mid, out.mid);
        CPPUNIT_ASSERT_EQUAL(in.iceUfrag, out.iceUfrag);
        CPPUNIT_ASSERT_EQUAL(in.icePwd, out.icePwd);
        CPPUNIT_ASSERT_EQUAL(in.fingerprint, out.fingerprint);
        CPPUNIT_ASSERT_EQUAL(in.setup, out.setup);
        CPPUNIT_ASSERT(in.direction == out.direction);
        CPPUNIT_ASSERT_EQUAL(in.msid, out.msid);
        CPPUNIT_ASSERT(in.candidates == out.candidates);
        CPPUNIT_ASSERT(out.rtcpMux);
        CPPUNIT_ASSERT_EQUAL(size_t(1), out.codecs.size());
        CPPUNIT_ASSERT_EQUAL(in.codecs[0].payloadType, out.codecs[0].payloadType);
        CPPUNIT_ASSERT_EQUAL(in.codecs[0].name, out.codecs[0].name);
        CPPUNIT_ASSERT_EQUAL(in.codecs[0].clockRate, out.codecs[0].clockRate);
        CPPUNIT_ASSERT_EQUAL(in.codecs[0].fmtp, out.codecs[0].fmtp);
    }
    CPPUNIT_ASSERT_EQUAL(2u, parsed->media[0].codecs[0].channels);
}

void
WebRtcSdpTest::offerListsTheAccountCodecs()
{
    // What the account prefers, among what browsers and the daemon both do.
    auto video = localCodecs(MediaType::MEDIA_VIDEO, {"VP8", "AV1", "H264", "VP8"});
    CPPUNIT_ASSERT_EQUAL(size_t(2), video.size());
    CPPUNIT_ASSERT_EQUAL(std::string("VP8"), video[0].name);
    CPPUNIT_ASSERT_EQUAL(std::string("H264"), video[1].name);
    auto audio = localCodecs(MediaType::MEDIA_AUDIO, {"G722", "opus", "H264"});
    CPPUNIT_ASSERT_EQUAL(size_t(1), audio.size());
    CPPUNIT_ASSERT_EQUAL(48000u, audio[0].clockRate);
    CPPUNIT_ASSERT(localCodecs(MediaType::MEDIA_VIDEO, {"opus"}).empty());

    SessionDescription offer;
    auto section = localSection(MediaType::MEDIA_VIDEO, "1");
    section.setup = "actpass";
    section.codecs = video;
    offer.media = {section};
    auto text = serialize(offer, 7, 1);
    CPPUNIT_ASSERT(text.find("m=video 9 UDP/TLS/RTP/SAVPF 96 102\r\n") != std::string::npos);
    CPPUNIT_ASSERT(text.find("a=setup:actpass\r\n") != std::string::npos);

    // A browser answering takes either; this side accepts the answer's.
    auto parsed = parse(text);
    CPPUNIT_ASSERT(parsed);
    CPPUNIT_ASSERT_EQUAL(size_t(2), parsed->media[0].codecs.size());
    CPPUNIT_ASSERT_EQUAL(std::string("actpass"), parsed->media[0].setup);
    auto h264 = chooseCodec(parsed->media[0], {"H264"});
    CPPUNIT_ASSERT(h264);
    CPPUNIT_ASSERT_EQUAL(102u, h264->payloadType);
}

void
WebRtcSdpTest::rejectedSectionsKeepTheirMid()
{
    auto offer = parse("v=0\r\n"
                       "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                       "a=mid:2\r\n");
    CPPUNIT_ASSERT(offer);
    CPPUNIT_ASSERT_EQUAL(MediaType::MEDIA_NONE, offer->media[0].type);

    auto text = serialize(*offer, 1, 1);
    CPPUNIT_ASSERT(text.find("m=application 0 UDP/DTLS/SCTP webrtc-datachannel\r\n") != std::string::npos);
    CPPUNIT_ASSERT(text.find("a=mid:2\r\n") != std::string::npos);
}

void
WebRtcSdpTest::answerDirectionComplementsTheOffer()
{
    CPPUNIT_ASSERT(answerDirection(MediaDirection::SENDRECV) == MediaDirection::SENDRECV);
    CPPUNIT_ASSERT(answerDirection(MediaDirection::SENDONLY) == MediaDirection::RECVONLY);
    CPPUNIT_ASSERT(answerDirection(MediaDirection::RECVONLY) == MediaDirection::SENDONLY);
    CPPUNIT_ASSERT(answerDirection(MediaDirection::INACTIVE) == MediaDirection::INACTIVE);
}

void
WebRtcSdpTest::receivingSdpDescribesOneStream()
{
    auto audio = receivingSdp(MediaType::MEDIA_AUDIO, {111, "opus", 48000, 2, "minptime=10;useinbandfec=1", {}});
    CPPUNIT_ASSERT(audio.find("m=audio 9 RTP/AVP 111\r\n") != std::string::npos);
    CPPUNIT_ASSERT(audio.find("a=rtpmap:111 opus/48000/2\r\n") != std::string::npos);
    CPPUNIT_ASSERT(audio.find("a=fmtp:111 minptime=10;useinbandfec=1\r\n") != std::string::npos);

    auto video = receivingSdp(MediaType::MEDIA_VIDEO, {96, "VP8", 90000, 1, "", {}});
    CPPUNIT_ASSERT(video.find("m=video 9 RTP/AVP 96\r\n") != std::string::npos);
    CPPUNIT_ASSERT(video.find("a=rtpmap:96 VP8/90000\r\n") != std::string::npos);
    CPPUNIT_ASSERT(video.find("a=fmtp") == std::string::npos);
}

} // namespace test
} // namespace jami

CORE_TEST_RUNNER(jami::test::WebRtcSdpTest::name())
