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

#include "media/media_codec.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Session descriptions as WebRTC peers (browsers) write and read them
// (RFC 8829): one ICE transport per m-line with RTP and RTCP multiplexed,
// DTLS-SRTP keying, trickle-free candidates in the description. Only what a
// call between a browser and the daemon uses.
namespace jami::webrtc {

struct RtpCodec
{
    unsigned payloadType {0};
    std::string name; // as in rtpmap: "opus", "VP8", "H264"
    unsigned clockRate {0};
    unsigned channels {1};
    std::string fmtp;
    std::vector<std::string> rtcpFeedback; // "nack pli", "ccm fir"
};

struct MediaSection
{
    MediaType type {MediaType::MEDIA_NONE}; // NONE for other media, such as data channels
    // The m-line as offered, to answer a rejected one in kind.
    std::string media;
    std::string protocol;
    std::string formats;
    std::string mid;
    bool rejected {false};
    std::vector<RtpCodec> codecs;
    MediaDirection direction {MediaDirection::SENDRECV};
    std::string iceUfrag;
    std::string icePwd;
    std::vector<std::string> candidates; // a=candidate values, without "candidate:"
    std::string fingerprintAlgorithm;    // "sha-256"
    std::string fingerprint;             // "AB:CD:...", upper case
    std::string setup;                   // "actpass", "active" or "passive"
    bool rtcpMux {false};
    std::string msid; // "<stream id> <track id>"
};

struct SessionDescription
{
    std::vector<MediaSection> media;
};

// Parses a peer's description: nothing when it is malformed or oversized.
std::optional<SessionDescription> parse(std::string_view sdp);

// The o= line takes the low 63 bits of `sessionId`: JSEP wants the session ID
// to fit a signed 64-bit integer, which Firefox enforces.
std::string serialize(const SessionDescription& session, uint64_t sessionId, uint64_t version);

// The remote codec to use, the first of `preferred` (codec names, such as
// the account's active codecs in order) that the m-line offers: nothing if
// none. H264 only in packetization mode 1 with a (constrained) baseline
// profile, which browsers decode and the daemon's encoder produces.
std::optional<RtpCodec> chooseCodec(const MediaSection& remote, const std::vector<std::string>& preferred);

// The codecs this side offers for `type`, in the order of `preferred` (codec
// names, such as the account's active codecs): Opus for audio; H264
// (constrained baseline, packetization mode 1) and VP8 for video, with the
// key frame requests the daemon answers. Browsers' usual payload types.
std::vector<RtpCodec> localCodecs(MediaType type, const std::vector<std::string>& preferred);

// The direction of an answer to an m-line offered with `offered`, for a side
// that wants to send and receive.
MediaDirection answerDirection(MediaDirection offered);

// The description FFmpeg's RTP demuxer reads to receive one stream.
std::string receivingSdp(MediaType type, const RtpCodec& codec);

} // namespace jami::webrtc
