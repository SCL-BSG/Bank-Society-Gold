// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2018 Profit Hunters Coin developers
// Copyright (c) 2023 Bank Society Gold Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.



#include "rpcserver.h"


#include "base58.h"
#include "init.h"
#include "util.h"
#include "sync.h"
#include "base58.h"
//#include "db.h"
#include "ui_interface.h"
//#include <filesystem>
#ifdef ENABLE_WALLET
#include "wallet.h"
#endif

//#include "random.h"

#include <boost/algorithm/string.hpp>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/foreach.hpp>
#include <boost/iostreams/concepts.hpp>
#include <boost/iostreams/stream.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/shared_ptr.hpp>
#include <list>

#include <univalue.h>

/* RGP */
#define DEFAULT_RPC_SERVER_THREADS      20

using namespace std;
using namespace boost;
using namespace RPCServer;

static bool fRPCRunning = false;
static bool fRPCInWarmup = true;
static std::string rpcWarmupStatus("RPC server started");
static CCriticalSection cs_rpcWarmup;

static std::string strRPCUserColonPass;

/* Timer-creating functions */
static RPCTimerInterface* timerInterface = NULL;
/* Map of name to timer.
 * @note Can be changed to std::unique_ptr when C++11 */
static std::map<std::string, boost::shared_ptr<RPCTimerBase> > deadlineTimers;

static struct CRPCSignals
{
    boost::signals2::signal<void ()> Started;
    boost::signals2::signal<void ()> Stopped;
    boost::signals2::signal<void (const CRPCCommand&)> PreCommand;
    boost::signals2::signal<void (const CRPCCommand&)> PostCommand;
} g_rpcSignals;


// These are created by StartRPCThreads, destroyed in StopRPCThreads
// static asio::io_service* rpc_io_service = NULL;
// static asio::ssl::context* rpc_io_service = NULL;
//static boost::asio::io_context* rpc_io_service;
//static map<string, boost::shared_ptr<deadline_timer> > deadlineTimers;
//static ssl::context* rpc_ssl_context = NULL;
//static boost::asio::ssl::context rpc_ssl_context;
static boost::thread_group* rpc_worker_group = NULL;



//printf("*** RGP DEBUG getblockchaininfo needs to be implemented for mining!!! \n");
//UniValue getblockchaininfo(const JSONRPCRequest& request);


void RPCServer::OnStarted(boost::function<void ()> slot)
{
    g_rpcSignals.Started.connect(slot);
}

void RPCServer::OnStopped(boost::function<void ()> slot)
{
    g_rpcSignals.Stopped.connect(slot);
}

void RPCServer::OnPreCommand(boost::function<void (const CRPCCommand&)> slot)
{
    g_rpcSignals.PreCommand.connect(boost::bind(slot, _1));
}

void RPCServer::OnPostCommand(boost::function<void (const CRPCCommand&)> slot)
{
    g_rpcSignals.PostCommand.connect(boost::bind(slot, _1));
}


void RPCTypeCheck(const UniValue& params,
                  const list<UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    unsigned int i = 0;
    BOOST_FOREACH(UniValue::VType t, typesExpected) {
        if (params.size() <= i)
            break;

        const UniValue& v = params[i];
        if (!((v.type() == t) || (fAllowNull && (v.isNull())))) {
            string err = strprintf("Expected type %s, got %s",
                                   uvTypeName(t), uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
        i++;
    }
}

void RPCTypeCheckObj(const UniValue& o,
                  const map<string, UniValue::VType>& typesExpected,
                  bool fAllowNull)
{
    BOOST_FOREACH(const PAIRTYPE(string, UniValue::VType)& t, typesExpected) {
        const UniValue& v = find_value(o, t.first);
        if (!fAllowNull && v.isNull())
            throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Missing %s", t.first));

        if (!((v.type() == t.second) || (fAllowNull && (v.isNull())))) {
            string err = strprintf("Expected type %s for %s, got %s",
                                   uvTypeName(t.second), t.first, uvTypeName(v.type()));
            throw JSONRPCError(RPC_TYPE_ERROR, err);
        }
    }
}

CAmount AmountFromValue(const UniValue& value)
{
    if (!value.isNum())
        throw JSONRPCError(RPC_TYPE_ERROR, "Amount is not a number");

    double dAmount = value.get_real();
    if (dAmount <= 0.0 || dAmount > 21000000.0)
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    CAmount nAmount = roundint64(dAmount * COIN);
    if (!MoneyRange(nAmount))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid amount");
    return nAmount;
}

UniValue ValueFromAmount(const CAmount& amount)
{
    bool sign = amount < 0;
    int64_t n_abs = (sign ? -amount : amount);
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;
    return UniValue(UniValue::VNUM,
            strprintf("%s%d.%08d", sign ? "-" : ",", quotient, remainder));
}

uint256 ParseHashV(const UniValue& v, std::string strName)
{
    std::string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex)) // Note: IsHex(",") is false
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    if (64 != strHex.length())
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be of length %d (not %d)", strName, 64, strHex.length()));
    uint256 result;
    result.SetHex(strHex);
    return result;
}
uint256 ParseHashO(const UniValue& o, string strKey)
{
    return ParseHashV(find_value(o, strKey), strKey);
}


vector<unsigned char> ParseHexV(const UniValue& v, string strName)
{
    string strHex;
    if (v.isStr())
        strHex = v.get_str();
    if (!IsHex(strHex))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strName + " must be hexadecimal string (not '" + strHex + "')");
    return ParseHex(strHex);
}
vector<unsigned char> ParseHexO(const UniValue& o, string strKey)
{
    return ParseHexV(find_value(o, strKey), strKey);
}

int ParseInt(const UniValue& o, string strKey)
{
    const UniValue& v = find_value(o, strKey);
    if (v.isNum())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, " + strKey + "is not an int");

    return v.get_int();
}

bool ParseBool(const UniValue& o, string strKey)
{
    const UniValue& v = find_value(o, strKey);
    if (v.isBool())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, " + strKey + "is not a bool");

    return v.get_bool();
}


/**
 * Note: This interface may still be subject to change.
 */

string CRPCTable::help(string strCommand) const
{
    string strRet;
    string category;
    set<rpcfn_type> setDone;
    vector<pair<string, const CRPCCommand*> > vCommands;

    for (map<string, const CRPCCommand*>::const_iterator mi = mapCommands.begin(); mi != mapCommands.end(); ++mi)
        vCommands.push_back(make_pair(mi->second->category + mi->first, mi->second));
    sort(vCommands.begin(), vCommands.end());

    BOOST_FOREACH (const PAIRTYPE(string, const CRPCCommand*) & command, vCommands) {
        const CRPCCommand* pcmd = command.second;
        string strMethod = pcmd->name;
        // We already filter duplicates, but these deprecated screw up the sort order
        if (strMethod.find("label") != string::npos)
            continue;
        if ((strCommand != "," || pcmd->category == "hidden") && strMethod != strCommand)
            continue;
#ifdef ENABLE_WALLET
        if (pcmd->reqWallet && !pwalletMain)
            continue;
#endif

        try {
            UniValue params;
            rpcfn_type pfn = pcmd->actor;
            if (setDone.insert(pfn).second)
                (*pfn)(params, true);
        } catch (std::exception& e) {
            // Help text is returned in an exception
            string strHelp = string(e.what());
            if (strCommand == ",") {
                if (strHelp.find('\n') != string::npos)
                    strHelp = strHelp.substr(0, strHelp.find('\n'));

                if (category != pcmd->category) {
                    if (!category.empty())
                        strRet += "\n";
                    category = pcmd->category;
                    string firstLetter = category.substr(0, 1);
                    boost::to_upper(firstLetter);
                    strRet += "== " + firstLetter + category.substr(1) + " ==\n";
                }
            }
            strRet += strHelp + "\n";
        }
    }
    if (strRet == ",")
        strRet = strprintf("help: unknown command: %s\n", strCommand);
    strRet = strRet.substr(0, strRet.size() - 1);
    return strRet;
}


UniValue help(const UniValue& params, bool fHelp)
{
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "help ( \"command\" )\n"
            "\nList all commands, or get help for a specified command.\n"
            "\nArguments:\n"
            "1. \"command\"     (string, optional) The command to get help on\n"
            "\nResult:\n"
            "\"text\"     (string) The help text\n");

    string strCommand;
    if (params.size() > 0)
        strCommand = params[0].get_str();

    return tableRPC.help(strCommand);
}


UniValue stop(const UniValue& params, bool fHelp)
{
    // Accept the deprecated and ignored 'detach' boolean argument
    if (fHelp || params.size() > 1)
        throw runtime_error(
            "stop\n"
            "\nStop BSG server.");
    // Event loop will exit after current HTTP requests have been handled, so
    // this reply will get back to the client.
    StartShutdown();
    return "BSG server stopping";
}

//class CRPCCommand
//
//    std::string category;
//    std::string name;
//    rpcfn_type actor;
//    bool okSafeMode;
//    bool threadSafe;
//    bool reqWallet;

/**
 * Call Table
 */
static const CRPCCommand vRPCCommands[] =
    {
        //  category		    name			        actor (function)	   okSafeMode	threadSafe	reqWallet
        //  ----------------	----------------- 	    -------------------    ---------- 	---------- 	---------
        /* Overall control/query calls */
        
        { "control",		    "help",		            &help,			       true,		true,		false		},
        { "control",		    "getinfo",		        &getinfo,		       true,		true,		false		}, /* uses wallet if enabled */
        { "control",		    "stop",		            &stop,			       true,		true,		false		},
        
         /* Block chain and UTXO */

        {"blockchain",          "getblockchaininfo",    &getblockchaininfo,    true,        false,      false       },
        {"blockchain",          "getbestblockhash",     &getbestblockhash,     true,        false,      false       },
        {"blockchain",		    "getblockcount", 	    &getblockcount, 	   true, 		false, 	    false		},
        {"blockchain",          "getblock",             &getblock, 		       true, 		false, 	    false		},
        {"blockchain", 	        "getblockhash",         &getblockhash, 		   true, 		false, 	    false		},
        {"blockchain",          "getchaintips",         &getchaintips, 		   true, 		false, 	    false		},	

        {"blockchain", 	        "getdifficulty",        &getdifficulty,        true,        false,      false       },
        {"blockchain", 	        "getrawmempool", 	    &getrawmempool, 	   true, 		false, 	    false		},

        {"blockchain", 	        "getblockbynumber",	    &getblockbynumber,	   true,    	false,     	false 		},
        {"blockchain",          "gettransaction",       &gettransaction,       true,        true,       false       },
        {"blockchain",		    "getcheckpoint",	    &getcheckpoint,        true,      	false,     	false		},

        
#ifdef ENABLE_WALLET
        /* Coin generation */
        {"generating", "getgenerate", &getgenerate, true, false, false},
//        {"generating", "gethashespersec", &gethashespersec, true, false, false},
        {"generating", "setgenerate", &setgenerate, true, true, false},
#endif
	        
  	/* P2P networking */
        {"network", 		"getnetworkinfo", 	&getnetworkinfo, 	true, 		false, 	false		},
        {"network", 		"addnode", 		&addnode, 		true, 		true, 		false		},
        {"network", 		"getaddednodeinfo", 	&getaddednodeinfo, 	true, 		true, 		false		},
        {"network", 		"getconnectioncount", 	&getconnectioncount, 	true, 		false, 	false		},
        {"network", 		"getnettotals", 	&getnettotals, 	true, 		true, 		false		},
        {"network", 		"getpeerinfo", 	&getpeerinfo, 		true, 		false, 	false		},
        {"network", 		"ping", 		&ping, 		true, 		false, 	false		},
        {"network", 		"setban", 		&setban, 		true, 		false, 	false		},
        {"network", 		"listbanned", 		&listbanned, 		true, 		false, 	false		},
        {"network", 		"clearbanned", 	&clearbanned, 		true, 		false, 	false		},

	 /* Raw transactions */
        {"rawtransactions", 	"createrawtransaction",&createrawtransaction, true, 		false, 	false		},
        {"rawtransactions", 	"decoderawtransaction",&decoderawtransaction, true, 		false, 	false		},
        {"rawtransactions", 	"decodescript", 	&decodescript, 	true, 		false, 	false		},
        {"rawtransactions", 	"getrawtransaction", 	&getrawtransaction, 	true, 		false, 	false		},
        {"rawtransactions", 	"sendrawtransaction", 	&sendrawtransaction, 	false, 	false, 	false		},
        {"rawtransactions", 	"signrawtransaction", 	&signrawtransaction, 	false, 	false, 	false		}, /* uses wallet if enabled */
        {"rawtransactions", 	"searchrawtransactions",&searchrawtransactions,false,     	false,     	false		},
   
    	 /* Utility functions */
//        {"util",		"sendalert",		&sendalert,		false,		false,		false		},
        {"util", 		"createmultisig", 	&createmultisig, 	true, 		true, 		false		},
        {"util", 		"validateaddress", 	&validateaddress, 	true, 		false, 	false		}, /* uses wallet if enabled */
        {"util", 		"verifymessage", 	&verifymessage, 	true, 		false, 	false		},
        {"util", 		"validatepubkey",	&validatepubkey,	true,		false,		false		},

    	/* Firewall General Session Settings */
    	{ "firewall", 		"firewallstatus",	&firewallstatus,	false,		false,		false		},
    	{ "firewall", 		"firewallenabled",	&firewallenabled,	false,      	false,		false 		},
    	{ "firewall", 		"firewallclearblacklist",&firewallclearblacklist,false,	false,		false 		},
    	{ "firewall", 		"firewallclearbanlist",&firewallclearbanlist,	false,		false,		false 		},
    	{ "firewall", 		"firewalltraffictolerance",&firewalltraffictolerance,false,	false,		false 		},
    	{ "firewall", 		"firewalltrafficzone",&firewalltrafficzone,	false,		false,		false 		},
    	{ "firewall", 		"firewalladdtowhitelist",&firewalladdtowhitelist,false,	false,		false 		},
    	{ "firewall", 		"firewalladdtoblacklist",&firewalladdtoblacklist,false,	false,		false 		},

    	/* Firewall Firewall Debug (Live Output) */
    	{ "firewall", 		"firewalldebug",                                 &firewalldebug,                               false,      false,    false },
    	{ "firewall", 		"firewalldebugexam",                             &firewalldebugexam,                           false,      false,    false },
    	{ "firewall", 		"firewalldebugbans",                             &firewallclearbanlist,                        false,      false,    false },
    	{ "firewall", 		"firewalldebugblacklist",                        &firewalldebugblacklist,                      false,      false,    false },
    	{ "firewall", 		"firewalldebugdisconnect",                       &firewalldebugdisconnect,                     false,      false,    false },
    	{ "firewall", 		"firewalldebugbandwidthabuse",                   &firewalldebugbandwidthabuse,                 false,      false,    false },
    	{ "firewall", 		"firewalldebugnofalsepositivebandwidthabuse",    &firewalldebugnofalsepositivebandwidthabuse,  false,      false,    false },
    	{ "firewall", 		"firewalldebuginvalidwallet",                    &firewalldebuginvalidwallet,                  false,      false,    false },
    	{ "firewall", 		"firewalldebugforkedwallet",                     &firewalldebugforkedwallet,                   false,      false,    false },
    	{ "firewall", 		"firewalldebugfloodingwallet",                   &firewalldebugfloodingwallet,                 false,      false,    false },

    	/* Firewall BandwidthAbuse Session Settings */
    	{ "firewall", 		"firewalldetectbandwidthabuse",                  &firewalldetectbandwidthabuse,                false,      false,    false },
    	{ "firewall", 		"firewallblacklistbandwidthabuse",               &firewallblacklistbandwidthabuse,             false,      false,    false },
    	{ "firewall", 		"firewallbanbandwidthabuse",                     &firewallbanbandwidthabuse,                   false,      false,    false },
    	{ "firewall", 		"firewallnofalsepositivebandwidthabuse",         &firewallnofalsepositivebandwidthabuse,       false,      false,    false },
    	{ "firewall", 		"firewallbantimebandwidthabuse",                 &firewallbantimebandwidthabuse,               false,      false,    false },
    	{ "firewall", 		"firewallbandwidthabusemaxcheck",                &firewallbandwidthabusemaxcheck,              false,      false,    false },
    	{ "firewall", 		"firewallbandwidthabuseminattack",               &firewallbandwidthabuseminattack,             false,      false,    false },
    	{ "firewall", 		"firewallbandwidthabuseminattack",               &firewallbandwidthabuseminattack,             false,      false,    false },

    /* Firewall Invalid Wallet Session Settings */
    { "firewall", 		"firewalldetectinvalidwallet",                   &firewalldetectinvalidwallet,                 false,      false,    false },
    { "firewall", 		"firewallblacklistinvalidwallet",                &firewallblacklistinvalidwallet,              false,      false,    false },
    { "firewall", 		"firewallbaninvalidwallet",                      &firewallbaninvalidwallet,                    false,      false,    false },
    { "firewall", 		"firewallbantimeinvalidwallet",                  &firewallbantimeinvalidwallet,                false,      false,    false },
    { "firewall", 		"firewallinvalidwalletminprotocol",              &firewallinvalidwalletminprotocol,            false,      false,    false },
    { "firewall", 		"firewallinvalidwalletmaxcheck",                 &firewallinvalidwalletmaxcheck,               false,      false,    false },

    /* Firewall Forked Wallet Session Settings */
    { "firewall", 		"firewalldetectforkedwallet",                    &firewalldetectforkedwallet,                  false,      false,    false },
    { "firewall", 		"firewallblacklistforkedwallet",                 &firewallblacklistforkedwallet,               false,      false,    false },
    { "firewall", 		"firewallbanforkedwallet",                       &firewallbanforkedwallet,                     false,      false,    false },
    { "firewall", 		"firewallbantimeforkedwallet",                   &firewallbantimeforkedwallet,                 false,      false,    false },
    { "firewall", 		"firewallforkedwalletnodeheight",                &firewallforkedwalletnodeheight,              false,      false,    false },

    /* Firewall Flooding Wallet Session Settings */
    { "firewall", 		"firewalldetectfloodingwallet",                  &firewalldetectfloodingwallet,                false,      false,    false },
    { "firewall", 		"firewallblacklistfloodingwallet",               &firewallblacklistfloodingwallet,             false,      false,    false },
    { "firewall", 		"firewallbanfloodingwallet",                     &firewallbanfloodingwallet,                   false,      false,    false },
    { "firewall", 		"firewallbantimefloodingwallet",                 &firewallbantimefloodingwallet,               false,      false,    false },
    { "firewall", 		"firewallfloodingwalletminbytes",                &firewallfloodingwalletminbytes,              false,      false,    false },
    { "firewall", 		"firewallfloodingwalletmaxbytes",                &firewallfloodingwalletmaxbytes,              false,      false,    false },
    { "firewall", 		"firewallfloodingwalletattackpatternadd",        &firewallfloodingwalletattackpatternadd,      false,      false,    false },
    { "firewall", 		"firewallfloodingwalletattackpatternremove",     &firewallfloodingwalletattackpatternremove,   false,      false,    false },
    { "firewall", 		"firewallfloodingwalletmintrafficavg",           &firewallfloodingwalletmintrafficavg,         false,      false,    false },
    { "firewall", 		"firewallfloodingwalletmaxtrafficavg",           &firewallfloodingwalletmaxtrafficavg,         false,      false,    false },
    { "firewall", 		"firewallfloodingwalletmincheck",                &firewallfloodingwalletmincheck,              false,      false,    false },
    { "firewall", 		"firewallfloodingwalletmaxcheck",                &firewallfloodingwalletmaxcheck,              false,      false,    false },

/* Dark features */
    { "firewall", 		"spork",                  &spork,                  true,      false,      false },
    { "firewall", 		"masternode",             &masternode,             true,      false,      true },
    { "firewall", 		"masternodelist",         &masternodelist,         true,      false,      false },
    
#ifdef ENABLE_WALLET
    { "Wallet_enabled", "darksend",               &darksend,               false,     false,      true },
    { "Wallet_enabled","getmininginfo",          &getmininginfo,          true,      false,     false },
    { "Wallet_enabled","getstakinginfo",         &getstakinginfo,         true,      false,     false },
    { "Wallet_enabled","getnewaddress",          &getnewaddress,          true,      false,     true },
    { "Wallet_enabled","getnewpubkey",           &getnewpubkey,           true,      false,     true },
    { "Wallet_enabled","getaccountaddress",      &getaccountaddress,      true,      false,     true },
    { "Wallet_enabled","setaccount",             &setaccount,             true,      false,     true },
    { "Wallet_enabled","getaccount",             &getaccount,             false,     false,     true },
    { "Wallet_enabled","getaddressesbyaccount",  &getaddressesbyaccount,  true,      false,     true },
    { "Wallet_enabled","sendtoaddress",          &sendtoaddress,          false,     false,     true },
    { "Wallet_enabled","getreceivedbyaddress",   &getreceivedbyaddress,   false,     false,     true },
    { "Wallet_enabled","getreceivedbyaccount",   &getreceivedbyaccount,   false,     false,     true },
    { "Wallet_enabled","listreceivedbyaddress",  &listreceivedbyaddress,  false,     false,     true },
    { "Wallet_enabled","listreceivedbyaccount",  &listreceivedbyaccount,  false,     false,     true },
    { "Wallet_enabled","backupwallet",           &backupwallet,           true,      false,     true },
    { "Wallet_enabled","keypoolrefill",          &keypoolrefill,          true,      false,     true },
    { "Wallet_enabled","walletpassphrase",       &walletpassphrase,       true,      false,     true },
    { "Wallet_enabled","walletpassphrasechange", &walletpassphrasechange, false,     false,     true },
    { "Wallet_enabled","walletlock",             &walletlock,             true,      false,     true },
    { "Wallet_enabled","encryptwallet",          &encryptwallet,          false,     false,     true },
    { "Wallet_enabled","getbalance",             &getbalance,             false,     false,     true },
    { "Wallet_enabled","move",                   &movecmd,                false,     false,     true },
    { "Wallet_enabled","sendfrom",               &sendfrom,               false,     false,     true },
    { "Wallet_enabled","sendmany",               &sendmany,               false,     false,     true },
    { "Wallet_enabled","addmultisigaddress",     &addmultisigaddress,     false,     false,     true },
    { "Wallet_enabled","addredeemscript",        &addredeemscript,        false,     false,     true },
    { "Wallet_enabled","gettransaction",         &gettransaction,         true,      true,      false },
    { "Wallet_enabled","listtransactions",       &listtransactions,       false,     false,     true },
    { "Wallet_enabled","listaddressgroupings",   &listaddressgroupings,   false,     false,     true },
    { "Wallet_enabled","signmessage",            &signmessage,            false,     false,     true },
    { "Wallet_enabled","getwork",                &getwork,                true,      false,     true },
    { "Wallet_enabled","getworkex",              &getworkex,              true,      false,     true },
    {"Wallet_enabled","listaccounts",           &listaccounts,           false,     false,     true },
    { "Wallet_enabled","getblocktemplate",       &getblocktemplate,       true,      false,     false },
    { "Wallet_enabled","submitblock",            &submitblock,            false,     false,     false },
    { "Wallet_enabled","listsinceblock",         &listsinceblock,         false,     false,     true },
    { "Wallet_enabled","dumpprivkey",            &dumpprivkey,            false,     false,     true },
    { "Wallet_enabled","dumpwallet",             &dumpwallet,             true,      false,     true },
    { "Wallet_enabled","imprivkey",          &importprivkey,          false,     false,     true },
    { "Wallet_enabled","importwallet",           &importwallet,           false,     false,     true },
    { "Wallet_enabled","importaddress",          &importaddress,          false,     false,     true },
    { "Wallet_enabled","listunspent",            &listunspent,            false,     false,     true },
    { "Wallet_enabled","settxfee",               &settxfee,               false,     false,     true },
    { "Wallet_enabled","getsubsidy",             &getsubsidy,             true,      true,      false },
    { "Wallet_enabled","getstakesubsidy",        &getstakesubsidy,        true,      true,      false },
    { "Wallet_enabled","reservebalance",         &reservebalance,         false,     true,      true },
    { "Wallet_enabled","createmultisig",         &createmultisig,         true,      true,      false },
    { "Wallet_enabled","checkwallet",            &checkwallet,            false,     true,      true },
    { "Wallet_enabled","repairwallet",           &repairwallet,           false,     true,      true },
    { "Wallet_enabled","resendtx",               &resendtx,               false,     true,      true },
    { "Wallet_enabled","makekeypair",            &makekeypair,            false,     true,      false },
    { "Wallet_enabled","checkkernel",            &checkkernel,            true,      false,     true },
    { "Wallet_enabled","getnewstealthaddress",   &getnewstealthaddress,   false,     false,     true },
    { "Wallet_enabled","liststealthaddresses",   &liststealthaddresses,   false,     false,     true },
    { "Wallet_enabled","scanforalltxns",         &scanforalltxns,         false,     false,     false },
    { "Wallet_enabled","scanforstealthtxns",     &scanforstealthtxns,     false,     false,     false },
    { "Wallet_enabled","importstealthaddress",   &importstealthaddress,   false,     false,     true },
    { "Wallet_enabled","sendtostealthaddress",   &sendtostealthaddress,   false,     false,     true },
    { "Wallet_enabled","smsgenable",             &smsgenable,             false,     false,     false },
    { "Wallet_enabled","smsgdisable",            &smsgdisable,            false,     false,     false },
    { "Wallet_enabled","smsglocalkeys",          &smsglocalkeys,          false,     false,     false },
    { "Wallet_enabled","smsgoptions",            &smsgoptions,            false,     false,     false },
    { "Wallet_enabled","smsgscanchain",          &smsgscanchain,          false,     false,     false },
    { "Wallet_enabled","smsgscanbuckets",        &smsgscanbuckets,        false,     false,     false },
    { "Wallet_enabled","smsgaddkey",             &smsgaddkey,             false,     false,     false },
    { "Wallet_enabled","smsggetpubkey",          &smsggetpubkey,          false,     false,     false },
    { "Wallet_enabled","smsgsend",               &smsgsend,               false,     false,     false },
    { "Wallet_enabled","smsgsendanon",           &smsgsendanon,           false,     false,     false },
    { "Wallet_enabled","smsginbox",              &smsginbox,              false,     false,     false },
    { "Wallet_enabled","smsgoutbox",             &smsgoutbox,             false,     false,     false },
    { "Wallet_enabled","smsgbuckets",            &smsgbuckets,            false,     false,     false },
 #endif       
        
 

        /* Mining */
        {"mining", "getblocktemplate", &getblocktemplate, true, false, false},
        {"mining", "getmininginfo", &getmininginfo, true, false, false},
//        {"mining", "getnetworkhashps", &getnetworkhashps, true, false, false},
//        {"mining", "prioritisetransaction", &prioritisetransaction, true, false, false},
        {"mining", "submitblock", &submitblock, true, true, false},
        {"mining", "reservebalance", &reservebalance, true, true, false},

#ifdef ENABLE_WALLET
        /* Coin generation */
        {"generating", "getgenerate", &getgenerate, true, false, false},
//        {"generating", "gethashespersec", &gethashespersec, true, false, false},
        {"generating", "setgenerate", &setgenerate, true, true, false},
#endif

        /* Raw transactions */
        {"rawtransactions", "createrawtransaction", &createrawtransaction, true, false, false},
        {"rawtransactions", "decoderawtransaction", &decoderawtransaction, true, false, false},
        {"rawtransactions", "decodescript", &decodescript, true, false, false},
        {"rawtransactions", "getrawtransaction", &getrawtransaction, true, false, false},
        {"rawtransactions", "sendrawtransaction", &sendrawtransaction, false, false, false},
        {"rawtransactions", "signrawtransaction", &signrawtransaction, false, false, false}, /* uses wallet if enabled */

       

        /* Not shown in help */
//        {"hidden", "invalidateblock", &invalidateblock, true, true, false},
//        {"hidden", "reconsiderblock", &reconsiderblock, true, true, false},
//        {"hidden", "setmocktime", &setmocktime, true, false, false},

        /* Myce features */
        {"myce", "masternode", &masternode, true, true, false},
//        {"myce", "listmasternodes", &listmasternodes, true, true, false},
//        {"myce", "getmasternodecount", &getmasternodecount, true, true, false},
//        {"myce", "masternodeconnect", &masternodeconnect, true, true, false},
//        {"myce", "createmasternodebroadcast", &createmasternodebroadcast, true, true, false},
//        {"myce", "decodemasternodebroadcast", &decodemasternodebroadcast, true, true, false},
//        {"myce", "relaymasternodebroadcast", &relaymasternodebroadcast, true, true, false},
//        {"myce", "masternodecurrent", &masternodecurrent, true, true, false},
//        {"myce", "masternodedebug", &masternodedebug, true, true, false},
//        {"myce", "startmasternode", &startmasternode, true, true, false},
//        {"myce", "createmasternodekey", &createmasternodekey, true, true, false},
//        {"myce", "getmasternodeoutputs", &getmasternodeoutputs, true, true, false},
//        {"myce", "listmasternodeconf", &listmasternodeconf, true, true, false},
//        {"myce", "getmasternodestatus", &getmasternodestatus, true, true, false},
//        {"myce", "getmasternodewinners", &getmasternodewinners, true, true, false},
//        {"myce", "getmasternodescores", &getmasternodescores, true, true, false},
//        {"myce", "mnbudget", &mnbudget, true, true, false},
//        {"myce", "preparebudget", &preparebudget, true, true, false},
//        {"myce", "submitbudget", &submitbudget, true, true, false},
//        {"myce", "mnbudgetvote", &mnbudgetvote, true, true, false},
//        {"myce", "getbudgetvotes", &getbudgetvotes, true, true, false},
//        {"myce", "getnextsuperblock", &getnextsuperblock, true, true, false},
//        {"myce", "getbudgetprojection", &getbudgetprojection, true, true, false},
//        {"myce", "getbudgetinfo", &getbudgetinfo, true, true, false},
//        {"myce", "mnbudgetrawvote", &mnbudgetrawvote, true, true, false},
//        {"myce", "mnfinalbudget", &mnfinalbudget, true, true, false},
//        {"myce", "checkbudgets", &checkbudgets, true, true, false},
//        {"myce", "mnsync", &mnsync, true, true, false},
        {"myce", "spork", &spork, true, true, false},
     

#ifdef ENABLE_WALLET
        /* Wallet */
//        {"wallet", "addmultisigaddress", &addmultisigaddress, true, false, true},
//        {"wallet", "autocombinerewards", &autocombinerewards, false, false, true},
//        {"wallet", "backupwallet", &backupwallet, true, false, true},
//        {"wallet", "dumpprivkey", &dumpprivkey, true, false, true},
//        {"wallet", "dumpwallet", &dumpwallet, true, false, true},
//        {"wallet", "encryptwallet", &encryptwallet, true, false, true},
//        {"wallet", "getaccountaddress", &getaccountaddress, true, false, true},
//        {"wallet", "getaccount", &getaccount, true, false, true},
//        {"wallet", "getaddressesbyaccount", &getaddressesbyaccount, true, false, true},
//        {"wallet", "getbalance", &getbalance, false, false, true},
//        {"wallet", "getnewaddress", &getnewaddress, true, false, true},
//        {"wallet", "getrawchangeaddress", &getrawchangeaddress, true, false, true},
//        {"wallet", "getreceivedbyaccount", &getreceivedbyaccount, false, false, true},
//        {"wallet", "getreceivedbyaddress", &getreceivedbyaddress, false, false, true},
//        {"wallet", "getstakingstatus", &getstakingstatus, false, false, true},
//        {"wallet", "getstakesplitthreshold", &getstakesplitthreshold, false, false, true},
//        {"wallet", "gettransaction", &gettransaction, true, false, true},
//        {"wallet", "getunconfirmedbalance", &getunconfirmedbalance, false, false, true},
//        {"wallet", "getwalletinfo", &getwalletinfo, false, false, true},
//        {"wallet", "importprivkey", &importprivkey, true, false, true},
//        {"wallet", "importwallet", &importwallet, true, false, true},
//        {"wallet", "importaddress", &importaddress, true, false, true},
//        {"wallet", "keypoolrefill", &keypoolrefill, true, false, true},
//        {"wallet", "listaccounts", &listaccounts, false, false, true},
//        {"wallet", "listaddressgroupings", &listaddressgroupings, false, false, true},
//        {"wallet", "listlockunspent", &listlockunspent, false, false, true},
//        {"wallet", "listreceivedbyaccount", &listreceivedbyaccount, false, false, true},
//        {"wallet", "listreceivedbyaddress", &listreceivedbyaddress, false, false, true},
//        {"wallet", "listsinceblock", &listsinceblock, false, false, true},
//        {"wallet", "listtransactions", &listtransactions, false, false, true},
//        {"wallet", "listunspent", &listunspent, false, false, true},
//        {"wallet", "lockunspent", &lockunspent, true, false, true},
//        {"wallet", "move", &movecmd, false, false, true},
//        {"wallet", "multisend", &multisend, false, false, true},
//        {"wallet", "sendfrom", &sendfrom, false, false, true},
//        {"wallet", "sendmany", &sendmany, false, false, true},
//        {"wallet", "sendtoaddress", &sendtoaddress, false, false, true},
//        {"wallet", "sendtoaddressix", &sendtoaddressix, false, false, true},
//        {"wallet", "burncoins", &burncoins, false, false, true},
//        {"wallet", "setaccount", &setaccount, true, false, true},
//        {"wallet", "setstakesplitthreshold", &setstakesplitthreshold, false, false, true},
//        {"wallet", "settxfee", &settxfee, true, false, true},
//        {"wallet", "signmessage", &signmessage, true, false, true},
//        {"wallet", "walletlock", &walletlock, true, false, true},
//        {"wallet", "walletpassphrasechange", &walletpassphrasechange, true, false, true},
//        {"wallet", "walletpassphrase", &walletpassphrase, true, false, true},


#endif // ENABLE_WALLET
};


CRPCTable::CRPCTable()
{
    unsigned int vcidx;
    
    
    for (vcidx = 0; vcidx < (sizeof(vRPCCommands) / sizeof(vRPCCommands[0])); vcidx++) {
        const CRPCCommand* pcmd;

        pcmd = &vRPCCommands[vcidx];
        mapCommands[pcmd->name] = pcmd;
    }
}

const CRPCCommand *CRPCTable::operator[](const std::string &name) const
{
    map<string, const CRPCCommand*>::const_iterator it = mapCommands.find(name);
    if (it == mapCommands.end())
        return NULL;
    return (*it).second;
}

bool StartRPC()
{
    LogPrint("rpc", "Starting RPC\n");

    fRPCRunning = true;
    g_rpcSignals.Started();
    return true;
}

void InterruptRPC()
{
    LogPrint("rpc", "Interrupting RPC\n");
    // Interrupt e.g. running longpolls
    fRPCRunning = false;
}

void StopRPC()
{
    LogPrint("rpc", "Stopping RPC\n");
    deadlineTimers.clear();
    g_rpcSignals.Stopped();
}

bool IsRPCRunning()
{
    return fRPCRunning;
}

void SetRPCWarmupStatus(const std::string& newStatus)
{
    LOCK(cs_rpcWarmup);
    rpcWarmupStatus = newStatus;
}

void SetRPCWarmupFinished()
{
    LOCK(cs_rpcWarmup);
    assert(fRPCInWarmup);
    fRPCInWarmup = false;
}

bool RPCIsInWarmup(std::string* outStatus)
{
    LOCK(cs_rpcWarmup);
    if (outStatus)
        *outStatus = rpcWarmupStatus;
    return fRPCInWarmup;
}



//Myce -> BSG
void JSONRequest::parse(const UniValue& valRequest)
{
    // Parse request
    if (!valRequest.isObject())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();



    // Parse id now so errors from here on will have the id
    id = find_value(request, "id");

    // Parse method
    UniValue valMethod = find_value(request, "method");
    if (valMethod.isNull())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Missing method");
    if (!valMethod.isStr())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Method must be a string");
        
        
    strMethod = valMethod.get_str();
    //if (strMethod != "getblocktemplate")
    if (strMethod != "getwork" && strMethod != "getblocktemplate")
        LogPrintf("rpc, ThreadRPCServer method=%s\n", SanitizeString(strMethod));


    // Parse params
    UniValue valParams = find_value(request, "params");
    if (valParams.isArray())
        params = valParams.get_array();
    else if (valParams.isNull())
        params = UniValue(UniValue::VARR);
    else
        throw JSONRPCError(RPC_INVALID_REQUEST, "Params must be an array");
        

}


static UniValue JSONRPCExecOne(const UniValue& req)
{
    UniValue rpc_result(UniValue::VOBJ);

    JSONRequest jreq;
    try {
        jreq.parse(req);

        UniValue result = tableRPC.execute(jreq.strMethod, jreq.params);
        rpc_result = JSONRPCReplyObj(result, NullUniValue, jreq.id);
    } catch (const UniValue& objError) {
        rpc_result = JSONRPCReplyObj(NullUniValue, objError, jreq.id);
    } catch (std::exception& e) {
        rpc_result = JSONRPCReplyObj(NullUniValue,
            JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
    }

    return rpc_result;
}

std::string JSONRPCExecBatch(const UniValue& vReq)
{
    UniValue ret(UniValue::VARR);
    for (unsigned int reqIdx = 0; reqIdx < vReq.size(); reqIdx++)
        ret.push_back(JSONRPCExecOne(vReq[reqIdx]));

    return ret.write() + "\n";
}

UniValue CRPCTable::execute(const std::string &strMethod, const UniValue &params) const
{

LogPrintf("RGP DEBUG CRPCTable::execute %s \n", strMethod );

    // Find method
    const CRPCCommand* pcmd = tableRPC[strMethod];
    if (!pcmd)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found");

    g_rpcSignals.PreCommand(*pcmd);

    try {
        // Execute

// 
// Note the return of getinfo is retuned back to HTTPReq_JSONRPC() in httprpc.cpp
//
        return pcmd->actor(params, false);
    } catch (std::exception& e) 
    {

        throw JSONRPCError(RPC_MISC_ERROR, e.what());
    }

    g_rpcSignals.PostCommand(*pcmd);
}

std::vector<std::string> CRPCTable::listCommands() const
{
    std::vector<std::string> commandList;
    typedef std::map<std::string, const CRPCCommand*> commandMap;

    std::transform( mapCommands.begin(), mapCommands.end(),
                   std::back_inserter(commandList),
                   boost::bind(&commandMap::value_type::first,_1) );
    return commandList;
}

std::string HelpExampleCli(string methodname, string args)
{
    return "> BSG-cli " + methodname + " " + args + "\n";
}

std::string HelpExampleRpc(string methodname, string args)
{
    return "> curl --user myusername --data-binary '{\"jsonrpc\": \"1.0\", \"id\":\"curltest\", "
           "\"method\": \"," +
           methodname + "\", \"params\": [" + args + "] }' -H 'content-type: text/plain;' http://127.0.0.1:23981/\n";
}

void RPCSetTimerInterfaceIfUnset(RPCTimerInterface *iface)
{
    if (!timerInterface)
        timerInterface = iface;
}

void RPCSetTimerInterface(RPCTimerInterface *iface)
{
    timerInterface = iface;
}

void RPCUnsetTimerInterface(RPCTimerInterface *iface)
{
    if (timerInterface == iface)
        timerInterface = NULL;
}

void RPCRunLater(const std::string& name, boost::function<void(void)> func, int64_t nSeconds)
{
    if (!timerInterface)
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No timer handler registered for RPC");
    deadlineTimers.erase(name);
    LogPrint("rpc", "queue run of timer %s in %i seconds (using %s)\n", name, nSeconds, timerInterface->Name());
    deadlineTimers.insert(std::make_pair(name, boost::shared_ptr<RPCTimerBase>(timerInterface->NewTimer(func, nSeconds*1000))));
}

const CRPCTable tableRPC;


bool HTTPAuthorized(map<string, string>& mapHeaders)
{

    string strAuth = mapHeaders["authorization"];
    if (strAuth.substr(0,6) != "Basic ")
        return false;
    string strUserPass64 = strAuth.substr(6); boost::trim(strUserPass64);
    string strUserPass = DecodeBase64(strUserPass64);

    return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}





