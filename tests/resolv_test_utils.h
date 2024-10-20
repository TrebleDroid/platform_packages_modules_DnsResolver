/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#pragma once

#include <arpa/nameser.h>
#include <netdb.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <aidl/android/net/INetd.h>
#include <aidl/android/net/ResolverParamsParcel.h>
#include <android-base/properties.h>
#include <android-modules-utils/sdk_level.h>
#include <firewall.h>
#include <gtest/gtest.h>
#include <netdutils/InternetAddresses.h>

#include "dns_responder/dns_responder.h"
#include "params.h"
#include "util.h"

class ScopeBlockedUIDRule {
    using INetd = aidl::android::net::INetd;

  public:
    ScopeBlockedUIDRule(INetd* netSrv, uid_t testUid)
        : mNetSrv(netSrv), mTestUid(testUid), mSavedUid(getuid()) {
        // Add drop rule for testUid. Also enable the standby chain because it might not be
        // enabled. Unfortunately we cannot use FIREWALL_CHAIN_NONE, or custom iptables rules, for
        // this purpose because netd calls fchown() on the DNS query sockets, and "iptables -m
        // owner" matches the UID of the socket creator, not the UID set by fchown().
        // TODO: migrate FIREWALL_CHAIN_NONE to eBPF as well.
        if (android::modules::sdklevel::IsAtLeastT()) {
            mFw = Firewall::getInstance();
            EXPECT_RESULT_OK(mFw->toggleStandbyMatch(true));
            EXPECT_RESULT_OK(mFw->addRule(mTestUid, STANDBY_MATCH));
        } else {
            EXPECT_TRUE(
                    mNetSrv->firewallEnableChildChain(INetd::FIREWALL_CHAIN_STANDBY, true).isOk());
            EXPECT_TRUE(mNetSrv->firewallSetUidRule(INetd::FIREWALL_CHAIN_STANDBY, mTestUid,
                                                    INetd::FIREWALL_RULE_DENY)
                                .isOk());
        }
        EXPECT_TRUE(seteuid(mTestUid) == 0);
    };
    ~ScopeBlockedUIDRule() {
        // Restore uid
        EXPECT_TRUE(seteuid(mSavedUid) == 0);
        // Remove drop rule for testUid, and disable the standby chain.
        if (android::modules::sdklevel::IsAtLeastT()) {
            EXPECT_RESULT_OK(mFw->removeRule(mTestUid, STANDBY_MATCH));
            EXPECT_RESULT_OK(mFw->toggleStandbyMatch(false));
        } else {
            EXPECT_TRUE(mNetSrv->firewallSetUidRule(INetd::FIREWALL_CHAIN_STANDBY, mTestUid,
                                                    INetd::FIREWALL_RULE_ALLOW)
                                .isOk());
            EXPECT_TRUE(
                    mNetSrv->firewallEnableChildChain(INetd::FIREWALL_CHAIN_STANDBY, false).isOk());
        }
    }

  private:
    INetd* mNetSrv;
    Firewall* mFw;
    const uid_t mTestUid;
    const uid_t mSavedUid;
};

// Supported from T+ only.
class ScopedSetDataSaverByBPF {
  public:
    ScopedSetDataSaverByBPF(bool wanted) {
        if (android::modules::sdklevel::IsAtLeastT()) {
            mFw = Firewall::getInstance();
            // Backup current setting.
            const Result<bool> current = mFw->getDataSaverSetting();
            EXPECT_RESULT_OK(current);
            if (wanted != current.value()) {
                mSavedDataSaverSetting = current;
                EXPECT_RESULT_OK(mFw->setDataSaver(wanted));
            }
        }
    };
    ~ScopedSetDataSaverByBPF() {
        // Restore the setting.
        if (mSavedDataSaverSetting.has_value()) {
            EXPECT_RESULT_OK(mFw->setDataSaver(mSavedDataSaverSetting.value()));
        }
    }

  private:
    Firewall* mFw;
    Result<bool> mSavedDataSaverSetting;
};

class ScopedChangeUID {
  public:
    ScopedChangeUID(uid_t testUid) : mTestUid(testUid), mSavedUid(getuid()) {
        EXPECT_TRUE(seteuid(mTestUid) == 0);
    };
    ~ScopedChangeUID() { EXPECT_TRUE(seteuid(mSavedUid) == 0); }

  private:
    const uid_t mTestUid;
    const uid_t mSavedUid;
};

class ScopedSystemProperties {
  public:
    ScopedSystemProperties(const std::string& key, const std::string& value) : mStoredKey(key) {
        mStoredValue = android::base::GetProperty(key, "");
        android::base::SetProperty(key, value);
    }
    ~ScopedSystemProperties() { android::base::SetProperty(mStoredKey, mStoredValue); }

  private:
    std::string mStoredKey;
    std::string mStoredValue;
};

class ScopedDefaultNetwork {
    using INetd = aidl::android::net::INetd;

  public:
    ScopedDefaultNetwork(INetd* netSrv, uid_t testDefaultNetwork) : mNetSrv(netSrv) {
        EXPECT_TRUE(mNetSrv->networkGetDefault(&mStoredDefaultNetwork).isOk());
        EXPECT_TRUE(mNetSrv->networkSetDefault(testDefaultNetwork).isOk());
    };
    ~ScopedDefaultNetwork() {
        EXPECT_TRUE(mNetSrv->networkSetDefault(mStoredDefaultNetwork).isOk());
    }

  private:
    INetd* mNetSrv;
    int mStoredDefaultNetwork;
};

struct DnsRecord {
    std::string host_name;  // host name
    ns_type type;           // record type
    std::string addr;       // ipv4/v6 address
};

// TODO: make this dynamic and stop depending on implementation details.
constexpr int TEST_NETID = 30;
// Use the biggest two reserved appId for applications to avoid conflict with existing uids.
constexpr int TEST_UID = 99999;
constexpr int TEST_UID2 = 99998;

constexpr char kDnsPortString[] = "53";
constexpr char kDohPortString[] = "443";
constexpr char kDotPortString[] = "853";

const std::string kFlagPrefix("persist.device_config.netd_native.");

const std::string kDohEarlyDataFlag(kFlagPrefix + "doh_early_data");
const std::string kDohIdleTimeoutFlag(kFlagPrefix + "doh_idle_timeout_ms");
const std::string kDohProbeTimeoutFlag(kFlagPrefix + "doh_probe_timeout_ms");
const std::string kDohQueryTimeoutFlag(kFlagPrefix + "doh_query_timeout_ms");
const std::string kDohSessionResumptionFlag(kFlagPrefix + "doh_session_resumption");
const std::string kDotAsyncHandshakeFlag(kFlagPrefix + "dot_async_handshake");
const std::string kDotConnectTimeoutMsFlag(kFlagPrefix + "dot_connect_timeout_ms");
const std::string kDotMaxretriesFlag(kFlagPrefix + "dot_maxtries");
const std::string kDotQueryTimeoutMsFlag(kFlagPrefix + "dot_query_timeout_ms");
const std::string kDotQuickFallbackFlag(kFlagPrefix + "dot_quick_fallback");
const std::string kDotRevalidationThresholdFlag(kFlagPrefix + "dot_revalidation_threshold");
const std::string kDotXportUnusableThresholdFlag(kFlagPrefix + "dot_xport_unusable_threshold");
const std::string kDotValidationLatencyFactorFlag(kFlagPrefix + "dot_validation_latency_factor");
const std::string kDotValidationLatencyOffsetMsFlag(kFlagPrefix +
                                                    "dot_validation_latency_offset_ms");
const std::string kFailFastOnUidNetworkBlockingFlag(kFlagPrefix +
                                                    "fail_fast_on_uid_network_blocking");
const std::string kKeepListeningUdpFlag(kFlagPrefix + "keep_listening_udp");
const std::string kParallelLookupSleepTimeFlag(kFlagPrefix + "parallel_lookup_sleep_time");
const std::string kRetransIntervalFlag(kFlagPrefix + "retransmission_time_interval");
const std::string kRetryCountFlag(kFlagPrefix + "retry_count");
const std::string kSortNameserversFlag(kFlagPrefix + "sort_nameservers");

const std::string kPersistNetPrefix("persist.net.");

const std::string kQueryLogSize(kPersistNetPrefix + "dns_query_log_size");

static constexpr char kLocalHost[] = "localhost";
static constexpr char kLocalHostAddr[] = "127.0.0.1";
static constexpr char kIp6LocalHost[] = "ip6-localhost";
static constexpr char kIp6LocalHostAddr[] = "::1";
static constexpr char kHelloExampleCom[] = "hello.example.com.";
static constexpr char kHelloExampleComAddrV4[] = "1.2.3.4";
static constexpr char kHelloExampleComAddrV4_2[] = "8.8.8.8";
static constexpr char kHelloExampleComAddrV4_3[] = "81.117.21.202";
static constexpr char kHelloExampleComAddrV6[] = "::1.2.3.4";
static constexpr char kHelloExampleComAddrV6_IPV4COMPAT[] = "::1.2.3.4";
static constexpr char kHelloExampleComAddrV6_TEREDO[] = "2001::47c1";
static constexpr char kHelloExampleComAddrV6_GUA[] = "2404:6800::5175:15ca";
static constexpr char kExampleComDomain[] = ".example.com";

static const std::string kNat64Prefix = "64:ff9b::/96";
static const std::string kNat64Prefix2 = "2001:db8:6464::/96";

constexpr size_t kMaxmiumLabelSize = 63;  // see RFC 1035 section 2.3.4.

static const std::vector<uint8_t> kHelloExampleComQueryV4 = {
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x01, 0x00, /* Flags: rd */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x00, /* Answer RRs: 0 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01              /* Class: IN */
};

static const std::vector<uint8_t> kHelloExampleComResponseV4 = {
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x81, 0x80, /* Flags: qr rd ra */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x01, /* Answer RRs: 1 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        /* Answers */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x04,             /* Data length: 4 */
        0x01, 0x02, 0x03, 0x04  /* Address: 1.2.3.4 */
};

const std::vector<uint8_t> kHelloExampleComResponsesV4 = {
        // scapy.DNS(
        //   id=0,
        //   qr=1,
        //   ra=1,
        //   qd=scapy.DNSQR(qname="hello.example.com",qtype="A"),
        //   an=scapy.DNSRR(rrname="hello.example.com",type="A",ttl=0,rdata='1.2.3.4') /
        //      scapy.DNSRR(rrname="hello.example.com",type="A",ttl=0,rdata='8.8.8.8') /
        //      scapy.DNSRR(rrname="hello.example.com",type="A",ttl=0,rdata='81.117.21.202'))
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x81, 0x80, /* Flags: qr rd ra */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x03, /* Answer RRs: 3 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6C, 0x6C, 0x6F, 0x07, 0x65, 0x78, 0x61, 0x6D, 0x70, 0x6C, 0x65, 0x03,
        0x63, 0x6F, 0x6D, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        /* Answers */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x04,             /* Data length: 4 */
        0x01, 0x02, 0x03, 0x04, /* Address: 1.2.3.4 */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x04,             /* Data length: 4 */
        0x08, 0x08, 0x08, 0x08, /* Address: 8.8.8.8 */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x01,             /* Type: A */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x04,             /* Data length: 4 */
        0x51, 0x75, 0x15, 0xca  /* Address: 81.117.21.202 */
};

static const std::vector<uint8_t> kHelloExampleComQueryV6 = {
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x01, 0x00, /* Flags: rd */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x00, /* Answer RRs: 0 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x1c,             /* Type: AAAA */
        0x00, 0x01              /* Class: IN */
};

const std::vector<uint8_t> kHelloExampleComResponsesV6 = {
        // The addresses are IPv4-compatible address, teredo tunneling address and global unicast
        // address.
        //
        // scapy.DNS(
        // id=0,
        // qr=1,
        // ra=1,
        // qd=scapy.DNSQR(qname="hello.example.com",qtype="AAAA"),
        // an=scapy.DNSRR(rrname="hello.example.com",type="AAAA",rdata='::1.2.3.4') /
        //    scapy.DNSRR(rrname="hello.example.com",type="AAAA",rdata='2001::47c1') /
        //    scapy.DNSRR(rrname="hello.example.com",type="AAAA",rdata='2404:6800::5175:15ca'))
        /* Header */
        0x00, 0x00, /* Transaction ID: 0x0000 */
        0x81, 0x80, /* Flags: qr rd ra */
        0x00, 0x01, /* Questions: 1 */
        0x00, 0x03, /* Answer RRs: 3 */
        0x00, 0x00, /* Authority RRs: 0 */
        0x00, 0x00, /* Additional RRs: 0 */
        /* Queries */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x1c,             /* Type: AAAA */
        0x00, 0x01,             /* Class: IN */
        /* Answers */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x1c,             /* Type: AAAA */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x10,             /* Data length: 4 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
        0x04, /* Address: ::1.2.3.4 */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x1c,             /* Type: AAAA */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x10,             /* Data length: 4 */
        0x20, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x47,
        0xc1, /* Address: 2001::47c1 */
        0x05, 0x68, 0x65, 0x6c, 0x6c, 0x6f, 0x07, 0x65, 0x78, 0x61, 0x6d, 0x70, 0x6c, 0x65, 0x03,
        0x63, 0x6f, 0x6d, 0x00, /* Name: hello.example.com */
        0x00, 0x1c,             /* Type: AAAA */
        0x00, 0x01,             /* Class: IN */
        0x00, 0x00, 0x00, 0x00, /* Time to live: 0 */
        0x00, 0x10,             /* Data length: 4 */
        0x24, 0x04, 0x68, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x51, 0x75, 0x15,
        0xCA /* Address: 2404:6800::5175:15ca */
};

// Illegal hostnames
static constexpr char kBadCharAfterPeriodHost[] = "hello.example.^com.";
static constexpr char kBadCharBeforePeriodHost[] = "hello.example^.com.";
static constexpr char kBadCharAtTheEndHost[] = "hello.example.com^.";
static constexpr char kBadCharInTheMiddleOfLabelHost[] = "hello.ex^ample.com.";

static const test::DNSHeader kDefaultDnsHeader = {
        // Don't need to initialize the flag "id" and "rd" because DNS responder assigns them from
        // query to response. See RFC 1035 section 4.1.1.
        .id = 0,                // unused. should be assigned from query to response
        .ra = false,            // recursive query support is not available
        .rcode = ns_r_noerror,  // no error
        .qr = true,             // message is a response
        .opcode = QUERY,        // a standard query
        .aa = false,            // answer/authority portion was not authenticated by the server
        .tr = false,            // message is not truncated
        .rd = false,            // unused. should be assigned from query to response
        .ad = false,            // non-authenticated data is unacceptable
};

// The CNAME chain records for building a response message which exceeds 512 bytes.
//
// Ignoring the other fields of the message, the response message has 8 CNAMEs in 5 answer RRs
// and each CNAME has 77 bytes as the follows. The response message at least has 616 bytes in
// answer section and has already exceeded 512 bytes totally.
//
// The CNAME is presented as:
//   0   1            64  65                          72  73          76  77
//   +---+--........--+---+---+---+---+---+---+---+---+---+---+---+---+---+
//   | 63| {x, .., x} | 7 | e | x | a | m | p | l | e | 3 | c | o | m | 0 |
//   +---+--........--+---+---+---+---+---+---+---+---+---+---+---+---+---+
//          ^-- x = {a, b, c, d}
//
const std::string kCnameA = std::string(kMaxmiumLabelSize, 'a') + kExampleComDomain + ".";
const std::string kCnameB = std::string(kMaxmiumLabelSize, 'b') + kExampleComDomain + ".";
const std::string kCnameC = std::string(kMaxmiumLabelSize, 'c') + kExampleComDomain + ".";
const std::string kCnameD = std::string(kMaxmiumLabelSize, 'd') + kExampleComDomain + ".";
const std::vector<DnsRecord> kLargeCnameChainRecords = {
        {kHelloExampleCom, ns_type::ns_t_cname, kCnameA},
        {kCnameA, ns_type::ns_t_cname, kCnameB},
        {kCnameB, ns_type::ns_t_cname, kCnameC},
        {kCnameC, ns_type::ns_t_cname, kCnameD},
        {kCnameD, ns_type::ns_t_a, kHelloExampleComAddrV4},
};

// TODO: Integrate GetNumQueries relevent functions
size_t GetNumQueries(const test::DNSResponder& dns, const char* name);
size_t GetNumQueriesForProtocol(const test::DNSResponder& dns, const int protocol,
                                const char* name);
size_t GetNumQueriesForType(const test::DNSResponder& dns, ns_type type, const char* name);
std::string ToString(const hostent* he);
std::string ToString(const addrinfo* ai);
std::string ToString(const android::netdutils::ScopedAddrinfo& ai);
std::string ToString(const sockaddr_storage* addr);
std::vector<std::string> ToStrings(const hostent* he);
std::vector<std::string> ToStrings(const addrinfo* ai);
std::vector<std::string> ToStrings(const android::netdutils::ScopedAddrinfo& ai);

// Wait for |condition| to be met until |timeout|.
bool PollForCondition(const std::function<bool()>& condition,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));

android::netdutils::ScopedAddrinfo safe_getaddrinfo(const char* node, const char* service,
                                                    const struct addrinfo* hints);

void SetMdnsRoute();
void RemoveMdnsRoute();
void AllowNetworkInBackground(int uid, bool allow);

// Local definition to avoid including resolv_cache.h.
int resolv_set_nameservers(const aidl::android::net::ResolverParamsParcel& params);

// For testing only. Production code passes the parcel down directly.
inline int resolv_set_nameservers(
        unsigned netid, const std::vector<std::string>& servers,
        const std::vector<std::string>& domains, const res_params& res_params,
        std::optional<aidl::android::net::ResolverOptionsParcel> resolverOptions,
        const std::vector<int32_t>& transportTypes = {}, bool metered = false) {
    aidl::android::net::ResolverParamsParcel params;
    params.netId = netid;
    params.servers = servers;
    params.domains = domains;
    params.resolverOptions = resolverOptions;
    params.transportTypes = transportTypes;
    params.meteredNetwork = metered;

    params.sampleValiditySeconds = res_params.sample_validity;
    params.successThreshold = res_params.success_threshold;
    params.minSamples = res_params.min_samples;
    params.maxSamples = res_params.max_samples;
    params.baseTimeoutMsec = res_params.base_timeout_msec;
    params.retryCount = res_params.retry_count;

    return resolv_set_nameservers(params);
}

#define SKIP_IF_BEFORE_T                                                         \
    do {                                                                         \
        if (!isAtLeastT()) {                                                     \
            GTEST_SKIP() << "Skipping test because SDK version is less than T."; \
        }                                                                        \
    } while (0)

bool is64bitAbi();

static const std::string DNS_HELPER =
        is64bitAbi() ? "/apex/com.android.tethering/lib64/libcom.android.tethering.dns_helper.so"
                     : "/apex/com.android.tethering/lib/libcom.android.tethering.dns_helper.so";

#define SKIP_IF_DEPENDENT_LIB_DOES_NOT_EXIST(libPath)                  \
    do {                                                               \
        if (!std::filesystem::exists(libPath))                         \
            GTEST_SKIP() << "Required " << (libPath) << " not found."; \
    } while (0)
