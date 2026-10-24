// Session descriptions captured from browsers (Playwright 1.62.1, fake
// devices, all candidates gathered, no STUN or TURN server), 2026-10-19.
// Lines end with LF here; browsers send CRLF, which the tests restore.
#pragma once

namespace jami::test::webrtc_samples {

constexpr const char* CHROMIUM_OFFER = R"SDP(v=0
o=- 986119757609479704 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1
a=extmap-allow-mixed
a=msid-semantic: WMS 7b02935d-7cfb-4938-87bf-cdef82345242
m=audio 55689 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
c=IN IP4 172.18.0.1
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:4147514935 1 udp 2122260223 172.18.0.1 55689 typ host generation 0 network-id 1
a=candidate:4270855564 1 udp 2122194687 172.17.0.1 53260 typ host generation 0 network-id 2
a=candidate:3150340210 1 udp 2122129151 192.168.1.92 40265 typ host generation 0 network-id 3
a=candidate:161274531 1 tcp 1518280447 172.18.0.1 9 typ host tcptype active generation 0 network-id 1
a=candidate:3850520 1 tcp 1518214911 172.17.0.1 9 typ host tcptype active generation 0 network-id 2
a=candidate:1164752102 1 tcp 1518149375 192.168.1.92 9 typ host tcptype active generation 0 network-id 3
a=ice-ufrag:HtjM
a=ice-pwd:UEXhgRX6v996GRSPbybIJ1wa
a=ice-options:trickle
a=fingerprint:sha-256 0B:C5:B5:5B:39:83:78:C2:FC:08:F3:55:AD:9C:72:A5:BF:8E:5A:59:4D:B7:06:D4:71:EB:B9:75:E6:8F:5C:78
a=setup:actpass
a=mid:0
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=sendrecv
a=msid:7b02935d-7cfb-4938-87bf-cdef82345242 4ba56b8f-13e0-4a93-8bd4-cf23d086bcc2
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:111 rrtr
a=rtcp-fb:63 rrtr
a=rtcp-fb:9 rrtr
a=rtcp-fb:0 rrtr
a=rtcp-fb:8 rrtr
a=rtcp-fb:13 rrtr
a=rtcp-fb:110 rrtr
a=rtcp-fb:126 rrtr
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
a=rtpmap:63 red/48000/2
a=fmtp:63 111/111
a=rtpmap:9 G722/8000
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:13 CN/8000
a=rtpmap:110 telephone-event/48000
a=rtpmap:126 telephone-event/8000
a=ssrc:2895693540 cname:IZbd9dFONi+cfxvy
a=ssrc:2895693540 msid:7b02935d-7cfb-4938-87bf-cdef82345242 4ba56b8f-13e0-4a93-8bd4-cf23d086bcc2
m=video 36891 UDP/TLS/RTP/SAVPF 96 97 102 103 104 107 108 109 114 115 116 117 39 40 45 46 98 99 100 101 118 119 120
c=IN IP4 172.18.0.1
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:4147514935 1 udp 2122260223 172.18.0.1 36891 typ host generation 0 network-id 1
a=candidate:4270855564 1 udp 2122194687 172.17.0.1 36731 typ host generation 0 network-id 2
a=candidate:3150340210 1 udp 2122129151 192.168.1.92 34144 typ host generation 0 network-id 3
a=candidate:161274531 1 tcp 1518280447 172.18.0.1 9 typ host tcptype active generation 0 network-id 1
a=candidate:3850520 1 tcp 1518214911 172.17.0.1 9 typ host tcptype active generation 0 network-id 2
a=candidate:1164752102 1 tcp 1518149375 192.168.1.92 9 typ host tcptype active generation 0 network-id 3
a=ice-ufrag:HtjM
a=ice-pwd:UEXhgRX6v996GRSPbybIJ1wa
a=ice-options:trickle
a=fingerprint:sha-256 0B:C5:B5:5B:39:83:78:C2:FC:08:F3:55:AD:9C:72:A5:BF:8E:5A:59:4D:B7:06:D4:71:EB:B9:75:E6:8F:5C:78
a=setup:actpass
a=mid:1
a=extmap:14 urn:ietf:params:rtp-hdrext:toffset
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:13 urn:3gpp:video-orientation
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type
a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing
a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id
a=sendrecv
a=msid:7b02935d-7cfb-4938-87bf-cdef82345242 37c2b392-27dd-4814-9736-5884dd7cdece
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:96 rrtr
a=rtcp-fb:97 rrtr
a=rtcp-fb:102 rrtr
a=rtcp-fb:103 rrtr
a=rtcp-fb:104 rrtr
a=rtcp-fb:107 rrtr
a=rtcp-fb:108 rrtr
a=rtcp-fb:109 rrtr
a=rtcp-fb:114 rrtr
a=rtcp-fb:115 rrtr
a=rtcp-fb:116 rrtr
a=rtcp-fb:117 rrtr
a=rtcp-fb:39 rrtr
a=rtcp-fb:40 rrtr
a=rtcp-fb:45 rrtr
a=rtcp-fb:46 rrtr
a=rtcp-fb:98 rrtr
a=rtcp-fb:99 rrtr
a=rtcp-fb:100 rrtr
a=rtcp-fb:101 rrtr
a=rtcp-fb:118 rrtr
a=rtcp-fb:119 rrtr
a=rtcp-fb:120 rrtr
a=rtpmap:96 VP8/90000
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
a=rtpmap:102 H264/90000
a=rtcp-fb:102 goog-remb
a=rtcp-fb:102 transport-cc
a=rtcp-fb:102 ccm fir
a=rtcp-fb:102 nack
a=rtcp-fb:102 nack pli
a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f
a=rtpmap:103 rtx/90000
a=fmtp:103 apt=102
a=rtpmap:104 H264/90000
a=rtcp-fb:104 goog-remb
a=rtcp-fb:104 transport-cc
a=rtcp-fb:104 ccm fir
a=rtcp-fb:104 nack
a=rtcp-fb:104 nack pli
a=fmtp:104 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f
a=rtpmap:107 rtx/90000
a=fmtp:107 apt=104
a=rtpmap:108 H264/90000
a=rtcp-fb:108 goog-remb
a=rtcp-fb:108 transport-cc
a=rtcp-fb:108 ccm fir
a=rtcp-fb:108 nack
a=rtcp-fb:108 nack pli
a=fmtp:108 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:109 rtx/90000
a=fmtp:109 apt=108
a=rtpmap:114 H264/90000
a=rtcp-fb:114 goog-remb
a=rtcp-fb:114 transport-cc
a=rtcp-fb:114 ccm fir
a=rtcp-fb:114 nack
a=rtcp-fb:114 nack pli
a=fmtp:114 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f
a=rtpmap:115 rtx/90000
a=fmtp:115 apt=114
a=rtpmap:116 H264/90000
a=rtcp-fb:116 goog-remb
a=rtcp-fb:116 transport-cc
a=rtcp-fb:116 ccm fir
a=rtcp-fb:116 nack
a=rtcp-fb:116 nack pli
a=fmtp:116 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f
a=rtpmap:117 rtx/90000
a=fmtp:117 apt=116
a=rtpmap:39 H264/90000
a=rtcp-fb:39 goog-remb
a=rtcp-fb:39 transport-cc
a=rtcp-fb:39 ccm fir
a=rtcp-fb:39 nack
a=rtcp-fb:39 nack pli
a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=4d001f
a=rtpmap:40 rtx/90000
a=fmtp:40 apt=39
a=rtpmap:45 AV1/90000
a=rtcp-fb:45 goog-remb
a=rtcp-fb:45 transport-cc
a=rtcp-fb:45 ccm fir
a=rtcp-fb:45 nack
a=rtcp-fb:45 nack pli
a=fmtp:45 level-idx=5;profile=0;tier=0
a=rtpmap:46 rtx/90000
a=fmtp:46 apt=45
a=rtpmap:98 VP9/90000
a=rtcp-fb:98 goog-remb
a=rtcp-fb:98 transport-cc
a=rtcp-fb:98 ccm fir
a=rtcp-fb:98 nack
a=rtcp-fb:98 nack pli
a=fmtp:98 profile-id=0
a=rtpmap:99 rtx/90000
a=fmtp:99 apt=98
a=rtpmap:100 VP9/90000
a=rtcp-fb:100 goog-remb
a=rtcp-fb:100 transport-cc
a=rtcp-fb:100 ccm fir
a=rtcp-fb:100 nack
a=rtcp-fb:100 nack pli
a=fmtp:100 profile-id=2
a=rtpmap:101 rtx/90000
a=fmtp:101 apt=100
a=rtpmap:118 red/90000
a=rtpmap:119 rtx/90000
a=fmtp:119 apt=118
a=rtpmap:120 ulpfec/90000
a=ssrc-group:FID 2664881317 343874369
a=ssrc:2664881317 cname:IZbd9dFONi+cfxvy
a=ssrc:2664881317 msid:7b02935d-7cfb-4938-87bf-cdef82345242 37c2b392-27dd-4814-9736-5884dd7cdece
a=ssrc:343874369 cname:IZbd9dFONi+cfxvy
a=ssrc:343874369 msid:7b02935d-7cfb-4938-87bf-cdef82345242 37c2b392-27dd-4814-9736-5884dd7cdece
)SDP";

constexpr const char* CHROMIUM_ANSWER = R"SDP(v=0
o=- 8421527331228149322 2 IN IP4 127.0.0.1
s=-
t=0 0
a=group:BUNDLE 0 1
a=extmap-allow-mixed
a=msid-semantic: WMS 7b02935d-7cfb-4938-87bf-cdef82345242
m=audio 35408 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126
c=IN IP4 172.18.0.1
a=rtcp:9 IN IP4 0.0.0.0
a=candidate:1744233887 1 udp 2122260223 172.18.0.1 35408 typ host generation 0 network-id 1
a=candidate:1850801700 1 udp 2122194687 172.17.0.1 55052 typ host generation 0 network-id 2
a=candidate:721849306 1 udp 2122129151 192.168.1.92 57560 typ host generation 0 network-id 3
a=ice-ufrag:Zg+b
a=ice-pwd:XqA7ko6EqPNx97N8o3Yf4NhX
a=ice-options:trickle
a=fingerprint:sha-256 4F:29:BA:23:63:A0:2F:15:BB:22:EF:4E:E2:85:7F:6D:C1:40:E6:C5:22:ED:D4:E5:F6:65:5F:50:C0:F0:AD:00
a=setup:active
a=mid:0
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=sendrecv
a=msid:7b02935d-7cfb-4938-87bf-cdef82345242 e5b1e8d3-593e-44f2-9ec4-43d96a803d0e
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:111 rrtr
a=rtcp-fb:63 rrtr
a=rtcp-fb:9 rrtr
a=rtcp-fb:0 rrtr
a=rtcp-fb:8 rrtr
a=rtcp-fb:13 rrtr
a=rtcp-fb:110 rrtr
a=rtcp-fb:126 rrtr
a=rtpmap:111 opus/48000/2
a=rtcp-fb:111 transport-cc
a=fmtp:111 minptime=10;useinbandfec=1
a=rtpmap:63 red/48000/2
a=fmtp:63 111/111
a=rtpmap:9 G722/8000
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:13 CN/8000
a=rtpmap:110 telephone-event/48000
a=rtpmap:126 telephone-event/8000
a=ssrc:1438824835 cname:YTWmXFjiZvULotKe
m=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 104 107 108 109 114 115 116 117 39 40 45 46 98 99 100 101 118 119 120
c=IN IP4 0.0.0.0
a=rtcp:9 IN IP4 0.0.0.0
a=ice-ufrag:Zg+b
a=ice-pwd:XqA7ko6EqPNx97N8o3Yf4NhX
a=ice-options:trickle
a=fingerprint:sha-256 4F:29:BA:23:63:A0:2F:15:BB:22:EF:4E:E2:85:7F:6D:C1:40:E6:C5:22:ED:D4:E5:F6:65:5F:50:C0:F0:AD:00
a=setup:active
a=mid:1
a=extmap:14 urn:ietf:params:rtp-hdrext:toffset
a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:13 urn:3gpp:video-orientation
a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type
a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing
a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space
a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id
a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id
a=sendrecv
a=msid:7b02935d-7cfb-4938-87bf-cdef82345242 5477926b-07b2-43c0-b241-4e84bea2c0f5
a=rtcp-mux
a=rtcp-rsize
a=rtcp-xr:rcvr-rtt=all
a=rtcp-fb:96 rrtr
a=rtcp-fb:97 rrtr
a=rtcp-fb:102 rrtr
a=rtcp-fb:103 rrtr
a=rtcp-fb:104 rrtr
a=rtcp-fb:107 rrtr
a=rtcp-fb:108 rrtr
a=rtcp-fb:109 rrtr
a=rtcp-fb:114 rrtr
a=rtcp-fb:115 rrtr
a=rtcp-fb:116 rrtr
a=rtcp-fb:117 rrtr
a=rtcp-fb:39 rrtr
a=rtcp-fb:40 rrtr
a=rtcp-fb:45 rrtr
a=rtcp-fb:46 rrtr
a=rtcp-fb:98 rrtr
a=rtcp-fb:99 rrtr
a=rtcp-fb:100 rrtr
a=rtcp-fb:101 rrtr
a=rtcp-fb:118 rrtr
a=rtcp-fb:119 rrtr
a=rtcp-fb:120 rrtr
a=rtpmap:96 VP8/90000
a=rtcp-fb:96 goog-remb
a=rtcp-fb:96 transport-cc
a=rtcp-fb:96 ccm fir
a=rtcp-fb:96 nack
a=rtcp-fb:96 nack pli
a=rtpmap:97 rtx/90000
a=fmtp:97 apt=96
a=rtpmap:102 H264/90000
a=rtcp-fb:102 goog-remb
a=rtcp-fb:102 transport-cc
a=rtcp-fb:102 ccm fir
a=rtcp-fb:102 nack
a=rtcp-fb:102 nack pli
a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f
a=rtpmap:103 rtx/90000
a=fmtp:103 apt=102
a=rtpmap:104 H264/90000
a=rtcp-fb:104 goog-remb
a=rtcp-fb:104 transport-cc
a=rtcp-fb:104 ccm fir
a=rtcp-fb:104 nack
a=rtcp-fb:104 nack pli
a=fmtp:104 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42001f
a=rtpmap:107 rtx/90000
a=fmtp:107 apt=104
a=rtpmap:108 H264/90000
a=rtcp-fb:108 goog-remb
a=rtcp-fb:108 transport-cc
a=rtcp-fb:108 ccm fir
a=rtcp-fb:108 nack
a=rtcp-fb:108 nack pli
a=fmtp:108 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f
a=rtpmap:109 rtx/90000
a=fmtp:109 apt=108
a=rtpmap:114 H264/90000
a=rtcp-fb:114 goog-remb
a=rtcp-fb:114 transport-cc
a=rtcp-fb:114 ccm fir
a=rtcp-fb:114 nack
a=rtcp-fb:114 nack pli
a=fmtp:114 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=42e01f
a=rtpmap:115 rtx/90000
a=fmtp:115 apt=114
a=rtpmap:116 H264/90000
a=rtcp-fb:116 goog-remb
a=rtcp-fb:116 transport-cc
a=rtcp-fb:116 ccm fir
a=rtcp-fb:116 nack
a=rtcp-fb:116 nack pli
a=fmtp:116 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=4d001f
a=rtpmap:117 rtx/90000
a=fmtp:117 apt=116
a=rtpmap:39 H264/90000
a=rtcp-fb:39 goog-remb
a=rtcp-fb:39 transport-cc
a=rtcp-fb:39 ccm fir
a=rtcp-fb:39 nack
a=rtcp-fb:39 nack pli
a=fmtp:39 level-asymmetry-allowed=1;packetization-mode=0;profile-level-id=4d001f
a=rtpmap:40 rtx/90000
a=fmtp:40 apt=39
a=rtpmap:45 AV1/90000
a=rtcp-fb:45 goog-remb
a=rtcp-fb:45 transport-cc
a=rtcp-fb:45 ccm fir
a=rtcp-fb:45 nack
a=rtcp-fb:45 nack pli
a=fmtp:45 level-idx=5;profile=0;tier=0
a=rtpmap:46 rtx/90000
a=fmtp:46 apt=45
a=rtpmap:98 VP9/90000
a=rtcp-fb:98 goog-remb
a=rtcp-fb:98 transport-cc
a=rtcp-fb:98 ccm fir
a=rtcp-fb:98 nack
a=rtcp-fb:98 nack pli
a=fmtp:98 profile-id=0
a=rtpmap:99 rtx/90000
a=fmtp:99 apt=98
a=rtpmap:100 VP9/90000
a=rtcp-fb:100 goog-remb
a=rtcp-fb:100 transport-cc
a=rtcp-fb:100 ccm fir
a=rtcp-fb:100 nack
a=rtcp-fb:100 nack pli
a=fmtp:100 profile-id=2
a=rtpmap:101 rtx/90000
a=fmtp:101 apt=100
a=rtpmap:118 red/90000
a=rtpmap:119 rtx/90000
a=fmtp:119 apt=118
a=rtpmap:120 ulpfec/90000
a=ssrc-group:FID 2348946760 4221818603
a=ssrc:2348946760 cname:YTWmXFjiZvULotKe
a=ssrc:4221818603 cname:YTWmXFjiZvULotKe
)SDP";

constexpr const char* FIREFOX_OFFER = R"SDP(v=0
o=mozilla...THIS_IS_SDPARTA-99.0 5608412714583718433 0 IN IP4 0.0.0.0
s=-
t=0 0
a=sendrecv
a=fingerprint:sha-256 9E:C3:89:EA:E5:11:76:E2:AF:42:1F:D9:23:92:78:CA:DD:A3:4B:E2:D2:DE:0C:62:65:99:D0:87:5D:56:E5:50
a=group:BUNDLE 0 1
a=ice-options:trickle
a=msid-semantic:WMS *
m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101
c=IN IP4 0.0.0.0
a=candidate:0 1 UDP 2122252543 62975b61-f650-4830-b9ff-76244136ac9e.local 57615 typ host
a=candidate:1 1 TCP 2105524479 62975b61-f650-4830-b9ff-76244136ac9e.local 9 typ host tcptype active
a=candidate:0 2 UDP 2122252542 62975b61-f650-4830-b9ff-76244136ac9e.local 40574 typ host
a=candidate:1 2 TCP 2105524478 62975b61-f650-4830-b9ff-76244136ac9e.local 9 typ host tcptype active
a=sendrecv
a=end-of-candidates
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:2/recvonly urn:ietf:params:rtp-hdrext:csrc-audio-level
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap-allow-mixed
a=fmtp:109 maxplaybackrate=48000;stereo=1;useinbandfec=1
a=fmtp:101 0-15
a=ice-pwd:e0ae479e4e9164af84cde13f9a9b4b60
a=ice-ufrag:291ca461
a=mid:0
a=msid:{0367b7d4-eb18-4b5d-92f1-7519c4e93f52} {ecaf9c44-83f5-470d-acbf-493527cb327a}
a=rtcp:40574 IN IP4 62975b61-f650-4830-b9ff-76244136ac9e.local
a=rtcp-fb:109 transport-cc
a=rtcp-fb:9 transport-cc
a=rtcp-fb:0 transport-cc
a=rtcp-fb:8 transport-cc
a=rtcp-mux
a=rtpmap:109 opus/48000/2
a=rtpmap:9 G722/8000/1
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:101 telephone-event/8000
a=setup:actpass
a=ssrc:2840625452 cname:{3f8903cc-bdf8-4d9c-b34a-8d3b3d0d2210}
m=video 9 UDP/TLS/RTP/SAVPF 120 124 121 125 99 100 123 122 119
c=IN IP4 0.0.0.0
a=candidate:0 1 UDP 2122252543 62975b61-f650-4830-b9ff-76244136ac9e.local 49068 typ host
a=candidate:1 1 TCP 2105524479 62975b61-f650-4830-b9ff-76244136ac9e.local 9 typ host tcptype active
a=candidate:0 2 UDP 2122252542 62975b61-f650-4830-b9ff-76244136ac9e.local 60211 typ host
a=candidate:1 2 TCP 2105524478 62975b61-f650-4830-b9ff-76244136ac9e.local 9 typ host tcptype active
a=sendrecv
a=end-of-candidates
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:5 urn:ietf:params:rtp-hdrext:toffset
a=extmap:6/recvonly http://www.webrtc.org/experiments/rtp-hdrext/playout-delay
a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap-allow-mixed
a=fmtp:120 max-fs=12288;max-fr=60
a=fmtp:124 apt=120
a=fmtp:121 max-fs=12288;max-fr=60
a=fmtp:125 apt=121
a=fmtp:100 apt=99
a=fmtp:119 apt=122
a=ice-pwd:e0ae479e4e9164af84cde13f9a9b4b60
a=ice-ufrag:291ca461
a=mid:1
a=msid:{0367b7d4-eb18-4b5d-92f1-7519c4e93f52} {9ca2cba3-e791-4c80-ac35-caf20024fa22}
a=rtcp:60211 IN IP4 62975b61-f650-4830-b9ff-76244136ac9e.local
a=rtcp-fb:120 nack
a=rtcp-fb:120 nack pli
a=rtcp-fb:120 ccm fir
a=rtcp-fb:120 goog-remb
a=rtcp-fb:120 transport-cc
a=rtcp-fb:121 nack
a=rtcp-fb:121 nack pli
a=rtcp-fb:121 ccm fir
a=rtcp-fb:121 goog-remb
a=rtcp-fb:121 transport-cc
a=rtcp-fb:99 nack
a=rtcp-fb:99 nack pli
a=rtcp-fb:99 ccm fir
a=rtcp-fb:99 goog-remb
a=rtcp-fb:99 transport-cc
a=rtcp-fb:123 nack
a=rtcp-fb:123 nack pli
a=rtcp-fb:123 ccm fir
a=rtcp-fb:123 goog-remb
a=rtcp-fb:123 transport-cc
a=rtcp-fb:122 nack
a=rtcp-fb:122 nack pli
a=rtcp-fb:122 ccm fir
a=rtcp-fb:122 goog-remb
a=rtcp-fb:122 transport-cc
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:120 VP8/90000
a=rtpmap:124 rtx/90000
a=rtpmap:121 VP9/90000
a=rtpmap:125 rtx/90000
a=rtpmap:99 AV1/90000
a=rtpmap:100 rtx/90000
a=rtpmap:123 ulpfec/90000
a=rtpmap:122 red/90000
a=rtpmap:119 rtx/90000
a=setup:actpass
a=ssrc:2649119836 cname:{3f8903cc-bdf8-4d9c-b34a-8d3b3d0d2210}
a=ssrc:1562470703 cname:{3f8903cc-bdf8-4d9c-b34a-8d3b3d0d2210}
a=ssrc-group:FID 2649119836 1562470703
)SDP";

constexpr const char* FIREFOX_ANSWER = R"SDP(v=0
o=mozilla...THIS_IS_SDPARTA-99.0 6446909498327208044 0 IN IP4 0.0.0.0
s=-
t=0 0
a=sendrecv
a=fingerprint:sha-256 C4:35:1F:72:7F:40:37:02:50:2D:1D:46:10:29:07:0C:70:03:51:28:44:FE:E2:E6:E6:59:9B:19:E7:9A:EA:F0
a=group:BUNDLE 0 1
a=ice-options:trickle
a=msid-semantic:WMS *
m=audio 9 UDP/TLS/RTP/SAVPF 109 9 0 8 101
c=IN IP4 0.0.0.0
a=candidate:0 1 UDP 2122252543 764fc8a2-bfa7-4fda-b316-91a31f9059df.local 40865 typ host
a=candidate:1 1 TCP 2105524479 764fc8a2-bfa7-4fda-b316-91a31f9059df.local 9 typ host tcptype active
a=sendrecv
a=end-of-candidates
a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap-allow-mixed
a=fmtp:109 maxplaybackrate=48000;stereo=1;useinbandfec=1
a=fmtp:101 0-15
a=ice-pwd:513f363e1b22ca0c6b126a492a3ae627
a=ice-ufrag:e80b39a0
a=mid:0
a=msid:{0367b7d4-eb18-4b5d-92f1-7519c4e93f52} {11d178e1-a77b-48cb-a2ad-30494870c16c}
a=rtcp-fb:109 transport-cc
a=rtcp-fb:9 transport-cc
a=rtcp-fb:0 transport-cc
a=rtcp-fb:8 transport-cc
a=rtcp-mux
a=rtpmap:109 opus/48000/2
a=rtpmap:9 G722/8000/1
a=rtpmap:0 PCMU/8000
a=rtpmap:8 PCMA/8000
a=rtpmap:101 telephone-event/8000
a=setup:active
a=ssrc:1105707289 cname:{8a6d7fd1-8d7a-4dd5-938f-1c8e0ccc0160}
m=video 9 UDP/TLS/RTP/SAVPF 120 124 121 125 99 100 123 122 119
c=IN IP4 0.0.0.0
a=sendrecv
a=extmap:3 urn:ietf:params:rtp-hdrext:sdes:mid
a=extmap:4 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time
a=extmap:5 urn:ietf:params:rtp-hdrext:toffset
a=extmap:7 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01
a=extmap-allow-mixed
a=fmtp:120 max-fs=12288;max-fr=60
a=fmtp:124 apt=120
a=fmtp:121 max-fs=12288;max-fr=60
a=fmtp:125 apt=121
a=fmtp:100 apt=99
a=fmtp:119 apt=122
a=ice-pwd:513f363e1b22ca0c6b126a492a3ae627
a=ice-ufrag:e80b39a0
a=mid:1
a=msid:{0367b7d4-eb18-4b5d-92f1-7519c4e93f52} {804f7595-d6f3-4d9f-b7e6-244dd3bab8a6}
a=rtcp-fb:120 nack
a=rtcp-fb:120 nack pli
a=rtcp-fb:120 ccm fir
a=rtcp-fb:120 goog-remb
a=rtcp-fb:120 transport-cc
a=rtcp-fb:121 nack
a=rtcp-fb:121 nack pli
a=rtcp-fb:121 ccm fir
a=rtcp-fb:121 goog-remb
a=rtcp-fb:121 transport-cc
a=rtcp-fb:99 nack
a=rtcp-fb:99 nack pli
a=rtcp-fb:99 ccm fir
a=rtcp-fb:99 goog-remb
a=rtcp-fb:99 transport-cc
a=rtcp-fb:123 nack
a=rtcp-fb:123 nack pli
a=rtcp-fb:123 ccm fir
a=rtcp-fb:123 goog-remb
a=rtcp-fb:123 transport-cc
a=rtcp-fb:122 nack
a=rtcp-fb:122 nack pli
a=rtcp-fb:122 ccm fir
a=rtcp-fb:122 goog-remb
a=rtcp-fb:122 transport-cc
a=rtcp-mux
a=rtcp-rsize
a=rtpmap:120 VP8/90000
a=rtpmap:124 rtx/90000
a=rtpmap:121 VP9/90000
a=rtpmap:125 rtx/90000
a=rtpmap:99 AV1/90000
a=rtpmap:100 rtx/90000
a=rtpmap:123 ulpfec/90000
a=rtpmap:122 red/90000
a=rtpmap:119 rtx/90000
a=setup:active
a=ssrc:675839462 cname:{8a6d7fd1-8d7a-4dd5-938f-1c8e0ccc0160}
a=ssrc:1567108589 cname:{8a6d7fd1-8d7a-4dd5-938f-1c8e0ccc0160}
a=ssrc-group:FID 675839462 1567108589
)SDP";

} // namespace jami::test::webrtc_samples
