// Copyright The Phorepoc Authors
// SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later

// xk_netlog_proto.h -- the debug link's wire format, with no kernel types (the host test, tests/netlog, builds it as is).
//
// The link needs no configuration and no lab-side setup on the machine: the kernel takes the IPv4 link-local address
// 169.254.<mac[4]>.<mac[5]> (RFC 3927's range, without its probing: the rig's LAN segment has one such host) and
//   - sends every kernel log byte as UDP broadcasts to port 6666 (255.255.255.255, Ethernet broadcast):
//       "PHL1" | boot id (u32) | seq (u32) | the bytes            (all little endian; seq counts packets of this boot,
//       so the lab sees a reboot (new boot id) and loss (a seq gap); up to 1460 bytes a packet)
//   - takes commands on UDP port 6667, addressed to it or broadcast, and answers the sender (its MAC + IP + port, so no
//     ARP is needed on either side; ARP requests for its address are answered anyway):
//       request  "PHC1" | token (u32) | id (u32) | op (u8) | args
//       reply    "PHR1" | id (u32) | op (u8) | status (i32) | data
//     ops: 'P' ping (data = boot id, MAC), 'R' reboot, 'D' thread dump (into the log), 'H' hang with interrupts off (tests
//     the watchdog), 'K' key {std key u16, down u8}, 'T' pointer {x u16, y u16, action u8: 0 move, 1 down, 2 up},
//     'S' screenshot: replies (data = width u16, height u16), then a stream of
//       "PHF1" | id (u32) | width u16 | height u16 | first pixel u32 | pixel count u16 | pixels (32-bit XRGB)
//     to the sender, in order, row-major, 363 pixels a packet; a last packet has count 0. UDP: the lab re-asks if a gap
//     shows (first pixel tells where each packet belongs).
//   A token other than the image's (NETLOG_TOKEN, 0 = any) is refused with status -1.
#ifndef XK_NETLOG_PROTO_H
#define XK_NETLOG_PROTO_H

namespace NetLog {

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;

const u16 KLogPort = 6666, KCmdPort = 6667;
const u32 KMaxPayload = 1472;	// 1500 - IPv4 - UDP
const u32 KHdr = 14 + 20 + 8;		// Ethernet + IPv4 + UDP

inline void Put16be(u8* p, u32 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
inline void Put32le(u8* p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
inline u32 Get16be(const u8* p) { return (u32)p[0] << 8 | p[1]; }
inline u32 Get16le(const u8* p) { return p[0] | (u32)p[1] << 8; }
inline u32 Get32le(const u8* p) { return p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24; }

inline void LinkLocal(const u8 aMac[6], u8 aIp[4])
	{
	aIp[0] = 169; aIp[1] = 254;
	aIp[2] = aMac[4] == 0 ? 1 : aMac[4] == 255 ? 254 : aMac[4];	// RFC 3927: 169.254.0.x and .255.x are reserved
	aIp[3] = aMac[5];
	}

inline u32 IpSum(const u8* p, u32 n)
	{
	u32 s = 0;
	for (u32 i = 0; i + 1 < n; i += 2)
		s += Get16be(p + i);
	if (n & 1)
		s += (u32)p[n - 1] << 8;
	while (s >> 16)
		s = (s & 0xFFFF) + (s >> 16);
	return ~s & 0xFFFF;
	}

// one UDP datagram as a whole Ethernet frame; the payload is already at aFrame + KHdr. Returns the frame length.
inline u32 BuildUdp(u8* aFrame, const u8 aSrcMac[6], const u8 aDstMac[6], const u8 aSrcIp[4], const u8 aDstIp[4],
	u32 aSport, u32 aDport, u32 aLen)
	{
	u8* e = aFrame; u8* ip = e + 14; u8* udp = ip + 20;
	for (int i = 0; i < 6; ++i) { e[i] = aDstMac[i]; e[6 + i] = aSrcMac[i]; }
	e[12] = 0x08; e[13] = 0x00;
	ip[0] = 0x45; ip[1] = 0; Put16be(ip + 2, 20 + 8 + aLen); Put16be(ip + 4, 0); Put16be(ip + 6, 0x4000);	// DF
	ip[8] = 64; ip[9] = 17; ip[10] = ip[11] = 0;
	for (int i = 0; i < 4; ++i) { ip[12 + i] = aSrcIp[i]; ip[16 + i] = aDstIp[i]; }
	Put16be(ip + 10, IpSum(ip, 20));
	Put16be(udp, aSport); Put16be(udp + 2, aDport); Put16be(udp + 4, 8 + aLen); Put16be(udp + 6, 0);	// no UDP checksum
	return KHdr + aLen;
	}

// an ARP reply for aFrame (a request for aIp); returns its length (42), or 0 if aFrame is not one
inline u32 ArpReply(const u8* aFrame, u32 aLen, const u8 aMac[6], const u8 aIp[4], u8* aOut)
	{
	if (aLen < 42 || aFrame[12] != 0x08 || aFrame[13] != 0x06)
		return 0;
	const u8* a = aFrame + 14;
	if (Get16be(a) != 1 || Get16be(a + 2) != 0x0800 || a[4] != 6 || a[5] != 4 || Get16be(a + 6) != 1)
		return 0;
	for (int i = 0; i < 4; ++i)
		if (a[24 + i] != aIp[i])
			return 0;
	for (int i = 0; i < 6; ++i) { aOut[i] = aFrame[6 + i]; aOut[6 + i] = aMac[i]; }
	aOut[12] = 0x08; aOut[13] = 0x06;
	u8* r = aOut + 14;
	Put16be(r, 1); Put16be(r + 2, 0x0800); r[4] = 6; r[5] = 4; Put16be(r + 6, 2);
	for (int i = 0; i < 6; ++i) { r[8 + i] = aMac[i]; r[18 + i] = a[8 + i]; }
	for (int i = 0; i < 4; ++i) { r[14 + i] = aIp[i]; r[24 + i] = a[14 + i]; }
	return 42;
	}

struct TCmd
	{
	u8 iMac[6];			// the sender
	u8 iIp[4];
	u32 iPort;
	u32 iToken, iId;
	u8 iOp;
	const u8* iArgs;
	u32 iArgLen;
	};

// a command datagram for us (our MAC or broadcast, IPv4 UDP to KCmdPort, "PHC1")? 1 = yes, aCmd filled
inline int ParseCmd(const u8* f, u32 n, const u8 aMac[6], TCmd& aCmd)
	{
	if (n < KHdr + 13 || f[12] != 0x08 || f[13] != 0x00)
		return 0;
	int mine = 1, bcast = 1;
	for (int i = 0; i < 6; ++i)
		{ mine &= f[i] == aMac[i]; bcast &= f[i] == 0xFF; }
	const u8* ip = f + 14;
	if (!(mine || bcast) || (ip[0] >> 4) != 4 || ip[9] != 17 || (Get16be(ip + 6) & 0x3FFF) != 0)
		return 0;										// not IPv4 UDP, or a fragment
	const u32 ihl = (ip[0] & 15) * 4, tot = Get16be(ip + 2);
	if (ihl < 20 || tot > n - 14 || tot < ihl + 8)
		return 0;
	const u8* udp = ip + ihl;
	const u32 ulen = Get16be(udp + 4);
	if (Get16be(udp + 2) != KCmdPort || ulen < 8 + 13 || ulen > tot - ihl)
		return 0;
	const u8* p = udp + 8;
	if (p[0] != 'P' || p[1] != 'H' || p[2] != 'C' || p[3] != '1')
		return 0;
	for (int i = 0; i < 6; ++i) aCmd.iMac[i] = f[6 + i];
	for (int i = 0; i < 4; ++i) aCmd.iIp[i] = ip[12 + i];
	aCmd.iPort = Get16be(udp);
	aCmd.iToken = Get32le(p + 4);
	aCmd.iId = Get32le(p + 8);
	aCmd.iOp = p[12];
	aCmd.iArgs = p + 13;
	aCmd.iArgLen = ulen - 8 - 13;
	return 1;
	}

}	// namespace NetLog
#endif
