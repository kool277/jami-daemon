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
#include "webrtc_sdp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <map>

namespace jami::webrtc {

namespace {

// A peer controls the description: bound everything it can grow.
constexpr size_t MAX_SDP_SIZE = 64 * 1024;
constexpr size_t MAX_LINE_SIZE = 4096;
constexpr size_t MAX_MEDIA = 8;
constexpr size_t MAX_CODECS = 64;
// What one dhtnet ICE transport can take (PJ_ICE_ST_MAX_CAND).
constexpr size_t MAX_CANDIDATES = 32;

constexpr std::string_view PROTOCOL = "UDP/TLS/RTP/SAVPF";

// ICE credentials (RFC 8839: ice-chars, 4 to 256 for the ufrag, 22 to 256
// for the password). pjnath joins both sides' ufrags in a 128-byte buffer
// without checking: the peer's is kept well under that.
constexpr size_t MIN_UFRAG = 4;
constexpr size_t MAX_UFRAG = 64;
constexpr size_t MIN_PWD = 22;
constexpr size_t MAX_PWD = 256;

bool
isIceChars(std::string_view s, size_t minSize, size_t maxSize)
{
    return s.size() >= minSize and s.size() <= maxSize and std::all_of(s.begin(), s.end(), [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) or c == '+' or c == '/';
           });
}

bool
equalsIgnoreCase(std::string_view a, std::string_view b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}

std::optional<unsigned>
toUnsigned(std::string_view s)
{
    unsigned value {};
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc() or ptr != s.data() + s.size())
        return std::nullopt;
    return value;
}

// Splits at the first space: {"1 2 3", ""} -> {"1", "2 3"}.
std::pair<std::string_view, std::string_view>
splitFirst(std::string_view s, char sep = ' ')
{
    auto pos = s.find(sep);
    if (pos == std::string_view::npos)
        return {s, {}};
    return {s.substr(0, pos), s.substr(pos + 1)};
}

std::vector<std::string_view>
splitAll(std::string_view s, char sep = ' ')
{
    std::vector<std::string_view> parts;
    while (not s.empty()) {
        auto [part, rest] = splitFirst(s, sep);
        if (not part.empty())
            parts.emplace_back(part);
        s = rest;
    }
    return parts;
}

std::string
upper(std::string_view s)
{
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) { return std::toupper(c); });
    return result;
}

const char*
directionString(MediaDirection direction)
{
    switch (direction) {
    case MediaDirection::SENDONLY:
        return "sendonly";
    case MediaDirection::RECVONLY:
        return "recvonly";
    case MediaDirection::INACTIVE:
        return "inactive";
    case MediaDirection::SENDRECV:
    default:
        return "sendrecv";
    }
}

// The value of a fmtp parameter, such as packetization-mode, in
// "a=1;b=2" form.
std::string_view
fmtpParameter(std::string_view fmtp, std::string_view name)
{
    for (auto param : splitAll(fmtp, ';')) {
        while (not param.empty() and param.front() == ' ')
            param.remove_prefix(1);
        auto [key, value] = splitFirst(param, '=');
        if (equalsIgnoreCase(key, name))
            return value;
    }
    return {};
}

bool
isUsableH264(const RtpCodec& codec)
{
    if (fmtpParameter(codec.fmtp, "packetization-mode") != "1")
        return false;
    // profile_idc 0x42 (baseline), with or without the constrained flag.
    auto profile = fmtpParameter(codec.fmtp, "profile-level-id");
    return profile.size() == 6 and equalsIgnoreCase(profile.substr(0, 2), "42");
}

struct Parser
{
    SessionDescription session;
    // Session-level values, the default of every m-line.
    std::string iceUfrag, icePwd, fingerprintAlgorithm, fingerprint, setup;
    // Payload type -> index in codecs, for the current m-line.
    std::map<unsigned, size_t> codecIndex;

    MediaSection* current() { return session.media.empty() ? nullptr : &session.media.back(); }

    bool mediaLine(std::string_view value)
    {
        if (session.media.size() >= MAX_MEDIA)
            return false;
        auto fields = splitAll(value);
        if (fields.size() < 3)
            return false;
        auto port = toUnsigned(fields[1]);
        if (not port)
            return false;
        auto& media = session.media.emplace_back();
        media.media = fields[0];
        media.protocol = fields[2];
        media.rejected = *port == 0;
        media.iceUfrag = iceUfrag;
        media.icePwd = icePwd;
        media.fingerprintAlgorithm = fingerprintAlgorithm;
        media.fingerprint = fingerprint;
        media.setup = setup;
        for (size_t i = 3; i < fields.size(); ++i) {
            if (i > 3)
                media.formats += ' ';
            media.formats += fields[i];
        }
        codecIndex.clear();
        if (fields[0] == "audio")
            media.type = MediaType::MEDIA_AUDIO;
        else if (fields[0] == "video")
            media.type = MediaType::MEDIA_VIDEO;
        else
            return true;
        // An m-line that does not carry SRTP over DTLS is of no use here.
        if (fields[2] != PROTOCOL and fields[2] != "UDP/TLS/RTP/SAVP")
            media.rejected = true;
        for (size_t i = 3; i < fields.size(); ++i) {
            auto pt = toUnsigned(fields[i]);
            if (not pt or *pt > 127)
                return false;
            if (media.codecs.size() >= MAX_CODECS)
                return false;
            codecIndex[*pt] = media.codecs.size();
            media.codecs.emplace_back().payloadType = *pt;
        }
        return true;
    }

    bool attribute(std::string_view value)
    {
        auto [name, content] = splitFirst(value, ':');
        auto* media = current();

        if (name == "ice-ufrag") {
            if (not isIceChars(content, MIN_UFRAG, MAX_UFRAG))
                return false;
            (media ? media->iceUfrag : iceUfrag) = content;
        } else if (name == "ice-pwd") {
            if (not isIceChars(content, MIN_PWD, MAX_PWD))
                return false;
            (media ? media->icePwd : icePwd) = content;
        } else if (name == "fingerprint") {
            auto [algorithm, fp] = splitFirst(content);
            (media ? media->fingerprintAlgorithm : fingerprintAlgorithm) = algorithm;
            (media ? media->fingerprint : fingerprint) = upper(fp);
        } else if (name == "setup") {
            (media ? media->setup : setup) = content;
        } else if (not media) {
            // Other session-level attributes (group, msid-semantic) do not matter.
        } else if (name == "mid") {
            media->mid = content;
        } else if (name == "sendrecv") {
            media->direction = MediaDirection::SENDRECV;
        } else if (name == "sendonly") {
            media->direction = MediaDirection::SENDONLY;
        } else if (name == "recvonly") {
            media->direction = MediaDirection::RECVONLY;
        } else if (name == "inactive") {
            media->direction = MediaDirection::INACTIVE;
        } else if (name == "rtcp-mux") {
            media->rtcpMux = true;
        } else if (name == "candidate") {
            if (media->candidates.size() >= MAX_CANDIDATES)
                return true; // more are not used, which is not an error
            media->candidates.emplace_back(content);
        } else if (name == "msid") {
            media->msid = content;
        } else if (name == "rtpmap") {
            auto [ptStr, encoding] = splitFirst(content);
            auto pt = toUnsigned(ptStr);
            if (not pt)
                return false;
            auto it = codecIndex.find(*pt);
            if (it == codecIndex.end())
                return true;
            auto parts = splitAll(encoding, '/');
            if (parts.size() < 2)
                return false;
            auto clockRate = toUnsigned(parts[1]);
            if (not clockRate)
                return false;
            auto& codec = media->codecs[it->second];
            codec.name = parts[0];
            codec.clockRate = *clockRate;
            if (parts.size() > 2) {
                auto channels = toUnsigned(parts[2]);
                if (not channels)
                    return false;
                codec.channels = *channels;
            }
        } else if (name == "fmtp") {
            auto [ptStr, params] = splitFirst(content);
            auto pt = toUnsigned(ptStr);
            if (not pt)
                return false;
            if (auto it = codecIndex.find(*pt); it != codecIndex.end())
                media->codecs[it->second].fmtp = params;
        }
        return true;
    }

    bool line(std::string_view line)
    {
        if (line.size() > MAX_LINE_SIZE)
            return false;
        if (line.size() < 2 or line[1] != '=')
            return line.empty();
        auto value = line.substr(2);
        switch (line[0]) {
        case 'm':
            return mediaLine(value);
        case 'a':
            return attribute(value);
        default:
            return true;
        }
    }
};

} // namespace

std::optional<SessionDescription>
parse(std::string_view sdp)
{
    if (sdp.size() > MAX_SDP_SIZE or not sdp.starts_with("v=0"))
        return std::nullopt;

    Parser parser;
    while (not sdp.empty()) {
        auto [line, rest] = splitFirst(sdp, '\n');
        if (not line.empty() and line.back() == '\r')
            line.remove_suffix(1);
        if (not parser.line(line))
            return std::nullopt;
        sdp = rest;
    }

    // Codecs without an rtpmap (static payload types such as PCMU) are not
    // ones the daemon uses with a browser.
    for (auto& media : parser.session.media)
        std::erase_if(media.codecs, [](const RtpCodec& codec) { return codec.name.empty(); });

    return std::move(parser.session);
}

std::string
serialize(const SessionDescription& session, uint64_t sessionId, uint64_t version)
{
    std::string sdp;
    sdp.reserve(2048);
    sdp += "v=0\r\n";
    sdp += "o=- " + std::to_string(sessionId & INT64_MAX) + " " + std::to_string(version) + " IN IP4 127.0.0.1\r\n";
    sdp += "s=-\r\n";
    sdp += "t=0 0\r\n";

    for (const auto& media : session.media) {
        if (media.rejected or media.type == MediaType::MEDIA_NONE) {
            // A rejected m-line keeps its media and formats, with port 0.
            std::string type = media.media;
            if (type.empty())
                type = media.type == MediaType::MEDIA_AUDIO ? "audio" : "video";
            auto formats = media.formats;
            if (formats.empty() and not media.codecs.empty())
                formats = std::to_string(media.codecs.front().payloadType);
            auto protocol = media.protocol.empty() ? std::string(PROTOCOL) : media.protocol;
            sdp += "m=" + type + " 0 " + protocol + " " + (formats.empty() ? "0" : formats) + "\r\n";
            sdp += "c=IN IP4 0.0.0.0\r\n";
            if (not media.mid.empty())
                sdp += "a=mid:" + media.mid + "\r\n";
            sdp += "a=inactive\r\n";
            continue;
        }

        sdp += media.type == MediaType::MEDIA_AUDIO ? "m=audio 9 " : "m=video 9 ";
        sdp += PROTOCOL;
        for (const auto& codec : media.codecs)
            sdp += " " + std::to_string(codec.payloadType);
        sdp += "\r\n";
        sdp += "c=IN IP4 0.0.0.0\r\n";
        sdp += "a=ice-ufrag:" + media.iceUfrag + "\r\n";
        sdp += "a=ice-pwd:" + media.icePwd + "\r\n";
        sdp += "a=fingerprint:" + media.fingerprintAlgorithm + " " + media.fingerprint + "\r\n";
        sdp += "a=setup:" + media.setup + "\r\n";
        if (not media.mid.empty())
            sdp += "a=mid:" + media.mid + "\r\n";
        sdp += std::string("a=") + directionString(media.direction) + "\r\n";
        if (not media.msid.empty())
            sdp += "a=msid:" + media.msid + "\r\n";
        sdp += "a=rtcp-mux\r\n";
        sdp += "a=rtcp-rsize\r\n";
        for (const auto& codec : media.codecs) {
            auto pt = std::to_string(codec.payloadType);
            sdp += "a=rtpmap:" + pt + " " + codec.name + "/" + std::to_string(codec.clockRate);
            if (media.type == MediaType::MEDIA_AUDIO and codec.channels > 1)
                sdp += "/" + std::to_string(codec.channels);
            sdp += "\r\n";
            for (const auto& feedback : codec.rtcpFeedback)
                sdp += "a=rtcp-fb:" + pt + " " + feedback + "\r\n";
            if (not codec.fmtp.empty())
                sdp += "a=fmtp:" + pt + " " + codec.fmtp + "\r\n";
        }
        for (const auto& candidate : media.candidates)
            sdp += "a=candidate:" + candidate + "\r\n";
        sdp += "a=end-of-candidates\r\n";
    }
    return sdp;
}

std::optional<RtpCodec>
chooseCodec(const MediaSection& remote, const std::vector<std::string>& preferred)
{
    for (const auto& name : preferred) {
        for (const auto& codec : remote.codecs) {
            if (not equalsIgnoreCase(codec.name, name))
                continue;
            if (equalsIgnoreCase(name, "opus")) {
                if (codec.clockRate == 48000)
                    return codec;
            } else if (equalsIgnoreCase(name, "H264")) {
                if (codec.clockRate == 90000 and isUsableH264(codec))
                    return codec;
            } else if (codec.clockRate == 90000 or remote.type == MediaType::MEDIA_AUDIO) {
                return codec;
            }
        }
    }
    return std::nullopt;
}

std::vector<RtpCodec>
localCodecs(MediaType type, const std::vector<std::string>& preferred)
{
    static const RtpCodec OPUS {111, "opus", 48000, 2, "minptime=10;useinbandfec=1", {}};
    static const RtpCodec H264 {102,
                                "H264",
                                90000,
                                1,
                                "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f",
                                {"nack pli", "ccm fir"}};
    static const RtpCodec VP8 {96, "VP8", 90000, 1, "", {"nack pli", "ccm fir"}};

    std::vector<RtpCodec> codecs;
    for (const auto& name : preferred) {
        const RtpCodec* codec = nullptr;
        if (type == MediaType::MEDIA_AUDIO and equalsIgnoreCase(name, OPUS.name))
            codec = &OPUS;
        else if (type == MediaType::MEDIA_VIDEO and equalsIgnoreCase(name, H264.name))
            codec = &H264;
        else if (type == MediaType::MEDIA_VIDEO and equalsIgnoreCase(name, VP8.name))
            codec = &VP8;
        if (codec and std::none_of(codecs.begin(), codecs.end(), [&](const RtpCodec& c) {
                return c.payloadType == codec->payloadType;
            }))
            codecs.emplace_back(*codec);
    }
    return codecs;
}

MediaDirection
answerDirection(MediaDirection offered)
{
    switch (offered) {
    case MediaDirection::SENDONLY:
        return MediaDirection::RECVONLY;
    case MediaDirection::RECVONLY:
        return MediaDirection::SENDONLY;
    case MediaDirection::INACTIVE:
        return MediaDirection::INACTIVE;
    case MediaDirection::SENDRECV:
    default:
        return MediaDirection::SENDRECV;
    }
}

std::string
receivingSdp(MediaType type, const RtpCodec& codec)
{
    auto pt = std::to_string(codec.payloadType);
    std::string sdp = "v=0\r\n"
                      "o=- 0 0 IN IP4 127.0.0.1\r\n"
                      "s=-\r\n"
                      "c=IN IP4 127.0.0.1\r\n"
                      "t=0 0\r\n";
    sdp += type == MediaType::MEDIA_AUDIO ? "m=audio 9 RTP/AVP " : "m=video 9 RTP/AVP ";
    sdp += pt + "\r\n";
    sdp += "a=rtpmap:" + pt + " " + codec.name + "/" + std::to_string(codec.clockRate);
    if (type == MediaType::MEDIA_AUDIO and codec.channels > 1)
        sdp += "/" + std::to_string(codec.channels);
    sdp += "\r\n";
    if (not codec.fmtp.empty())
        sdp += "a=fmtp:" + pt + " " + codec.fmtp + "\r\n";
    return sdp;
}

} // namespace jami::webrtc
