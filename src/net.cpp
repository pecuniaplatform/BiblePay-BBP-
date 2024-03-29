// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Copyright (c) 2014-2017 The D�sh Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/biblepay-config.h"
#endif


#include "net.h"

#include "addrman.h"
#include "chainparams.h"
#include "clientversion.h"
#include "consensus/consensus.h"
#include "crypto/common.h"
#include "hash.h"
#include "primitives/transaction.h"
#include "scheduler.h"
#include "ui_interface.h"
#include "wallet/wallet.h"
#include "utilstrencodings.h"
#include "podc.h"
#include "darksend.h"
#include "instantx.h"
#include "masternode-sync.h"
#include "masternodeman.h"

// For Internet SSL (for the pool communication)
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
// End of Internet SSL


#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_UPNP
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/miniwget.h>
#include <miniupnpc/upnpcommands.h>
#include <miniupnpc/upnperrors.h>
#endif

#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

#include <math.h>

#include <fstream>

using std::ofstream;

// Dump addresses to peers.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// Fix for ancient MinGW versions, that don't have defined these in ws2tcpip.h.
// To do: Can be removed when our pull-tester is upgraded to a modern MinGW version.
#ifdef WIN32
#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
#ifndef IPV6_PROTECTION_LEVEL
#define IPV6_PROTECTION_LEVEL 23
#endif
#endif

bool AddSeedNode(std::string sNode);
void HealthCheckup();

extern std::string BiblepayHttpPost(bool bPost, int iThreadID, std::string sActionName, std::string sDistinctUser, std::string sPayload, std::string sBaseURL, 
	std::string sPage, int iPort, std::string sSolution, int iOptBreak);
extern std::string BiblepayHTTPSPost(bool bPost, int iThreadID, std::string sActionName, std::string sDistinctUser, std::string sPayload, std::string sBaseURL, std::string sPage, int iPort,
	std::string sSolution, int iTimeoutSecs, int iMaxSize, int iBreakOnError = 0);
extern std::string BiblepayIPFSPost(std::string sFN, std::string sPayload);
std::string GetIPFromAddress(std::string sAddress);

extern std::string SQL(std::string sCommand, std::string sAddress, std::string sArguments, std::string& sError);
extern std::string PrepareHTTPPost(bool bPost, std::string sPage, std::string sHostHeader, const string& sMsg, const map<string,string>& mapRequestHeaders);
extern std::string GetDomainFromURL(std::string sURL);
extern bool DownloadDistributedComputingFile(int iNextSuperblock, std::string& sError);
bool FilterFile(int iBufferSize, int iNextSuperblock, std::string& sError);
std::string GetSporkValue(std::string sKey);
int ipfs_socket_connect(string ip_address, int port);
int ipfs_http_get(const string& request, const string& ip_address, int port, const string& fname, double dTimeoutSecs);
extern int ipfs_download(const string& url, const string& filename, double dTimeoutSecs, double dRangeRequestMin, double dRangeRequestMax);
int64_t GetFileSize(std::string sPath);

using namespace std;

namespace {
    const int MAX_OUTBOUND_CONNECTIONS = 8;
    const int MAX_OUTBOUND_MASTERNODE_CONNECTIONS = 20;

    struct ListenSocket {
        SOCKET socket;
        bool whitelisted;

        ListenSocket(SOCKET socket, bool whitelisted) : socket(socket), whitelisted(whitelisted) {}
    };
}

//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
uint64_t nLocalServices = NODE_NETWORK;
CCriticalSection cs_mapLocalHost;
map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
uint64_t nLocalHostNonce = 0;
static std::vector<ListenSocket> vhListenSocket;
CAddrMan addrman;
int nMaxConnections = DEFAULT_MAX_PEER_CONNECTIONS;
bool fAddressesInitialized = false;
std::string strSubVersion;

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
limitedmap<uint256, int64_t> mapAlreadyAskedFor(MAX_INV_SZ);

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

vector<std::string> vAddedNodes;
CCriticalSection cs_vAddedNodes;

NodeId nLastNodeId = 0;
CCriticalSection cs_nLastNodeId;

static CSemaphore *semOutbound = NULL;
static CSemaphore *semMasternodeOutbound = NULL;
boost::condition_variable messageHandlerCondition;

// Signals for message handling
static CNodeSignals g_signals;
CNodeSignals& GetNodeSignals() { return g_signals; }

void AddOneShot(const std::string& strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
    return (unsigned short)(GetArg("-port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (!fListen)
        return false;

    int nBestScore = -1;
    int nBestReachability = -1;
    {
        LOCK(cs_mapLocalHost);
        for (map<CNetAddr, LocalServiceInfo>::iterator it = mapLocalHost.begin(); it != mapLocalHost.end(); it++)
        {
            int nScore = (*it).second.nScore;
            int nReachability = (*it).first.GetReachabilityFrom(paddrPeer);
            if (nReachability > nBestReachability || (nReachability == nBestReachability && nScore > nBestScore))
            {
                addr = CService((*it).first, (*it).second.nPort);
                nBestReachability = nReachability;
                nBestScore = nScore;
            }
        }
    }
    return nBestScore >= 0;
}

//! Convert the pnSeeds6 array into usable address objects.
static std::vector<CAddress> convertSeed6(const std::vector<SeedSpec6> &vSeedsIn)
{
    // It'll only connect to one or two seed nodes because once it connects,
    // it'll get a pile of addresses with newer timestamps.
    // Seed nodes are given a random 'last seen time' of between one and two
    // weeks ago.
    const int64_t nOneWeek = 7*24*60*60;
    std::vector<CAddress> vSeedsOut;
    vSeedsOut.reserve(vSeedsIn.size());
    for (std::vector<SeedSpec6>::const_iterator i(vSeedsIn.begin()); i != vSeedsIn.end(); ++i)
    {
        struct in6_addr ip;
        memcpy(&ip, i->addr, sizeof(ip));
        CAddress addr(CService(ip, i->port));
        addr.nTime = GetTime() - GetRand(nOneWeek) - nOneWeek;
        vSeedsOut.push_back(addr);
    }
    return vSeedsOut;
}

// get best local address for a particular peer as a CAddress
// Otherwise, return the unroutable 0.0.0.0 but filled in with
// the normal parameters, since the IP may be changed to a useful
// one by discovery.
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",GetListenPort()),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
    }
    ret.nServices = nLocalServices;
    ret.nTime = GetAdjustedTime();
    return ret;
}

int GetnScore(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == LOCAL_NONE)
        return 0;
    return mapLocalHost[addr].nScore;
}

// Is our peer's addrLocal potentially useful as an external IP source?
bool IsPeerAddrLocalGood(CNode *pnode)
{
    return fDiscover && pnode->addr.IsRoutable() && pnode->addrLocal.IsRoutable() &&
           !IsLimited(pnode->addrLocal.GetNetwork());
}

// pushes our own address to a peer
void AdvertiseLocal(CNode *pnode)
{
    if (fListen && pnode->fSuccessfullyConnected)
    {
        CAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
             GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8:2) == 0))
        {
            addrLocal.SetIP(pnode->addrLocal);
        }
        if (addrLocal.IsRoutable())
        {
            if (fDebugMaster) LogPrint("net","AdvertiseLocal: advertising address %s\n", addrLocal.ToString());
            pnode->PushAddress(addrLocal);
        }
    }
}

// learn a new local address
bool AddLocal(const CService& addr, int nScore)
{
    if (!addr.IsRoutable())
        return false;

    if (!fDiscover && nScore < LOCAL_MANUAL)
        return false;

    if (IsLimited(addr))
        return false;

    LogPrint("net","AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
    }

    return true;
}

bool AddLocal(const CNetAddr &addr, int nScore)
{
    return AddLocal(CService(addr, GetListenPort()), nScore);
}

bool RemoveLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    LogPrintf("RemoveLocal(%s)\n", addr.ToString());
    mapLocalHost.erase(addr);
    return true;
}

/** Make a particular network entirely off-limits (no automatic connects to it) */
void SetLimited(enum Network net, bool fLimited)
{
    if (net == NET_UNROUTABLE)
        return;
    LOCK(cs_mapLocalHost);
    vfLimited[net] = fLimited;
}

bool IsLimited(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return vfLimited[net];
}

bool IsLimited(const CNetAddr &addr)
{
    return IsLimited(addr.GetNetwork());
}

/** vote for a local address */
bool SeenLocal(const CService& addr)
{
    {
        LOCK(cs_mapLocalHost);
        if (mapLocalHost.count(addr) == 0)
            return false;
        mapLocalHost[addr].nScore++;
    }
    return true;
}


/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/** check whether a given network is one we can probably connect to */
bool IsReachable(enum Network net)
{
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
}

/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    enum Network net = addr.GetNetwork();
    return IsReachable(net);
}

void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}


uint64_t CNode::nTotalBytesRecv = 0;
uint64_t CNode::nTotalBytesSent = 0;
CCriticalSection CNode::cs_totalBytesRecv;
CCriticalSection CNode::cs_totalBytesSent;

uint64_t CNode::nMaxOutboundLimit = 0;
uint64_t CNode::nMaxOutboundTotalBytesSentInCycle = 0;
uint64_t CNode::nMaxOutboundTimeframe = 60*60*24; //1 day
uint64_t CNode::nMaxOutboundCycleStartTime = 0;

std::vector<unsigned char> CNode::vchSecretKey;

CNode* FindNode(const CNetAddr& ip)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if ((CNetAddr)pnode->addr == ip)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CSubNet& subNet)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    if (subNet.Match((CNetAddr)pnode->addr))
        return (pnode);
    return NULL;
}

CNode* FindNode(const std::string& addrName)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        if(Params().NetworkIDString() == CBaseChainParams::REGTEST){
            //if using regtest, just check the IP
            if((CNetAddr)pnode->addr == (CNetAddr)addr)
                return (pnode);
        } else {
            if((CService)pnode->addr == addr)
                return (pnode);
        }
    }
    return NULL;
}

CNode* ConnectNode(CAddress addrConnect, const char *pszDest, bool fConnectToMasternode)
{
    if (pszDest == NULL) {
        // we clean masternode connections in CMasternodeMan::ProcessMasternodeConnections()
        // so should be safe to skip this and connect to local Hot MN on CActiveMasternode::ManageState()
        if (IsLocal(addrConnect) && !fConnectToMasternode)
            return NULL;

        LOCK(cs_vNodes);
        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {
            // we have existing connection to this node but it was not a connection to masternode,
            // change flag and add reference so that we can correctly clear it later
            if(fConnectToMasternode && !pnode->fMasternode) {
                pnode->AddRef();
                pnode->fMasternode = true;
            }
            return pnode;
        }
    }

    /// debug print
    LogPrint("net", "trying connection %s lastseen=%.1fhrs\n",
        pszDest ? pszDest : addrConnect.ToString(),
        pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;
    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
                  ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed))
    {
        if (!IsSelectableSocket(hSocket)) {
            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            CloseSocket(hSocket);
            return NULL;
        }

        addrman.Attempt(addrConnect);

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false, true);

        pnode->nTimeConnected = GetTime();
        if(fConnectToMasternode) {
            pnode->AddRef();
            pnode->fMasternode = true;
        }

        LOCK(cs_vNodes);
        vNodes.push_back(pnode);

        return pnode;
    } else if (!proxyConnectionFailed) {
        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return NULL;
}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET)
    {
        LogPrint("net", "disconnecting peer=%d\n", id);
        CloseSocket(hSocket);
    }

    // in case this fails, we'll empty the recv buffer when the CNode is deleted
    TRY_LOCK(cs_vRecvMsg, lockRecv);
    if (lockRecv)
        vRecvMsg.clear();
}

void CNode::PushVersion()
{
    int nBestHeight = g_signals.GetHeight().get_value_or(0);

    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);
    GetRandBytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    if (fLogIPs)
        LogPrint("net", "send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), addrYou.ToString(), id);
    else
        LogPrint("net", "send version message: version %d, blocks=%d, us=%s, peer=%d\n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), id);
    PushMessage(NetMsgType::VERSION, PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, strSubVersion, nBestHeight, !GetBoolArg("-blocksonly", DEFAULT_BLOCKSONLY));
}





banmap_t CNode::setBanned;
CCriticalSection CNode::cs_setBanned;
bool CNode::setBannedIsDirty;

void CNode::ClearBanned()
{
    LOCK(cs_setBanned);
    setBanned.clear();
    setBannedIsDirty = true;
}

bool CNode::IsBanned(CNetAddr ip)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        for (banmap_t::iterator it = setBanned.begin(); it != setBanned.end(); it++)
        {
            CSubNet subNet = (*it).first;
            CBanEntry banEntry = (*it).second;

            if(subNet.Match(ip) && GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

bool CNode::IsBanned(CSubNet subnet)
{
    bool fResult = false;
    {
        LOCK(cs_setBanned);
        banmap_t::iterator i = setBanned.find(subnet);
        if (i != setBanned.end())
        {
            CBanEntry banEntry = (*i).second;
            if (GetTime() < banEntry.nBanUntil)
                fResult = true;
        }
    }
    return fResult;
}

void CNode::Ban(const CNetAddr& addr, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CSubNet subNet(addr);
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CNode::Ban(const CSubNet& subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0)
    {
        bantimeoffset = GetArg("-bantime", DEFAULT_MISBEHAVING_BANTIME);
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;

    LOCK(cs_setBanned);
    if (setBanned[subNet].nBanUntil < banEntry.nBanUntil)
        setBanned[subNet] = banEntry;

    setBannedIsDirty = true;
}

bool CNode::Unban(const CNetAddr &addr) {
    CSubNet subNet(addr);
    return Unban(subNet);
}

bool CNode::Unban(const CSubNet &subNet) {
    LOCK(cs_setBanned);
    if (setBanned.erase(subNet))
    {
        setBannedIsDirty = true;
        return true;
    }
    return false;
}

void CNode::GetBanned(banmap_t &banMap)
{
    LOCK(cs_setBanned);
    banMap = setBanned; //create a thread safe copy
}

void CNode::SetBanned(const banmap_t &banMap)
{
    LOCK(cs_setBanned);
    setBanned = banMap;
    setBannedIsDirty = true;
}

void CNode::SweepBanned()
{
    int64_t now = GetTime();

    LOCK(cs_setBanned);
    banmap_t::iterator it = setBanned.begin();
    while(it != setBanned.end())
    {
        CBanEntry banEntry = (*it).second;
        if(now > banEntry.nBanUntil)
        {
            setBanned.erase(it++);
            setBannedIsDirty = true;
        }
        else
            ++it;
    }
}

bool CNode::BannedSetIsDirty()
{
    LOCK(cs_setBanned);
    return setBannedIsDirty;
}

void CNode::SetBannedSetDirty(bool dirty)
{
    LOCK(cs_setBanned); //reuse setBanned lock for the isDirty flag
    setBannedIsDirty = dirty;
}


std::vector<CSubNet> CNode::vWhitelistedRange;
CCriticalSection CNode::cs_vWhitelistedRange;

bool CNode::IsWhitelistedRange(const CNetAddr &addr) {
    LOCK(cs_vWhitelistedRange);
    BOOST_FOREACH(const CSubNet& subnet, vWhitelistedRange) {
        if (subnet.Match(addr))
            return true;
    }
    return false;
}

void CNode::AddWhitelistedRange(const CSubNet &subnet) {
    LOCK(cs_vWhitelistedRange);
    vWhitelistedRange.push_back(subnet);
}

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(fRelayTxes);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    X(addrName);
    X(nVersion);
    X(cleanSubVer);
    X(fInbound);
    X(nStartingHeight);
    X(nSendBytes);
    X(nRecvBytes);
    X(fWhitelisted);

    // It is common for nodes with good ping times to suddenly become lagged,
    // due to a new block arriving or other large transfer.
    // Merely reporting pingtime might fool the caller into thinking the node was still responsive,
    // since pingtime does not update until the ping is complete, which might take a while.
    // So, if a ping is taking an unusually long time in flight,
    // the caller can immediately detect that this is happening.
    int64_t nPingUsecWait = 0;
    if ((0 != nPingNonceSent) && (0 != nPingUsecStart)) {
        nPingUsecWait = GetTimeMicros() - nPingUsecStart;
    }

    // Raw ping time is in microseconds, but show it to user as whole seconds (Biblepay users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
    stats.dPingMin  = (((double)nMinPingUsecTime) / 1e6);
    stats.dPingWait = (((double)nPingUsecWait) / 1e6);

    // Leave string empty if addrLocal invalid (not filled in yet)
    stats.addrLocal = addrLocal.IsValid() ? addrLocal.ToString() : "";
}
#undef X

// requires LOCK(cs_vRecvMsg)
bool CNode::ReceiveMsgBytes(const char *pch, unsigned int nBytes)
{
    while (nBytes > 0) {

        // get current incomplete message, or create a new one
        if (vRecvMsg.empty() ||
            vRecvMsg.back().complete())
            vRecvMsg.push_back(CNetMessage(Params().MessageStart(), SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
                return false;

        if (msg.in_data && msg.hdr.nMessageSize > MAX_PROTOCOL_MESSAGE_LENGTH) {
            LogPrint("net", "Oversized message from peer=%i, disconnecting\n", GetId());
            return false;
        }

        pch += handled;
        nBytes -= handled;

        if (msg.complete()) {
            msg.nTime = GetTimeMicros();
            messageHandlerCondition.notify_one();
        }
    }

    return true;
}

int CNetMessage::readHeader(const char *pch, unsigned int nBytes)
{
    // copy data to temporary parsing buffer
    unsigned int nRemaining = 24 - nHdrPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    memcpy(&hdrbuf[nHdrPos], pch, nCopy);
    nHdrPos += nCopy;

    // if header incomplete, exit
    if (nHdrPos < 24)
        return nCopy;

    // deserialize to CMessageHeader
    try {
        hdrbuf >> hdr;
    }
    catch (const std::exception&) {
        return -1;
    }

    // reject messages larger than MAX_SIZE
    if (hdr.nMessageSize > MAX_SIZE)
            return -1;

    // switch state to reading message data
    in_data = true;

    return nCopy;
}

int CNetMessage::readData(const char *pch, unsigned int nBytes)
{
    unsigned int nRemaining = hdr.nMessageSize - nDataPos;
    unsigned int nCopy = std::min(nRemaining, nBytes);

    if (vRecv.size() < nDataPos + nCopy) {
        // Allocate up to 256 KiB ahead, but never more than the total message size.
        vRecv.resize(std::min(hdr.nMessageSize, nDataPos + nCopy + 256 * 1024));
    }

    memcpy(&vRecv[nDataPos], pch, nCopy);
    nDataPos += nCopy;

    return nCopy;
}









// requires LOCK(cs_vSend)
void SocketSendData(CNode *pnode)
{
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

    while (it != pnode->vSendMsg.end()) {
        const CSerializeData &data = *it;
        assert(data.size() > pnode->nSendOffset);
        int nBytes = send(pnode->hSocket, &data[pnode->nSendOffset], data.size() - pnode->nSendOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (nBytes > 0) {
            pnode->nLastSend = GetTime();
            pnode->nSendBytes += nBytes;
            pnode->nSendOffset += nBytes;
            pnode->RecordBytesSent(nBytes);
            if (pnode->nSendOffset == data.size()) {
                pnode->nSendOffset = 0;
                pnode->nSendSize -= data.size();
                it++;
            } else {
                // could not send full message; stop sending more
                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    if (fDebugMaster) LogPrintf("socket send error %s\n", NetworkErrorString(nErr));
                    pnode->fDisconnect = true;
                }
            }
            // couldn't send anything at all
            break;
        }
    }

    if (it == pnode->vSendMsg.end()) {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

static list<CNode*> vNodesDisconnected;

struct NodeEvictionCandidate
{
    NodeEvictionCandidate(CNode* pnode)
        : id(pnode->id),
          nTimeConnected(pnode->nTimeConnected),
          nMinPingUsecTime(pnode->nMinPingUsecTime),
          vchNetGroup(pnode->addr.GetGroup()),
          vchKeyedNetGroup(pnode->vchKeyedNetGroup)
        {}

    int id;
    int64_t nTimeConnected;
    int64_t nMinPingUsecTime;
    std::vector<unsigned char> vchNetGroup;
    std::vector<unsigned char> vchKeyedNetGroup;
};

static bool ReverseCompareNodeMinPingTime(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.nMinPingUsecTime > b.nMinPingUsecTime;
}

static bool ReverseCompareNodeTimeConnected(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.nTimeConnected > b.nTimeConnected;
}

static bool CompareKeyedNetGroup(const NodeEvictionCandidate& a, const NodeEvictionCandidate& b)
{
    return a.vchKeyedNetGroup < b.vchKeyedNetGroup;
}

static bool AttemptToEvictConnection(bool fPreferNewConnection) {
    std::vector<NodeEvictionCandidate> vEvictionCandidates;
    {
        LOCK(cs_vNodes);

        for(size_t i = 0; i < vNodes.size(); ++i) {
            CNode* pnode = vNodes[i];
            if (pnode->fWhitelisted)
                continue;
            if (!pnode->fInbound)
                continue;
            if (pnode->fDisconnect)
                continue;
            vEvictionCandidates.push_back(NodeEvictionCandidate(pnode));
        }
    }

    if (vEvictionCandidates.empty()) return false;

    // Protect connections with certain characteristics

    // Deterministically select 4 peers to protect by netgroup.
    // An attacker cannot predict which netgroups will be protected.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), CompareKeyedNetGroup);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(4, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the 8 nodes with the best ping times.
    // An attacker cannot manipulate this metric without physically moving nodes closer to the target.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeMinPingTime);
    vEvictionCandidates.erase(vEvictionCandidates.end() - std::min(8, static_cast<int>(vEvictionCandidates.size())), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Protect the half of the remaining nodes which have been connected the longest.
    // This replicates the existing implicit behavior.
    std::sort(vEvictionCandidates.begin(), vEvictionCandidates.end(), ReverseCompareNodeTimeConnected);
    vEvictionCandidates.erase(vEvictionCandidates.end() - static_cast<int>(vEvictionCandidates.size() / 2), vEvictionCandidates.end());

    if (vEvictionCandidates.empty()) return false;

    // Identify the network group with the most connections and youngest member.
    // (vEvictionCandidates is already sorted by reverse connect time)
    std::vector<unsigned char> naMostConnections;
    unsigned int nMostConnections = 0;
    int64_t nMostConnectionsTime = 0;
    std::map<std::vector<unsigned char>, std::vector<NodeEvictionCandidate> > mapAddrCounts;
    for(size_t i = 0; i < vEvictionCandidates.size(); ++i) {
        const NodeEvictionCandidate& candidate = vEvictionCandidates[i];
        mapAddrCounts[candidate.vchNetGroup].push_back(candidate);
        int64_t grouptime = mapAddrCounts[candidate.vchNetGroup][0].nTimeConnected;
        size_t groupsize = mapAddrCounts[candidate.vchNetGroup].size();

        if (groupsize > nMostConnections || (groupsize == nMostConnections && grouptime > nMostConnectionsTime)) {
            nMostConnections = groupsize;
            nMostConnectionsTime = grouptime;
            naMostConnections = candidate.vchNetGroup;
        }
    }

    // Reduce to the network group with the most connections
    std::vector<NodeEvictionCandidate> vEvictionNodes = mapAddrCounts[naMostConnections];

    if(vEvictionNodes.empty()) {
        return false;
    }

    // Do not disconnect peers if there is only one unprotected connection from their network group.
    if (vEvictionNodes.size() <= 1)
        // unless we prefer the new connection (for whitelisted peers)
        if (!fPreferNewConnection)
            return false;

    // Disconnect from the network group with the most connections
    int nEvictionId = vEvictionNodes[0].id;
    {
        LOCK(cs_vNodes);
        for(size_t i = 0; i < vNodes.size(); ++i) {
            CNode* pnode = vNodes[i];
            if(pnode->id == nEvictionId) {
                pnode->fDisconnect = true;
                return true;
            }
        }
    }

    return false;
}

static void AcceptConnection(const ListenSocket& hListenSocket) {
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    SOCKET hSocket = accept(hListenSocket.socket, (struct sockaddr*)&sockaddr, &len);
    CAddress addr;
    int nInbound = 0;
    int nMaxInbound = nMaxConnections - MAX_OUTBOUND_CONNECTIONS;

    if (hSocket != INVALID_SOCKET)
        if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
            LogPrintf("Warning: Unknown socket family\n");

    bool whitelisted = hListenSocket.whitelisted || CNode::IsWhitelistedRange(addr);
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->fInbound)
                nInbound++;
    }

    if (hSocket == INVALID_SOCKET)
    {
        int nErr = WSAGetLastError();
        if (nErr != WSAEWOULDBLOCK)
            LogPrint("net","socket error accept failed: %s\n", NetworkErrorString(nErr));
        return;
    }

    if (!IsSelectableSocket(hSocket))
    {
        LogPrint("net","connection from %s dropped: non-selectable socket\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    // According to the internet TCP_NODELAY is not carried into accepted sockets
    // on all platforms.  Set it again here just to be sure.
    int set = 1;
#ifdef WIN32
    setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&set, sizeof(int));
#else
    setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (void*)&set, sizeof(int));
#endif

    if (CNode::IsBanned(addr) && !whitelisted)
    {
        LogPrint("net","connection from %s dropped (banned)\n", addr.ToString());
        CloseSocket(hSocket);
        return;
    }

    if (nInbound >= nMaxInbound)
    {
        if (!AttemptToEvictConnection(whitelisted)) {
            // No connection to evict, disconnect the new connection
            LogPrint("net", "failed to find an eviction candidate - connection dropped (full)\n");
            CloseSocket(hSocket);
            return;
        }
    }

    // don't accept incoming connections until fully synced
    if(fMasterNode && !masternodeSync.IsSynced() && !whitelisted && fProd) 
	{
        if (fDebugMaster) LogPrintf("AcceptConnection -- masternode is not synced yet, skipping inbound connection attempt\n");
        CloseSocket(hSocket);
        return;
    }

    CNode* pnode = new CNode(hSocket, addr, "", true);
    pnode->fWhitelisted = whitelisted;

    LogPrint("net", "connection from %s accepted\n", addr.ToString());
    {
        LOCK(cs_vNodes);
        vNodes.push_back(pnode);
    }
}

void ThreadSocketHandler()
{
    unsigned int nPrevNodeCount = 0;
    while (true)
    {
        //
        // Disconnect nodes
        //
        {
            LOCK(cs_vNodes);
            // Disconnect unused nodes
            vector<CNode*> vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                if (pnode->fDisconnect ||
                    (pnode->GetRefCount() <= 0 && pnode->vRecvMsg.empty() && pnode->nSendSize == 0 && pnode->ssSend.empty()))
                {
                    LogPrint("net","ThreadSocketHandler -- removing node: peer=%d addr=%s nRefCount=%d fNetworkNode=%d fInbound=%d fMasternode=%d\n",
                              pnode->id, pnode->addr.ToString(), pnode->GetRefCount(), pnode->fNetworkNode, pnode->fInbound, pnode->fMasternode);

                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();
                    pnode->grantMasternodeOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    if (pnode->fMasternode)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }
            }
        }
        {
            // Delete disconnected nodes
            list<CNode*> vNodesDisconnectedCopy = vNodesDisconnected;
            BOOST_FOREACH(CNode* pnode, vNodesDisconnectedCopy)
            {
                // wait until threads are done using it
                if (pnode->GetRefCount() <= 0)
                {
                    bool fDelete = false;
                    {
                        TRY_LOCK(pnode->cs_vSend, lockSend);
                        if (lockSend)
                        {
                            TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                            if (lockRecv)
                            {
                                TRY_LOCK(pnode->cs_inventory, lockInv);
                                if (lockInv)
                                    fDelete = true;
                            }
                        }
                    }
                    if (fDelete)
                    {
                        vNodesDisconnected.remove(pnode);
                        delete pnode;
                    }
                }
            }
        }
        if(vNodes.size() != nPrevNodeCount) {
            nPrevNodeCount = vNodes.size();
            uiInterface.NotifyNumConnectionsChanged(nPrevNodeCount);
        }

        //
        // Find which sockets have data to receive
        //
        struct timeval timeout;
        timeout.tv_sec  = 0;
        timeout.tv_usec = 50000; // frequency to poll pnode->vSend

        fd_set fdsetRecv;
        fd_set fdsetSend;
        fd_set fdsetError;
        FD_ZERO(&fdsetRecv);
        FD_ZERO(&fdsetSend);
        FD_ZERO(&fdsetError);
        SOCKET hSocketMax = 0;
        bool have_fds = false;

        BOOST_FOREACH(const ListenSocket& hListenSocket, vhListenSocket) {
            FD_SET(hListenSocket.socket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket.socket);
            have_fds = true;
        }

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                if (pnode->hSocket == INVALID_SOCKET)
                    continue;
                FD_SET(pnode->hSocket, &fdsetError);
                hSocketMax = max(hSocketMax, pnode->hSocket);
                have_fds = true;

                // Implement the following logic:
                // * If there is data to send, select() for sending data. As this only
                //   happens when optimistic write failed, we choose to first drain the
                //   write buffer in this case before receiving more. This avoids
                //   needlessly queueing received data, if the remote peer is not themselves
                //   receiving data. This means properly utilizing TCP flow control signalling.
                // * Otherwise, if there is no (complete) message in the receive buffer,
                //   or there is space left in the buffer, select() for receiving data.
                // * (if neither of the above applies, there is certainly one message
                //   in the receiver buffer ready to be processed).
                // Together, that means that at least one of the following is always possible,
                // so we don't deadlock:
                // * We send some data.
                // * We wait for data to be received (and disconnect after timeout).
                // * We process a message in the buffer (message handler thread).
                {
                    TRY_LOCK(pnode->cs_vSend, lockSend);
                    if (lockSend && !pnode->vSendMsg.empty()) {
                        FD_SET(pnode->hSocket, &fdsetSend);
                        continue;
                    }
                }
                {
                    TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                    if (lockRecv && (
                        pnode->vRecvMsg.empty() || !pnode->vRecvMsg.front().complete() ||
                        pnode->GetTotalRecvSize() <= ReceiveFloodSize()))
                        FD_SET(pnode->hSocket, &fdsetRecv);
                }
            }
        }

        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        boost::this_thread::interruption_point();

        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                int nErr = WSAGetLastError();
                LogPrintf("socket select error %s\n", NetworkErrorString(nErr));
                for (unsigned int i = 0; i <= hSocketMax; i++)
                    FD_SET(i, &fdsetRecv);
            }
            FD_ZERO(&fdsetSend);
            FD_ZERO(&fdsetError);
            MilliSleep(timeout.tv_usec/1000);
        }

        //
        // Accept new connections
        //
        BOOST_FOREACH(const ListenSocket& hListenSocket, vhListenSocket)
        {
            if (hListenSocket.socket != INVALID_SOCKET && FD_ISSET(hListenSocket.socket, &fdsetRecv))
            {
                AcceptConnection(hListenSocket);
            }
        }

        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy = CopyNodeVector();
        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            boost::this_thread::interruption_point();

            //
            // Receive
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetRecv) || FD_ISSET(pnode->hSocket, &fdsetError))
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    {
                        // typical socket buffer is 8K-64K
                        char pchBuf[0x10000];
                        int nBytes = recv(pnode->hSocket, pchBuf, sizeof(pchBuf), MSG_DONTWAIT);
                        if (nBytes > 0)
                        {
                            if (!pnode->ReceiveMsgBytes(pchBuf, nBytes))
                                pnode->CloseSocketDisconnect();
                            pnode->nLastRecv = GetTime();
                            pnode->nRecvBytes += nBytes;
                            pnode->RecordBytesRecv(nBytes);
                        }
                        else if (nBytes == 0)
                        {
                            // socket closed gracefully
                            if (!pnode->fDisconnect)
                                LogPrint("net", "socket closed\n");
                            pnode->CloseSocketDisconnect();
                        }
                        else if (nBytes < 0)
                        {
                            // error
                            int nErr = WSAGetLastError();
                            if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                            {
                                if (!pnode->fDisconnect)
								{
									LogPrint("net","socket recv error %s\n", NetworkErrorString(nErr));
								}
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }
                }
            }

            //
            // Send
            //
            if (pnode->hSocket == INVALID_SOCKET)
                continue;
            if (FD_ISSET(pnode->hSocket, &fdsetSend))
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    SocketSendData(pnode);
            }

            //
            // Inactivity checking
            //
            int64_t nTime = GetTime();
            if (nTime - pnode->nTimeConnected > 60)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    LogPrint("net", "socket no message in first 60 seconds, %d %d from %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0, pnode->id);
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastSend > TIMEOUT_INTERVAL)
                {
                    LogPrint("net","socket sending timeout: %is\n", nTime - pnode->nLastSend);
                    pnode->fDisconnect = true;
                }
                else if (nTime - pnode->nLastRecv > (pnode->nVersion > BIP0031_VERSION ? TIMEOUT_INTERVAL : 90*60))
                {
                    LogPrint("net","socket receive timeout: %is\n", nTime - pnode->nLastRecv);
                    pnode->fDisconnect = true;
                }
                else if (pnode->nPingNonceSent && pnode->nPingUsecStart + TIMEOUT_INTERVAL * 1000000 < GetTimeMicros())
                {
                    LogPrint("net","ping timeout: %fs\n", 0.000001 * (GetTimeMicros() - pnode->nPingUsecStart));
                    pnode->fDisconnect = true;
                }
            }
        }
        ReleaseNodeVector(vNodesCopy);
    }
}









#ifdef USE_UPNP
void ThreadMapPort()
{
    std::string port = strprintf("%u", GetListenPort());
    const char * multicastif = 0;
    const char * minissdpdpath = 0;
    struct UPNPDev * devlist = 0;
    char lanaddr[64];

#ifndef UPNPDISCOVER_SUCCESS
    /* miniupnpc 1.5 */
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0);
#elif MINIUPNPC_API_VERSION < 14
    /* miniupnpc 1.6 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, &error);
#else
    /* miniupnpc 1.9.20150730 */
    int error = 0;
    devlist = upnpDiscover(2000, multicastif, minissdpdpath, 0, 0, 2, &error);
#endif

    struct UPNPUrls urls;
    struct IGDdatas data;
    int r;

    r = UPNP_GetValidIGD(devlist, &urls, &data, lanaddr, sizeof(lanaddr));
    if (r == 1)
    {
        if (fDiscover) {
            char externalIPAddress[40];
            r = UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, externalIPAddress);
            if(r != UPNPCOMMAND_SUCCESS)
                LogPrintf("UPnP: GetExternalIPAddress() returned %d\n", r);
            else
            {
                if(externalIPAddress[0])
                {
                    LogPrintf("UPnP: ExternalIPAddress = %s\n", externalIPAddress);
                    AddLocal(CNetAddr(externalIPAddress), LOCAL_UPNP);
                }
                else
                    LogPrintf("UPnP: GetExternalIPAddress failed.\n");
            }
        }

        string strDesc = "Biblepay Core " + FormatFullVersion();

        try {
            while (true) {
#ifndef UPNPDISCOVER_SUCCESS
                /* miniupnpc 1.5 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0);
#else
                /* miniupnpc 1.6 */
                r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype,
                                    port.c_str(), port.c_str(), lanaddr, strDesc.c_str(), "TCP", 0, "0");
#endif

                if(r!=UPNPCOMMAND_SUCCESS)
                    LogPrintf("AddPortMapping(%s, %s, %s) failed with code %d (%s)\n",
                        port, port, lanaddr, r, strupnperror(r));
                else
                    LogPrintf("UPnP Port Mapping successful.\n");;

                MilliSleep(20*60*1000); // Refresh every 20 minutes
            }
        }
        catch (const boost::thread_interrupted&)
        {
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() returned: %d\n", r);
            freeUPNPDevlist(devlist); devlist = 0;
            FreeUPNPUrls(&urls);
            throw;
        }
    } else {
        LogPrintf("No valid UPnP IGDs found\n");
        freeUPNPDevlist(devlist); devlist = 0;
        if (r != 0)
            FreeUPNPUrls(&urls);
    }
}

void MapPort(bool fUseUPnP)
{
    static boost::thread* upnp_thread = NULL;

    if (fUseUPnP)
    {
        if (upnp_thread) {
            upnp_thread->interrupt();
            upnp_thread->join();
            delete upnp_thread;
        }
        upnp_thread = new boost::thread(boost::bind(&TraceThread<void (*)()>, "upnp", &ThreadMapPort));
    }
    else if (upnp_thread) {
        upnp_thread->interrupt();
        upnp_thread->join();
        delete upnp_thread;
        upnp_thread = NULL;
    }
}

#else
void MapPort(bool)
{
    // Intentionally left blank.
}
#endif






void ThreadDNSAddressSeed()
{
    // goal: only query DNS seeds if address need is acute
    if ((addrman.size() > 0) &&
        (!GetBoolArg("-forcednsseed", DEFAULT_FORCEDNSSEED))) {
        MilliSleep(11 * 1000);

        LOCK(cs_vNodes);
        if (vNodes.size() >= 2) {
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const vector<CDNSSeedData> &vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    BOOST_FOREACH(const CDNSSeedData &seed, vSeeds) {
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
        } else {
            vector<CNetAddr> vIPs;
            vector<CAddress> vAdd;
            if (LookupHost(seed.host.c_str(), vIPs))
            {
                BOOST_FOREACH(const CNetAddr& ip, vIPs)
                {
                    int nOneDay = 24*3600;
                    CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()));
                    addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                    vAdd.push_back(addr);
                    found++;
                }
            }
            addrman.Add(vAdd, CNetAddr(seed.name, true));
        }
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}





void DumpAddresses()
{
    int64_t nStart = GetTimeMillis();

    CAddrDB adb;
    adb.Write(addrman);

    LogPrint("net", "Flushed %d addresses to peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
}

void DumpData()
{
    DumpAddresses();

    if (CNode::BannedSetIsDirty())
    {
        DumpBanlist();
        CNode::SetBannedSetDirty(false);
    }
	HealthCheckup();
}

void static ProcessOneShot()
{
    string strDest;
    {
        LOCK(cs_vOneShots);
        if (vOneShots.empty())
            return;
        strDest = vOneShots.front();
        vOneShots.pop_front();
    }
    CAddress addr;
    CSemaphoreGrant grant(*semOutbound, true);
    if (grant) {
        if (!OpenNetworkConnection(addr, &grant, strDest.c_str(), true))
            AddOneShot(strDest);
    }
}

void ThreadOpenConnections()
{
    // Connect to specific addresses
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(const std::string& strAddr, mapMultiArgs["-connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    MilliSleep(500);
                }
            }
            MilliSleep(500);
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();
    while (true)
    {
        ProcessOneShot();

        MilliSleep(500);

        CSemaphoreGrant grant(*semOutbound);
        boost::this_thread::interruption_point();

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                addrman.Add(convertSeed6(Params().FixedSeeds()), CNetAddr("127.0.0.1"));
                done = true;
            }
        }

        //
        // Choose an address to connect to based on most recently seen
        //
        CAddress addrConnect;

        // Only connect out to one peer per network group (/16 for IPv4).
        // Do this here so we don't have to critsect vNodes inside mapAddresses critsect.
        int nOutbound = 0;
        set<vector<unsigned char> > setConnected;
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes) {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
            }
        }

        int64_t nANow = GetAdjustedTime();

        int nTries = 0;
        while (true)
        {
            CAddrInfo addr = addrman.Select();

            // if we selected an invalid address, restart
            if (!addr.IsValid() || setConnected.count(addr.GetGroup()) || IsLocal(addr))
                break;

            // If we didn't find an appropriate destination after trying 100 addresses fetched from addrman,
            // stop this loop, and let the outer loop run again (which sleeps, adds seed nodes, recalculates
            // already-connected network ranges, ...) before trying new addrman addresses.
            nTries++;
            if (nTries > 100)
                break;

            if (IsLimited(addr))
                continue;

            // only consider very recently tried nodes after 30 failed attempts
            if (nANow - addr.nLastTry < 600 && nTries < 30)
                continue;

            // do not allow non-default ports, unless after 50 invalid addresses selected already
            if (addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;

            addrConnect = addr;
            break;
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);
    }
}

void ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
		// dd Seed Nodes
		AddSeedNode("node.biblepay.org"); // Volunteers welcome to run external nodes and we will add during future releases
		AddSeedNode("node.biblepay-explorer.org");  // Licht
		AddSeedNode("dns1.biblepay.org");  // Rob
		AddSeedNode("dns2.biblepay.org");  // Rob
		AddSeedNode("dns3.biblepay.org");  // Rob
		AddSeedNode("dns4.biblepay.org");  // Rob
		AddSeedNode("dns5.biblepay.org");  // Rob
		if (!fProd) AddSeedNode("testnet.biblepay.org");
		if (!fProd) AddSeedNode("test.dnsseed.biblepay-explorer.org"); // Alex (TESTNET)
    }

    if (HaveNameProxy()) {
        while(true) {
            list<string> lAddresses(0);
            {
                LOCK(cs_vAddedNodes);
                BOOST_FOREACH(const std::string& strAddNode, vAddedNodes)
                    lAddresses.push_back(strAddNode);
            }
            BOOST_FOREACH(const std::string& strAddNode, lAddresses) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                MilliSleep(500);
            }
            MilliSleep(120000); // Retry every 2 minutes
        }
    }

    for (unsigned int i = 0; true; i++)
    {
        list<string> lAddresses(0);
        {
            LOCK(cs_vAddedNodes);
            BOOST_FOREACH(const std::string& strAddNode, vAddedNodes)
                lAddresses.push_back(strAddNode);
        }

        list<vector<CService> > lservAddressesToAdd(0);
        BOOST_FOREACH(const std::string& strAddNode, lAddresses) {
            vector<CService> vservNode(0);
            if(Lookup(strAddNode.c_str(), vservNode, Params().GetDefaultPort(), fNameLookup, 0))
            {
                lservAddressesToAdd.push_back(vservNode);
                {
                    LOCK(cs_setservAddNodeAddresses);
                    BOOST_FOREACH(const CService& serv, vservNode)
                        setservAddNodeAddresses.insert(serv);
                }
            }
        }
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
                for (list<vector<CService> >::iterator it = lservAddressesToAdd.begin(); it != lservAddressesToAdd.end(); it++)
                    BOOST_FOREACH(const CService& addrNode, *(it))
                        if (pnode->addr == addrNode)
                        {
                            it = lservAddressesToAdd.erase(it);
                            it--;
                            break;
                        }
        }
        BOOST_FOREACH(vector<CService>& vserv, lservAddressesToAdd)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(vserv[i % vserv.size()]), &grant);
            MilliSleep(500);
        }
        MilliSleep(120000); // Retry every 2 minutes
    }
}

void ThreadMnbRequestConnections()
{
    // Connecting to specific addresses, no masternode connections available
    if (mapArgs.count("-connect") && mapMultiArgs["-connect"].size() > 0)
        return;

    while (true)
    {
        MilliSleep(1000);

        CSemaphoreGrant grant(*semMasternodeOutbound);
        boost::this_thread::interruption_point();

        std::pair<CService, std::set<uint256> > p = mnodeman.PopScheduledMnbRequestConnection();
        if(p.first == CService() || p.second.empty()) continue;

        CNode* pnode = NULL;
        {
            LOCK2(cs_main, cs_vNodes);
            pnode = ConnectNode(CAddress(p.first), NULL, true);
            if(!pnode) continue;
            pnode->AddRef();
        }

        grant.MoveTo(pnode->grantMasternodeOutbound);

        // compile request vector
        std::vector<CInv> vToFetch;
        std::set<uint256>::iterator it = p.second.begin();
        while(it != p.second.end()) {
            if(*it != uint256()) {
                vToFetch.push_back(CInv(MSG_MASTERNODE_ANNOUNCE, *it));
                LogPrint("masternode", "ThreadMnbRequestConnections -- asking for mnb %s from addr=%s\n", it->ToString(), p.first.ToString());
            }
            ++it;
        }

        // ask for data
        pnode->PushMessage(NetMsgType::GETDATA, vToFetch);

        pnode->Release();
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *pszDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //
    boost::this_thread::interruption_point();
    if (!pszDest) {
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect) ||
            FindNode(addrConnect.ToStringIPPort()))
            return false;
    } else if (FindNode(std::string(pszDest)))
        return false;

    CNode* pnode = ConnectNode(addrConnect, pszDest);
    boost::this_thread::interruption_point();

    if (!pnode)
        return false;
    if (grantOutbound)
        grantOutbound->MoveTo(pnode->grantOutbound);
    if (fOneShot)
        pnode->fOneShot = true;

    return true;
}


void ThreadMessageHandler()
{
    boost::mutex condition_mutex;
    boost::unique_lock<boost::mutex> lock(condition_mutex);

    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
    while (true)
    {
        vector<CNode*> vNodesCopy = CopyNodeVector();

        bool fSleep = true;

        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (pnode->fDisconnect)
                continue;

            // Receive messages
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    if (!g_signals.ProcessMessages(pnode))
                        pnode->fDisconnect = true;

                    if (pnode->nSendSize < SendBufferSize())
                    {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete()))
                        {
                            fSleep = false;
                        }
                    }
                }
            }
            boost::this_thread::interruption_point();

            // Send messages
            {
                TRY_LOCK(pnode->cs_vSend, lockSend);
                if (lockSend)
                    g_signals.SendMessages(pnode);
            }
            boost::this_thread::interruption_point();
        }

        ReleaseNodeVector(vNodesCopy);

        if (fSleep)
            messageHandlerCondition.timed_wait(lock, boost::posix_time::microsec_clock::universal_time() + boost::posix_time::milliseconds(100));
    }
}






bool BindListenPort(const CService &addrBind, string& strError, bool fWhitelisted)
{
    strError = "";
    int nOne = 1;

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: Bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %s)", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }
    if (!IsSelectableSocket(hListenSocket))
    {
        strError = "Error: Couldn't create a listenable socket for incoming connections";
        LogPrintf("%s\n", strError);
        return false;
    }


#ifndef WIN32
#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
    // Disable Nagle's algorithm
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (void*)&nOne, sizeof(int));
#else
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&nOne, sizeof(int));
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nOne, sizeof(int));
#endif

    // Set to non-blocking, incoming connections will also inherit this
    if (!SetSocketNonBlocking(hListenSocket, true)) {
        strError = strprintf("BindListenPort: Setting listening socket to non-blocking failed, error %s\n", NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        return false;
    }

    // some systems don't have IPV6_V6ONLY but are always v6only; others do have the option
    // and enable it by default or not. Try to enable it, if possible.
    if (addrBind.IsIPv6()) {
#ifdef IPV6_V6ONLY
#ifdef WIN32
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&nOne, sizeof(int));
#else
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_V6ONLY, (void*)&nOne, sizeof(int));
#endif
#endif
#ifdef WIN32
        int nProtLevel = PROTECTION_LEVEL_UNRESTRICTED;
        setsockopt(hListenSocket, IPPROTO_IPV6, IPV6_PROTECTION_LEVEL, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();
        if (nErr == WSAEADDRINUSE)
            strError = strprintf(_("Unable to bind to %s on this computer. Biblepay Core is probably already running."), addrBind.ToString());
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %s)"), addrBind.ToString(), NetworkErrorString(nErr));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf(_("Error: Listening for incoming connections failed (listen returned error %s)"), NetworkErrorString(WSAGetLastError()));
        LogPrintf("%s\n", strError);
        CloseSocket(hListenSocket);
        return false;
    }

    vhListenSocket.push_back(ListenSocket(hListenSocket, fWhitelisted));

    if (addrBind.IsRoutable() && fDiscover && !fWhitelisted)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover(boost::thread_group& threadGroup)
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[256] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: %s - %s\n", __func__, pszHostName, addr.ToString());
            }
        }
    }
#else
    // Get local host ip
    struct ifaddrs* myaddrs;
    if (getifaddrs(&myaddrs) == 0)
    {
        for (struct ifaddrs* ifa = myaddrs; ifa != NULL; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr == NULL) continue;
            if ((ifa->ifa_flags & IFF_UP) == 0) continue;
            if (strcmp(ifa->ifa_name, "lo") == 0) continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0) continue;
            if (ifa->ifa_addr->sa_family == AF_INET)
            {
                struct sockaddr_in* s4 = (struct sockaddr_in*)(ifa->ifa_addr);
                CNetAddr addr(s4->sin_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv4 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("%s: IPv6 %s: %s\n", __func__, ifa->ifa_name, addr.ToString());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif
}

void StartNode(boost::thread_group& threadGroup, CScheduler& scheduler)
{
    uiInterface.InitMessage(_("Loading addresses..."));
    // Load addresses for peers.dat
    int64_t nStart = GetTimeMillis();
    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    //try to read stored banlist
    CBanDB bandb;
    banmap_t banmap;
    if (!bandb.Read(banmap))
        LogPrintf("Invalid or missing banlist.dat; recreating\n");

    CNode::SetBanned(banmap); //thread save setter
    CNode::SetBannedSetDirty(false); //no need to write down just read or nonexistent data
    CNode::SweepBanned(); //sweap out unused entries

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n",
           addrman.size(), GetTimeMillis() - nStart);
    fAddressesInitialized = true;

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, nMaxConnections);
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (semMasternodeOutbound == NULL) {
        // initialize semaphore
        semMasternodeOutbound = new CSemaphore(MAX_OUTBOUND_MASTERNODE_CONNECTIONS);
    }

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover(threadGroup);

    //
    // Start threads
    //

    if (!GetBoolArg("-dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "dnsseed", &ThreadDNSAddressSeed));

    // Map ports with UPnP
    MapPort(GetBoolArg("-upnp", DEFAULT_UPNP));

    // Send and receive from sockets, accept connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "net", &ThreadSocketHandler));

    // Initiate outbound connections from -addnode
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "addcon", &ThreadOpenAddedConnections));

    // Initiate outbound connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "opencon", &ThreadOpenConnections));

    // Initiate masternode connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "mnbcon", &ThreadMnbRequestConnections));

    // Process messages
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "msghand", &ThreadMessageHandler));

    // Dump network addresses
	scheduler.scheduleEvery(&DumpData, 180);
}


bool StopNode()
{
    LogPrintf("StopNode()\n");
    MapPort(false);
    if (semOutbound)
        for (int i=0; i<MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();

    if (semMasternodeOutbound)
        for (int i=0; i<MAX_OUTBOUND_MASTERNODE_CONNECTIONS; i++)
            semMasternodeOutbound->post();

    if (fAddressesInitialized)
    {
        DumpData();
        fAddressesInitialized = false;
    }

    return true;
}

class CNetCleanup
{
public:
    CNetCleanup() {}

    ~CNetCleanup()
    {
        // Close sockets
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                CloseSocket(pnode->hSocket);
        BOOST_FOREACH(ListenSocket& hListenSocket, vhListenSocket)
            if (hListenSocket.socket != INVALID_SOCKET)
                if (!CloseSocket(hListenSocket.socket))
                    LogPrintf("CloseSocket(hListenSocket) failed with error %s\n", NetworkErrorString(WSAGetLastError()));

        // clean up some globals (to help leak detection)
        BOOST_FOREACH(CNode *pnode, vNodes)
            delete pnode;
        BOOST_FOREACH(CNode *pnode, vNodesDisconnected)
            delete pnode;
        vNodes.clear();
        vNodesDisconnected.clear();
        vhListenSocket.clear();
        delete semOutbound;
        semOutbound = NULL;
        delete semMasternodeOutbound;
        semMasternodeOutbound = NULL;
        delete pnodeLocalHost;
        pnodeLocalHost = NULL;

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}
instance_of_cnetcleanup;

void CExplicitNetCleanup::callCleanup()
{
    // Explicit call to destructor of CNetCleanup because it's not implicitly called
    // when the wallet is restarted from within the wallet itself.
    CNetCleanup *tmp = new CNetCleanup();
    delete tmp; // Stroustrup's gonna kill me for that
}

void RelayTransaction(const CTransaction& tx)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    uint256 hash = tx.GetHash();
    CTxLockRequest txLockRequest;
    if(mapDarksendBroadcastTxes.count(hash)) { // MSG_DSTX
        ss << mapDarksendBroadcastTxes[hash];
    } else if(instantsend.GetTxLockRequest(hash, txLockRequest)) { // MSG_TXLOCK_REQUEST
        ss << txLockRequest;
    } else { // MSG_TX
        ss << tx;
    }
    RelayTransaction(tx, ss);
}

void RelayTransaction(const CTransaction& tx, const CDataStream& ss)
{
    uint256 hash = tx.GetHash();
    int nInv = mapDarksendBroadcastTxes.count(hash) ? MSG_DSTX :
                (instantsend.HasTxLockRequest(hash) ? MSG_TXLOCK_REQUEST : MSG_TX);
    CInv inv(nInv, hash);
    {
        LOCK(cs_mapRelay);
        // Expire old relay messages
        while (!vRelayExpiration.empty() && vRelayExpiration.front().first < GetTime())
        {
            mapRelay.erase(vRelayExpiration.front().second);
            vRelayExpiration.pop_front();
        }

        // Save original serialized message so newer versions are preserved
        mapRelay.insert(std::make_pair(inv, ss));
        vRelayExpiration.push_back(std::make_pair(GetTime() + 15 * 60, inv));
    }
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!pnode->fRelayTxes)
            continue;
        LOCK(pnode->cs_filter);
        if (pnode->pfilter)
        {
            if (pnode->pfilter->IsRelevantAndUpdate(tx))
                pnode->PushInventory(inv);
        } else
            pnode->PushInventory(inv);
    }
}

void RelayInv(CInv &inv, const int minProtoVersion) {
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        if(pnode->nVersion >= minProtoVersion)
            pnode->PushInventory(inv);
}

void CNode::RecordBytesRecv(uint64_t bytes)
{
    LOCK(cs_totalBytesRecv);
    nTotalBytesRecv += bytes;
}

void CNode::RecordBytesSent(uint64_t bytes)
{
    LOCK(cs_totalBytesSent);
    nTotalBytesSent += bytes;

    uint64_t now = GetTime();
    if (nMaxOutboundCycleStartTime + nMaxOutboundTimeframe < now)
    {
        // timeframe expired, reset cycle
        nMaxOutboundCycleStartTime = now;
        nMaxOutboundTotalBytesSentInCycle = 0;
    }

    // TO DO, exclude whitebind peers
    nMaxOutboundTotalBytesSentInCycle += bytes;
}

void CNode::SetMaxOutboundTarget(uint64_t limit)
{
    LOCK(cs_totalBytesSent);
    uint64_t recommendedMinimum = (nMaxOutboundTimeframe / 600) * MAX_BLOCK_SIZE;
    nMaxOutboundLimit = limit;

    if (limit > 0 && limit < recommendedMinimum)
        LogPrintf("Max outbound target is very small (%s bytes) and will be overshot. Recommended minimum is %s bytes.\n", nMaxOutboundLimit, recommendedMinimum);
}

uint64_t CNode::GetMaxOutboundTarget()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundLimit;
}

uint64_t CNode::GetMaxOutboundTimeframe()
{
    LOCK(cs_totalBytesSent);
    return nMaxOutboundTimeframe;
}

uint64_t CNode::GetMaxOutboundTimeLeftInCycle()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    if (nMaxOutboundCycleStartTime == 0)
        return nMaxOutboundTimeframe;

    uint64_t cycleEndTime = nMaxOutboundCycleStartTime + nMaxOutboundTimeframe;
    uint64_t now = GetTime();
    return (cycleEndTime < now) ? 0 : cycleEndTime - GetTime();
}

void CNode::SetMaxOutboundTimeframe(uint64_t timeframe)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundTimeframe != timeframe)
    {
        // reset measure-cycle in case of changing
        // the timeframe
        nMaxOutboundCycleStartTime = GetTime();
    }
    nMaxOutboundTimeframe = timeframe;
}

bool CNode::OutboundTargetReached(bool historicalBlockServingLimit)
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return false;

    if (historicalBlockServingLimit)
    {
        // keep a large enought buffer to at least relay each block once
        uint64_t timeLeftInCycle = GetMaxOutboundTimeLeftInCycle();
        uint64_t buffer = timeLeftInCycle / 600 * MAX_BLOCK_SIZE;
        if (buffer >= nMaxOutboundLimit || nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit - buffer)
            return true;
    }
    else if (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit)
        return true;

    return false;
}

uint64_t CNode::GetOutboundTargetBytesLeft()
{
    LOCK(cs_totalBytesSent);
    if (nMaxOutboundLimit == 0)
        return 0;

    return (nMaxOutboundTotalBytesSentInCycle >= nMaxOutboundLimit) ? 0 : nMaxOutboundLimit - nMaxOutboundTotalBytesSentInCycle;
}

uint64_t CNode::GetTotalBytesRecv()
{
    LOCK(cs_totalBytesRecv);
    return nTotalBytesRecv;
}

uint64_t CNode::GetTotalBytesSent()
{
    LOCK(cs_totalBytesSent);
    return nTotalBytesSent;
}

void CNode::Fuzz(int nChance)
{
    if (!fSuccessfullyConnected) return; // Don't fuzz initial handshake
    if (GetRand(nChance) != 0) return; // Fuzz 1 of every nChance messages

    switch (GetRand(3))
    {
    case 0:
        // xor a random byte with a random value:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend[pos] ^= (unsigned char)(GetRand(256));
        }
        break;
    case 1:
        // delete a random byte:
        if (!ssSend.empty()) {
            CDataStream::size_type pos = GetRand(ssSend.size());
            ssSend.erase(ssSend.begin()+pos);
        }
        break;
    case 2:
        // insert a random byte at a random position
        {
            CDataStream::size_type pos = GetRand(ssSend.size());
            char ch = (char)GetRand(256);
            ssSend.insert(ssSend.begin()+pos, ch);
        }
        break;
    }
    // Chance of more than one change half the time:
    // (more changes exponentially less likely):
    Fuzz(2);
}

//
// CAddrDB
//

CAddrDB::CAddrDB()
{
    pathAddr = GetDataDir() / "peers.dat";
}

bool CAddrDB::Write(const CAddrMan& addr)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("peers.dat.%04x", randv);

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssPeers(SER_DISK, CLIENT_VERSION);
    ssPeers << FLATDATA(Params().MessageStart());
    ssPeers << addr;
    uint256 hash = Hash(ssPeers.begin(), ssPeers.end());
    ssPeers << hash;

    // open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try {
        fileout << ssPeers;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing peers.dat, if any, with new peers.dat.XXXX
    if (!RenameOver(pathTmp, pathAddr))
        return error("%s: Rename-into-place failed", __func__);

    return true;
}

bool CAddrDB::Read(CAddrMan& addr)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathAddr.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathAddr.string());

    // use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathAddr);
    uint64_t dataSize = 0;
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);

        // de-serialize address data into one CAddrMan object
        ssPeers >> addr;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }

    return true;
}

unsigned int ReceiveFloodSize() { return 1000*GetArg("-maxreceivebuffer", DEFAULT_MAXRECEIVEBUFFER); }
unsigned int SendBufferSize() { return 1000*GetArg("-maxsendbuffer", DEFAULT_MAXSENDBUFFER); }

CNode::CNode(SOCKET hSocketIn, const CAddress& addrIn, const std::string& addrNameIn, bool fInboundIn, bool fNetworkNodeIn) :
    ssSend(SER_NETWORK, INIT_PROTO_VERSION),
    addrKnown(5000, 0.001),
    filterInventoryKnown(50000, 0.000001)
{
    nServices = 0;
    hSocket = hSocketIn;
    nRecvVersion = INIT_PROTO_VERSION;
    nLastSend = 0;
    nLastRecv = 0;
    nSendBytes = 0;
    nRecvBytes = 0;
    nTimeConnected = GetTime();
    nTimeOffset = 0;
    addr = addrIn;
    addrName = addrNameIn == "" ? addr.ToStringIPPort() : addrNameIn;
    nVersion = 0;
    nNumWarningsSkipped = 0;
    nLastWarningTime = 0;
    strSubVer = "";
    fWhitelisted = false;
    fOneShot = false;
    fClient = false; // set by version message
    fInbound = fInboundIn;
    fNetworkNode = fNetworkNodeIn;
    fSuccessfullyConnected = false;
    fDisconnect = false;
    nRefCount = 0;
    nSendSize = 0;
    nSendOffset = 0;
    hashContinue = uint256();
    nStartingHeight = -1;
    filterInventoryKnown.reset();
    fGetAddr = false;
    nNextLocalAddrSend = 0;
    nNextAddrSend = 0;
    nNextInvSend = 0;
    fRelayTxes = false;
    pfilter = new CBloomFilter();
    nPingNonceSent = 0;
    nPingUsecStart = 0;
    nPingUsecTime = 0;
    fPingQueued = false;
    fMasternode = false;
    nMinPingUsecTime = std::numeric_limits<int64_t>::max();
    vchKeyedNetGroup = CalculateKeyedNetGroup(addr);

    {
        LOCK(cs_nLastNodeId);
        id = nLastNodeId++;
    }

    if(fNetworkNode || fInbound)
        AddRef();

    if (fLogIPs)
        LogPrint("net", "Added connection to %s peer=%d\n", addrName, id);
    else
        LogPrint("net", "Added connection peer=%d\n", id);

    // Be shy and don't send version until we hear
    if (hSocket != INVALID_SOCKET && !fInbound)
        PushVersion();

    GetNodeSignals().InitializeNode(GetId(), this);
}

CNode::~CNode()
{
    CloseSocket(hSocket);

    if (pfilter)
        delete pfilter;

    GetNodeSignals().FinalizeNode(GetId());
}

void CNode::AskFor(const CInv& inv)
{
    if (mapAskFor.size() > MAPASKFOR_MAX_SZ || setAskFor.size() > SETASKFOR_MAX_SZ) {
        int64_t nNow = GetTime();
        if(nNow - nLastWarningTime > WARNING_INTERVAL) {
            if (fDebugMaster) LogPrintf("CNode::AskFor -- WARNING: inventory message dropped: mapAskFor.size = %d, setAskFor.size = %d, MAPASKFOR_MAX_SZ = %d, SETASKFOR_MAX_SZ = %d, nSkipped = %d, peer=%d\n",
                      mapAskFor.size(), setAskFor.size(), MAPASKFOR_MAX_SZ, SETASKFOR_MAX_SZ, nNumWarningsSkipped, id);
            nLastWarningTime = nNow;
            nNumWarningsSkipped = 0;
        }
        else {
            ++nNumWarningsSkipped;
        }
        return;
    }
    // a peer may not have multiple non-responded queue positions for a single inv item
    if (!setAskFor.insert(inv.hash).second)
        return;

    // We're using mapAskFor as a priority queue,
    // the key is the earliest time the request can be sent
    int64_t nRequestTime;
    limitedmap<uint256, int64_t>::const_iterator it = mapAlreadyAskedFor.find(inv.hash);
    if (it != mapAlreadyAskedFor.end())
        nRequestTime = it->second;
    else
        nRequestTime = 0;

    LogPrint("net", "askfor %s  %d (%s) peer=%d\n", inv.ToString(), nRequestTime, DateTimeStrFormat("%H:%M:%S", nRequestTime/1000000), id);

    // Make sure not to reuse time indexes to keep things in the same order
    int64_t nNow = GetTimeMicros() - 1000000;
    static int64_t nLastTime;
    ++nLastTime;
    nNow = std::max(nNow, nLastTime);
    nLastTime = nNow;

    // Each retry is 2 minutes after the last
    nRequestTime = std::max(nRequestTime + 2 * 60 * 1000000, nNow);
    if (it != mapAlreadyAskedFor.end())
        mapAlreadyAskedFor.update(it, nRequestTime);
    else
        mapAlreadyAskedFor.insert(std::make_pair(inv.hash, nRequestTime));
    mapAskFor.insert(std::make_pair(nRequestTime, inv));
}

void CNode::BeginMessage(const char* pszCommand) EXCLUSIVE_LOCK_FUNCTION(cs_vSend)
{
    ENTER_CRITICAL_SECTION(cs_vSend);
    assert(ssSend.size() == 0);
    ssSend << CMessageHeader(Params().MessageStart(), pszCommand, 0);
    LogPrint("net", "sending: %s ", SanitizeString(pszCommand));
}

void CNode::AbortMessage() UNLOCK_FUNCTION(cs_vSend)
{
    ssSend.clear();

    LEAVE_CRITICAL_SECTION(cs_vSend);

    LogPrint("net", "(aborted)\n");
}

void CNode::EndMessage() UNLOCK_FUNCTION(cs_vSend)
{
    // The -*messagestest options are intentionally not documented in the help message,
    // since they are only used during development to debug the networking code and are
    // not intended for end-users.
    if (mapArgs.count("-dropmessagestest") && GetRand(GetArg("-dropmessagestest", 2)) == 0)
    {
        LogPrint("net", "dropmessages DROPPING SEND MESSAGE\n");
        AbortMessage();
        return;
    }
    if (mapArgs.count("-fuzzmessagestest"))
        Fuzz(GetArg("-fuzzmessagestest", 10));

    if (ssSend.size() == 0)
    {
        LEAVE_CRITICAL_SECTION(cs_vSend);
        return;
    }
    // Set the size
    unsigned int nSize = ssSend.size() - CMessageHeader::HEADER_SIZE;
    WriteLE32((uint8_t*)&ssSend[CMessageHeader::MESSAGE_SIZE_OFFSET], nSize);

    // Set the checksum
    uint256 hash = Hash(ssSend.begin() + CMessageHeader::HEADER_SIZE, ssSend.end());
    unsigned int nChecksum = 0;
    memcpy(&nChecksum, &hash, sizeof(nChecksum));
    assert(ssSend.size () >= CMessageHeader::CHECKSUM_OFFSET + sizeof(nChecksum));
    memcpy((char*)&ssSend[CMessageHeader::CHECKSUM_OFFSET], &nChecksum, sizeof(nChecksum));

    LogPrint("net", "(%d bytes) peer=%d\n", nSize, id);

    std::deque<CSerializeData>::iterator it = vSendMsg.insert(vSendMsg.end(), CSerializeData());
    ssSend.GetAndClear(*it);
    nSendSize += (*it).size();

    // If write queue empty, attempt "optimistic write"
    if (it == vSendMsg.begin())
        SocketSendData(this);

    LEAVE_CRITICAL_SECTION(cs_vSend);
}

std::vector<unsigned char> CNode::CalculateKeyedNetGroup(CAddress& address)
{
    if(vchSecretKey.size() == 0) {
        vchSecretKey.resize(32, 0);
        GetRandBytes(vchSecretKey.data(), vchSecretKey.size());
    }

    std::vector<unsigned char> vchGroup;
    CSHA256 hash;
    std::vector<unsigned char> vch(32);

    vchGroup = address.GetGroup();

    hash.Write(begin_ptr(vchGroup), vchGroup.size());

    hash.Write(begin_ptr(vchSecretKey), vchSecretKey.size());

    hash.Finalize(begin_ptr(vch));
    return vch;
}

//
// CBanDB
//

CBanDB::CBanDB()
{
    pathBanlist = GetDataDir() / "banlist.dat";
}

bool CBanDB::Write(const banmap_t& banSet)
{
    // Generate random temporary filename
    unsigned short randv = 0;
    GetRandBytes((unsigned char*)&randv, sizeof(randv));
    std::string tmpfn = strprintf("banlist.dat.%04x", randv);

    // serialize banlist, checksum data up to that point, then append csum
    CDataStream ssBanlist(SER_DISK, CLIENT_VERSION);
    ssBanlist << FLATDATA(Params().MessageStart());
    ssBanlist << banSet;
    uint256 hash = Hash(ssBanlist.begin(), ssBanlist.end());
    ssBanlist << hash;

    // open temp output file, and associate with CAutoFile
    boost::filesystem::path pathTmp = GetDataDir() / tmpfn;
    FILE *file = fopen(pathTmp.string().c_str(), "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s: Failed to open file %s", __func__, pathTmp.string());

    // Write and commit header, data
    try {
        fileout << ssBanlist;
    }
    catch (const std::exception& e) {
        return error("%s: Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing banlist.dat, if any, with new banlist.dat.XXXX
    if (!RenameOver(pathTmp, pathBanlist))
        return error("%s: Rename-into-place failed", __func__);

    return true;
}

bool CBanDB::Read(banmap_t& banSet)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathBanlist.string().c_str(), "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s: Failed to open file %s", __func__, pathBanlist.string());

    // use file size to size memory buffer
    uint64_t fileSize = boost::filesystem::file_size(pathBanlist);
    uint64_t dataSize = 0;
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    filein.fclose();

    CDataStream ssBanlist(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssBanlist.begin(), ssBanlist.end());
    if (hashIn != hashTmp)
        return error("%s: Checksum mismatch, data corrupted", __func__);

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssBanlist >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("%s: Invalid network magic number", __func__);
        
        // de-serialize address data into one CAddrMan object
        ssBanlist >> banSet;
    }
    catch (const std::exception& e) {
        return error("%s: Deserialize or I/O error - %s", __func__, e.what());
    }
    
    return true;
}

void DumpBanlist()
{
    int64_t nStart = GetTimeMillis();

    CNode::SweepBanned(); //clean unused entries (if bantime has expired)

    CBanDB bandb;
    banmap_t banmap;
    CNode::GetBanned(banmap);
    bandb.Write(banmap);

    LogPrint("net", "Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
             banmap.size(), GetTimeMillis() - nStart);
}

int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds) {
    return nNow + (int64_t)(log1p(GetRand(1ULL << 48) * -0.0000000000000035527136788 /* -1/2^48 */) * average_interval_seconds * -1000000.0 + 0.5);
}

std::vector<CNode*> CopyNodeVector()
{
    std::vector<CNode*> vecNodesCopy;
    LOCK(cs_vNodes);
    for(size_t i = 0; i < vNodes.size(); ++i) {
        CNode* pnode = vNodes[i];
        pnode->AddRef();
        vecNodesCopy.push_back(pnode);
    }
    return vecNodesCopy;
}

void ReleaseNodeVector(const std::vector<CNode*>& vecNodes)
{
    for(size_t i = 0; i < vecNodes.size(); ++i) {
        CNode* pnode = vecNodes[i];
        pnode->Release();
    }
}


bool RecvHttpLine(SOCKET hSocket, string& strLine, int iMaxLineSize, int iTimeoutSecs)
{
	try
	{
		strLine = "";
		clock_t begin = clock();
		while (true)
		{
			char c;
     		int nBytes = recv(hSocket, &c, 1, MSG_DONTWAIT);
			clock_t end = clock();
			double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC + .01);
			if (elapsed_secs > iTimeoutSecs) 
			{
				if (fDebugMaster) LogPrintf(" http timeout ");
				return true;
			}
			if (nBytes > 0)
			{
				strLine += c;
				if (c == '\n')      return true;
				if (c == '\r')      return true;
				if (strLine.find("</html>") != string::npos) return true;
				if (strLine.find("</HTML>") != string::npos) return true;
				if (strLine.find("<EOF>") != string::npos) return true;
				if (strLine.find("<END>") != string::npos) return true;
				if ((int)strLine.size() >= iMaxLineSize)
					return true;
			}
			else if (nBytes <= 0)
			{
				boost::this_thread::interruption_point();
				if (nBytes < 0)
				{

					int nErr = WSAGetLastError();
					if (nErr == WSAEMSGSIZE)
						continue;
					if (nErr == WSAEWOULDBLOCK || nErr == WSAEINTR || nErr == WSAEINPROGRESS)
					{
						MilliSleep(1);
						clock_t end = clock();
						double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC+.01);
						if (elapsed_secs > iTimeoutSecs) return true;
						continue;
					}
				}
				if (!strLine.empty())
					return true;
				if (nBytes == 0)
				{
					// socket closed
					return false;
				}
				else
				{
					// socket error
					int nErr = WSAGetLastError();
					if (nErr > 0)
					{
						if (fDebugMaster) LogPrintf("HTTP Socket Error: %d\n", nErr);
						return false;
					}
				}
			}
		}

	}
	catch (std::exception &e)
	{
        return false;
    }
	catch (...)
	{
		return false;
	}

}



std::string GetHTTPContent(const CService& addrConnect, std::string getdata, int iTimeoutSecs, int iOptBreak)
{
	try
	{
		char *pszGet = (char*)getdata.c_str();
		SOCKET hSocket;

	    bool proxyConnectionFailed = false;
    

		if (!ConnectSocket(addrConnect, hSocket, iTimeoutSecs*1000, &proxyConnectionFailed))
		{
			return "GetHttpContent() : connection to address failed";
		}

		send(hSocket, pszGet, strlen(pszGet), MSG_NOSIGNAL);
		string strLine;
		std::string strOut="null";
		MilliSleep(1);
		double timeout = 0;
		clock_t begin = clock();
		while (RecvHttpLine(hSocket, strLine, 50000, iTimeoutSecs))
		{
            strOut = strOut + strLine + "\r\n";
			MilliSleep(1);
			timeout=timeout+1;
		    clock_t end = clock();
			double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC+.01);
			if (elapsed_secs > iTimeoutSecs) break;
		    if (strLine.find("<END>") != string::npos) break;
			if (strLine.find("<eof>") != string::npos) break;
			if (strLine.find("</html>") != string::npos) break;
			if (strLine.find("</HTML>") != string::npos) break;
			if (iOptBreak == 1) if (strLine.find("}") != string::npos) break;
		}
		CloseSocket(hSocket);
		return strOut;
	}
    catch (std::exception &e)
	{
        return "";
    }
	catch (...)
	{
		return "";
	}
}

std::string GetDomainFromURL(std::string sURL)
{
	std::string sDomain = "";
	if (sURL.find("https://") != string::npos)
	{
		sDomain = sURL.substr(8,sURL.length()-8);
	}
	else if(sURL.find("http://") != string::npos)
	{
		sDomain = sURL.substr(7,sURL.length()-7);
	}
	else
	{
		sDomain = sURL;
	}
	return sDomain;
}


std::string PrepareHTTPPost(bool bPost, std::string sPage, std::string sHostHeader, const string& sMsg, const map<string,string>& mapRequestHeaders)
{
    ostringstream s;
	std::string sUserAgent = "Mozilla/5.0";
	std::string sMethod = bPost ? "POST" : "GET";
    s << sMethod + " /" + sPage + " HTTP/1.1\r\n"
      << "User-Agent: " + sUserAgent + "/" << FormatFullVersion() << "\r\n"
	  << "Host: " + sHostHeader + "" << "\r\n"
      << "Content-Length: " << sMsg.size() << "\r\n";
    BOOST_FOREACH(const PAIRTYPE(string, string)& item, mapRequestHeaders)
        s << item.first << ": " << item.second << "\r\n";
        s << "\r\n" << sMsg;
    return s.str();
}

std::string SQL(std::string sCommand, std::string sAddress, std::string sArguments, std::string& sError)
{
	// After Christmas 2017, the chosen Sanctuary runs the SQL Service (this is used for Private node-node communication and market making)
	std::string sSQLURL = GetArg("-sqlnode", "http://pool.biblepay.org");
	int iPort = (int)cdbl(GetArg("-sqlport", "80"),0);
	std::string sSqlPage = "Action.aspx";
	std::string sMultiResponse = BiblepayHttpPost(true, 0, "POST", sAddress, sCommand, sSQLURL, sSqlPage, iPort, sArguments, 0);
	sError = ExtractXML(sMultiResponse,"<ERROR>","</ERROR>");
	std::string sResponse = ExtractXML(sMultiResponse,"<RESPONSE>","</RESPONSE>");
	if (!sError.empty())
	{
		return "";
	}
	return sResponse;
}


std::string BiblepayHttpPost(bool bPost, int iThreadID, std::string sActionName, std::string sDistinctUser, 
	std::string sPayload, std::string sBaseURL, std::string sPage, int iPort, std::string sSolution, int iOptBreak)
{
	try
	{
		    map<string, string> mapRequestHeaders;
			mapRequestHeaders["Miner"] = sDistinctUser;
			mapRequestHeaders["Action"] = sPayload;
			mapRequestHeaders["Solution"] = sSolution;
			mapRequestHeaders["Agent"] = FormatFullVersion();
			// BiblePay supported pool Network Chain modes: main, test, regtest
			const CChainParams& chainparams = Params();
			mapRequestHeaders["NetworkID"] = chainparams.NetworkIDString();
			mapRequestHeaders["ThreadID"] = RoundToString(iThreadID,0);
			mapRequestHeaders["OS"] = sOS;

			CService addrConnect;
			std::string sDomain = GetDomainFromURL(sBaseURL);
			if (sDomain.empty()) return "DOMAIN_MISSING";
			CService addrIP(sDomain, iPort, true);
     		if (addrIP.IsValid())
			{
				addrConnect = addrIP;
			}
			else
			{
  				return "DNS_ERROR"; 
			}
			std::string sPost = PrepareHTTPPost(bPost, sPage, sDomain, sPayload, mapRequestHeaders);
			std::string sResponse = GetHTTPContent(addrConnect, sPost, 15, iOptBreak);
			if (fDebug10) LogPrintf("\r\n  HTTP_RESPONSE:    %s    \r\n",sResponse.c_str());
			return sResponse;
	}
    catch (std::exception &e)
	{
        return "WEB_EXCEPTION";
    }
	catch (...)
	{
		return "GENERAL_WEB_EXCEPTION";
	}
}

bool DownloadIndividualDistributedComputingFile2(int iNextSuperblock, std::string sBaseURL, std::string sPage, std::string sUserFile, std::string& sError)
{
	std::string sPath = GetSANDirectory2() + sUserFile + ".gz";
	std::string sTarget = GetSANDirectory2() + sUserFile;
	boost::filesystem::remove(sTarget);
    boost::filesystem::remove(sPath);
	std::string sURL = sBaseURL + sPage;
	std::string sCommand = "wget " + sURL + " -O " + sPath + " -q";
    std::string sResult = SystemCommand2(sCommand.c_str());
	sCommand = "gunzip " + sPath;
    sResult = SystemCommand2(sCommand.c_str());
	int64_t nFileSize = GetFileSize(sTarget);
	if (fDebugMaster) LogPrintf(" DIDCF2 phase1 %s, sz %f \n", sResult.c_str(), nFileSize);
	if (nFileSize < 1)
	{
		sError = sResult;
		return false;
	}
	else
	{
		return true;
	}
}

bool DownloadIndividualDistributedComputingFile(int iNextSuperblock, std::string sBaseURL, std::string sPage, std::string sUserFile, std::string& sError)
{
	// First delete:
	std::string sPath2 = GetSANDirectory2() + sUserFile + ".gz";
	std::string sTarget2 = GetSANDirectory2() + sUserFile;
	boost::filesystem::remove(sTarget2);
    boost::filesystem::remove(sPath2);
	int iMaxSize = 900000000;
    int iTimeoutSecs = 60 * 7;
	LogPrintf("Downloading DC File NAME %s FROM URL %s ", sPath2.c_str(), sBaseURL.c_str());
	int iIterations = 0;
	try
	{
		map<string, string> mapRequestHeaders;
		mapRequestHeaders["Agent"] = FormatFullVersion();
		const CChainParams& chainparams = Params();
		BIO* bio;
		SSL_CTX* ctx;
		SSL_library_init();
		ctx = SSL_CTX_new(SSLv23_client_method());
		if (ctx == NULL)
		{
			sError = "<ERROR>CTX_IS_NULL</ERROR>";
			fDistributedComputingCycleDownloading = false;
			return false;
		}
		bio = BIO_new_ssl_connect(ctx);
		std::string sDomain = GetDomainFromURL(sBaseURL);
		std::string sDomainWithPort = sDomain + ":" + "443";
		BIO_set_conn_hostname(bio, sDomainWithPort.c_str());
		if(BIO_do_connect(bio) <= 0)
		{
			sError = "Failed connection to " + sDomainWithPort;
			fDistributedComputingCycleDownloading = false;
			return false;
		}
		CService addrConnect;
		if (sDomain.empty()) 
		{
			sError = "DOMAIN_MISSING";
			fDistributedComputingCycleDownloading = false;
			return false;
		}
		CService addrIP(sDomain, 443, true);
    	if (addrIP.IsValid())
		{
			addrConnect = addrIP;
		}
		else
		{
  			sError = "<ERROR>DNS_ERROR</ERROR>"; 
			fDistributedComputingCycleDownloading = false;
			return false;
		}
		std::string sPayload = "";
		std::string sPost = PrepareHTTPPost(true, sPage, sDomain, sPayload, mapRequestHeaders);
		const char* write_buf = sPost.c_str();
		if(BIO_write(bio, write_buf, strlen(write_buf)) <= 0)
		{
			sError = "<ERROR>FAILED_HTTPS_POST</ERROR>";
			fDistributedComputingCycleDownloading = false;
			return false;
		}
		int iSize;
		int iBufSize = 256000;
		clock_t begin = clock();
		std::string sData = "";
		char bigbuf[iBufSize];
	
		FILE *outUserFile = fopen(sPath2.c_str(), "wb");
		for(;;)
		{
	
			iSize = BIO_read(bio, bigbuf, iBufSize);
			bool fShouldRetry = BIO_should_retry(bio);

			if(iSize <= 0 && !fShouldRetry)
			{
				LogPrintf("DCC download finished \n");
				break;
			}

			size_t bytesWritten=0;
			if (iSize > 0)
			{
				if (iIterations == 0)
				{
					// GZ magic bytes: 31 139
					int iPos = 0;
					for (iPos = 0; iPos < iBufSize; iPos++)
					{
						if (bigbuf[iPos] == 31) if (bigbuf[iPos + 1] == (char)0x8b)
						{
							break;
						}
					}
					int iNewSize = iBufSize - iPos;
					char smallbuf[iNewSize];
					for (int i=0; i < iNewSize; i++)
					{
						smallbuf[i] = bigbuf[i + iPos];
					}
					bytesWritten = fwrite(smallbuf, 1, iNewSize, outUserFile);
				}
				else
				{
					bytesWritten = fwrite(bigbuf, 1, iSize, outUserFile);
				}
				iIterations++;
			}
			clock_t end = clock();
			double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC + .01);
			if (elapsed_secs > iTimeoutSecs) 
			{
				LogPrintf(" download timed out ... (bytes written %f)  \n", (double)bytesWritten);
				break;
			}
			if (false && (int)sData.size() >= iMaxSize) 
			{
				LogPrintf(" download oversized .. \n");
				break;
			}
		}
		// R ANDREW - JAN 4 2018: Free bio resources
		BIO_free_all(bio);
	 	fclose(outUserFile);
	}
	catch (std::exception &e)
	{
        sError = "<ERROR>WEB_EXCEPTION</ERROR>";
		return false;
    }
	catch (...)
	{
		sError = "<ERROR>GENERAL_WEB_EXCEPTION</ERROR>";
		return false;
	}
	LogPrintf("Gunzip %s",sPath2.c_str());
	std::string sCommand = "gunzip " + sPath2;
    std::string result = SystemCommand2(sCommand.c_str());
	return true;
}

bool DownloadDistributedComputingFile(int iNextSuperblock, std::string& sError)
{
	if (!fDistributedComputingEnabled) return true;
	if (fDistributedComputingCycleDownloading) return false;
	TouchDailyMagnitudeFile();
	fDistributedComputingCycleDownloading = true;
	// Project #1 - Currently Rosetta 
	std::string sProjectId = "project1"; // Cancer Research
	std::string sSrc = GetSporkValue(sProjectId);
	std::string sBaseURL = "https://" + sSrc;
	std::string sPage = "/rosetta/stats/user.gz";
	// Project #2 - Currently WCG
	std::string sProjectId2 = "project2"; // WCG
	std::string sSrc2 = GetSporkValue(sProjectId2);
	std::string sBaseURL2 = "https://" + sSrc2;
	std::string sPage2 = "/boinc/stats/user.gz";
	DownloadIndividualDistributedComputingFile2(iNextSuperblock, sBaseURL, sPage, "user1", sError);
	DownloadIndividualDistributedComputingFile2(iNextSuperblock, sBaseURL2, sPage2, "user2", sError);
	LogPrintf("Filter File %f",iNextSuperblock);
	FilterFile(50, iNextSuperblock, sError);
	fDistributedComputingCycleDownloading = false;
	return true;
}


std::string BiblepayIPFSPost(std::string sFileName, std::string sPayload)
{
		int iTimeoutSecs = 30;
		int iMaxSize = 900000;
	    map<string, string> mapRequestHeaders;
		mapRequestHeaders["Agent"] = FormatFullVersion();
		mapRequestHeaders["Filename"] = sFileName;
		const CChainParams& chainparams = Params();
		mapRequestHeaders["NetworkID"] = chainparams.NetworkIDString();
		BIO* bio;
		SSL_CTX* ctx;
		SSL_library_init();
		ctx = SSL_CTX_new(SSLv23_client_method());
		if (ctx == NULL) 
			return "<ERROR>CTX_IS_NULL</ERROR>";
		bio = BIO_new_ssl_connect(ctx);
		std::string sDomain = GetDomainFromURL("ipfs.biblepay.org");
		int iPort = 443;
		std::string sDomainWithPort = sDomain + ":" + RoundToString(iPort, 0);
		BIO_set_conn_hostname(bio, sDomainWithPort.c_str());
		if(BIO_do_connect(bio) <= 0)
			return "<ERROR>BIO_FAILURE while connecting " + sDomainWithPort + "</ERROR>";
		CService addrConnect;
		if (sDomain.empty()) return "<ERROR>DOMAIN_MISSING</ERROR>";
		CService addrIP(sDomain, iPort, true);
    	if (addrIP.IsValid())
		{
			addrConnect = addrIP;
		}
		else
		{ 
			return "<ERROR>DNS_ERROR</ERROR>"; 
		}
		
		std::string sPost = PrepareHTTPPost(true, "ipfs.bible", sDomain, sPayload, mapRequestHeaders);
		const char* write_buf = sPost.c_str();
		if(BIO_write(bio, write_buf, strlen(write_buf)) <= 0)
		{
		return "<ERROR>FAILED_HTTPS_POST</ERROR>";
		}
		int size;
		char buf[1024];
		clock_t begin = clock();
		std::string sData = "";
		for(;;)
		{
			size = BIO_read(bio, buf, 1023);
			if(size <= 0)
			{
				break;
			}
			buf[size] = 0;
			string MyData(buf);
			sData += MyData;
			clock_t end = clock();
			double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC + .01);
			if (elapsed_secs > iTimeoutSecs) break;
			if (sData.find("</html>") != string::npos) break;
			if (sData.find("</HTML>") != string::npos) break;
			if (sData.find("<EOF>") != string::npos) break;
			if (sData.find("Content-Length:") != string::npos)
			{
				double dMaxSize = cdbl(ExtractXML(sData,"Content-Length: ","\n"),0);
				std::size_t foundPos = sData.find("Content-Length:");
				if (dMaxSize > 0)
				{
					iMaxSize = dMaxSize + (int)foundPos + 16;
				}
			}
			if ((int)sData.size() >= (iMaxSize-1)) break;
		}
		BIO_free_all(bio);
		return sData;
}


std::string BiblepayHTTPSPost(bool bPost, int iThreadID, std::string sActionName, std::string sDistinctUser, std::string sPayload, std::string sBaseURL, 
	std::string sPage, int iPort, std::string sSolution, int iTimeoutSecs, int iMaxSize, int iBreakOnError)
{
	try
	{
		    map<string, string> mapRequestHeaders;
			mapRequestHeaders["Miner"] = sDistinctUser;
			mapRequestHeaders["Action"] = sPayload;
			mapRequestHeaders["Solution"] = sSolution;
			mapRequestHeaders["Agent"] = FormatFullVersion();
			// BiblePay supported pool Network Chain modes: main, test, regtest
			const CChainParams& chainparams = Params();
			mapRequestHeaders["NetworkID"] = chainparams.NetworkIDString();
			mapRequestHeaders["ThreadID"] = RoundToString(iThreadID,0);
			mapRequestHeaders["OS"] = sOS;

			BIO* bio;
			SSL_CTX* ctx;
			//   Registers the SSL/TLS ciphers and digests and starts the security layer.
			SSL_library_init();
			ctx = SSL_CTX_new(SSLv23_client_method());
			if (ctx == NULL)
			{
				return "<ERROR>CTX_IS_NULL</ERROR>";
			}
			bio = BIO_new_ssl_connect(ctx);
			std::string sDomain = GetDomainFromURL(sBaseURL);
			std::string sDomainWithPort = sDomain + ":" + "443";
			BIO_set_conn_hostname(bio, sDomainWithPort.c_str());
			if(BIO_do_connect(bio) <= 0)
			{
				return "<ERROR>Failed connection to " + sDomainWithPort + "</ERROR>";
			}
	
			CService addrConnect;
			if (sDomain.empty()) return "<ERROR>DOMAIN_MISSING</ERROR>";
			CService addrIP(sDomain, 443, true);
            if (addrIP.IsValid())
			{
				addrConnect = addrIP;
			}
			else
			{
  				return "<ERROR>DNS_ERROR</ERROR>"; 
			}
			std::string sPost = PrepareHTTPPost(bPost, sPage, sDomain, sPayload, mapRequestHeaders);
			const char* write_buf = sPost.c_str();
			if(BIO_write(bio, write_buf, strlen(write_buf)) <= 0)
			{
				//  Handle failed writes here
				if(!BIO_should_retry(bio))
				{
					// ToDo: Server requested a retry.  Handle this someday.
				}
				return "<ERROR>FAILED_HTTPS_POST</ERROR>";
			}
      		//  Variables used to read the response from the server
			int size;
			char buf[1024];
			clock_t begin = clock();
			std::string sData = "";
			for(;;)
			{
				//  Get chunks of the response 1023 at the time.
				size = BIO_read(bio, buf, 1023);
				if(size <= 0)
				{
					break;
				}
				buf[size] = 0;
				string MyData(buf);
				sData += MyData;
				clock_t end = clock();
				double elapsed_secs = double(end - begin) / (CLOCKS_PER_SEC + .01);
				if (elapsed_secs > iTimeoutSecs) break;
				if (sData.find("</html>") != string::npos) break;
				if (sData.find("</HTML>") != string::npos) break;
				if (sData.find("<EOF>") != string::npos) break;
				if (sData.find("<END>") != string::npos) break;
				if (sData.find("</account_out>") != string::npos) break;
				if (sData.find("</am_set_info_reply>") != string::npos) break;
				if (sData.find("</am_get_info_reply>") != string::npos) break;
				if (iBreakOnError == 1) if (sData.find("</user>") != string::npos) break;
				if (iBreakOnError == 1) if (sData.find("</error>") != string::npos) break;
				if (iBreakOnError == 1) if (sData.find("</error_msg>") != string::npos) break;
				if (iBreakOnError == 2) if (sData.find("</results>") != string::npos) break;
				if (iBreakOnError == 3) if (sData.find("}}") != string::npos) break;
				if (sData.find("Content-Length:") != string::npos)
				{
					double dMaxSize = cdbl(ExtractXML(sData,"Content-Length: ","\n"),0);
					std::size_t foundPos = sData.find("Content-Length:");
					if (dMaxSize > 0)
					{
						iMaxSize = dMaxSize + (int)foundPos + 16;
					}
				}
				if ((int)sData.size() >= (iMaxSize-1)) break;
			}
			// R ANDREW - JAN 4 2018: Free bio resources

			BIO_free_all(bio);
			
			return sData;
	}
	catch (std::exception &e)
	{
        return "<ERROR>WEB_EXCEPTION</ERROR>";
    }
	catch (...)
	{
		return "<ERROR>GENERAL_WEB_EXCEPTION</ERROR>";
	}
}


/*                                                                          IPFS                                                                 */


string ipfs_header_value(const string& full_header, const string& header_name)
{
    size_t pos = full_header.find(header_name);
    string r;
    if (pos!=string::npos)
    {
        size_t begin = full_header.find_first_not_of(": ", pos + header_name.length());
        size_t until = full_header.find_first_of("\r\n\t ", begin + 1);
        if (begin!=string::npos && until!=string::npos)
        {
            r = full_header.substr(begin,until-begin);
        }
    }
    return r;
}

int ipfs_http_get(const string& request, const string& ip_address, int port, const string& fname, double dTimeoutSecs)
{
    char buffer[65535];
	int bytes_total=0;
	int bytes_expected=99999999;
    int mode = 0;
	SOCKET socketnumber;
	CService addrConnect(ip_address, port, true);
    if (!addrConnect.IsValid()) return -4;
	bool proxyConnectionFailed = false;
   
	if (!ConnectSocket(addrConnect, socketnumber, dTimeoutSecs * 1000, &proxyConnectionFailed))
	{
		return -3;
	}

	ofstream fd(fname.c_str());
	if (!fd.good()) return -1;
	
	int iOffset = 0;
	LogPrintf(" sending %s ",ip_address.c_str());

    ::send(socketnumber, request.c_str(), request.length(), MSG_NOSIGNAL | MSG_DONTWAIT);
	double begin = clock();
    while (bytes_total < bytes_expected)
	{
		    char tempbuffer[4096];
			int iTempBytesRec = ::recv(socketnumber, tempbuffer, sizeof(tempbuffer), MSG_DONTWAIT);
			if (iTempBytesRec > 0)
			{
				for (int k = 0; k < iTempBytesRec; k++)
				{
					buffer[iOffset + k] = tempbuffer[k];
				}
				iOffset += iTempBytesRec;
			}
		                
			double elapsed_secs = double(clock() - begin) / (CLOCKS_PER_SEC + .01);
			if (elapsed_secs > dTimeoutSecs) break;
			if (iOffset > 255 || (iOffset+bytes_total > bytes_expected-1))
			{
				if (mode == 1) 
				{
					fd.write(buffer, iOffset);
					bytes_total += iOffset;
					//	LogPrintf(" . rec %f  bytestotal %f . ", iOffset, bytes_total);
					iOffset = 0;
					memset(buffer, 0, sizeof(buffer));
				}
				else if (mode == 0)
				{
					string sHeader(buffer);
					std::string CL = ExtractXML(sHeader, "Content-Length:","\n");
					int ContentSize = (int)cdbl(CL, 0);
					if (ContentSize > 0)
					{
						// Find offset
						int body_start = sHeader.find("\r\n\r\n");
						bytes_expected  = ContentSize + body_start + 3;
						bytes_total += iOffset;
						// LogPrintf(" data %s bytes expected %f  elapsed %f bodystart %f   bytestotal %f ....... ",CL.c_str(), bytes_expected, elapsed_secs, body_start, bytes_total);
						mode=1;
						// Write first chunk
						int iFirstByte = body_start + 4;
						int iFirstChunk = iOffset - iFirstByte;
						char tempbuffer[iFirstChunk];
						for (int j = 0; j < iFirstChunk; j++)
						{
							tempbuffer[j] = buffer[j + iFirstByte];
						}
						fd.write(tempbuffer, iFirstChunk);
						memset(buffer, 0, sizeof(buffer));
						iOffset = 0;
					}
			}
		}
    }
    ::close(socketnumber);
    fd.close();
	if (bytes_total >= bytes_expected && bytes_total > 0 && bytes_expected > 0) return 1;
	return -2;
}


int ipfs_download(const string& url, const string& filename, double dTimeoutSecs, double dRangeRequestMin, double dRangeRequestMax)
{
	int port = 0;
    string protocol, domain, path, query, url_port;
    vector<string> ip_addresses;
    int offset = 0;
    size_t pos1,pos2,pos3,pos4;
    offset = offset==0 && url.compare(0, 8, "https://")==0 ? 8 : offset;
    offset = offset==0 && url.compare(0, 7, "http://" )==0 ? 7 : offset;
    pos1 = url.find_first_of('/', offset+1 );
    path = pos1==string::npos ? "" : url.substr(pos1);
    domain = string( url.begin()+offset, pos1 != string::npos ? url.begin()+pos1 : url.end() );
    path = (pos2 = path.find("#"))!=string::npos ? path.substr(0,pos2) : path;
    url_port = (pos3 = domain.find(":"))!=string::npos ? domain.substr(pos3+1) : "";
    domain = domain.substr(0, pos3!=string::npos ? pos3 : domain.length());
    protocol = offset > 0 ? url.substr(0,offset-3) : "";
    query = (pos4 = path.find("?"))!=string::npos ? path.substr(pos4+1) : "";
    path = pos4!=string::npos ? path.substr(0,pos4) : path;
 
    if (query.length()>0)
    {
        path.reserve( path.length() + 1 + query.length() );
        path.append("?").append(query);
    }
    if (url_port.length()==0 && protocol.length()>0)
    {
        url_port = protocol=="http" ? "80" : "443";
    }

	// DNS
	CService addrIP(domain, (int)cdbl(url_port, 0), true);
    if (addrIP.IsValid())
	{
		domain = GetIPFromAddress(addrIP.ToString());
		LogPrintf(" domain %s ",domain.c_str());
	}
	
    if (domain.length() > 0)
            ip_addresses.push_back(domain);
    int r = -1;
    if (ip_addresses.size() > 0)
    {
        stringstream(url_port) >> port;
        stringstream request;
        request << "GET " << path << " HTTP/1.1\r\n";
        request << "Host: " << domain << "\r\n";
		if (dRangeRequestMax > 0)
		{
			request << "Range: bytes=" + (RoundToString(dRangeRequestMin, 0) + "-" + RoundToString(dRangeRequestMax, 0)) << "\r\n";
		}

		request << "\r\n";

		int ix = ip_addresses.size();

        for(int i = 0; i < ix; i++)
        {
            r = ipfs_http_get(request.str(), ip_addresses[i], port, filename, dTimeoutSecs);
			if (r==1) return r;
        }
    }

	return r;
}

