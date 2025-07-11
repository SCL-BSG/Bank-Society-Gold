// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2018 Profit Hunters Coin developers
// Copyright (c) 2020 Bank Society Gold Coin developer [RGPickles]
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/* -----------------------------------------------------------
   -- File      :   net.cpp                                 --
   -- Version   :   1.0.9.7 [based on wallet]               --
   -- Author    :   RGPickles                               --
   -- Date      :   14th February 2020                      --
   -- Detail    :   Net.cpp handles all socket management   --
   --               for the wallet, adjusted timeout        --
   --               settings to ensure network connections  --
   --               are more consistent and permanent.      --
   ----------------------------------------------------------- */

#include "db.h"
#include "net.h"
#include "main.h"
#include "addrman.h"
#include "chainparams.h"
#include "core.h"
#include "ui_interface.h"
#include "darksend.h"


#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

#ifdef WIN32
#include <string.h>
#else
#include <fcntl.h>
#endif

#ifdef USE_UPNP
    #ifdef WIN32
        #include "c:\deps\miniupnpc\miniupnpc.h"
        #include "c:\deps\miniupnpc\miniwget.h"
        #include "c:\deps\miniupnpc\upnpcommands.h"
        #include "c:\deps\miniupnpc\upnperrors.h"
    #endif
#endif

#include <boost/filesystem.hpp>
#include <boost/thread.hpp>
#include <boost/lexical_cast.hpp>

// Dump addresses to peers.dat every 15 minutes (900s)
#define DUMP_ADDRESSES_INTERVAL 900

#if !defined(HAVE_MSG_NOSIGNAL) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

// Fix for ancient MinGW versions, that don't have defined these in ws2tcpip.h.
// Todo: Can be removed when our pull-tester is upgraded to a modern MinGW version.
#ifdef WIN32
#ifndef PROTECTION_LEVEL_UNRESTRICTED
#define PROTECTION_LEVEL_UNRESTRICTED 10
#endif
#ifndef IPV6_PROTECTION_LEVEL
#define IPV6_PROTECTION_LEVEL 23
#endif
#endif

using namespace std;
using namespace boost;

namespace
{
    const int MAX_OUTBOUND_CONNECTIONS = 30;

    struct ListenSocket
    {
        SOCKET socket;
        bool whitelisted;

        ListenSocket(SOCKET socket, bool whitelisted) : socket(socket), whitelisted(whitelisted) {}
    };

}



bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound = NULL, const char *strDest = NULL, bool fOneShot = false);


//
// Global state variables
//
bool fDiscover = true;
bool fListen = true;
uint64_t nLocalServices = NODE_NETWORK;
CCriticalSection cs_mapLocalHost;
map<CNetAddr, LocalServiceInfo> mapLocalHost;
static bool vfReachable[NET_MAX] = {};
static bool vfLimited[NET_MAX] = {};
static CNode* pnodeLocalHost = NULL;
static CNode* pnodeSync = NULL;
uint64_t nLocalHostNonce = 0;
static std::vector<SOCKET> vhListenSocket;
CAddrMan addrman;
std::string strSubVersion;
int nMaxConnections = GetArg("maxconnections", 125);

vector<CNode*> vNodes;
CCriticalSection cs_vNodes;
map<CInv, CDataStream> mapRelay;
deque<pair<int64_t, CInv> > vRelayExpiration;
CCriticalSection cs_mapRelay;
limitedmap<CInv, int64_t> mapAlreadyAskedFor(MAX_INV_SZ);

static deque<string> vOneShots;
CCriticalSection cs_vOneShots;

set<CNetAddr> setservAddNodeAddresses;
CCriticalSection cs_setservAddNodeAddresses;

vector<std::string> vAddedNodes;
CCriticalSection cs_vAddedNodes;

NodeId nLastNodeId = 0;
CCriticalSection cs_nLastNodeId;

static CSemaphore *semOutbound = NULL;

// Signals for message handling
static CNodeSignals g_signals;
CNodeSignals& GetNodeSignals() { return g_signals; }


// * Function: BoolToString *
inline const char * const BoolToString(bool b)
{
  return b ? "true" : "false";
}


// * Function: CountArray *
int CountStringArray(string *ArrayName)
{
    int tmp_cnt;
    tmp_cnt = 0;

    while(ArrayName[tmp_cnt] != "")
    {
        tmp_cnt++;
    }


return tmp_cnt;
}


// * Function: CountArray *
int CountIntArray(int *ArrayName)
{
    int tmp_cnt;
    tmp_cnt = 0;

    while(ArrayName[tmp_cnt] > 0)
    {
        tmp_cnt++;
    }

return tmp_cnt;
}


// ||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||
// 
// [Bitcoin Firewall 1.2.2.4
// March, 2018 - Biznatch Enterprises & BATA Development & Profit Hunters Coin (SocietyG)
// http://bata.io & https://github.com/BiznatchEnterprises/BitcoinFirewall
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.


string ModuleName = "[Bitcoin Firewall 1.2.2.4]";

// *** Firewall Controls (General) ***
bool FIREWALL_ENABLED = false;
bool FIREWALL_CLEAR_BLACKLIST = false;
bool FIREWALL_CLEAR_BANS = true;
int FIREWALL_CLEARBANS_MINNODES = 10;
double FIREWALL_TRAFFIC_TOLERANCE = 0.0001; // Reduce for minimal fluctuation
double FIREWALL_TRAFFIC_ZONE = 4; // + or - Traffic Range 

// *** Firewall Debug (Live Output) ***
bool FIREWALL_LIVE_DEBUG = false;
bool FIREWALL_LIVEDEBUG_EXAM = true;
bool FIREWALL_LIVEDEBUG_BANS = true;
bool FIREWALL_LIVEDEBUG_BLACKLIST = true;
bool FIREWALL_LIVEDEBUG_DISCONNECT = true;
bool FIREWALL_LIVEDEBUG_BANDWIDTHABUSE = true;
bool FIREWALL_LIVEDEBUG_NOFALSEPOSITIVE = true;
bool FIREWALL_LIVEDEBUG_INVALIDWALLET = true;
bool FIREWALL_LIVEDEBUG_FORKEDWALLET = true;
bool FIREWALL_LIVEDEBUG_FLOODINGWALLET = true;

// *** Firewall Controls (Bandwidth Abuse) ***
bool FIREWALL_DETECT_BANDWIDTHABUSE = true;
bool FIREWALL_BLACKLIST_BANDWIDTHABUSE = true;
bool FIREWALL_BAN_BANDWIDTHABUSE = true;
bool FIREWALL_NOFALSEPOSITIVE_BANDWIDTHABUSE = true;

// *** Firewall Controls (Invalid Peer Wallets) ***
bool FIREWALL_DETECT_INVALIDWALLET = true;
bool FIREWALL_BLACKLIST_INVALIDWALLET = true;
bool FIREWALL_BAN_INVALIDWALLET = true;

// *** Firewall Controls (Forked Peer Wallets) ***
bool FIREWALL_DETECT_FORKEDWALLET = true;
bool FIREWALL_BLACKLIST_FORKEDWALLET = true;
bool FIREWALL_BAN_FORKEDWALLET = true;

// *** Firewall Controls (Flooding Peer Wallets) ***
bool FIREWALL_DETECT_FLOODINGWALLET = true;
bool FIREWALL_BLACKLIST_FLOODINGWALLET = true;
bool FIREWALL_BAN_FLOODINGWALLET = true;

// * Firewall Settings (General) *
//int FIREWALL_CHECK_MAX = 3;  // minutes interval for some detection settings

// * Firewall Settings (Exam) *
int FIREWALL_AVERAGE_TOLERANCE = 2;    // Reduce for minimal fluctuation 2 Blocks tolerance
int FIREWALL_AVERAGE_RANGE = 100;   // + or - Starting Height Range

// *** Firewall Settings (Bandwidth Abuse) ***
int FIREWALL_BANTIME_BANDWIDTHABUSE = 0; // 24 hours
int FIREWALL_BANDWIDTHABUSE_MAXCHECK = 10;
double FIREWALL_BANDWIDTHABUSE_MINATTACK = 17.1;
double FIREWALL_BANDWIDTHABUSE_MAXATTACK = 17.2;

// * Firewall Settings (Invalid Wallet)
int FIREWALL_MINIMUM_PROTOCOL = MIN_PEER_PROTO_VERSION;
int FIREWALL_BANTIME_INVALIDWALLET = 2600000; // 30 days
int FIREWALL_INVALIDWALLET_MAXCHECK = 60; // seconds

// * Firewall Settings (Forked Wallet)
int FIREWALL_BANTIME_FORKEDWALLET = 2600000; // 30 days
// FORKLIST (ignore)
int FIREWALL_FORKED_NODEHEIGHT[256] =
{
    10000,
    39486,
    48405
};

// * Firewall Settings (Flooding Wallet)
int FIREWALL_BANTIME_FLOODINGWALLET = 2600000; // 30 days
int FIREWALL_FLOODINGWALLET_MINBYTES = 1000000;
int FIREWALL_FLOODINGWALLET_MAXBYTES = 1000000;
// Flooding Patterns (WARNINGS)
string FIREWALL_FLOODPATTERNS[256] =
{
    "56810121416192123", 
    "57910121517202223",
    "57910121416202223"
};
double FIREWALL_FLOODINGWALLET_MINTRAFFICAVERAGE = 2000; // Ratio Up/Down
double FIREWALL_FLOODINGWALLET_MAXTRAFFICAVERAGE = 2000; // Ratio Up/Down
int FIREWALL_FLOODINGWALLET_MINCHECK = 30; // seconds
int FIREWALL_FLOODINGWALLET_MAXCHECK = 90; // seconds

// Firewall Whitelist (ignore)
string FIREWALL_WHITELIST[256] =
{

};

// * Firewall BlackList Settings *
string FIREWALL_BLACKLIST[256] =
{

};

// * Global Firewall Variables *
bool Firewall_FirstRun = false;
int Firewall_AverageHeight = 0;
int Firewall_AverageHeight_Min = 0;
int Firewall_AverageHeight_Max = 0;
double Firewall_AverageTraffic = 0;
double Firewall_AverageTraffic_Min = 0;
double Firewall_AverageTraffic_Max = 0;
int Firewall_AverageSend = 0;
int Firewall_AverageRecv = 0;
int ALL_CHECK_TIMER = GetTime();

// * Function: LoadFirewallSettings (societyG.conf)*
void LoadFirewallSettings()
{
    // *** Firewall Controls (General) ***
    FIREWALL_ENABLED = GetBoolArg("firewallenabled", FIREWALL_ENABLED);
    FIREWALL_CLEAR_BLACKLIST = GetBoolArg("firewallclearblacklist", FIREWALL_CLEAR_BLACKLIST);
    FIREWALL_CLEAR_BANS = GetBoolArg("firewallclearbanlist", FIREWALL_CLEAR_BANS);

    // *** Firewall Debug (Live Output) ***
    FIREWALL_LIVE_DEBUG = GetBoolArg("firewalldebug", FIREWALL_LIVE_DEBUG);
    FIREWALL_LIVEDEBUG_EXAM = GetBoolArg("firewalldebugexam", FIREWALL_LIVEDEBUG_EXAM);
    FIREWALL_LIVEDEBUG_BANS = GetBoolArg("firewalldebugbans", FIREWALL_LIVEDEBUG_BANS);
    FIREWALL_LIVEDEBUG_BLACKLIST = GetBoolArg("firewalldebugblacklist", FIREWALL_LIVEDEBUG_BLACKLIST);
    FIREWALL_LIVEDEBUG_DISCONNECT = GetBoolArg("firewalldebugdisconnect", FIREWALL_LIVEDEBUG_DISCONNECT);
    FIREWALL_LIVEDEBUG_BANDWIDTHABUSE = GetBoolArg("firewalldebugbandwidthabuse", FIREWALL_LIVEDEBUG_BANDWIDTHABUSE);
    FIREWALL_LIVEDEBUG_NOFALSEPOSITIVE = GetBoolArg("firewalldebugnofalsepositivebandwidthabuse", FIREWALL_LIVEDEBUG_NOFALSEPOSITIVE);
    FIREWALL_LIVEDEBUG_INVALIDWALLET = GetBoolArg("firewalldebuginvalidwallet", FIREWALL_LIVEDEBUG_INVALIDWALLET);
    FIREWALL_LIVEDEBUG_FORKEDWALLET = GetBoolArg("firewalldebugforkedwallet", FIREWALL_LIVEDEBUG_FORKEDWALLET);
    FIREWALL_LIVEDEBUG_FLOODINGWALLET = GetBoolArg("firewalldebugfloodingwallet", FIREWALL_LIVEDEBUG_FLOODINGWALLET);

    // *** Firewall Controls (Bandwidth Abuse) ***
    FIREWALL_DETECT_BANDWIDTHABUSE = GetBoolArg("firewalldetectbandwidthabuse", FIREWALL_DETECT_BANDWIDTHABUSE);
    FIREWALL_BLACKLIST_BANDWIDTHABUSE = GetBoolArg("firewallblacklistbandwidthabuse", FIREWALL_BLACKLIST_BANDWIDTHABUSE);
    FIREWALL_BAN_BANDWIDTHABUSE = GetBoolArg("firewallbanbandwidthabuse", FIREWALL_BAN_BANDWIDTHABUSE);
    FIREWALL_NOFALSEPOSITIVE_BANDWIDTHABUSE = GetBoolArg("firewallnofalsepositivebandwidthabuse", FIREWALL_NOFALSEPOSITIVE_BANDWIDTHABUSE);

    // *** Firewall Controls (Invalid Peer Wallets) ***
    FIREWALL_DETECT_INVALIDWALLET = GetBoolArg("firewalldetectinvalidwallet", FIREWALL_DETECT_INVALIDWALLET);
    FIREWALL_BLACKLIST_INVALIDWALLET = GetBoolArg("firewallblacklistinvalidwallet", FIREWALL_BLACKLIST_INVALIDWALLET);
    FIREWALL_BAN_INVALIDWALLET = GetBoolArg("firewallbaninvalidwallet", FIREWALL_BAN_INVALIDWALLET);

    // *** Firewall Controls (Forked Peer Wallets) ***
    FIREWALL_DETECT_FORKEDWALLET = GetBoolArg("firewalldetectforkedwallet", FIREWALL_DETECT_FORKEDWALLET);
    FIREWALL_BLACKLIST_FORKEDWALLET = GetBoolArg("firewallblacklistforkedwallet", FIREWALL_BLACKLIST_FORKEDWALLET);
    FIREWALL_BAN_FORKEDWALLET = GetBoolArg("firewallbanforkedwallet", FIREWALL_BAN_FORKEDWALLET);

    // *** Firewall Controls (Flooding Peer Wallets) ***
    FIREWALL_DETECT_FLOODINGWALLET = GetBoolArg("firewalldetectfloodingwallet", FIREWALL_DETECT_FLOODINGWALLET);
    FIREWALL_BLACKLIST_FLOODINGWALLET = GetBoolArg("firewallblacklistfloodingwallet", FIREWALL_BLACKLIST_FLOODINGWALLET);
    FIREWALL_BAN_FLOODINGWALLET = GetBoolArg("firewallbanfloodingwallet", FIREWALL_BAN_FLOODINGWALLET);

    // * Firewall Settings (Exam) *
    FIREWALL_TRAFFIC_TOLERANCE = GetArg("firewalltraffictolerance", FIREWALL_TRAFFIC_TOLERANCE);
    FIREWALL_TRAFFIC_ZONE = GetArg("firewalltrafficzone", FIREWALL_TRAFFIC_ZONE);

    // * Firewall Settings (Bandwidth Abuse) *
    FIREWALL_BANTIME_BANDWIDTHABUSE = GetArg("firewallbantimebandwidthabuse", FIREWALL_BANTIME_BANDWIDTHABUSE);
    FIREWALL_BANDWIDTHABUSE_MAXCHECK = GetArg("firewallbandwidthabusemaxcheck", FIREWALL_BANDWIDTHABUSE_MAXCHECK);
    FIREWALL_BANDWIDTHABUSE_MINATTACK = GetArg("firewallbandwidthabuseminattack", FIREWALL_BANDWIDTHABUSE_MINATTACK);
    FIREWALL_BANDWIDTHABUSE_MAXATTACK = GetArg("firewallbandwidthabusemaxattack", FIREWALL_BANDWIDTHABUSE_MAXATTACK);

    // * Firewall Settings (Invalid Wallet)
    FIREWALL_MINIMUM_PROTOCOL = GetArg("firewallinvalidwalletminprotocol", FIREWALL_MINIMUM_PROTOCOL);
    FIREWALL_BANTIME_INVALIDWALLET = GetArg("firewallbantimeinvalidwallet", FIREWALL_BANTIME_INVALIDWALLET);
    FIREWALL_INVALIDWALLET_MAXCHECK = GetArg("firewallinvalidwalletmaxcheck", FIREWALL_INVALIDWALLET_MAXCHECK);

    // * Firewall Settings (Forked Wallet)
    FIREWALL_BANTIME_FORKEDWALLET = GetArg("firewallbantimeforkedwallet", FIREWALL_BANTIME_FORKEDWALLET);

    // * Firewall Settings (Flooding Wallet)
    FIREWALL_BANTIME_FLOODINGWALLET = GetArg("firewallbantimefloodingwallet", FIREWALL_BANTIME_FLOODINGWALLET);
    FIREWALL_FLOODINGWALLET_MINBYTES = GetArg("firewallfloodingwalletminbytes", FIREWALL_FLOODINGWALLET_MINBYTES);
    FIREWALL_FLOODINGWALLET_MAXBYTES = GetArg("firewallfloodingwalletmaxbytes", FIREWALL_FLOODINGWALLET_MAXBYTES);

    if (GetArg("firewallfloodingwalletattackpattern", "") != "")
    {
        FIREWALL_FLOODPATTERNS[CountStringArray(FIREWALL_FLOODPATTERNS)] = GetArg("firewallfloodingwalletattackpattern", "");
    }

    FIREWALL_FLOODINGWALLET_MINTRAFFICAVERAGE = GetArg("firewallfloodingwalletmintrafficavg", FIREWALL_FLOODINGWALLET_MINTRAFFICAVERAGE);
    FIREWALL_FLOODINGWALLET_MAXTRAFFICAVERAGE = GetArg("firewallfloodingwalletmaxtrafficavg", FIREWALL_FLOODINGWALLET_MAXTRAFFICAVERAGE);
    FIREWALL_FLOODINGWALLET_MINCHECK = GetArg("firewallfloodingwalletmincheck", FIREWALL_FLOODINGWALLET_MINCHECK);
    FIREWALL_FLOODINGWALLET_MAXCHECK = GetArg("firewallfloodingwalletmaxcheck", FIREWALL_FLOODINGWALLET_MAXCHECK);

return;
}


// * Function: ForceDisconnectNode *
bool ForceDisconnectNode(CNode *pnode, string FromFunction)
{
    TRY_LOCK(pnode->cs_vSend, lockSend);
    if (lockSend){

        // release outbound grant (if any)
        pnode->CloseSocketDisconnect();
        LogPrintf("net, %s (%s) Panic Disconnect: %s\n", ModuleName.c_str(), FromFunction, pnode->addrName.c_str());

       if ( FIREWALL_LIVE_DEBUG == true)
       {
            if ( FIREWALL_LIVEDEBUG_DISCONNECT == true)
            {
                 cout << ModuleName << "Panic Disconnect: " << pnode->addrName << "]\n" << endl;
            }
        }

        return true;
    }
    else {
        pnode->vSendMsg.end();
    }

    return false;
}


// * Function: CheckBlackList *
bool CheckBlackList(CNode *pnode)
{
    int i;
    int TmpBlackListCount;
    TmpBlackListCount = CountStringArray(FIREWALL_BLACKLIST);


    if ( fDebug ){
       for ( i = 0; i < TmpBlackListCount; i++) {
           LogPrintf("*** RGP CheckBlackList Debug, BANNED %s \n ", FIREWALL_BLACKLIST[i] );
       }
    }

    if (TmpBlackListCount > 0) {

        for ( i = 0; i < TmpBlackListCount; i++) {
            if ( pnode->addrName == FIREWALL_BLACKLIST[i]) {
                 // Banned IP FOUND!
                 return true;
            }
        }
    }

    // Banned IP not found
    return false;
}


// * Function:  *
bool CheckBanned(CNode *pnode)
{

    if ( CNode::IsBanned( pnode->addr ) == true ) {
         // Yes Banned!

         return true;
    }

    // Not Banned!
    return false;
}


// * Function: AddToBlackList *
bool AddToBlackList(CNode *pnode)
{
    int TmpBlackListCount;
    TmpBlackListCount = CountStringArray(FIREWALL_BLACKLIST);

        // Restart Blacklist count
        if (TmpBlackListCount >  255)
        {
            TmpBlackListCount = 0;
        }

        if (CheckBlackList(pnode) == false)
        {
            // increase Blacklist count
            TmpBlackListCount = TmpBlackListCount + 1;
            // Add node IP to blacklist
            FIREWALL_BLACKLIST[TmpBlackListCount] = pnode->addrName;

            if (FIREWALL_LIVE_DEBUG == true)
            {
                if (FIREWALL_LIVEDEBUG_BLACKLIST == true)
                {
                    cout << ModuleName << "Blacklisted: " << pnode->addrName << "]\n" << endl;
                }
            }

            // Append Blacklist to debug.log
            LogPrint("net", "%s Blacklisted: %s\n", ModuleName.c_str(), pnode->addrName.c_str());

return true;
        }

return false;
}


// * Function: AddToBanList *
bool AddToBanList(CNode *pnode, BanReason BAN_REASON, int BAN_TIME)
{
    CNode::Ban(pnode->addr, BAN_REASON, BAN_TIME, false);
    DumpBanlist();

    LogPrint("net", "%s Banned: %s\n", ModuleName.c_str(), pnode->addrName);

    if (FIREWALL_LIVE_DEBUG == true)
    {
        if (FIREWALL_LIVEDEBUG_BANS == true)
        {
            cout << ModuleName << "Banned: " << pnode->addrName << "]\n" << endl;
        }
    }

return true;
}


// * Function: CheckAttack *
// Artificially Intelligent Attack Detection & Mitigation
bool CheckAttack(CNode *pnode, string FromFunction)
{
    bool DETECTED_ATTACK = false;
    
    bool BLACKLIST_ATTACK = false;

    int BAN_TIME = 0; // Default 24 hours
    bool BAN_ATTACK = false;

    BanReason BAN_REASON;

    string ATTACK_CHECK_NAME;
    string ATTACK_CHECK_LOG;

    int nTimeConnected = GetTime() - pnode->nTimeConnected;
    string ATTACK_TYPE = "";

    int NodeHeight;

    if (pnode->nSyncHeight == 0)
    {
        NodeHeight = pnode->nStartingHeight;
    }
    else
    {
        NodeHeight = pnode->nSyncHeight;
    }

    if (pnode->nSyncHeight < pnode->nStartingHeight)
    {
        NodeHeight = pnode->nStartingHeight;
    }
   

    // ---Filter 1 -------------
    if (FIREWALL_DETECT_BANDWIDTHABUSE == true)
    {
        ATTACK_CHECK_NAME = "Bandwidth Abuse";

        // ### Attack Detection ###
        // Calculate the ratio between Recieved bytes and Sent Bytes
        // Detect a valid syncronizaion vs. a flood attack
        
        if (nTimeConnected > FIREWALL_BANDWIDTHABUSE_MAXCHECK)
        {
            // * Attack detection #2
            // Node is further ahead on the chain than average minimum
            if (NodeHeight > Firewall_AverageHeight_Min)
            {
                if (pnode->nTrafficAverage < Firewall_AverageTraffic_Min)
                {
                    // too low bandiwidth ratio limits
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = "2-LowBW-HighHeight";
                }

                if (pnode->nTrafficAverage > Firewall_AverageTraffic_Max)
                {
                    // too high bandiwidth ratio limits
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = "2-HighBW-HighHeight";
                }
            }

            // * Attack detection #3
            // Node is behind on the chain than average minimum
            if (NodeHeight < Firewall_AverageHeight_Min)
            {  
                if (pnode->nTrafficAverage < Firewall_AverageTraffic_Min)
                {
                    // too low bandiwidth ratio limits
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = "3-LowBW-LowHeight";
                }

                if (pnode->nTrafficAverage > Firewall_AverageTraffic_Max)
                {

                    // too high bandiwidth ratio limits
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = "3-HighBW-LowHeight";
                }
            }
        }

        if (FIREWALL_LIVEDEBUG_BANDWIDTHABUSE == true)
        {
            ATTACK_CHECK_LOG = ATTACK_CHECK_LOG  + " {" +  ATTACK_CHECK_NAME + ":" + BoolToString(DETECTED_ATTACK) + "}";
        }

        // ### Attack Mitigation ###
        if (DETECTED_ATTACK == true)
        {
            if (FIREWALL_BLACKLIST_BANDWIDTHABUSE == true)
            {
                BLACKLIST_ATTACK = true;
            }

            if (FIREWALL_BAN_BANDWIDTHABUSE == true)
            {
                BAN_ATTACK = true;
                BAN_TIME = FIREWALL_BANTIME_BANDWIDTHABUSE;
                BAN_REASON = BanReasonBandwidthAbuse;
            }

        }
        // ##########################
    }
    // ----------------

    if (FIREWALL_NOFALSEPOSITIVE_BANDWIDTHABUSE == true)
    {
        ATTACK_CHECK_NAME = "No False Positive - Bandwidth Abuse";

        // ### AVOID FALSE POSITIVE FROM BANDWIDTH ABUSE ###
        if (DETECTED_ATTACK == true)
        {

            if (ATTACK_TYPE == "2-LowBW-HighHeight")
            {
                ATTACK_TYPE = "";
                DETECTED_ATTACK = false;
            }   

            if (ATTACK_TYPE == "2-HighBW-HighHeight")
            {
                // Node/peer is in wallet sync (catching up to full blockheight)
                ATTACK_TYPE = "";
                DETECTED_ATTACK = false;
            }

            if (ATTACK_TYPE == "3-LowBW-LowHeight")
            {
                ATTACK_TYPE = "";
                DETECTED_ATTACK = false;
            }   

            if (ATTACK_TYPE == "3-HighBW-LowHeight")
            {
                double tnTraffic = pnode->nSendBytes / pnode->nRecvBytes;
                if (pnode->nTrafficAverage < Firewall_AverageTraffic_Max)
                {
                    if (tnTraffic < FIREWALL_BANDWIDTHABUSE_MINATTACK || tnTraffic > FIREWALL_BANDWIDTHABUSE_MAXATTACK)
                    {
                        // wallet full sync
                        ATTACK_TYPE = "";
                        DETECTED_ATTACK = false;
                    }
                }

                if (pnode->nSendBytes > pnode->nRecvBytes)
                {
                    // wallet full sync
                    ATTACK_TYPE = "";
                    DETECTED_ATTACK = false;
                }
            }   
        }
        
        if (FIREWALL_LIVEDEBUG_NOFALSEPOSITIVE == true)
        {
            ATTACK_CHECK_LOG = ATTACK_CHECK_LOG  + " {" +  ATTACK_CHECK_NAME + ":" + BoolToString(DETECTED_ATTACK) + "}";
        }

        // ##########################
    }
    // ----------------

    // ---Filter 2-------------
    if (FIREWALL_DETECT_INVALIDWALLET == true)
    {
        ATTACK_CHECK_NAME = "Invalid Wallet";

        // ### Attack Detection ###
        // Start Height = -1
        // Check for more than FIREWALL_INVALIDWALLET_MAXCHECK minutes connection length
        if (nTimeConnected > FIREWALL_INVALIDWALLET_MAXCHECK)
        {
            // Check for -1 blockheight
            if (pnode->nStartingHeight == -1)
            {
                // Trigger Blacklisting
                DETECTED_ATTACK = true;
                ATTACK_TYPE = "1-StartHeight-Invalid";
            }
        }

        // Check for -1 blockheight
        if (nTimeConnected > FIREWALL_INVALIDWALLET_MAXCHECK)
        {
            // Check for -1 blockheight
            if (pnode->nStartingHeight < 0)
            {
                // Trigger Blacklisting
                DETECTED_ATTACK = true;
                ATTACK_TYPE = "1-StartHeight-Invalid";
            }
        }
        
        // (Protocol: 0
        // Check for more than FIREWALL_INVALIDWALLET_MAXCHECK minutes connection length
        if (nTimeConnected > FIREWALL_INVALIDWALLET_MAXCHECK)
        {
            // Check for 0 protocol
            if (pnode->nRecvVersion == 0)
            {
                // Trigger Blacklisting
                DETECTED_ATTACK = true;
                ATTACK_TYPE = "1-Protocol-Invalid";
            }
        }

        // (Protocol: 0
        // Check for more than FIREWALL_INVALIDWALLET_MAXCHECK minutes connection length
        if (nTimeConnected > FIREWALL_INVALIDWALLET_MAXCHECK)
        {
            // Check for 
            if (pnode->nRecvVersion < 1)
            {
                // Trigger Blacklisting
                DETECTED_ATTACK = true;
                ATTACK_TYPE = "1-Protocol-Invalid";
            }
        }

        //// Resetting sync Height
        //if (nTimeConnected > 60)
        //{
            //if (pnode->nSyncHeight > pnode->nSyncHeightOld)
            //{
                //pnode->nSyncHeightOld = pnode->nSyncHeight;
            //}

            //if (pnode->nSyncHeight < pnode->nSyncHeightOld - FIREWALL_AVERAGE_RANGE)
            //{
                // Trigger Blacklisting
                //DETECTED = true;
                //ATTACK_TYPE = "1-SyncReset";
            //}

        //}
        // ##########################

        if (FIREWALL_LIVEDEBUG_INVALIDWALLET == true)
        {
            ATTACK_CHECK_LOG = ATTACK_CHECK_LOG  + " {" +  ATTACK_CHECK_NAME + ":" + BoolToString(DETECTED_ATTACK) + "}";
        }

        // ### Attack Mitigation ###
        if (DETECTED_ATTACK == true)
        {
            if (FIREWALL_BLACKLIST_INVALIDWALLET == true)
            {
                BLACKLIST_ATTACK = true;
            }

            if (FIREWALL_BAN_INVALIDWALLET == true)
            {
                BAN_ATTACK = true;
                BAN_TIME = FIREWALL_BANTIME_INVALIDWALLET;
                BAN_REASON = BanReasonInvalidWallet;
            }
        }
        // ##########################
    }
    //--------------------------


    // ---Filter 3-------------
    if (FIREWALL_DETECT_FORKEDWALLET == true)
    {

        ATTACK_CHECK_NAME = "Forked Wallet";

        // ### Attack Detection ###

        int i;
        int TmpNodeHeightCount;
        TmpNodeHeightCount = CountIntArray(FIREWALL_FORKED_NODEHEIGHT) - 2;
        
        if (TmpNodeHeightCount > 0)
        {
            for (i = 0; i < TmpNodeHeightCount; i++)
            { 
                // Check for Forked Wallet (stuck on blocks)
                if (pnode->nStartingHeight == (int)FIREWALL_FORKED_NODEHEIGHT[i])
                {
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = ATTACK_CHECK_NAME;
                }
                // Check for Forked Wallet (stuck on blocks)
                if (pnode->nSyncHeight == (int)FIREWALL_FORKED_NODEHEIGHT[i])
                {
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = ATTACK_CHECK_NAME;
                }
            }          
        }
        // #######################

        // ### LIVE DEBUG OUTPUT ####
        if (FIREWALL_LIVEDEBUG_FORKEDWALLET == true)
        {
            ATTACK_CHECK_LOG = ATTACK_CHECK_LOG  + " {" +  ATTACK_CHECK_NAME + ":" + BoolToString(DETECTED_ATTACK) + "}";
        }
        // #######################

        // ### Attack Mitigation ###
        if (DETECTED_ATTACK == true)
        {
            if (FIREWALL_BLACKLIST_FORKEDWALLET == true)
            {
                BLACKLIST_ATTACK = true;
            }

            if (FIREWALL_BAN_FORKEDWALLET == true)
            {
                BAN_ATTACK = true;

                BAN_TIME = FIREWALL_BANTIME_FORKEDWALLET;
                BAN_REASON = BanReasonForkedWallet;
            }
        }
        // #######################

    }
    //--------------------------


    // ---Filter 4-------------
    if (FIREWALL_DETECT_FLOODINGWALLET == true)
    {
        ATTACK_CHECK_NAME = "Flooding Wallet";
        std::size_t FLOODING_MAXBYTES = FIREWALL_FLOODINGWALLET_MAXBYTES;
        std::size_t FLOODING_MINBYTES = FIREWALL_FLOODINGWALLET_MINBYTES;
        
        string WARNINGS = "";

        // WARNING #1 - Too high of bandwidth with low BlockHeight
        if (NodeHeight < Firewall_AverageHeight_Min)
        {  
            if (pnode->nTrafficAverage > Firewall_AverageTraffic_Max)
            {
                WARNINGS = WARNINGS + "1";
            }
        }
        
        // WARNING #2 - Send Bytes below minimum
        if (pnode->nSendBytes < FLOODING_MINBYTES)
        {
            WARNINGS = WARNINGS + "2";
        }

        // WARNING #3 - Send Bytes above minimum
        if (pnode->nSendBytes < FLOODING_MINBYTES)
        {
            WARNINGS = WARNINGS + "3";
        }

        // WARNING #4 - Send Bytes below maximum
        if (pnode->nSendBytes < FLOODING_MAXBYTES)
        {
            WARNINGS = WARNINGS + "4";
        }

        // WARNING #5 - Send Bytes above maximum
        if (pnode->nSendBytes > FLOODING_MAXBYTES)
        {
            WARNINGS = WARNINGS + "5";
        }

        // WARNING #6 - Recv Bytes above min 
        if (pnode->nRecvBytes > FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "6";
        }

        // WARNING #7 - Recv Bytes below min
        if (pnode->nRecvBytes < FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "7";
        }

        // WARNING #8 - Recv Bytes above max 
        if (pnode->nRecvBytes > FLOODING_MAXBYTES / 2)
        {
            WARNINGS = WARNINGS + "8";
        }

        // WARNING #9 - Recv Bytes below max
        if (pnode->nRecvBytes < FLOODING_MAXBYTES / 2)
        {
            WARNINGS = WARNINGS + "9";
        }

        // WARNING #10 - Recv Bytes above min 
        if (pnode->nSendBytes > FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "10";
        }

        // WARNING #11 - Recv Bytes below min
        if (pnode->nSendBytes < FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "11";
        }

        // WARNING #12 - Recv Bytes above max 
        if (pnode->nSendBytes > FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "12";
        }

        // WARNING #13 - Recv Bytes below max
        if (pnode->nSendBytes < FLOODING_MINBYTES / 2)
        {
            WARNINGS = WARNINGS + "13";
        }

        // WARNING #14 - 
        if (pnode->nTrafficAverage > FIREWALL_FLOODINGWALLET_MINTRAFFICAVERAGE)
        {
            WARNINGS = WARNINGS + "14";
        }

        // WARNING #15 - 
        if (pnode->nTrafficAverage < FIREWALL_FLOODINGWALLET_MINTRAFFICAVERAGE)
        {
            WARNINGS = WARNINGS + "15";
        }

        // WARNING #16 - 
        if (pnode->nTrafficAverage > FIREWALL_FLOODINGWALLET_MAXTRAFFICAVERAGE)
        {
            WARNINGS = WARNINGS + "16";
        }

        // WARNING #17 - 
        if (pnode->nTrafficAverage < FIREWALL_FLOODINGWALLET_MAXTRAFFICAVERAGE)
        {
            WARNINGS = WARNINGS + "17";
        }

        // WARNING #18 - Starting Height = SyncHeight above max
        if (pnode->nStartingHeight == pnode->nSyncHeight)
        {
            WARNINGS = WARNINGS + "18";
        }

        // WARNING #19 - Connected Time above min
        if (nTimeConnected > FIREWALL_FLOODINGWALLET_MINCHECK * 60)
        {
            WARNINGS = WARNINGS + "19";
        }

        // WARNING #20 - Connected Time below min
        if (nTimeConnected < FIREWALL_FLOODINGWALLET_MINCHECK * 60)
        {
            WARNINGS = WARNINGS + "20";
        }

        // WARNING #21 - Connected Time above max
        if (nTimeConnected > FIREWALL_FLOODINGWALLET_MAXCHECK * 60)
        {
            WARNINGS = WARNINGS + "21";
        }

        // WARNING #22 - Connected Time below max
        if (nTimeConnected < FIREWALL_FLOODINGWALLET_MAXCHECK * 60)
        {
            WARNINGS = WARNINGS + "22";
        }

        // WARNING #23 - Current BlockHeight
        if (NodeHeight > Firewall_AverageHeight)
        {  
            if (NodeHeight < Firewall_AverageHeight_Max)
            {  
                WARNINGS = WARNINGS + "23";
            }
        }

        // WARNING #24 - 
        if (pnode->nSyncHeight < Firewall_AverageTraffic_Max)
        {
            if (pnode->nSyncHeight > Firewall_AverageHeight_Min)
            {
                WARNINGS = WARNINGS + "24";
            }
        }

        // WARNING #25 - 
        if (DETECTED_ATTACK == true)
        {
            WARNINGS = WARNINGS + "25";
        }      
    
        // IF WARNINGS is matches pattern for ATTACK = TRUE
        int i;
        int TmpFloodPatternsCount;

        TmpFloodPatternsCount = CountStringArray(FIREWALL_FLOODPATTERNS);

        if (TmpFloodPatternsCount > 0)
        {
            for (i = 0; i < TmpFloodPatternsCount; i++)
            {  
                // Check for Static Whitelisted Seed Node
                if (WARNINGS == FIREWALL_FLOODPATTERNS[i])
                {
                    DETECTED_ATTACK = true;
                    ATTACK_TYPE = ATTACK_CHECK_NAME;
                }

            }
        }

        // ### LIVE DEBUG OUTPUT ####
        if (FIREWALL_LIVEDEBUG_FLOODINGWALLET == true)
        {
            ATTACK_CHECK_LOG = ATTACK_CHECK_LOG  + " {" +  ATTACK_CHECK_NAME + ":" + WARNINGS + ":" + BoolToString(DETECTED_ATTACK) + "}";
        }
        // #######################

        if (DETECTED_ATTACK == true)
        {
            if (FIREWALL_BLACKLIST_FLOODINGWALLET == true)
            {
                BLACKLIST_ATTACK = true;
            }

            if (FIREWALL_BAN_FLOODINGWALLET == true)
            {
                BAN_ATTACK = true;
                BAN_TIME = FIREWALL_BANTIME_FLOODINGWALLET;
                BAN_REASON = BanReasonFloodingWallet;
            }

        }
    }
    //--------------------------

    // ---Filter 5-------------
    //if (DETECT_HIGH_BANSCORE == true)
    //{
        //DETECTED_ATTACK = false;

        //nMisbehavior
        //checkbanned function integration *todo*

        //if (DETECTED_ATTACK == true)
        //{
            //if (BLACKLIST_HIGH_BANSCORE == true)
            //{
                //BLACKLIST_ATTACK = true;
            //}

            //if (BAN_HIGH_BANSCORE == true)
            //{
                //BAN_ATTACK = true;
                //BAN_TIME = BANTIME_HIGH_BANSCORE;
            //}

        //}
    //}
    //--------------------------

        if (FIREWALL_LIVE_DEBUG == true)
        {
            cout << ModuleName << " [Checking: " << pnode->addrName << "] [Attacks: " << ATTACK_CHECK_LOG << "]\n" << endl;
        }

    // ----------------
    // ATTACK DETECTED (TRIGGER)!
    if (DETECTED_ATTACK == true)
    {
        if (FIREWALL_LIVE_DEBUG == true)
        {
            cout << ModuleName << " [Attack Type: " << ATTACK_TYPE << "] [Detected from: " << pnode->addrName << "] [Node Traffic: " << pnode->nTrafficRatio << "] [Node Traffic Avrg: " << pnode->nTrafficAverage << "] [Traffic Avrg: " << Firewall_AverageTraffic << "] [Sent Bytes: " << pnode->nSendBytes << "] [Recv Bytes: " << pnode->nRecvBytes << "] [Sync Height: " << pnode->nSyncHeight << "] [Protocol: " << pnode->nRecvVersion <<"\n" << endl;
        }

        LogPrint("net", "%s [Attack Type: %s] [Detected from: %s] [Node Traffic: %d] [Node Traffic Avrg: %d] [Traffic Avrg: %d] [Sent Bytes: %d] [Recv Bytes: %d] [Sync Height: %i] [Protocol: %i\n", ModuleName.c_str(), ATTACK_TYPE.c_str(), pnode->addrName.c_str(), pnode->nTrafficRatio, pnode->nTrafficAverage, Firewall_AverageTraffic, pnode->nSendBytes, pnode->nRecvBytes, pnode->nSyncHeight, pnode->nRecvVersion);

        // Blacklist IP on Attack detection
        // * add node/peer IP to blacklist
        if (BLACKLIST_ATTACK == true)
        {
            AddToBlackList(pnode);
        }

        // Peer/Node Ban if required
        if (BAN_ATTACK == true)
        {
            AddToBanList(pnode, BAN_REASON, BAN_TIME);
        }

        // Peer/Node Panic Disconnect
        ForceDisconnectNode(pnode, FromFunction);

// ATTACK DETECTED!
return true;

    }
    else
    {

//NO ATTACK DETECTED...
return false;
    }
    // ----------------
}

// * Function: Examination *
void Examination(CNode *pnode, string FromFunction)
{
// Calculate new Height Average from all peers connected

    bool UpdateNodeStats = false;
    int NodeHeight;

    if (pnode->nSyncHeight == 0)
    {
        NodeHeight = pnode->nStartingHeight;
    }
    else
    {
        NodeHeight = pnode->nSyncHeight;
    }

    if (pnode->nSyncHeight < pnode->nStartingHeight)
    {
        NodeHeight = pnode->nStartingHeight;
    }


    // ** Update current average if increased ****
    if (NodeHeight > Firewall_AverageHeight) 
    {
        Firewall_AverageHeight = Firewall_AverageHeight + NodeHeight; 
        Firewall_AverageHeight = Firewall_AverageHeight / 2;
        Firewall_AverageHeight = Firewall_AverageHeight - FIREWALL_AVERAGE_TOLERANCE;      // reduce with tolerance
        Firewall_AverageHeight_Min = Firewall_AverageHeight - FIREWALL_AVERAGE_RANGE;
        Firewall_AverageHeight_Max = Firewall_AverageHeight + FIREWALL_AVERAGE_RANGE;
    }

    if (pnode->nRecvBytes > 0)
    {
        pnode->nTrafficRatio = pnode->nSendBytes / (double)pnode->nRecvBytes;

        if (pnode->nTrafficTimestamp == 0)
        {
            UpdateNodeStats = true;
        }

        if (GetTime() - pnode->nTrafficTimestamp > 5){
            UpdateNodeStats = true;
        }

            pnode->nTrafficAverage = pnode->nTrafficAverage + (double)pnode->nTrafficRatio / 2;
            pnode->nTrafficTimestamp = GetTime();

        if (UpdateNodeStats == true)
        {   
            Firewall_AverageTraffic = Firewall_AverageTraffic + (double)pnode->nTrafficAverage;
            Firewall_AverageTraffic = Firewall_AverageTraffic / (double)2;
            Firewall_AverageTraffic = Firewall_AverageTraffic - (double)FIREWALL_TRAFFIC_TOLERANCE;      // reduce with tolerance
            Firewall_AverageTraffic_Min = Firewall_AverageTraffic - (double)FIREWALL_TRAFFIC_ZONE;
            Firewall_AverageTraffic_Max = Firewall_AverageTraffic + (double)FIREWALL_TRAFFIC_ZONE;    
            Firewall_AverageSend = Firewall_AverageSend + pnode->nSendBytes / vNodes.size();
            Firewall_AverageRecv = Firewall_AverageRecv + pnode->nRecvBytes / vNodes.size();        

            if (FIREWALL_LIVE_DEBUG == true)
            {
                if (FIREWALL_LIVEDEBUG_EXAM == true)
                {
                    cout << ModuleName << " [BlackListed Nodes/Peers: " << CountStringArray(FIREWALL_BLACKLIST) << "] [Traffic: " << Firewall_AverageTraffic << "] [Traffic Min: " << Firewall_AverageTraffic_Min << "] [Traffic Max: " << Firewall_AverageTraffic_Max << "]" << " [Safe Height: " << Firewall_AverageHeight << "] [Height Min: " << Firewall_AverageHeight_Min << "] [Height Max: " << Firewall_AverageHeight_Max <<"] [Send Avrg: " << Firewall_AverageSend<< "] [Rec Avrg: " << Firewall_AverageRecv << "]\n" <<endl;

                    cout << ModuleName << "[Check Node IP: " << pnode->addrName.c_str() << "] [Traffic: " << pnode->nTrafficRatio << "] [Traffic Average: " << pnode->nTrafficAverage << "] [Starting Height: " << pnode->nStartingHeight << "] [Sync Height: " << NodeHeight << "] [Node Sent: " << pnode->nSendBytes << "] [Node Recv: " << pnode->nRecvBytes << "] [Protocol: " << pnode->nRecvVersion << "]\n" << endl;
                }

            }

        }

    CheckAttack(pnode, FromFunction);
    }
}

// * Function: FireWall *
bool FireWall(CNode *pnode, string FromFunction)
{

    if (Firewall_FirstRun == false)
    {
        Firewall_FirstRun = true;
        LoadFirewallSettings();
    }

    if (FIREWALL_ENABLED == false)
    {
        return false;
    }

    int i;
    int TmpWhiteListCount;

    TmpWhiteListCount = CountStringArray(FIREWALL_WHITELIST);

    if (TmpWhiteListCount > 0)
    {
        for (i = 0; i < TmpWhiteListCount; i++)
        {  

            // Check for Static Whitelisted Seed Node
            if (pnode->addrName == FIREWALL_WHITELIST[i])
            {
               return false;
            }
        }
    }

    if (FIREWALL_CLEAR_BANS == true)
    {
        if (FIREWALL_CLEARBANS_MINNODES <= vNodes.size())
        {
            pnode->ClearBanned();
            int TmpBlackListCount;
            TmpBlackListCount = CountStringArray(FIREWALL_BLACKLIST);
            std::fill_n(FIREWALL_BLACKLIST, TmpBlackListCount, 0);
            LogPrint("net", "%s Cleared ban: %s\n", ModuleName.c_str(), pnode->addrName.c_str());
        }
    }

    if (CheckBlackList(pnode) == true)
    {
        FromFunction = "CheckBlackList";

        LogPrint("net", "%s Disconnected Blacklisted IP: %s\n", ModuleName.c_str(), pnode->addrName.c_str());

// Peer/Node Panic Disconnect
ForceDisconnectNode(pnode, FromFunction);
return true;

    }

    if (CheckBanned(pnode) == true)
    {
        FromFunction = "CheckBanned";

        LogPrint("net", "%s Disconnected Banned IP: %s\n", ModuleName.c_str(), pnode->addrName.c_str());

// Peer/Node Panic Disconnect
ForceDisconnectNode(pnode, FromFunction);
return true;

    }

    // Perform a Node consensus examination
    Examination(pnode, FromFunction);

// Peer/Node Safe    
return false;
}

// |||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||||

void AddOneShot(string strDest)
{
    LOCK(cs_vOneShots);
    vOneShots.push_back(strDest);
}

unsigned short GetListenPort()
{
unsigned short myport;


myport = (unsigned short)(GetArg("port", Params().GetDefaultPort()));

    return (unsigned short)(GetArg("port", Params().GetDefaultPort()));
}

// find 'best' local address for a particular peer
bool GetLocal(CService& addr, const CNetAddr *paddrPeer)
{
    if (fNoListen)
    {
        return false;
    }

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

// get best local address for a particular peer as a CAddress
CAddress GetLocalAddress(const CNetAddr *paddrPeer)
{
    CAddress ret(CService("0.0.0.0",0),0);
    CService addr;
    if (GetLocal(addr, paddrPeer))
    {
        ret = CAddress(addr);
        ret.nServices = nLocalServices;
        ret.nTime = GetAdjustedTime();
    }
    return ret;
}

bool RecvLine(SOCKET hSocket, string& strLine)
{
    strLine = "";
    while (true)
    {
        char c;
        int nBytes = recv(hSocket, &c, 1, 0);
        if (nBytes > 0)
        {
            if (c == '\n')
                continue;
            if (c == '\r')
                return true;
            strLine += c;
            if (strLine.size() >= 9000)
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
                    MilliSleep(100); //50
                    continue;
                }
            }
            if (!strLine.empty())
                return true;
            if (nBytes == 0)
            {
                // socket closed
                LogPrint("net", "socket closed\n");
                return false;
            }
            else
            {
                // socket error
                int nErr = WSAGetLastError();
                LogPrint("net", "recv failed: %s\n", nErr);
                return false;
            }
        }
    }
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

// used when scores of local addresses may have changed
// pushes better local address to peers

void AdvertizeLocal(CNode* pnode)
{
    if (fListen && pnode->fSuccessfullyConnected) {
        CAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
                                              GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0)) {
            addrLocal.SetIP(pnode->addrLocal);
        }
        if (addrLocal.IsRoutable()) {
            LogPrintf("AdvertizeLocal: advertizing address %s\n", addrLocal.ToString());
            pnode->PushAddress(addrLocal);
        }
    }
}



//void static AdvertizeLocal()
//{
//    LOCK(cs_vNodes);
//    BOOST_FOREACH(CNode* pnode, vNodes)
//    {
//        if (pnode->fSuccessfullyConnected)
//        {
//            CAddress addrLocal = GetLocalAddress(&pnode->addr);
//            if (addrLocal.IsRoutable() && (CService)addrLocal != (CService)pnode->addrLocal)
//            {
//                pnode->PushAddress(addrLocal);
//                pnode->addrLocal = addrLocal;
//            }
//        }
//    }
//}

// pushes our own address to a peer
//void AdvertizeLocal(CNode* pnode)
//{
//    if (fListen && pnode->fSuccessfullyConnected) {
//        CAddress addrLocal = GetLocalAddress(&pnode->addr);
        // If discovery is enabled, sometimes give our peer the address it
        // tells us that it sees us as in case it has a better idea of our
        // address than we do.
//        if (IsPeerAddrLocalGood(pnode) && (!addrLocal.IsRoutable() ||
//                                              GetRand((GetnScore(addrLocal) > LOCAL_MANUAL) ? 8 : 2) == 0)) {
//            addrLocal.SetIP(pnode->addrLocal);
//        }
//        if (addrLocal.IsRoutable()) {
//            LogPrintf("AdvertizeLocal: advertizing address %s\n", addrLocal.ToString());
//            pnode->PushAddress(addrLocal);
//        }
//    }
//}



void SetReachable(enum Network net, bool fFlag)
{
    LOCK(cs_mapLocalHost);
    vfReachable[net] = fFlag;
    if (net == NET_IPV6 && fFlag)
        vfReachable[NET_IPV4] = true;
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

    LogPrintf("AddLocal(%s,%i)\n", addr.ToString(), nScore);

    {
        LOCK(cs_mapLocalHost);
        bool fAlready = mapLocalHost.count(addr) > 0;
        LocalServiceInfo &info = mapLocalHost[addr];
        if (!fAlready || nScore >= info.nScore) {
            info.nScore = nScore + (fAlready ? 1 : 0);
            info.nPort = addr.GetPort();
        }
        SetReachable(addr.GetNetwork());
    }

    //AdvertizeLocal();

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

    LOCK(cs_mapLocalHost);
    if (mapLocalHost.count(addr) == 0)
        return false;

    mapLocalHost[addr].nScore++;

    return true;
}





/** check whether a given address is potentially local */
bool IsLocal(const CService& addr)
{
    LOCK(cs_mapLocalHost);
    return mapLocalHost.count(addr) > 0;
}

/* ---------------------
   -- RGP JIRA BSG-51 --
   ------------------------------------------------------------
   -- added IsReachable() overloaded function for rpcnet.cpp --
   ------------------------------------------------------------ */

/* check whether a given network is one we can probably connect to */
bool IsReachable(enum Network net )
{
    LOCK(cs_mapLocalHost);
    return !vfLimited[net];
}


/** check whether a given address is in a network we can probably connect to */
bool IsReachable(const CNetAddr& addr)
{
    LOCK(cs_mapLocalHost);
    enum Network net = addr.GetNetwork();
    return vfReachable[net] && !vfLimited[net];
}

void AddressCurrentlyConnected(const CService& addr)
{
    addrman.Connected(addr);
}


uint64_t CNode::nTotalBytesRecv = 0;
uint64_t CNode::nTotalBytesSent = 0;
CCriticalSection CNode::cs_totalBytesRecv;
CCriticalSection CNode::cs_totalBytesSent;

CNode* FindNode(const CNetAddr& ip)
{
    LOCK(cs_vNodes);

//LogPrintf("RGP Net FindNode Debug IP %s \n", ip.ToString() );

    BOOST_FOREACH(CNode* pnode, vNodes)
        if ((CNetAddr)pnode->addr == ip)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CSubNet& subNet)
{
    LOCK(cs_vNodes);

//LogPrintf("RGP Net FindNode Debug subNet  \n" );

    BOOST_FOREACH(CNode* pnode, vNodes)
    if (subNet.Match((CNetAddr)pnode->addr))
        return (pnode);
    return NULL;
}

CNode* FindNode(std::string addrName)
{
    LOCK(cs_vNodes);

//LogPrintf("RGP Net FindNode Debug addrName  \n" );

    BOOST_FOREACH(CNode* pnode, vNodes)
        if (pnode->addrName == addrName)
            return (pnode);
    return NULL;
}

CNode* FindNode(const CService& addr)
{
    LOCK(cs_vNodes);

//LogPrintf("RGP Net FindNode Debug addr %s vnode size %d \n", addr.ToString(), vNodes.size() );

    for (CNode* pnode : vNodes)
    {
//LogPrintf("RGP Net FindNode Debug pnodes in FindNode %s \n", pnode->addrName );
        //if (Params().NetworkID() == CBaseChainParams::REGTEST) {
            //if using regtest, just check the IP
        //    if ((CNetAddr)pnode->addr == (CNetAddr)addr)
        //        return (pnode);
        //} else {
        if ( pnode->addr == addr)
        {
//LogPrintf("RGP Net FindNode Debug TEST ALL FOUND addr %s \n", pnode->addrName );
             return (pnode);
        }
       // }
    }

    
//LogPrintf("RGP Net FindNode Debug NONE FOUND  \n" );
   
    return NULL;
}

//CNode* FindNode(const CService& addr)
//{
//    LOCK(cs_vNodes);
//    BOOST_FOREACH(CNode* pnode, vNodes)
//        if ((CService)pnode->addr == addr)
//            return (pnode);
//    return NULL;
//}





bool CheckNode(CAddress addrConnect)
{
    // Look for an existing connection. If found then just add it to masternode list.

LogPrintf("\n RGP Net FindNode Debug addrConnect %s \n", addrConnect.ToString().c_str() );

    CNode* pnode = FindNode((CService)addrConnect);
    if (pnode)
    {
LogPrintf("RGP Net CheckNode found! \n");
        return true;
    }
    else
        LogPrintf("RGP Net CheckNode NOT found! \n");

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;
    LogPrintf("*** RGP Establish Timeout Value \n");

    if (ConnectSocket(addrConnect, hSocket, 5000 ))
    {
        LogPrintf("net, connected masternode %s\n", addrConnect.ToString());
        closesocket(hSocket);
        
/*        // Set to non-blocking
#ifdef WIN32
        u_long nOne = 1;
        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
            LogPrintf("ConnectSocket() : ioctlsocket non-blocking setting failed, error %d\n", WSAGetLastError());
#else
        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
            LogPrintf("ConnectSocket() : fcntl non-blocking setting failed, error %d\n", errno);
#endif
        CNode* pnode = new CNode(hSocket, addrConnect, "", false);
        // Close connection
        pnode->CloseSocketDisconnect();
*/
        return true;
    }

    //if ( fDebug )
    {
       LogPrintf("*** RGP Net CheckNode %s failed \n", addrConnect.ToString() );
       //CNode::Ban(pnode->addr, BanReasonManuallyAdded );

       LogPrintf("net, connecting to masternode %s failed\n", addrConnect.ToString());
    }
    return false;
}

CNode* ConnectNode(CAddress addrConnect, const char* pszDest)
{
unsigned int connecting_port;

    //LogPrintf("*** RGP ConnectNode address %s port %s \n", addrConnect.ToStringIP(), addrConnect.ToStringPort());

    //LogPrintf("*** RGP IP port connection %d \n", addrConnect.GetPort());
    connecting_port = addrConnect.GetPort();

    if ( connecting_port == 23980 || connecting_port == 23981 )
    {
       // LogPrintf("*** RGP IP SUCCESS checking %d \n", addrConnect.GetPort());
    }
    else
    {
        //LogPrintf("*** RGP IP FAILED PORT incorrect [IGNORED] %d \n", addrConnect.GetPort());
        return NULL;
    }

    if (pszDest == NULL)
    {
        // we clean masternode connections in CMasternodeMan::ProcessMasternodeConnections()
        // so should be safe to skip this and connect to local Hot MN on CActiveMasternode::ManageStatus()
        if (IsLocal(addrConnect))
            return NULL;

        // Look for an existing connection
        CNode* pnode = FindNode((CService)addrConnect);
        if (pnode)
        {
            /* ------------------------------------------------------
               -- RGP, Take care of Masternodes aka DarkSendMaster --
               ------------------------------------------------------ */
           // if( fDarkSendMaster )
           // {

           //   pnode->fDarkSendMaster = true;
           // }

            pnode->AddRef();
            return pnode;
        }
    }

    /// debug print
    //LogPrintf("*** RGP, ConnectNode trying connection %s lastseen=%.1fhrs\n",
    //    pszDest ? pszDest : addrConnect.ToString(),
    //    pszDest ? 0.0 : (double)(GetAdjustedTime() - addrConnect.nTime) / 3600.0);

    // Connect
    SOCKET hSocket;
    bool proxyConnectionFailed = false;

   // LogPrintf("*** RGP IP to connect %s debug 1 \n", addrConnect.ToString() );
   // LogPrintf("*** RGP pszDest is %s \n", &pszDest );

    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort()) : ConnectSocket(addrConnect, hSocket))
    {

     //LogPrintf("*** RGP IP to connect %s debug 2 ", addrConnect.ToString() );

    //if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
    //              ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed))
    //{
        if (!IsSelectableSocket(hSocket))
        {
            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
            //CloseSocket(hSocket);
            return NULL;
        }

        addrman.Attempt(addrConnect);

        //LogPrintf("*** RGP connected %s\n", pszDest ? pszDest : addrConnect.ToString());

        // Add node
        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
        pnode->AddRef();
        {
            LOCK(cs_vNodes);
            vNodes.push_back(pnode);
        }

        pnode->nTimeConnected = GetTime();

        return pnode;

    }
    else if (!proxyConnectionFailed)
    {
        //LogPrintf("*** RGP IP to connect FAILED %s debug 1 \n", addrConnect.ToString() );

        // If connecting to the node failed, and failure is not caused by a problem connecting to
        // the proxy, mark this as an attempt.
        addrman.Attempt(addrConnect);
    }

    return NULL;
}



//CNode* ConnectNode(CAddress addrConnect, const char *pszDest, bool darkSendMaster)
//{

//    if (pszDest == NULL) {
//        if (IsLocal(addrConnect)){
//           return NULL;
//        }

        // Look for an existing connection
//        CNode* pnode = FindNode((CService)addrConnect);
//        if (pnode)
//        {
//            if(darkSendMaster)
//                pnode->fDarkSendMaster = true;
//
//            pnode->AddRef();
//            return pnode;
//        }
//    }

    // debug print
//    LogPrint("net", "trying connection %s lastseen=%.1fhrs\n",
//        pszDest ? pszDest : addrConnect.ToString(),
//        pszDest ? 0 : (double)(GetAdjustedTime() - addrConnect.nTime)/3600.0);

    // Connect
//    SOCKET hSocket;
//    bool proxyConnectionFailed = false;

//    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
//                 ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed)) {
//        if (!IsSelectableSocket(hSocket)) {
//            LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
//            CloseSocket(hSocket);
//            return NULL;
//        }

//    if (pszDest ? ConnectSocketByName(addrConnect, hSocket, pszDest, Params().GetDefaultPort(), nConnectTimeout, &proxyConnectionFailed) :
//                  ConnectSocket(addrConnect, hSocket, nConnectTimeout, &proxyConnectionFailed))
//    {

 //       if (!IsSelectableSocket(hSocket))
 //       {
 //           LogPrintf("Cannot create connection: non-selectable socket created (fd >= FD_SETSIZE ?)\n");
 //           CloseSocket(hSocket);
 //           return NULL;
//        }
//
//        addrman.Attempt(addrConnect);

//        LogPrintf("*** RGP connected %s\n", pszDest ? pszDest : addrConnect.ToString());

        // Set to non-blocking
//#ifdef WIN32
//        u_long nOne = 1;
//        if (ioctlsocket(hSocket, FIONBIO, &nOne) == SOCKET_ERROR)
//            LogPrintf("ConnectSocket() : ioctlsocket non-blocking setting failed, error %d\n", WSAGetLastError());
//#else
//        if (fcntl(hSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
//            LogPrintf("ConnectSocket() : fcntl non-blocking setting failed, error %d\n", errno);
//#endif

        // Add node
//        CNode* pnode = new CNode(hSocket, addrConnect, pszDest ? pszDest : "", false);
//        pnode->AddRef();
//        {
//            LOCK(cs_vNodes);
//            vNodes.push_back(pnode);
//        }

//        pnode->nTimeConnected = GetTime();
//        return pnode;
//    }
//    else
//    {
//        return NULL;
//    }
//}

void CNode::CloseSocketDisconnect()
{
    fDisconnect = true;
    if (hSocket != INVALID_SOCKET)
    {
        //LogPrintf("*** RGP, disconnecting node %s /n", addrName);
        closesocket(hSocket);
        hSocket = INVALID_SOCKET;
    }

    // in case this fails, we'll empty the recv buffer when the CNode is deleted
    TRY_LOCK(cs_vRecvMsg, lockRecv);
    if (lockRecv)
        vRecvMsg.clear();

    // if this was the sync node, we'll need a new one
    if (this == pnodeSync)
        pnodeSync = NULL;
}

void CNode::PushVersion()
{
    /// when NTP implemented, change to just nTime = GetAdjustedTime()
    int64_t nTime = (fInbound ? GetAdjustedTime() : GetTime());
    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService("0.0.0.0",0)));
    CAddress addrMe = GetLocalAddress(&addr);

    GetRandBytes((unsigned char*)&nLocalHostNonce, sizeof(nLocalHostNonce));
    LogPrint("net", "send version message: version %d, blocks=%d, us=%s, them=%s, peer=%s /n", PROTOCOL_VERSION, nBestHeight, addrMe.ToString(), addrYou.ToString(), addr.ToString());
    PushMessage("version", PROTOCOL_VERSION, nLocalServices, nTime, addrYou, addrMe,
                nLocalHostNonce, strSubVersion, nBestHeight);

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
    //bool nodestatus;          UNUSED?

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
    //bool tester;              UNUSED

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
    CSubNet subNet(addr.ToString()+(addr.IsIPv4() ? "/32" : "/128"));
    Ban(subNet, banReason, bantimeoffset, sinceUnixEpoch);
}

void CNode::Ban(const CSubNet& subNet, const BanReason &banReason, int64_t bantimeoffset, bool sinceUnixEpoch) {
    CBanEntry banEntry(GetTime());
    banEntry.banReason = banReason;
    if (bantimeoffset <= 0)
    {
        bantimeoffset = GetArg("bantime", 60*60*24); // Default 24-hour ban
        sinceUnixEpoch = false;
    }
    banEntry.nBanUntil = (sinceUnixEpoch ? 0 : GetTime() )+bantimeoffset;


    LOCK(cs_setBanned);
    if (setBanned[subNet].nBanUntil < banEntry.nBanUntil)
        setBanned[subNet] = banEntry;

    setBannedIsDirty = true;
}

bool CNode::Unban(const CNetAddr &addr) {
    CSubNet subNet(addr.ToString()+(addr.IsIPv4() ? "/32" : "/128"));
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

        MilliSleep( 1 ); /* RGP optimise */
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

#undef X
#define X(name) stats.name = name
void CNode::copyStats(CNodeStats &stats)
{
    stats.nodeid = this->GetId();
    X(nServices);
    X(nLastSend);
    X(nLastRecv);
    X(nTimeConnected);
    X(nTimeOffset);
    X(addrName);
    X(nVersion);
    X(cleanSubVer);
    X(strSubVer);
    X(fInbound);
    X(nStartingHeight);
    X(nSendBytes);
    X(nRecvBytes);
    stats.fSyncNode = (this == pnodeSync);

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

    // Raw ping time is in microseconds, but show it to user as whole seconds (Bitcoin users should be well used to small numbers with many decimal places by now :)
    stats.dPingTime = (((double)nPingUsecTime) / 1e6);
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
            vRecvMsg.push_back(CNetMessage(SER_NETWORK, nRecvVersion));

        CNetMessage& msg = vRecvMsg.back();

        // absorb network data
        int handled;
        if (!msg.in_data)
            handled = msg.readHeader(pch, nBytes);
        else
            handled = msg.readData(pch, nBytes);

        if (handled < 0)
                return false;

        pch += handled;
        nBytes -= handled;

        MilliSleep( 1 ); /* RGP optimise */

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
    catch (std::exception &e) {
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



static int LastRefreshstamp = 0;
static int RefreshesDone = 0;
static bool FirstCycle = true;

#define MAX_CONNECTIONS 1 /* was 8 */

void RefreshRecentConnections(int RefreshMinutes)
{

    if (vNodes.size() >= MAX_CONNECTIONS )
    {
        return;
    }

    time_t timer;
    int SecondsPassed = 0;
    int MinutesPassed = 0;
    int CurrentTimestamp = time(&timer);


    if (LastRefreshstamp > 0)
    {

        SecondsPassed = CurrentTimestamp - LastRefreshstamp;
        MinutesPassed = SecondsPassed / 60;

        if (MinutesPassed > RefreshMinutes - 2)
        {
             FirstCycle = false;
        }

    }
    else
    {
        LastRefreshstamp = CurrentTimestamp;
        return;
    }

if (FirstCycle == false)
{
    if (MinutesPassed < RefreshMinutes) 
    {
return;
    }
    else
    {

        RefreshesDone = RefreshesDone + 1;

        //cout<<"         Last refresh: "<<LastRefreshstamp<<endl;
        //cout<<"         Minutes ago: "<<MinutesPassed<<endl;
        //cout<<"         Peer/node refresh cycles: "<<RefreshesDone<<endl;

        LastRefreshstamp = CurrentTimestamp;

        // Load addresses for peers.dat
        int64_t nStart = GetTimeMillis();
        {
            CAddrDB adb;
            if (!adb.Read(addrman))
                LogPrintf("Invalid or missing peers.dat; recreating\n");
        }

        if ( fDebug ){
            LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman.size(), GetTimeMillis() - nStart);
        }

        const vector<CDNSSeedData> &vSeeds = Params().DNSSeeds();
        int found = 0;

        if ( fDebug ){
            LogPrintf("Loading addresses from DNS seeds (could take a while)\n");
        }

            BOOST_FOREACH(const CDNSSeedData &seed, vSeeds) 
            {
                if (HaveNameProxy()) {
                    AddOneShot(seed.host);
                } else {
                    vector<CNetAddr> vIPs;
                    vector<CAddress> vAdd;
                    if (LookupHost(seed.host.c_str(), vIPs))
                    {
                        BOOST_FOREACH(CNetAddr& ip, vIPs)
                        {
                            if (found < 16){
                            int nOneDay = 24*3600;
                            CAddress addr = CAddress(CService(ip, Params().GetDefaultPort()));
                            addr.nTime = GetTime() - 3*nOneDay - GetRand(4*nOneDay); // use a random age between 3 and 7 days old
                            vAdd.push_back(addr);
                            found++;
                            }
                            MilliSleep( 1 ); /* RGP optimise */

                        }
                    }
                    addrman.Add(vAdd, CNetAddr(seed.name, true));
                }

                MilliSleep( 1 ); /* RGP optimise */

            }

            if ( fDebug ){
                LogPrintf("%d addresses found from DNS seeds\n", found);
            }

            //DumpAddresses();

            CSemaphoreGrant grant(*semOutbound);
            boost::this_thread::interruption_point();

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

                    MilliSleep( 1 ); /* RGP optimise */

                }
            }

            int64_t nANow = GetAdjustedTime();

            int nTries = 0;
            while (true)
            {
                CAddress addr = addrman.Select();

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

                if (addrConnect.IsValid()){
                    OpenNetworkConnection(addrConnect, &grant);
                }

                MilliSleep( 1 ); /* RGP optimise */

            }

    }
}

// requires LOCK(cs_vSend)
void SocketSendData(CNode *pnode)
{
    std::deque<CSerializeData>::iterator it = pnode->vSendMsg.begin();

//LogPrintf("*** SocketSendData node %s \n", pnode->addr.ToString()  );
MilliSleep(1);

    while (it != pnode->vSendMsg.end())
    {

        FireWall(pnode, "SendData");

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

                if ( fDebug ){
                    LogPrintf("socket send error: interruption\n");
                }

                    // Disconnect node/peer if send/recv data becomes idle
                    if (GetTime() - pnode->nTimeConnected > 60)
                    {
                        if (GetTime() - pnode->nLastRecv > 60)
                        {
                            if (GetTime() - pnode->nLastSend < 60)
                            {
                                if ( fDebug )
                                {
                                    LogPrintf("Error: Unexpected idle interruption 1 %s\n", pnode->addrName);
                                }
                                pnode->CloseSocketDisconnect();
                            }
                        }
                    }


                break;
            }
        } else {
            if (nBytes < 0) {
                // error
                int nErr = WSAGetLastError();
                if (nErr != WSAEWOULDBLOCK && nErr != WSAEMSGSIZE && nErr != WSAEINTR && nErr != WSAEINPROGRESS)
                {
                    if ( fDebug ){
                        LogPrintf("socket send error %d socket %ld  \n", nErr, pnode->hSocket);
                    }

                    // Disconnect node/peer if send/recv data becomes idle
                    if (GetTime() - pnode->nTimeConnected > 90)
                    {
                        if (GetTime() - pnode->nLastRecv > 120)
                        {
                            if (GetTime() - pnode->nLastSend < 120)
                            {
                                if ( fDebug )
                                {
                                    LogPrintf("Error: Unexpected idle interruption 2 %s\n", pnode->addrName);
                                }
                                //pnode->CloseSocketDisconnect();
                            }
                        }
                    }

                }
            }
            // couldn't send anything at all
            if ( fDebug ){
                LogPrintf("socket send error: data failure socket %ld  \n", pnode->hSocket );
            }


            // Disconnect node/peer if send/recv data becomes idle
            if (GetTime() - pnode->nTimeConnected > 90)
            {
                if (GetTime() - pnode->nLastRecv > 120)
                {
                    if (GetTime() - pnode->nLastSend < 120)
                    {
                        if ( fDebug )
                        {
                            LogPrintf("Error: Unexpected idle interruption  3 %s\n", pnode->addrName);
                        }
                        //pnode->CloseSocketDisconnect();
                    }
                }
            }

            break;

        }

        MilliSleep( 1 ); /* RGP optimise */

    }

    if (it == pnode->vSendMsg.end()) {
        assert(pnode->nSendOffset == 0);
        assert(pnode->nSendSize == 0);
    }
    pnode->vSendMsg.erase(pnode->vSendMsg.begin(), it);
}

static list<CNode*> vNodesDisconnected;

void ThreadSocketHandler()
{
    extern volatile bool fRequestShutdown;
    unsigned int nPrevNodeCount = 0;
    int nErr;
    bool nodestatus;
    string ipAnalysis;
    string portinfo;

    while ( !fRequestShutdown )
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
                    // remove from vNodes
                    vNodes.erase(remove(vNodes.begin(), vNodes.end(), pnode), vNodes.end());

                    // release outbound grant (if any)
                    pnode->grantOutbound.Release();

                    // close socket and cleanup
                    pnode->CloseSocketDisconnect();

                    // hold in disconnected pool until all refs are released
                    if (pnode->fNetworkNode || pnode->fInbound)
                        pnode->Release();
                    vNodesDisconnected.push_back(pnode);
                }

                MilliSleep( 1 ); /* RGP optimise */
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

                MilliSleep( 1 ); /* RGP optimise */
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

        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket) {
            FD_SET(hListenSocket, &fdsetRecv);
            hSocketMax = max(hSocketMax, hListenSocket);
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

            MilliSleep( 1 ); /* RGP optimise */
        }

        int nSelect = select(have_fds ? hSocketMax + 1 : 0,
                             &fdsetRecv, &fdsetSend, &fdsetError, &timeout);
        boost::this_thread::interruption_point();

        if (nSelect == SOCKET_ERROR)
        {
            if (have_fds)
            {
                nErr = WSAGetLastError();

                if ( fDebug ){
                    LogPrintf("socket select error %d\n", nErr);
                }

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
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
        if (hListenSocket != INVALID_SOCKET && FD_ISSET(hListenSocket, &fdsetRecv))
        {
            struct sockaddr_storage sockaddr;
            socklen_t len = sizeof(sockaddr);
            SOCKET hSocket = accept(hListenSocket, (struct sockaddr*)&sockaddr, &len);
            CAddress addr;
            int nInbound = 0;

            if (hSocket != INVALID_SOCKET)
                if (!addr.SetSockAddr((const struct sockaddr*)&sockaddr))
                    LogPrintf("Warning: Unknown socket family\n");

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
                    LogPrintf("socket error accept failed: %d\n", nErr);
            }
            else if (nInbound >= nMaxConnections - MAX_OUTBOUND_CONNECTIONS)
            {
                /* RGP */
                LogPrintf("MAX CONNECTION connection from %s dropped (banned)\n", addr.ToString());
                closesocket(hSocket);
            }
            else if (CNode::IsBanned(addr))
            {
                /* RGP, Added some monitoring code, as low volume connections
                   at the start of the project was causing many bans to occur.
                   Once traffic improved there were no bans, will look at this in the
                   future if this happens again during the life of the coin.
                   Removed all BANNING code for now.                                      */


                nodestatus = false ;

                ipAnalysis = addr.ToStringIP();
                portinfo   = addr.ToStringPort();

                if ( portinfo == "23980" ) {
                    if ( fDebug ){
                        LogPrintf("*** RGP UNBANNED analise %s \n", ipAnalysis  );
                    }
                    nodestatus = CNode::Unban( addr );
                    /* Do not close the socket */

                    //bool CNode::Unban(const CNetAddr &addr) {
                    //    CSubNet subNet(addr.ToString()+(addr.IsIPv4() ? "/32" : "/128"));
                    //    return Unban(subNet);
                   // }
                   //CNode::IsBanned(addr)

                }
                else
                {
                    portinfo   = addr.ToStringPort();


                    /* UNBANN for now until rectified */
                    nodestatus = CNode::Unban( addr );

                    if ( portinfo == "23980" ) {


                    } else {

                        if ( fDebug ){
                            LogPrintf("connection from %s dropped (banned)\n", addr.ToString());
                        }
                        //closesocket(hSocket);

                    }
                }
            }
            else
            {
                // According to the internet TCP_NODELAY is not carried into accepted sockets
                // on all platforms.  Set it again here just to be sure.
                int set = 1;
#ifdef WIN32
                setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&set, sizeof(int));
#else
                setsockopt(hSocket, IPPROTO_TCP, TCP_NODELAY, (void*)&set, sizeof(int));
#endif

                LogPrint("net", "accepted connection %s\n", addr.ToString());
                CNode* pnode = new CNode(hSocket, addr, "", true);
                pnode->AddRef();
                {
                    LOCK(cs_vNodes);
                    vNodes.push_back(pnode);
                }
            }
        }


        //
        // Service each socket
        //
        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->AddRef();
        }
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
                    if (pnode->GetTotalRecvSize() > ReceiveFloodSize()) {
                        if (!pnode->fDisconnect)
                            LogPrintf("socket recv flood control disconnect (%u bytes)\n", pnode->GetTotalRecvSize());
                        pnode->CloseSocketDisconnect();
                    }
                    else {
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
                                    LogPrintf("socket recv error %d\n", nErr);
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
            if (pnode->vSendMsg.empty())
            {
                pnode->nLastSendEmpty = GetTime();
            }

            if (GetTime() - pnode->nTimeConnected > IDLE_TIMEOUT)
            {
                if (pnode->nLastRecv == 0 || pnode->nLastSend == 0)
                {
                    LogPrint("net", "socket no message in timeout, %d %d\n", pnode->nLastRecv != 0, pnode->nLastSend != 0);
                    pnode->fDisconnect = true;
                    pnode->CloseSocketDisconnect();
                }
                else if (GetTime() - pnode->nLastSend > DATA_TIMEOUT)
                {
                   LogPrintf("socket not sending node %s \n", pnode );
                    pnode->fDisconnect = true;
                    pnode->CloseSocketDisconnect();
                }
                else if (GetTime() - pnode->nLastRecv > DATA_TIMEOUT)
                {
                    LogPrintf("socket inactivity timeout\n");
                    pnode->fDisconnect = true;
                    pnode->CloseSocketDisconnect();
                }
            }

            MilliSleep( 1 ); /* RGP optimise */
        }
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
                pnode->Release();
        }

    // Refresh nodes/peers every X minutes
    RefreshRecentConnections(2);

        MilliSleep( 1 ); /* RGP optimise */
    }

    /* End of while, check if we are closing */
    if ( fRequestShutdown )
    {
       printf("\n Bank Society Gold, Net Process ending...");
       MilliSleep(10000);
    }

    // Refresh nodes/peers every X minutes
    RefreshRecentConnections(2);

}

#ifdef WIN32
//#ifdef USE_UPNP
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

        string strDesc = "SocietyG " + FormatFullVersion();

        try {
            while (!ShutdownRequested()) {
                boost::this_thread::interruption_point();

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
                    LogPrintf("UPnP Port Mapping successful.\n");

                MilliSleep(20*60*1000); // Refresh every 20 minutes
            }
        }
        catch (boost::thread_interrupted)
        {
            r = UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, port.c_str(), "TCP", 0);
            LogPrintf("UPNP_DeletePortMapping() returned : %d\n", r);
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
        (!GetBoolArg("forcednsseed", true))) {
        MilliSleep(11 * 1000);

        LOCK(cs_vNodes);
        if (vNodes.size() >= MAX_CONNECTIONS )
        {
            /* was 2 */
            LogPrintf("P2P peers available. Skipped DNS seeding.\n");
            return;
        }
    }

    const vector<CDNSSeedData> &vSeeds = Params().DNSSeeds();
    int found = 0;

    LogPrintf("Loading addresses from DNS seeds (could take a while)\n");

    BOOST_FOREACH(const CDNSSeedData &seed, vSeeds) 
    {
        if (HaveNameProxy()) {
            AddOneShot(seed.host);
        } else {
            vector<CNetAddr> vIPs;
            vector<CAddress> vAdd;
            if (LookupHost(seed.host.c_str(), vIPs))
            {
                BOOST_FOREACH(CNetAddr& ip, vIPs)
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

        MilliSleep( 1 ); /* RGP optimise */
    }

    LogPrintf("%d addresses found from DNS seeds\n", found);
}

void DumpAddresses()
{
int64_t nStart;
//const char name = "DumpAddress Exc";

    nStart = GetTimeMillis();

    try
    {

        CAddrDB adb;
        adb.Write(addrman);

        LogPrint("net", "Flushed %d addresses to peers.dat  %dms\n",
               addrman.size(), GetTimeMillis() - nStart);


    }
    catch (boost::thread_interrupted)
    {
        LogPrintf("%s DumpAddresses exception\n", "DumpAddress Exc");
        // throw;
    }
    catch (std::exception& e) {
        PrintException(&e, "DumpAddress Exc");
    }
    catch (...) {
        PrintException(NULL, "DumpAddress Exc");
    }

}

void DumpData()
{
//const char name = "DumpData Exc";

    try
    {
        DumpAddresses();

        if (CNode::BannedSetIsDirty())
        {
            DumpBanlist();
            CNode::SetBannedSetDirty(false);
        }

     }
     catch (boost::thread_interrupted)
     {
           LogPrintf("%s DumpData exception \n", "DumpData Exc");
           // throw;
     }
     catch (std::exception& e)
     {
           PrintException(&e,"DumpData Exc");
     }
     catch (...)
     {
           PrintException(NULL, "DumpData Exc");
     }



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
    extern volatile bool fRequestShutdown;
    // Connect to specific addresses
    if (mapArgs.count("connect") && mapMultiArgs["connect"].size() > 0)
    {
        for (int64_t nLoop = 0;; nLoop++)
        {
            ProcessOneShot();
            BOOST_FOREACH(string strAddr, mapMultiArgs["connect"])
            {
                CAddress addr;
                OpenNetworkConnection(addr, NULL, strAddr.c_str());
                for (int i = 0; i < 10 && i < nLoop; i++)
                {
                    MilliSleep(1000); //500
                }
            }
            MilliSleep(200); //100
        }
    }

    // Initiate network connections
    int64_t nStart = GetTime();
    while (!fRequestShutdown )
    {
        ProcessOneShot();

        MilliSleep(200);// 500

        CSemaphoreGrant grant(*semOutbound);
        boost::this_thread::interruption_point();

        // Add seed nodes if DNS seeds are all down (an infrastructure attack?).
        if (addrman.size() == 0 && (GetTime() - nStart > 60)) {
            static bool done = false;
            if (!done) {
                LogPrintf("Adding fixed seed nodes as DNS doesn't seem to be available.\n");
                addrman.Add(Params().FixedSeeds(), CNetAddr("127.0.0.1"));
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
            BOOST_FOREACH(CNode* pnode, vNodes) 
            {
                if (!pnode->fInbound) {
                    setConnected.insert(pnode->addr.GetGroup());
                    nOutbound++;
                }
                MilliSleep( 2 );
            }
        }

        int64_t nANow = GetAdjustedTime();

        int nTries = 0;
        while (true)
        {
            // use an nUnkBias between 10 (no outgoing connections) and 90 (8 outgoing connections)
            CAddress addr = addrman.Select();

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
            /*if (addr.GetPort() != Params().GetDefaultPort() && nTries < 50)
                continue;*/

            addrConnect = addr;
            break;

            MilliSleep( 5 ); /* RGP optimise */
        }

        if (addrConnect.IsValid())
            OpenNetworkConnection(addrConnect, &grant);

        MilliSleep( 5 ); /* RGP optimise */

    }
}

void ThreadOpenAddedConnections()
{
    {
        LOCK(cs_vAddedNodes);
        vAddedNodes = mapMultiArgs["-addnode"];
    }

    if (HaveNameProxy()) {
        while(true) {
            list<string> lAddresses(0);
            {
                LOCK(cs_vAddedNodes);
                BOOST_FOREACH(string& strAddNode, vAddedNodes)
                    lAddresses.push_back(strAddNode);
            }
            BOOST_FOREACH(string& strAddNode, lAddresses) {
                CAddress addr;
                CSemaphoreGrant grant(*semOutbound);
                OpenNetworkConnection(addr, &grant, strAddNode.c_str());
                MilliSleep(500);//500
            }
            MilliSleep(120000); // Retry every 2 minutes
        }
    }

    for (unsigned int i = 0; true; i++)
    {
        list<string> lAddresses(0);
        {
            LOCK(cs_vAddedNodes);
            BOOST_FOREACH(string& strAddNode, vAddedNodes)
                lAddresses.push_back(strAddNode);
        }

        list<vector<CService> > lservAddressesToAdd(0);
        BOOST_FOREACH(string& strAddNode, lAddresses)
        {
            vector<CService> vservNode(0);
            if(Lookup(strAddNode.c_str(), vservNode, Params().GetDefaultPort(), fNameLookup, 0))
            {
                lservAddressesToAdd.push_back(vservNode);
                {
                    LOCK(cs_setservAddNodeAddresses);
                    BOOST_FOREACH(CService& serv, vservNode)
                        setservAddNodeAddresses.insert(serv);
                }
            }

            MilliSleep( 1 ); /* RGP optimise */
        }
        // Attempt to connect to each IP for each addnode entry until at least one is successful per addnode entry
        // (keeping in mind that addnode entries can have many IPs if fNameLookup)
        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodes)
            {
                for (list<vector<CService> >::iterator it = lservAddressesToAdd.begin(); it != lservAddressesToAdd.end(); it++)
                {
                    BOOST_FOREACH(CService& addrNode, *(it))
                    {
                        if (pnode->addr == addrNode)
                        {
                            it = lservAddressesToAdd.erase(it);
                            it--;
                            break;
                        }
                    }
                }
                MilliSleep( 1 ); /* RGP optimise */
            }
        }
        BOOST_FOREACH(vector<CService>& vserv, lservAddressesToAdd)
        {
            CSemaphoreGrant grant(*semOutbound);
            OpenNetworkConnection(CAddress(vserv[i % vserv.size()]), &grant);
            MilliSleep(500);//500
        }
        MilliSleep(120000); // Retry every 2 minutes
    }
}

// if successful, this moves the passed grant to the constructed node
bool OpenNetworkConnection(const CAddress& addrConnect, CSemaphoreGrant *grantOutbound, const char *strDest, bool fOneShot)
{
    //
    // Initiate outbound network connection
    //

    boost::this_thread::interruption_point();
    if (!strDest)
        if (IsLocal(addrConnect) ||
            FindNode((CNetAddr)addrConnect) || CNode::IsBanned(addrConnect) ||
                FindNode(addrConnect.ToStringIPPort().c_str())){

            return false;
        }

    if (strDest && FindNode(strDest)){
        return false;
    }

    CNode* pnode = ConnectNode(addrConnect, strDest);
    boost::this_thread::interruption_point();

    if (!pnode)
    {
        return false;
    }


    if (FireWall(pnode, "OpenNetConnection"))
    {

        if (grantOutbound)
        {
            grantOutbound->MoveTo(pnode->grantOutbound);
        }

        pnode->fNetworkNode = true;

        if (fOneShot)
        {
            pnode->fOneShot = true;
        }

        return true;

    }
    else
    {
        return false;
    }


}


// for now, use a very simple selection metric: the node from which we received
// most recently
static int64_t NodeSyncScore(const CNode *pnode) {
    return pnode->nLastRecv;
}

void static StartSync(const vector<CNode*> &vNodes)
{
    CNode *pnodeNewSync = NULL;
    int64_t nBestScore = 0;
    int64_t nScore = 0;

    // fImporting and fReindex are accessed out of cs_main here, but only
    // as an optimization - they are checked again in SendMessages.
    if (fImporting || fReindex)
        return;

    // Iterate over all nodes
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        //LogPrintf("*** RGP StartSyncn start \n" );

        // check preconditions for allowing a sync
        if (!pnode->fClient && !pnode->fOneShot &&
            !pnode->fDisconnect && pnode->fSuccessfullyConnected &&
            (pnode->nStartingHeight > (nBestHeight - 144)) &&
            (pnode->nVersion < NOBLKS_VERSION_START || pnode->nVersion >= NOBLKS_VERSION_END))
        {
            // if ok, compare node's score with the best so far
            nScore = NodeSyncScore(pnode);
            if (pnodeNewSync == NULL || nScore > nBestScore)
            {
                pnodeNewSync = pnode;
                nBestScore = nScore;
            }

        }
        else
        {

           /* set one anyway */
           //LogPrintf("*** RGP StartSyncn node failed checks \n" );

            if ( pnodeNewSync == 0 )
            {
                pnodeNewSync = pnode;
                nBestScore = nScore;
            }
        }

        MilliSleep( 1 ); /* RGP optimise */
    }




    // if a new sync candidate was found, start sync!
    if (pnodeNewSync)
    {


        pnodeNewSync->fStartSync = true;
        pnodeSync = pnodeNewSync;
    }
}

void ThreadMessageHandler()
{
extern volatile bool fRequestShutdown;
int64_t Time_to_Last_block;
int64_t start_time;

    SetThreadPriority(THREAD_PRIORITY_BELOW_NORMAL);
LogPrintf("RGP NET ThreadMessageHandler start \n" );

    while ( !fRequestShutdown )
    {
        start_time = GetTime();

        bool fHaveSyncNode = false;

        vector<CNode*> vNodesCopy;
        {
            LOCK(cs_vNodes);
            vNodesCopy = vNodes;
            BOOST_FOREACH(CNode* pnode, vNodesCopy) 
            {
                pnode->AddRef();
                if (pnode == pnodeSync)
                    fHaveSyncNode = true;

                MilliSleep(2); /* RGP Optimisation */
                
            }
        }

        MilliSleep(5); /* RGP Optimisation */


        if (!fHaveSyncNode){
            StartSync(vNodesCopy);
        }

        // Poll the connected nodes for messages
        CNode* pnodeTrickle = NULL;
        if (!vNodesCopy.empty()){
            pnodeTrickle = vNodesCopy[GetRand(vNodesCopy.size())];
        }

        bool fSleep = true;

        BOOST_FOREACH(CNode* pnode, vNodesCopy)
        {
            if (pnode->fDisconnect)
            {

                MilliSleep(5); /* RGP Optimisation */
                continue;
            }
            // Receive messages

            MilliSleep( 1 ); /* RGP Optimize */

            /* -- RGP, Check if the incoming message queue is empty -- */
            if ( pnode->vRecvMsg.empty() )
            {

                // The only occasion that this could happen in when the Daemon
                // is at the top of the chain or there is a network issue

                Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
                // LogPrintf("RGP NET ThreadMessageHandler Last block was %d \n", Time_to_Last_block );
                if ( Time_to_Last_block < 4000 )
                {
                    // RGP, we are close to synch or are synched
                    MilliSleep( 100 );
                }
                else
                {
                    // This is the scenario where there is no messages, as the nodes 
                    // have stopped sending messages.
                    
                    PushGetBlocks(pnode, pindexBest, pindexBest->GetBlockHash() );
                    MilliSleep(200);
                    StartSync(vNodesCopy);
                    /* do nothing */
                }
            }
            else
            {
                TRY_LOCK(pnode->cs_vRecvMsg, lockRecv);
                if (lockRecv)
                {
                    if (!g_signals.ProcessMessages(pnode))
                    {
                        pnode->CloseSocketDisconnect();
                        MilliSleep(10); /* RGP Optimisation */
                    }

                    // Disconnect node/peer if send/recv data becomes idle
                    if (GetTime() - pnode->nTimeConnected > 180 )
                    { /* 90 to 180 rgp */

                        if (GetTime() - pnode->nLastRecv > 120)
                        {/* was 60 */

                            if (GetTime() - pnode->nLastSend > 90 )
                            { /* 60 was < 30 */
                               pnode->CloseSocketDisconnect();
                               MilliSleep(10); /* RGP Optimisation */
                            }
                        }
                    }

                    if (pnode->nSendSize < SendBufferSize())
                    {
                        if (!pnode->vRecvGetData.empty() || (!pnode->vRecvMsg.empty() && pnode->vRecvMsg[0].complete()))
                        {
                            fSleep = false;
                        }
                        MilliSleep(5); /* RGP Optimisation */
                    }
                }
                else
                {
                    MilliSleep(5); /* RGP Optimisation */
                    LogPrintf("RGP ThreadMessageHandler lockRecv FAILED \n");
                }

            }
            //boost::this_thread::interruption_point();

            MilliSleep(5); /* RGP Optimisation */

            // Send messages
            {

                //TRY_LOCK(pnode->cs_vSend, lockSend);
                //if (lockSend)
                //{
                    g_signals.SendMessages(pnode, pnode == pnodeTrickle);
                    MilliSleep(10); /* RGP Optimisation */


                //}
                //else
                //{
                //    MilliSleep(20); /* RGP Optimisation */
                //}
            }
            //boost::this_thread::interruption_point();

            MilliSleep( 30 );
        }
        
        if ( ( GetTime() - start_time ) > 5 )
        {
LogPrintf("RGP ThreadMessageHandler debug 004 %d \n", GetTime() - start_time );
        }
        MilliSleep( 200 );

        {
            LOCK(cs_vNodes);
            BOOST_FOREACH(CNode* pnode, vNodesCopy)
            {
                pnode->Release();
                MilliSleep(2); /* RGP Optimisation */
            }
        }

        if (fSleep)
        {
            MilliSleep(200); // RGP was MilliSleep(100);
        }
        else
        {
            MilliSleep(50); /* RGP Optimisation */
        }
    }
}






bool BindListenPort(const CService &addrBind, string& strError)
{
    strError = "";
    int nOne = 1;

#ifdef WIN32
    // Initialize Windows Sockets
    WSADATA wsadata;
    int ret = WSAStartup(MAKEWORD(2,2), &wsadata);
    if (ret != NO_ERROR)
    {
        strError = strprintf("Error: TCP/IP socket library failed to start (WSAStartup returned error %d)", ret);
        LogPrintf("%s\n", strError);
        return false;
    }
#endif

    // Create socket for listening for incoming connections
    struct sockaddr_storage sockaddr;
    socklen_t len = sizeof(sockaddr);
    if (!addrBind.GetSockAddr((struct sockaddr*)&sockaddr, &len))
    {
        strError = strprintf("Error: bind address family for %s not supported", addrBind.ToString());
        LogPrintf("%s\n", strError);
        return false;
    }

    SOCKET hListenSocket = socket(((struct sockaddr*)&sockaddr)->sa_family, SOCK_STREAM, IPPROTO_TCP);
    if (hListenSocket == INVALID_SOCKET)
    {
        strError = strprintf("Error: Couldn't open socket for incoming connections (socket returned error %d)", WSAGetLastError());
        LogPrintf("%s\n", strError);
        return false;
    }


#ifndef WIN32
#ifdef SO_NOSIGPIPE
    // Different way of disabling SIGPIPE on BSD
    setsockopt(hListenSocket, SOL_SOCKET, SO_NOSIGPIPE, (void*)&nOne, sizeof(int));
#endif
    // Allow binding if the port is still in TIME_WAIT state after
    // the program was closed and restarted.  Not an issue on windows.
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (void*)&nOne, sizeof(int));
    // Disable Nagle's algorithm
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (void*)&nOne, sizeof(int));
#else
    setsockopt(hListenSocket, SOL_SOCKET, SO_REUSEADDR, (const char*)&nOne, sizeof(int));
    setsockopt(hListenSocket, IPPROTO_TCP, TCP_NODELAY, (const char*)&nOne, sizeof(int));
#endif


#ifdef WIN32
    // Set to non-blocking, incoming connections will also inherit this
    if (ioctlsocket(hListenSocket, FIONBIO, (u_long*)&nOne) == SOCKET_ERROR)
#else
    if (fcntl(hListenSocket, F_SETFL, O_NONBLOCK) == SOCKET_ERROR)
#endif
    {
        strError = strprintf("Error: Couldn't set properties on socket for incoming connections (error %d)", WSAGetLastError());
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
        int nProtLevel = 10 /* PROTECTION_LEVEL_UNRESTRICTED */;
        int nParameterId = 23 /* IPV6_PROTECTION_LEVEl */;
        // this call is allowed to fail
        setsockopt(hListenSocket, IPPROTO_IPV6, nParameterId, (const char*)&nProtLevel, sizeof(int));
#endif
    }

    if (::bind(hListenSocket, (struct sockaddr*)&sockaddr, len) == SOCKET_ERROR)
    {
        int nErr = WSAGetLastError();

        LogPrintf("RGP Debug BIND error %d \n", nErr );
LogPrintf("RGP DEBUG Net Bind %s  \n", addrBind.ToString() );

        if (nErr == WSAEADDRINUSE)
        {
LogPrintf("RGP DEBUG Net Bind %s WSAEADDRINUSE \n", addrBind.ToString() );
            strError = strprintf(_("Unable to bind to %s on this computer. SocietyG is probably already running."), addrBind.ToString());
        }
        else
            strError = strprintf(_("Unable to bind to %s on this computer (bind returned error %d, %s)"), addrBind.ToString(), nErr, strerror(nErr));
        LogPrintf("%s\n", strError);
        return false;
    }
    LogPrintf("Bound to %s\n", addrBind.ToString());

    // Listen for incoming connections
    if (listen(hListenSocket, SOMAXCONN) == SOCKET_ERROR)
    {
        strError = strprintf("Error: Listening for incoming connections failed (listen returned error %d)", WSAGetLastError());
        LogPrintf("%s\n", strError);
        return false;
    }

    vhListenSocket.push_back(hListenSocket);

    if (addrBind.IsRoutable() && fDiscover)
        AddLocal(addrBind, LOCAL_BIND);

    return true;
}

void static Discover(boost::thread_group& threadGroup)
{
    if (!fDiscover)
        return;

#ifdef WIN32
    // Get local host IP
    char pszHostName[1000] = "";
    if (gethostname(pszHostName, sizeof(pszHostName)) != SOCKET_ERROR)
    {
        vector<CNetAddr> vaddr;
        if (LookupHost(pszHostName, vaddr))
        {
            BOOST_FOREACH (const CNetAddr &addr, vaddr)
            {
                AddLocal(addr, LOCAL_IF);
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
                    LogPrintf("IPv4 %s: %s\n", ifa->ifa_name, addr.ToString());
            }
            else if (ifa->ifa_addr->sa_family == AF_INET6)
            {
                struct sockaddr_in6* s6 = (struct sockaddr_in6*)(ifa->ifa_addr);
                CNetAddr addr(s6->sin6_addr);
                if (AddLocal(addr, LOCAL_IF))
                    LogPrintf("IPv6 %s: %s\n", ifa->ifa_name, addr.ToString());
            }
        }
        freeifaddrs(myaddrs);
    }
#endif

}

void StartNode(boost::thread_group& threadGroup)
{

    //try to read stored banlist
    CBanDB bandb;
    banmap_t banmap;
    if (!bandb.Read(banmap))
        LogPrintf("Invalid or missing banlist.dat; recreating\n");

    CNode::SetBanned(banmap); //thread save setter
    CNode::SetBannedSetDirty(false); //no need to write down just read or nonexistent data
    CNode::SweepBanned(); //sweap out unused entries

    if (semOutbound == NULL) {
        // initialize semaphore
        int nMaxOutbound = min(MAX_OUTBOUND_CONNECTIONS, nMaxConnections);
        semOutbound = new CSemaphore(nMaxOutbound);
    }

    if (pnodeLocalHost == NULL)
        pnodeLocalHost = new CNode(INVALID_SOCKET, CAddress(CService("127.0.0.1", 0), nLocalServices));

    Discover(threadGroup);

    //
    // Start threads
    //

    if (!GetBoolArg("dnsseed", true))
        LogPrintf("DNS seeding disabled\n");
    else
        threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "dnsseed", &ThreadDNSAddressSeed));

#ifdef USE_UPNP
    // Map ports with UPnP
    MapPort(GetBoolArg("upnp", USE_UPNP));
#endif
    
    // Send and receive from sockets, accept connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "net", &ThreadSocketHandler));

    // Initiate outbound connections from -addnode
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "addcon", &ThreadOpenAddedConnections));

    // Initiate outbound connections
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "opencon", &ThreadOpenConnections));

    // Process messages
    threadGroup.create_thread(boost::bind(&TraceThread<void (*)()>, "msghand", &ThreadMessageHandler));

    // Dump network addresses
    threadGroup.create_thread(boost::bind(&LoopForever<void (*)()>, "dumpaddr", &DumpData, DUMP_ADDRESSES_INTERVAL * 1000));
}

bool StopNode()
{
    LogPrintf("StopNode()\n");
    MapPort(false);
    mempool.AddTransactionsUpdated(1);
    if (semOutbound)
        for (int i=0; i<MAX_OUTBOUND_CONNECTIONS; i++)
            semOutbound->post();
    DumpData();
    MilliSleep(50);
    DumpAddresses();
    return true;
}

class CNetCleanup
{
public:
    CNetCleanup()
    {
    }
    ~CNetCleanup()
    {
        // Close sockets
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (pnode->hSocket != INVALID_SOCKET)
                closesocket(pnode->hSocket);
        BOOST_FOREACH(SOCKET hListenSocket, vhListenSocket)
            if (hListenSocket != INVALID_SOCKET)
                if (closesocket(hListenSocket) == SOCKET_ERROR)
                    LogPrintf("closesocket(hListenSocket) failed with error %d\n", WSAGetLastError());

        // clean up some globals (to help leak detection)
        BOOST_FOREACH(CNode *pnode, vNodes)
            delete pnode;
        BOOST_FOREACH(CNode *pnode, vNodesDisconnected)
            delete pnode;
        vNodes.clear();
        vNodesDisconnected.clear();
        delete semOutbound;
        semOutbound = NULL;
        delete pnodeLocalHost;
        pnodeLocalHost = NULL;

#ifdef WIN32
        // Shutdown Windows Sockets
        WSACleanup();
#endif
    }
}
instance_of_cnetcleanup;

void RelayTransaction(const CTransaction& tx, const uint256& hash)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(10000);
    ss << tx;
    RelayTransaction(tx, hash, ss);
}

void RelayTransaction(const CTransaction& tx, const uint256& hash, const CDataStream& ss)
{
    CInv inv(MSG_TX, hash);
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

    RelayInventory(inv);
}

void RelayTransactionLockReq(const CTransaction& tx, bool relayToAll)
{
    CInv inv(MSG_TXLOCK_REQUEST, tx.GetHash());

    //broadcast the new lock
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        if(!relayToAll && !pnode->fRelayTxes)
            continue;

        pnode->PushMessage("txlreq", tx);
    }

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
    GetRandBytes((unsigned char *)&randv, sizeof(randv));
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
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("CAddrman::Write() : open failed");

    // Write and commit header, data
    try {
        fileout << ssPeers;
    }
    catch (std::exception &e) {
        return error("CAddrman::Write() : I/O error");
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    // replace existing peers.dat, if any, with new peers.dat.XXXX
    if (!RenameOver(pathTmp, pathAddr))
        return error("CAddrman::Write() : Rename-into-place failed");

    return true;
}

bool CAddrDB::Read(CAddrMan& addr)
{
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathAddr.string().c_str(), "rb");
    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("CAddrman::Read() : open failed");

    // use file size to size memory buffer
    std::size_t fileSize = boost::filesystem::file_size(pathAddr);
    int64_t dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (fileSize >= sizeof(uint256))
        dataSize = fileSize - sizeof(uint256);
    if ( dataSize < 0 )
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e) {
        return error("CAddrman::Read() 2 : I/O error or stream data corrupted");
    }
    filein.fclose();

    CDataStream ssPeers(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssPeers.begin(), ssPeers.end());
    if (hashIn != hashTmp)
        return error("CAddrman::Read() : checksum mismatch; data corrupted");

    unsigned char pchMsgTmp[4];
    try {
        // de-serialize file header (network specific magic number) and ..
        ssPeers >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
            return error("CAddrman::Read() : invalid network magic number");

        // de-serialize address data into one CAddrMan object
        ssPeers >> addr;
    }
    catch (std::exception &e) {
        return error("CAddrman::Read() : I/O error or stream data corrupted");
    }

    return true;
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

    CNode::SweepBanned(); //clean unused entires (if bantime has expired)

    CBanDB bandb;
    banmap_t banmap;
    CNode::GetBanned(banmap);
    bandb.Write(banmap);

    LogPrint("net", "Flushed %d banned node ips/subnets to banlist.dat  %dms\n",
             banmap.size(), GetTimeMillis() - nStart);
}


