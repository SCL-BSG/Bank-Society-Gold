      // Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "init.h"
#include "addrman.h"
#include "main.h"
#include "chainparams.h"
#include "txdb.h"
#include "rpcserver.h"
#include "httpserver.h"
#include "httprpc.h" 
#include "net.h"
#include "key.h"
#include "pubkey.h"
#include "util.h"
#include "torcontrol.h"
#include "ui_interface.h"
#include "checkpoints.h"
#include "darksend-relay.h"
#include "activemasternode.h"
#include "masternode-payments.h"
#include "masternode.h"
#include "masternodeman.h"
#include "masternodeconfig.h"
#include "spork.h"
#include "smessage.h"

#ifdef ENABLE_WALLET
#include "db.h"
#include "wallet.h"
#include "walletdb.h"
#endif

#include <fstream>
#include <stdint.h>
#include <stdio.h>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/function.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/thread.hpp>
#include <openssl/crypto.h>

#ifndef WIN32
#include <signal.h>
#endif


using namespace std;
using namespace boost;

#ifdef ENABLE_WALLET
CWallet* pwalletMain = NULL;
int nWalletBackups = 10;
#endif
CClientUIInterface uiInterface;
bool fConfChange;
unsigned int nNodeLifespan;
unsigned int nDerivationMethodIndex;
unsigned int nMinerSleep;
bool fUseFastIndex;
bool fOnlyTor = false;



//////////////////////////////////////////////////////////////////////////////
//
// Shutdown
//

//
// Thread management and startup/shutdown:
//
// The network-processing threads are all part of a thread group
// created by AppInit() or the Qt main() function.
//
// A clean exit happens when StartShutdown() or the SIGTERM
// signal handler sets fRequestShutdown, which triggers
// the DetectShutdownThread(), which interrupts the main thread group.
// DetectShutdownThread() then exits, which causes AppInit() to
// continue (it .joins the shutdown thread).
// Shutdown() is then
// called to clean up database connections, and stop other
// threads that should only be stopped after the main network-processing
// threads have exited.
//
// Note that if running daemon the parent process returns from AppInit2
// before adding any threads to the threadGroup, so .join_all() returns
// immediately and the parent exits from main().
//
// Shutdown for Qt is very similar, only it uses a QTimer to detect
// fRequestShutdown getting set, and then does the normal Qt
// shutdown thing.
//

volatile bool fRequestShutdown = false;

void DetectShutdownThread( int status_info );

void DetectShutdownThread( int status_info )
{
//extern boost::thread_group threadGroup;

    LogPrintf("*** RGP DetectShutdownThread started \n");

    RenameThread("SocietyG-DetShut");

    while( !fRequestShutdown )
    {
        /* RGP Delay for 500ms */
        MilliSleep( 500 );
        //LogPrintf("*** RGP Waiting for Shutdown \n");
    }
LogPrintf("*** RGP calling Shutdown1 \n");
    if ( fRequestShutdown )
    {
        LogPrintf("*** RGP calling Shutdown1 \n");
        printf("Bank Society Gold Init process shutdown detected...");
        MilliSleep( 10000 );
        Shutdown();

        //threadGroup.interrupt_all();
        //threadGroup.join_all();


    }

}


void StartShutdown()
{

    MilliSleep(10000);
    printf("\n\n Bank Society Gold SHUTDOWN, delaying 10 seconds...\n\n");
    fRequestShutdown = true;
    LogPrintf("*** RGP StartShutdown\n");
    MilliSleep(10000);
}
bool ShutdownRequested()
{
    return fRequestShutdown;
}

static boost::scoped_ptr<ECCVerifyHandle> globalVerifyHandle;

void Shutdown()
{
    fRequestShutdown = true; // Needed when we shutdown the wallet
    LogPrintf("Shutdown : In progress...\n");
    MilliSleep(10000);
    
    static CCriticalSection cs_Shutdown;
    TRY_LOCK(cs_Shutdown, lockShutdown);
    if (!lockShutdown) return;

    RenameThread("SocietyG-shutoff");
    mempool.AddTransactionsUpdated(1);
    
    StopRPC();
    //StopRPCThreads();
    SecureMsgShutdown();

#ifdef ENABLE_WALLET
    ShutdownRPCMining();
    if (pwalletMain)
        bitdb.Flush(false);

    GeneratePoWcoins(false, NULL, false);

#endif
    StopNode();
    UnregisterNodeSignals(GetNodeSignals());
    DumpMasternodes();
    {
        LOCK(cs_main);
#ifdef ENABLE_WALLET
        if (pwalletMain)
            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
#endif
    }
#ifdef ENABLE_WALLET
    if (pwalletMain)
        bitdb.Flush(true);
#endif
    boost::filesystem::remove(GetPidFile());
    UnregisterAllWallets();
#ifdef ENABLE_WALLET
    delete pwalletMain;
    pwalletMain = NULL;
#endif
    globalVerifyHandle.reset();
    ECC_Stop();
    LogPrintf("Shutdown : done\n");
}

void Interrupt()
{
    InterruptHTTPServer();
    InterruptHTTPRPC();
    InterruptRPC();
    InterruptREST();
    InterruptTorControl();
}


//
// Signal handlers are very limited in what they are allowed to do, so:
//
void HandleSIGTERM(int)
{
    fRequestShutdown = true;
    return;
}

void HandleSIGHUP(int)
{
    fReopenDebugLog = true;
}

bool static InitError(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_ERROR);
    return false;
}

bool static InitWarning(const std::string &str)
{
    uiInterface.ThreadSafeMessageBox(str, "", CClientUIInterface::MSG_WARNING);
    return true;
}

bool static Bind(const CService &addr, bool fError = true) 
{
LogPrintf("RGP DEBUG Bind called \n" );

    if (IsLimited(addr))
        return false;
    std::string strError;
LogPrintf("RGO Inir BIND \n");
    if (!BindListenPort(addr, strError)) {
        if (fError)
            return InitError(strError);
        return false;
    }
    return true;
}




// Core-specific options shared between UI and daemon
std::string HelpMessage()
{
    string strUsage = _("Options:") + "\n";
    strUsage += "   ?                     " + _("This help message") + "\n";
    strUsage += "   conf=<file>           " + _("Specify configuration file (default: societyG.conf)") + "\n";
    strUsage += "   pid=<file>            " + _("Specify pid file (default: societyGd.pid)") + "\n";
    strUsage += "   datadir=<dir>         " + _("Specify data directory") + "\n";
    strUsage += "   wallet=<dir>          " + _("Specify wallet file (within data directory)") + "\n";
    strUsage += "   dbcache=<n>           " + _("Set database cache size in megabytes (default: 10)") + "\n";
    strUsage += "   dbwalletcache=<n>     " + _("Set wallet database cache size in megabytes (default: 1)") + "\n";
    strUsage += "   dblogsize=<n>         " + _("Set database disk log size in megabytes (default: 100)") + "\n";
    strUsage += "   timeout=<n>           " + _("Specify connection timeout in milliseconds (default: 5000)") + "\n";
    strUsage += "   proxy=<ip:port>       " + _("Connect through SOCKS5 proxy") + "\n";
    strUsage += "   tor=<ip:port>         " + _("Use proxy to reach tor hidden services (default: same as -proxy)") + "\n";
    strUsage += "   dns                   " + _("Allow DNS lookups for -addnode, -seednode and -connect") + "\n";
    strUsage += "   port=<port>           " + _("Listen for connections on <port> (default: 30140)") + "\n";
    strUsage += "   maxconnections=<n>    " + _("Maintain at most <n> connections to peers (default: 125)") + "\n";
    strUsage += "   addnode=<ip>          " + _("Add a node to connect to and attempt to keep the connection open") + "\n";
    strUsage += "   connect=<ip>          " + _("Connect only to the specified node(s)") + "\n";
    strUsage += "   seednode=<ip>         " + _("Connect to a node to retrieve peer addresses, and disconnect") + "\n";
    strUsage += "   externalip=<ip>       " + _("Specify your own public address") + "\n";
    strUsage += "   onlynet=<net>         " + _("Only connect to nodes in network <net> (IPv4, IPv6 or Tor)") + "\n";
    strUsage += "   discover              " + _("Discover own IP address (default: 1 when listening and no -externalip)") + "\n";
    strUsage += "   listen                " + _("Accept connections from outside (default: 1 if no -proxy or -connect)") + "\n";
    strUsage += "   bind=<addr>           " + _("Bind to given address. Use [host]:port notation for IPv6") + "\n";
    strUsage += "   dnsseed               " + _("Query for peer addresses via DNS lookup, if low on addresses (default: 1 unless -connect)") + "\n";
    strUsage += "   forcednsseed          " + _("Always query for peer addresses via DNS lookup (default: 0)") + "\n";
    strUsage += "   synctime              " + _("Sync time with other nodes. Disable if time on your system is precise e.g. syncing with NTP (default: 1)") + "\n";
    strUsage += "   banscore=<n>          " + _("Threshold for disconnecting misbehaving peers (default: 100)") + "\n";
    strUsage += "   bantime=<n>           " + _("Number of seconds to keep misbehaving peers from reconnecting (default: 86400)") + "\n";
    strUsage += "   maxreceivebuffer=<n>  " + _("Maximum per-connection receive buffer, <n>*1000 bytes (default: 5000)") + "\n";
    strUsage += "   maxsendbuffer=<n>     " + _("Maximum per-connection send buffer, <n>*1000 bytes (default: 1000)") + "\n";
#ifdef USE_UPNP
#if USE_UPNP
    strUsage += "   upnp                  " + _("Use UPnP to map the listening port (default: 1 when listening)") + "\n";
#else
    strUsage += "   upnp                  " + _("Use UPnP to map the listening port (default: 0)") + "\n";
#endif
#endif
    strUsage += "   paytxfee=<amt>        " + _("Fee per KB to add to transactions you send") + "\n";
    strUsage += "   mininput=<amt>        " + _("When creating transactions, ignore inputs with value less than this (default: 0.01)") + "\n";
    if (fHaveGUI)
        strUsage += "   server                " + _("Accept command line and JSON-RPC commands") + "\n";
#if !defined(WIN32)
    if (fHaveGUI)
        strUsage += "   daemon                " + _("Run in the background as a daemon and accept commands") + "\n";
#endif
    strUsage += "   testnet               " + _("Use the test network") + "\n";
    strUsage += "   debug=<category>      " + _("Output debugging information (default: 0, supplying <category> is optional)") + "\n";
    strUsage +=                               _("If <category> is not supplied, output all debugging information.") + "\n";
    strUsage +=                               _("<category> can be:");
    strUsage +=                                 " addrman, alert, db, lock, rand, rpc, selectcoins, mempool, net,"; // Don't translate these and qt below
    strUsage +=                                 " coinage, coinstake, creation, stakemodifier";
    if (fHaveGUI){
        strUsage += ", qt.\n";
    }else{
        strUsage += ".\n";
    }
    strUsage += "   logtimestamps         " + _("Prepend debug output with timestamp") + "\n";
    strUsage += "   shrinkdebugfile       " + _("Shrink debug.log file on client startup (default: 1 when no -debug)") + "\n";
    strUsage += "   printtoconsole        " + _("Send trace/debug info to console instead of debug.log file") + "\n";
    strUsage += "   regtest               " + _("Enter regression test mode, which uses a special chain in which blocks can be "
                                                "solved instantly. This is intended for regression testing tools and app development.") + "\n";
    strUsage += "   rpcuser=<user>        " + _("Username for JSON-RPC connections") + "\n";
    strUsage += "   rpcpassword=<pw>      " + _("Password for JSON-RPC connections") + "\n";
    strUsage += "   rpcport=<port>        " + _("Listen for JSON-RPC connections on <port> (default: 17171)") + "\n";
    strUsage += "   rpcallowip=<ip>       " + _("Allow JSON-RPC connections from specified IP address") + "\n";
    if (!fHaveGUI){
        strUsage += "   rpcconnect=<ip>       " + _("Send commands to node running on <ip> (default: 127.0.0.1)") + "\n";
        strUsage += "   rpcwait               " + _("Wait for RPC server to start") + "\n";
    }
    strUsage += "   rpcthreads=<n>        " + _("Set the number of threads to service RPC calls (default: 4)") + "\n";
    strUsage += "   blocknotify=<cmd>     " + _("Execute command when the best block changes (%s in cmd is replaced by block hash)") + "\n";
    strUsage += "   walletnotify=<cmd>    " + _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)") + "\n";
    strUsage += "   confchange            " + _("Require a confirmations for change (default: 0)") + "\n";
    strUsage += "   alertnotify=<cmd>     " + _("Execute command when a relevant alert is received (%s in cmd is replaced by message)") + "\n";
    strUsage += "   upgradewallet         " + _("Upgrade wallet to latest format") + "\n";
    strUsage += "   createwalletbackups=<n> " + _("Number of automatic wallet backups (default: 10)") + "\n";
    strUsage += "   keypool=<n>           " + _("Set key pool size to <n> (default: 100) (litemode: 10)") + "\n";
    strUsage += "   rescan                " + _("Rescan the block chain for missing wallet transactions") + "\n";
    strUsage += "   salvagewallet         " + _("Attempt to recover private keys from a corrupt wallet.dat") + "\n";
    strUsage += "   checkblocks=<n>       " + _("How many blocks to check at startup (default: 500, 0 = all)") + "\n";
    strUsage += "   checklevel=<n>        " + _("How thorough the block verification is (0-6, default: 1)") + "\n";
    strUsage += "   loadblock=<file>      " + _("Imports blocks from external blk000?.dat file") + "\n";
    strUsage += "   maxorphanblocks=<n>   " + strprintf(_("Keep at most <n> unconnectable blocks in memory (default: %u)"), DEFAULT_MAX_ORPHAN_BLOCKS) + "\n";

    strUsage += "\n" + _("Block creation options:") + "\n";
    strUsage += "   blockminsize=<n>      "   + _("Set minimum block size in bytes (default: 0)") + "\n";
    strUsage += "   blockmaxsize=<n>      "   + _("Set maximum block size in bytes (default: 250000)") + "\n";
    strUsage += "   blockprioritysize=<n> "   + _("Set maximum size of high-priority/low-fee transactions in bytes (default: 27000)") + "\n";

    strUsage += "\n" + _("SSL options: (see the Bitcoin Wiki for SSL setup instructions)") + "\n";
    strUsage += "   rpcssl                                  " + _("Use OpenSSL (https) for JSON-RPC connections") + "\n";
    strUsage += "   rpcsslcertificatechainfile=<file.cert>  " + _("Server certificate file (default: server.cert)") + "\n";
    strUsage += "   rpcsslprivatekeyfile=<file.pem>         " + _("Server private key (default: server.pem)") + "\n";
    strUsage += "   rpcsslciphers=<ciphers>                 " + _("Acceptable ciphers (default: TLSv1.2+HIGH:TLSv1+HIGH:!SSLv3:!SSLv2:!aNULL:!eNULL:!3DES:@STRENGTH)") + "\n";
    strUsage += "   litemode=<n>          " + _("Disable all Darksend and Stealth Messaging related functionality (0-1, default: 0)") + "\n";
    strUsage += "\n" + _("Masternode options:") + "\n";
    strUsage += "   masternode=<n>            " + _("Enable the client to act as a masternode (0-1, default: 0)") + "\n";
    strUsage += "   mnconf=<file>             " + _("Specify masternode configuration file (default: masternode.conf)") + "\n";
    strUsage += "   mnconflock=<n>            " + _("Lock masternodes from masternode configuration file (default: 1)") + "\n";
    strUsage += "   masternodeprivkey=<n>     " + _("Set the masternode private key") + "\n";
    strUsage += "   masternodeaddr=<n>        " + _("Set external address:port to get to this masternode (example: address:port)") + "\n";
    strUsage += "   masternodeminprotocol=<n> " + _("Ignore masternodes less than version (example: 61401; default : 0)") + "\n";

    strUsage += "\n" + _("Darksend options:") + "\n";
    strUsage += "   enabledarksend=<n>          " + _("Enable use of automated darksend for funds stored in this wallet (0-1, default: 0)") + "\n";
    strUsage += "   darksendrounds=<n>          " + _("Use N separate masternodes to anonymize funds  (2-8, default: 2)") + "\n";
    strUsage += "   anonymizeSocietyGamount=<n> " + _("Keep N SocietyG anonymized (default: 0)") + "\n";
    strUsage += "   liquidityprovider=<n>       " + _("Provide liquidity to Darksend by infrequently mixing coins on a continual basis (0-100, default: 0, 1=very frequent, high fees, 100=very infrequent, low fees)") + "\n";

    strUsage += "\n" + _("InstantX options:") + "\n";
    strUsage += "   enableinstantx=<n>    " + _("Enable instantx, show confirmations for locked transactions (bool, default: true)") + "\n";
    strUsage += "   instantxdepth=<n>     " + strprintf(_("Show N confirmations for a successfully locked transaction (0-9999, default: %u)"), nInstantXDepth) + "\n"; 
    strUsage += _("Secure messaging options:") + "\n" +
        "   nosmsg                                  " + _("Disable secure messaging.") + "\n" +
        "   debugsmsg                               " + _("Log extra debug messages.") + "\n" +
        "   smsgscanchain                           " + _("Scan the block chain for public key addresses on startup.") + "\n" +
    strUsage += "   stakethreshold=<n> " + _("This will set the output size of your stakes to never be below this number (default: 100)") + "\n";

    return strUsage;
}

/** Sanity checks
 *  Ensure that Bitcoin is running in a usable environment with all
 *  necessary library support.
 */
bool InitSanityCheck(void)
{
    if(!ECC_InitSanityCheck()) {
        InitError("OpenSSL appears to lack support for elliptic curve cryptography. For more "
                  "information, visit https://en.bitcoin.it/wiki/OpenSSL_and_EC_Libraries");
        return false;
    }

    // TODO: remaining sanity checks, see #4081

    return true;
}
void OnRPCStarted()
{
     //uiInterface.NotifyBlockTip.connect ( &RPCNotifyBlockChange );
}

//void OnRPCStopped()
//{
//     uiInterface.NotifyBlockTip.disconnect(&RPCNotifyBlockChange);
     //RPCNotifyBlockChange(false, nullptr);
//     RPCNotifyBlockChange(false);
//     g_best_block_cv.notify_all();
//     printf(" RGP BCLog::RPC, RPC stopped \n");
//}


void OnRPCStopped()
{
    cvBlockChange.notify_all();
    LogPrintf("rpc, RPC stopped.\n");
}

void OnRPCPreCommand(const CRPCCommand& cmd)
{
#ifdef ENABLE_WALLET
    if (cmd.reqWallet && !pwalletMain)
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (disabled)");
#endif

    // Observe safe mode
    string strWarning = GetWarnings("rpc");
    if (strWarning != "" && !GetBoolArg("disablesafemode", false) &&
        !cmd.okSafeMode)
        throw JSONRPCError(RPC_FORBIDDEN_BY_SAFE_MODE, string("Safe mode: ") + strWarning);
}

bool AppInitServers()
{
printf("RGP AppInitServers called \n");

    RPCServer::OnStarted(&OnRPCStarted);
    RPCServer::OnStopped(&OnRPCStopped);
    RPCServer::OnPreCommand(&OnRPCPreCommand);
    
    if (!InitHTTPServer())
        return false;
printf("RGP AppInitServers InitHTTPServer called \n");     
    if (!StartRPC())
    {
printf("RGP AppInitServers StartRPC FAILED!!! \n");  
        return false;
    }
printf("RGP AppInitServers StartRPC called \n");  
    if (!StartHTTPRPC())
        return false;
printf("RGP AppInitServers StartHTTPRPC called \n");  
    if (GetBoolArg("rest", false) && !StartREST())
        return false;
printf("RGP AppInitServers StartREST called \n");  
    if (!StartHTTPServer())
        return false;
printf("RGP AppInitServers StartHTTPServer called \n");  
    return true;
}


/** Initialize Bank Society Gold.
 *  @pre Parameters should be parsed and config file should be read.
 */
bool AppInit2(boost::thread_group& threadGroup)
{

    LogPrintf("*** RGP AppInit2() Started \n");

    // ********************************************************* Step 1: setup
#ifdef _MSC_VER
    // Turn off Microsoft heap dump noise
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, CreateFileA("NUL", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0));
#endif
#if _MSC_VER >= 1400
    // Disable confusing "helpful" text message on abort, Ctrl-C
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#ifdef WIN32
    // Enable Data Execution Prevention (DEP)
    // Minimum supported OS versions: WinXP SP3, WinVista >= SP1, Win Server 2008
    // A failure is non-critical and needs no further attention!
#ifndef PROCESS_DEP_ENABLE
    // We define this here, because GCCs winbase.h limits this to _WIN32_WINNT >= 0x0601 (Windows 7),
    // which is not correct. Can be removed, when GCCs winbase.h is fixed!
#define PROCESS_DEP_ENABLE 0x00000001
#endif
    typedef BOOL (WINAPI *PSETPROCDEPPOL)(DWORD);
    PSETPROCDEPPOL setProcDEPPol = (PSETPROCDEPPOL)GetProcAddress(GetModuleHandleA("Kernel32.dll"), "SetProcessDEPPolicy");
    if (setProcDEPPol != NULL) setProcDEPPol(PROCESS_DEP_ENABLE);
#endif
#ifndef WIN32
    umask(077);

    // Clean shutdown on SIGTERM
    struct sigaction sa;
    sa.sa_handler = HandleSIGTERM;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    // Reopen debug.log on SIGHUP
    struct sigaction sa_hup;
    sa_hup.sa_handler = HandleSIGHUP;
    sigemptyset(&sa_hup.sa_mask);
    sa_hup.sa_flags = 0;
    sigaction(SIGHUP, &sa_hup, NULL);
#endif

    // ********************************************************* Step 2: parameter interactions

    nNodeLifespan = GetArg("addrlifespan", 7);
    fUseFastIndex = GetBoolArg("fastindex", true);
    nMinerSleep = GetArg("minersleep", 500);

    nDerivationMethodIndex = 0;

    if (!SelectParamsFromCommandLine()) {
        return InitError("Invalid combination of -testnet and -regtest.");
    }

    if (mapArgs.count("bind")) {
        // when specifying an explicit binding address, you want to listen on it
        // even when -connect or -proxy is specified
        if (SoftSetBoolArg("listen", true))
            LogPrintf("AppInit2 : parameter interaction: bind set -> setting listen=1\n");
    }

    LogPrintf("*** RGP AppInit2() Read Masternode config \n");


    // Process masternode config
    masternodeConfig.read(GetMasternodeConfigFile());

    if (mapArgs.count("connect") && mapMultiArgs["connect"].size() > 0) {
        // when only connecting to trusted nodes, do not seed via DNS, or listen by default
        if (SoftSetBoolArg("dnsseed", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -dnsseed=0\n");
        if (SoftSetBoolArg("listen", false))
            LogPrintf("AppInit2 : parameter interaction: -connect set -> setting -listen=0\n");
    }

    if (mapArgs.count("proxy")) {
        // to protect privacy, do not listen by default if a default proxy server is specified
        if (SoftSetBoolArg("listen", false))
            LogPrintf("AppInit2 : parameter interaction: proxy set -> setting -listen=0\n");
        // to protect privacy, do not use UPNP when a proxy is set. The user may still specify -listen=1
        // to listen locally, so don't rely on this happening through -listen below.
        if (SoftSetBoolArg("upnp", false))
            LogPrintf("AppInit2 : parameter interaction: proxy set -> setting -upnp=0\n");
        // to protect privacy, do not discover addresses by default
        if (SoftSetBoolArg("discover", false))
            LogPrintf("AppInit2 : parameter interaction: proxy set -> setting -discover=0\n");
    }

    if (!GetBoolArg("listen", true)) {
        // do not map ports or try to retrieve public IP when not listening (pointless)
        if (SoftSetBoolArg("upnp", false))
            LogPrintf("AppInit2 : parameter interaction: listen=0 -> setting -upnp=0\n");
        if (SoftSetBoolArg("discover", false))
            LogPrintf("AppInit2 : parameter interaction: listen=0 -> setting -discover=0\n");
    }

    if (mapArgs.count("externalip")) {
        // if an explicit public IP is specified, do not try to find others
        if (SoftSetBoolArg("discover", false))
            LogPrintf("AppInit2 : parameter interaction: externalip set -> setting -discover=0\n");
    }

    if (GetBoolArg("salvagewallet", false)) {
        // Rewrite just private keys: rescan to find transactions
        if (SoftSetBoolArg("rescan", true))
            LogPrintf("AppInit2 : parameter interaction: salvagewallet=1 -> setting -rescan=1\n");
    }

    // ********************************************************* Step 3: parameter-to-internal-flags

    fDebug = !mapMultiArgs["debug"].empty();
    // Special-case: if -debug=0/-nodebug is set, turn off debugging messages
    const vector<string>& categories = mapMultiArgs["debug"];
    if (GetBoolArg("nodebug", false) || find(categories.begin(), categories.end(), string("0")) != categories.end())
        fDebug = false;

    if(fDebug)
    {
	fDebugSmsg = true;
    } else
    {
        fDebugSmsg = GetBoolArg("debugsmsg", false);
    }
    if (fLiteMode)
        fNoSmsg = true;
    else
        fNoSmsg = GetBoolArg("nosmsg", false);


    // Check for -debugnet (deprecated)
    if (GetBoolArg("debugnet", false))
        InitWarning(_("Warning: Deprecated argument debugnet ignored, use -debug=net"));
    // Check for -socks - as this is a privacy risk to continue, exit here
    if (mapArgs.count("socks"))
    {
        /* RGP commented out, if the user selects SOCKS, the wallet never opens again */
        /* return InitError(_("Error: Unsupported argument -socks found. Setting SOCKS version isn't possible anymore, only SOCKS5 proxies are supported."));  */
    }

    LogPrintf("*** RGP AppInit2() Daemon check \n");


    if (fDaemon)
        fServer = true;
    else
    	fServer = GetBoolArg("server", false);
    if (!fHaveGUI) 
       fServer = true;
    fPrintToConsole = GetBoolArg("printtoconsole", false);
    fLogTimestamps = GetBoolArg("logtimestamps", true);
#ifdef ENABLE_WALLET
    bool fDisableWallet = GetBoolArg("disablewallet", false);
#endif

    if (mapArgs.count("timeout"))
    {
        int nNewTimeout = GetArg("timeout", 5000);
        if (nNewTimeout > 0 && nNewTimeout < 600000)
            nConnectTimeout = nNewTimeout;
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("paytxfee"))
    {
        if (!ParseMoney(mapArgs["paytxfee"], nTransactionFee))
            return InitError(strprintf(_("Invalid amount for -paytxfee=<amount>: '%s'"), mapArgs["paytxfee"]));
        if (nTransactionFee > 0.25 * COIN)
            InitWarning(_("Warning: -paytxfee is set very high! This is the transaction fee you will pay if you send a transaction."));
    }
#endif

    fConfChange = GetBoolArg("confchange", false);

#ifdef ENABLE_WALLET
    if (mapArgs.count("mininput"))
    {
        if (!ParseMoney(mapArgs["mininput"], nMinimumInputValue))
            return InitError(strprintf(_("Invalid amount for -mininput=<amount>: '%s'"), mapArgs["mininput"]));
    }
#endif

    // ********************************************************* Step 4: application initialization: dir lock, daemonize, pidfile, debug log

    // Initialize elliptic curve code
    ECC_Start();
    globalVerifyHandle.reset(new ECCVerifyHandle());

    // Sanity check
    if (!InitSanityCheck())
        return InitError(_("Initialization sanity check failed. SocietyG is shutting down."));

    std::string strDataDir = GetDataDir().string();

printf("RGP init.cpp %s  \n", strDataDir );

#ifdef ENABLE_WALLET
    std::string strWalletFileName = GetArg("wallet", "wallet.dat");

    // strWalletFileName must be a plain filename without a directory
    if (strWalletFileName != boost::filesystem::basename(strWalletFileName) + boost::filesystem::extension(strWalletFileName))
        return InitError(strprintf(_("Wallet %s resides outside data directory %s."), strWalletFileName, strDataDir));
#endif
    // Make sure only a single Bitcoin process is using the data directory.
    boost::filesystem::path pathLockFile = GetDataDir() / ".lock";
    FILE* file = fopen(pathLockFile.string().c_str(), "a"); // empty lock file; created if it doesn't exist.
    if (file) fclose(file);
    static boost::interprocess::file_lock lock(pathLockFile.string().c_str());
    if (!lock.try_lock())
        return InitError(strprintf(_("Cannot obtain a lock on data directory %s. SocietyG is probably already running."), strDataDir));

    if (GetBoolArg("shrinkdebugfile", !fDebug))
        ShrinkDebugFile();

    LogPrintf("\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
    LogPrintf("Bank Society Gold version %s (%s)\n", FormatFullVersion(), CLIENT_DATE);
    LogPrintf("Using OpenSSL version %s\n", SSLeay_version(SSLEAY_VERSION));
    if (!fLogTimestamps)
        LogPrintf("Startup time: %s\n", DateTimeStrFormat("%x %H:%M:%S", GetTime()));
    LogPrintf("Default data directory %s\n", GetDefaultDataDir().string());
    LogPrintf("Used data directory %s\n", strDataDir);
    std::ostringstream strErrors;

    if (mapArgs.count("masternodepaymentskey")) // masternode payments priv key
    {
        if (!masternodePayments.SetPrivKey(GetArg("masternodepaymentskey", "")))
            return InitError(_("Unable to sign masternode payment winner, wrong key?"));
        if (!sporkManager.SetPrivKey(GetArg("masternodepaymentskey", "")))
            return InitError(_("Unable to sign spork message, wrong key?"));
    }

    //ignore masternodes below protocol version
    nMasternodeMinProtocol = GetArg("masternodeminprotocol", MIN_POOL_PEER_PROTO_VERSION);

    if (fDaemon)
        fprintf(stdout, "SocietyG server starting\n"); 

   /* Start the RPC server already.  It will be started in "warmup" mode
     * and not really process calls already (but it will signify connections
     * that the server is there and will be ready later).  Warmup mode will
     * be disabled when initialisation is finished.
     */
    //if (GetBoolArg("server", false)) 
    //{

LogPrintf("RGP DEBUG BEFORE AppInitServers  \n");

        uiInterface.InitMessage.connect(SetRPCWarmupStatus);
        if (!AppInitServers())
        {
            LogPrintf("RGP DEBUG AppInitServers FAILED \n");
            return InitError(_("Unable to start HTTP server. See debug log for details."));
        }
        
       
LogPrintf("RGP DEBUG AFTER AppInitServers \n");
        
    //}


    int64_t nStart;

    // ********************************************************* Step 5: Backup wallet and verify wallet database integrity
#ifdef ENABLE_WALLET

LogPrintf("RGP DEBUG AFTER AppInitServers Debug 1 \n");

    if (!fDisableWallet) {
LogPrintf("RGP DEBUG AFTER AppInitServers Debug 1\n");
        filesystem::path backupDir = GetDataDir() / "backups";
        if (!filesystem::exists(backupDir))
        {
            // Always create backup folder to not confuse the operating system's file browser
            filesystem::create_directories(backupDir);
        }
        nWalletBackups = GetArg("createwalletbackups", 10);
        nWalletBackups = std::max(0, std::min(10, nWalletBackups));
        if(nWalletBackups > 0)
        {
            if (filesystem::exists(backupDir))
            {
                // Create backup of the wallet
                std::string dateTimeStr = DateTimeStrFormat(".%Y-%m-%d-%H.%M", GetTime());
                std::string backupPathStr = backupDir.string();
                backupPathStr += "/" + strWalletFileName;
                std::string sourcePathStr = GetDataDir().string();
                sourcePathStr += "/" + strWalletFileName;
                boost::filesystem::path sourceFile = sourcePathStr;
                boost::filesystem::path backupFile = backupPathStr + dateTimeStr;
                sourceFile.make_preferred();
                backupFile.make_preferred();
                try {
                    boost::filesystem::copy_file(sourceFile, backupFile);
                    LogPrintf("Creating backup of %s -> %s\n", sourceFile, backupFile);
                } catch(boost::filesystem::filesystem_error &error) {
                    LogPrintf("Failed to create backup %s\n", error.what());
                }
                // Keep only the last 10 backups, including the new one of course
                typedef std::multimap<std::time_t, boost::filesystem::path> folder_set_t;
                folder_set_t folder_set;
                boost::filesystem::directory_iterator end_iter;
                boost::filesystem::path backupFolder = backupDir.string();
                backupFolder.make_preferred();
                // Build map of backup files for current(!) wallet sorted by last write time
                boost::filesystem::path currentFile;
                for (boost::filesystem::directory_iterator dir_iter(backupFolder); dir_iter != end_iter; ++dir_iter)
                {
                    // Only check regular files
                    if ( boost::filesystem::is_regular_file(dir_iter->status()))
                    {
                        currentFile = dir_iter->path().filename();
                        // Only add the backups for the current wallet, e.g. wallet.dat.*
                        if(currentFile.string().find(strWalletFileName) != string::npos)
                        {
                            folder_set.insert(folder_set_t::value_type(boost::filesystem::last_write_time(dir_iter->path()), *dir_iter));
                        }
                    }
                }
                // Loop backward through backup files and keep the N newest ones (1 <= N <= 10)
                int counter = 0;
                BOOST_REVERSE_FOREACH(PAIRTYPE(const std::time_t, boost::filesystem::path) file, folder_set)
                {
                    counter++;
                    if (counter > nWalletBackups)
                    {
                        // More than nWalletBackups backups: delete oldest one(s)
                        try {
                            boost::filesystem::remove(file.second);
                            LogPrintf("Old backup deleted: %s\n", file.second);
                        } catch(boost::filesystem::filesystem_error &error) {
                            LogPrintf("Failed to delete backup %s\n", error.what());
                        }

                    }
                }
            }
        }

LogPrintf("RGP Debug 002 \n");
        uiInterface.InitMessage(_("Verifying database integrity..."));

        if (!bitdb.Open(GetDataDir()))
        {
            // try moving the database env out of the way
            boost::filesystem::path pathDatabase = GetDataDir() / "database";
            boost::filesystem::path pathDatabaseBak = GetDataDir() / strprintf("database.%d.bak", GetTime());
            try {
                boost::filesystem::rename(pathDatabase, pathDatabaseBak);
                LogPrintf("Moved old %s to %s. Retrying.\n", pathDatabase.string(), pathDatabaseBak.string());
            } catch(boost::filesystem::filesystem_error &error) {
                 // failure is ok (well, not really, but it's not worse than what we started with)
            }

            // try again
            if (!bitdb.Open(GetDataDir())) {
                // if it still fails, it probably means we can't even create the database env
                string msg = strprintf(_("Error initializing wallet database environment %s!"), strDataDir);
                return InitError(msg);
            }
        }

LogPrintf("RGP Init SALAVGE wallet \n");

        if (GetBoolArg("salvagewallet", false ))
        {
            LogPrintf("RGP Init SALAVGE wallet FALSE \n");

            // Recover readable keypairs:
            if (!CWalletDB::Recover(bitdb, strWalletFileName, true))
                return false;
        }
        else
            LogPrintf("RGP Init SALAVGE wallet not processed \n");

        if (filesystem::exists(GetDataDir() / strWalletFileName))
        {
            CDBEnv::VerifyResult r = bitdb.Verify(strWalletFileName, CWalletDB::Recover);
            if (r == CDBEnv::RECOVER_OK)
            {
                string msg = strprintf(_("Warning: wallet.dat corrupt, data salvaged!"
                                         " Original wallet.dat saved as wallet.{timestamp}.bak in %s; if"
                                         " your balance or transactions are incorrect you should"
                                         " restore from a backup."), strDataDir);
                InitWarning(msg);
            }
            if (r == CDBEnv::RECOVER_FAIL)
                return InitError(_("wallet.dat corrupt, salvage failed"));
        }

    } // (!fDisableWallet)
#endif // ENABLE_WALLET
    // ********************************************************* Step 6: network initialization




    //LogPrintf("RGP Debug 006 \n");

    RegisterNodeSignals(GetNodeSignals());

    // format user agent, check total size
    strSubVersion = FormatSubVersion(CLIENT_NAME, CLIENT_VERSION, mapMultiArgs.count("uacomment") ? mapMultiArgs["uacomment"] : std::vector<string>());
    if (strSubVersion.size() > MAX_SUBVERSION_LENGTH) {
        return InitError(strprintf("Total length of network version string %i exceeds maximum of %i characters. Reduce the number and/or size of uacomments.",
            strSubVersion.size(), MAX_SUBVERSION_LENGTH));
    }
 //LogPrintf("RGP Debug 006a \n");   
    if (mapArgs.count("onlynet")) {
        std::set<enum Network> nets;
        BOOST_FOREACH(std::string snet, mapMultiArgs["onlynet"]) {
            enum Network net = ParseNetwork(snet);
	    if(net == NET_TOR)
		fOnlyTor = true;

            if (net == NET_UNROUTABLE)
                return InitError(strprintf(_("Unknown network specified in -onlynet: '%s'"), snet));
            nets.insert(net);
        }
        for (int n = 0; n < NET_MAX; n++) {
            enum Network net = (enum Network)n;
            if (!nets.count(net))
                SetLimited(net);
        }
    } else {
        SetReachable(NET_IPV4);
        SetReachable(NET_IPV6);
    }
//LogPrintf("RGP Debug 006b \n");
    CService addrProxy;
    bool fProxy = false;
    if (mapArgs.count("proxy")) {
        addrProxy = CService(mapArgs["proxy"], 9050);
        if (!addrProxy.IsValid())
            return InitError(strprintf(_("Invalid -proxy address: '%s'"), mapArgs["proxy"]));

        if (!IsLimited(NET_IPV4))
            SetProxy(NET_IPV4, addrProxy);
        if (!IsLimited(NET_IPV6))
            SetProxy(NET_IPV6, addrProxy);
        SetNameProxy(addrProxy);
        fProxy = true;
    }

    // tor can override normal proxy, -notor disables tor entirely
    if (!(mapArgs.count("tor") && mapArgs["tor"] == "0") && (fProxy || mapArgs.count("tor"))) {
        CService addrOnion;
        if (!mapArgs.count("tor"))
            addrOnion = addrProxy;
        else
            addrOnion = CService(mapArgs["tor"], 9050);
        if (!addrOnion.IsValid())
            return InitError(strprintf(_("Invalid -tor address: '%s'"), mapArgs["tor"]));
        SetProxy(NET_TOR, addrOnion);
        SetReachable(NET_TOR);
    }

    // see Step 2: parameter interactions for more information about these
    fNoListen = GetBoolArg("-listen", false);

    if ( fNoListen )
       LogPrintf("RGP DEBUG fNoListen is TRUE \n");
    else
       LogPrintf("RGP DEBUG fNoListen is FALSE \n");

    fDiscover = GetBoolArg("-discover", true);
    fNameLookup = GetBoolArg("-dns", true);
    
//LogPrintf("RGP Debug 006c \n");

    bool fBound = false;
    if (!fNoListen)
    {
    //LogPrintf("RGP Debug 006ca \n");
        std::string strError;
        if (mapArgs.count("-bind"))
         {
             //LogPrintf("RGP Debug 006cb \n");
            BOOST_FOREACH(std::string strBind, mapMultiArgs["-bind"]) 
            {
                CService addrBind;
                if (!Lookup(strBind.c_str(), addrBind, GetListenPort(), false))
                {
                    //LogPrintf("RGP Debug 006cc \n");
                    return InitError(strprintf(_("Cannot resolve -bind address: '%s'"), strBind));
                }
                fBound |= Bind(addrBind);
            }
        } else 
        {
            //LogPrintf("RGP Debug 006cd \n");
        
            struct in_addr inaddr_any;
            inaddr_any.s_addr = INADDR_ANY;
            if (!IsLimited(NET_IPV6))
            {
                LogPrintf("RGP Init Bind Address6  %s \n", CService(in6addr_any, GetListenPort()).ToStringIP() );
                fBound |= Bind(CService(in6addr_any, GetListenPort()), false);
            }
            if (!IsLimited(NET_IPV4))
            {
                LogPrintf("RGP Init Bind Address4 %s \n", CService(inaddr_any, GetListenPort()).ToStringIP() );
                fBound |= Bind(CService(inaddr_any, GetListenPort()), !fBound);
            }
        }
            //LogPrintf("RGP Debug 006ce \n");
        if (!fBound)
	{
	    //LogPrintf("RGP Debug 006cf \n");
            return InitError(_("Failed to listen on any port. Use listen=0 if you want this."));
        }
    }

LogPrintf("RGP Debug 006c \n");

    if (mapArgs.count("externalip"))
    {
        BOOST_FOREACH(string strAddr, mapMultiArgs["externalip"]) {
            CService addrLocal(strAddr, GetListenPort(), fNameLookup);
            if (!addrLocal.IsValid())
                return InitError(strprintf(_("Cannot resolve -externalip address: '%s'"), strAddr));
            AddLocal(CService(strAddr, GetListenPort(), fNameLookup), LOCAL_MANUAL);
        }
    }

#ifdef ENABLE_WALLET
    if (mapArgs.count("reservebalance")) // ppcoin: reserve balance amount
    {
        if (!ParseMoney(mapArgs["reservebalance"], nReserveBalance))
        {
            InitError(_("Invalid amount for -reservebalance=<amount>"));
            return false;
        }
    }
#endif
LogPrintf("RGP Debug 006d \n");
    BOOST_FOREACH(string strDest, mapMultiArgs["seednode"])
        AddOneShot(strDest);

LogPrintf("RGP Debug 006e \n");
// https://medium.com/@brakmic/a-tool-for-listening-to-bitcoin-zmq-notifications-in-c-c516298ebd71
// https://bitcoindev.network/accessing-bitcoins-zeromq-interface/

    // ********************************************************* Step 7: load blockchain

    if (GetBoolArg("loadblockindextest", false))
    {
        CTxDB txdb("r");
        txdb.LoadBlockIndex();
        PrintBlockTree();
        return false;
    }
LogPrintf("RGP Debug 006f \n");
    uiInterface.InitMessage(_("Loading block index..."));

    nStart = GetTimeMillis();
    if (!LoadBlockIndex())
        return InitError(_("Error loading block database"));
LogPrintf("RGP Debug 006g \n");

    // as LoadBlockIndex can take several minutes, it's possible the user
    // requested to kill bitcoin-qt during the last operation. If so, exit.
    // As the program has not fully started yet, Shutdown() is possibly overkill.
    if (fRequestShutdown)
    {
        LogPrintf("Shutdown requested. Exiting.\n");
        return false;
    }
    LogPrintf(" block index %15dms\n", GetTimeMillis() - nStart);

    if (GetBoolArg("printblockindex", false) || GetBoolArg("printblocktree", false))
    {
        PrintBlockTree();
        return false;
    }

    if (mapArgs.count("printblock"))
    {
        string strMatch = mapArgs["printblock"];
        int nFound = 0;
        for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
        {
            uint256 hash = (*mi).first;
            if (strncmp(hash.ToString().c_str(), strMatch.c_str(), strMatch.size()) == 0)
            {
                CBlockIndex* pindex = (*mi).second;
                CBlock block;
                block.ReadFromDisk(pindex);
                block.BuildMerkleTree();
                LogPrintf("%s\n", block.ToString());
                nFound++;
            }
        }
        if (nFound == 0)
            LogPrintf("No blocks matching %s were found\n", strMatch);
        return false;
    }
LogPrintf("RGP Debug 006h \n");
    // ********************************************************* Step 8: load wallet
#ifdef ENABLE_WALLET
    if (fDisableWallet) {
        pwalletMain = NULL;
        LogPrintf("Wallet disabled!\n");
    } else {

        uiInterface.InitMessage(_("Loading wallet..."));
LogPrintf("RGP Debug 006i \n");
        nStart = GetTimeMillis();
        bool fFirstRun = true;
        pwalletMain = new CWallet(strWalletFileName);
        DBErrors nLoadWalletRet = pwalletMain->LoadWallet(fFirstRun);
        if (nLoadWalletRet != DB_LOAD_OK)
        {
            if (nLoadWalletRet == DB_CORRUPT)
                strErrors << _("Error loading wallet.dat: Wallet corrupted") << "\n";
            else if (nLoadWalletRet == DB_NONCRITICAL_ERROR)
            {
                string msg(_("Warning: error reading wallet.dat! All keys read correctly, but transaction data"
                             " or address book entries might be missing or incorrect."));
                InitWarning(msg);
            }
            else if (nLoadWalletRet == DB_TOO_NEW)
                strErrors << _("Error loading wallet.dat: Wallet requires newer version of SocietyG") << "\n";
            else if (nLoadWalletRet == DB_NEED_REWRITE)
            {
                strErrors << _("Wallet needed to be rewritten: restart SocietyG to complete") << "\n";
                LogPrintf("%s", strErrors.str());
                return InitError(strErrors.str());
            }
            else
                strErrors << _("Error loading wallet.dat") << "\n";
        }


        if (GetBoolArg("upgradewallet", fFirstRun))
        {
            int nMaxVersion = GetArg("upgradewallet", 0);
            if (nMaxVersion == 0) // the -upgradewallet without argument case
            {
                LogPrintf("Performing wallet upgrade to %i\n", FEATURE_LATEST);
                nMaxVersion = CLIENT_VERSION;
                pwalletMain->SetMinVersion(FEATURE_LATEST); // permanently upgrade the wallet immediately
            }
            else
                LogPrintf("Allowing wallet upgrade up to %i\n", nMaxVersion);
            if (nMaxVersion < pwalletMain->GetVersion())
                strErrors << _("Cannot downgrade wallet") << "\n";
            pwalletMain->SetMaxVersion(nMaxVersion);
        }

        if (fFirstRun)
        {
            // Create new keyUser and set as default key
            RandAddSeedPerfmon();

            CPubKey newDefaultKey;
            if (pwalletMain->GetKeyFromPool(newDefaultKey)) {
                pwalletMain->SetDefaultKey(newDefaultKey);
                if (!pwalletMain->SetAddressBookName(pwalletMain->vchDefaultKey.GetID(), ""))
                    strErrors << _("Cannot write default address") << "\n";
            }

            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
        }
LogPrintf("RGP Debug 006h \n");
        LogPrintf("%s", strErrors.str());
        LogPrintf(" wallet      %15dms\n", GetTimeMillis() - nStart);

        RegisterWallet(pwalletMain);

        CBlockIndex *pindexRescan = pindexBest;
        if (GetBoolArg("rescan", false))
            pindexRescan = pindexGenesisBlock;
        else
        {
            CWalletDB walletdb(strWalletFileName);
            CBlockLocator locator;
            if (walletdb.ReadBestBlock(locator))
                pindexRescan = locator.GetBlockIndex();
            else
                pindexRescan = pindexGenesisBlock;
        }
        if (pindexBest != pindexRescan && pindexBest && pindexRescan && pindexBest->nHeight > pindexRescan->nHeight)
        {
            uiInterface.InitMessage(_("Rescanning..."));
            LogPrintf("Rescanning last %i blocks (from block %i)...\n", pindexBest->nHeight - pindexRescan->nHeight, pindexRescan->nHeight);
            nStart = GetTimeMillis();
            pwalletMain->ScanForWalletTransactions(pindexRescan, true);
            LogPrintf(" rescan      %15dms\n", GetTimeMillis() - nStart);
            pwalletMain->SetBestChain(CBlockLocator(pindexBest));
            nWalletDBUpdated++;
        }
    } // (!fDisableWallet)
#else // ENABLE_WALLET
    LogPrintf("No wallet compiled in!\n");
#endif // !ENABLE_WALLET
    // ********************************************************* Step 9: import blocks

    std::vector<boost::filesystem::path> vImportFiles;
    if (mapArgs.count("loadblock"))
    {
        BOOST_FOREACH(string strFile, mapMultiArgs["loadblock"])
            vImportFiles.push_back(strFile);
    }
    threadGroup.create_thread(boost::bind(&ThreadImport, vImportFiles));

    // ********************************************************* Step 10: load peers

    uiInterface.InitMessage(_("Loading addresses..."));

    nStart = GetTimeMillis();

    {
        CAddrDB adb;
        if (!adb.Read(addrman))
            LogPrintf("Invalid or missing peers.dat; recreating\n");
    }

    LogPrintf("Loaded %i addresses from peers.dat  %dms\n", addrman.size(), GetTimeMillis() - nStart);

    // ********************************************************* Step 10.1: startup secure messaging
    
    SecureMsgStart(fNoSmsg, GetBoolArg("smsgscanchain", false));

    // ********************************************************* Step 11: start node

    if (!CheckDiskSpace())
        return false;

    if (!strErrors.str().empty())
        return InitError(strErrors.str());

    uiInterface.InitMessage(_("Loading masternode cache..."));

    CMasternodeDB mndb;
    CMasternodeDB::ReadResult readResult = mndb.Read(mnodeman);
    if (readResult == CMasternodeDB::FileError)
        LogPrintf("Missing masternode cache file - mncache.dat, will try to recreate\n");
    else if (readResult != CMasternodeDB::Ok)
    {
        LogPrintf("Error reading mncache.dat: ");
        if(readResult == CMasternodeDB::IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
    }

vector<std::string> MasterNodeAddress_List;
vector<std::string> MasterNodePrivateKey_List;

    LogPrintf("RGP Debug fMasterNode checks start in init.cpp \n");

    fMasterNode = GetBoolArg("masternode", true);
    if(fMasterNode) 
    {
        LogPrintf("IS DARKSEND MASTER NODE\n");


MasterNodeAddress_List = mapMultiArgs["-masternodeaddr"];
MasterNodePrivateKey_List = mapMultiArgs["-masternodeprivkey"];

//map<string, string> mapArgs;
//map<string, vector<string> > mapMultiArgs;

list<string> MN_IP_addresses(0);
list<string> MN_private_keys(0);
std::string MasterNode_Address;
std::string MasterNode_Private_Key;

        BOOST_FOREACH(string& strAddNode, MasterNodeAddress_List)
        {
             printf("RGP DEBUG Init getting masternode address %s \n", &strAddNode[0] );
             MN_IP_addresses.push_back(strAddNode);
        }

        BOOST_FOREACH(string& strAddNode, MN_IP_addresses) 
        {
            MasterNode_Address = strAddNode;
        }

printf("RGP DEBUG Init MasterNode IP  addr %s\n", MasterNode_Address.c_str() );

        strMasterNodeAddr = MasterNode_Address;
        if( !strMasterNodeAddr.empty() )
        {
            CService addrTest = CService(strMasterNodeAddr, fNameLookup);
            if ( !addrTest.IsValid() ) 
            {
                return InitError("Invalid -masternodeaddr address: " + strMasterNodeAddr);
            }
        }

       BOOST_FOREACH(string& strAddNode, MasterNodePrivateKey_List)
        {
//             printf("RGP DEBUG Init getting masternode private key %s \n", &strAddNode[0] );
             MN_private_keys.push_back(strAddNode);
        }

        BOOST_FOREACH(string& strAddNode, MN_private_keys) 
        {
            MasterNode_Private_Key = strAddNode;
        }
//printf("RGP DEBUG Init MasterNode Private Key  %s\n", MasterNode_Private_Key.c_str() );

        strMasterNodePrivKey = MasterNode_Private_Key;
        if(!strMasterNodePrivKey.empty()){
            std::string errorMessage;

            CKey key;
            CPubKey pubkey;



            if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key, pubkey))
            {
                return InitError(_("Invalid masternodeprivkey. Please see documenation."));
            }

            printf("RGP Init darkSendSigner Success \n");
            activeMasternode.pubKeyMasternode = pubkey;

        } 
        else 
        {
            return InitError(_("You must specify a masternodeprivkey in the configuration. Please see documentation for help."));
        }

        activeMasternode.ManageStatus();
    }
    else
        LogPrintf("RGP Debug Masternode is false and not active \n");

    if(GetBoolArg("mnconflock", false)) {
        LogPrintf("Locking Masternodes:\n");
        uint256 mnTxHash;
        BOOST_FOREACH(CMasternodeConfig::CMasternodeEntry mne, masternodeConfig.getEntries()) {
            LogPrintf("  %s %s\n", mne.getTxHash(), mne.getOutputIndex());
            mnTxHash.SetHex(mne.getTxHash());
            COutPoint outpoint = COutPoint(mnTxHash, boost::lexical_cast<unsigned int>(mne.getOutputIndex()));
            pwalletMain->LockCoin(outpoint);
        }
    }

    fEnableDarksend = GetBoolArg("enabledarksend", true);

    nDarksendRounds = GetArg("darksendrounds", 2);
    if(nDarksendRounds > 16) nDarksendRounds = 16;
    if(nDarksendRounds < 1) nDarksendRounds = 1;

    nLiquidityProvider = GetArg("liquidityprovider", 0); //0-100
    if(nLiquidityProvider != 0) {
        darkSendPool.SetMinBlockSpacing(std::min(nLiquidityProvider,100)*15);
        fEnableDarksend = true;
        nDarksendRounds = 99999;
    }

    nAnonymizeSocietyGAmount = GetArg("anonymizeSocietyGamount", 0);
    if(nAnonymizeSocietyGAmount > 999999) nAnonymizeSocietyGAmount = 999999;
    if(nAnonymizeSocietyGAmount < 2) nAnonymizeSocietyGAmount = 2;

    fEnableInstantX = GetBoolArg("enableinstantx", fEnableInstantX);
    nInstantXDepth = GetArg("instantxdepth", nInstantXDepth);
    nInstantXDepth = std::min(std::max(nInstantXDepth, 0), 60);

    //lite mode disables all Masternode and Darksend related functionality
    fLiteMode = GetBoolArg("litemode", false);
    if(fMasterNode && fLiteMode){
        return InitError("You can not start a masternode in litemode");
    }

    LogPrintf("fLiteMode %d\n", fLiteMode);
    LogPrintf("nInstantXDepth %d\n", nInstantXDepth);
    LogPrintf("Darksend rounds %d\n", nDarksendRounds);
    LogPrintf("Anonymize SocietyG Amount %d\n", nAnonymizeSocietyGAmount);

    /* Denominations
       A note about convertability. Within Darksend pools, each denomination
       is convertable to another.
       For example:
       1SocietyG+1000 == (.1SocietyG+100)*10
       10SocietyG+10000 == (1SocietyG+1000)*10
    */
    darkSendDenominations.push_back( (1000        * COIN)+1000000 );
    darkSendDenominations.push_back( (100         * COIN)+100000 );
    darkSendDenominations.push_back( (10          * COIN)+10000 );
    darkSendDenominations.push_back( (1           * COIN)+1000 );
    darkSendDenominations.push_back( (.1          * COIN)+100 );
    /* Disabled till we need them
    darkSendDenominations.push_back( (.01      * COIN)+10 );
    darkSendDenominations.push_back( (.001     * COIN)+1 );
    */

    darkSendPool.InitCollateralAddress();

    threadGroup.create_thread(boost::bind(&ThreadCheckDarkSendPool));

    RandAddSeedPerfmon();

    // reindex addresses found in blockchain
    if(GetBoolArg("reindexaddr", false))
    {
        uiInterface.InitMessage(_("Rebuilding address index..."));
        CBlockIndex *pblockAddrIndex = pindexBest;
	    CTxDB txdbAddr("rw");
	
        while ( pblockAddrIndex )
	    {
	          uiInterface.InitMessage(strprintf("Rebuilding address index, block %i", pblockAddrIndex->nHeight));
	          bool ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions=true);
	          CBlock pblockAddr;
	          if ( pblockAddr.ReadFromDisk(pblockAddrIndex, true))
	             pblockAddr.RebuildAddressIndex(txdbAddr);
	             
              pblockAddrIndex = pblockAddrIndex->pprev;
	    }
    }

    //// debug print
    LogPrintf("mapBlockIndex.size() = %u\n",   mapBlockIndex.size());
    LogPrintf("nBestHeight = %d\n",                   nBestHeight);
#ifdef ENABLE_WALLET
    LogPrintf("setKeyPool.size() = %u\n",      pwalletMain ? pwalletMain->setKeyPool.size() : 0);
    LogPrintf("mapWallet.size() = %u\n",       pwalletMain ? pwalletMain->mapWallet.size() : 0);
    LogPrintf("mapAddressBook.size() = %u\n",  pwalletMain ? pwalletMain->mapAddressBook.size() : 0);
#endif

//AddToWallet

    StartNode(threadGroup);
#ifdef ENABLE_WALLET
    // InitRPCMining is needed here so getwork/getblocktemplate in the GUI debug console works properly.
    InitRPCMining();
#endif

    if (fServer == true )
       printf("RGP FSERVER is TRUE \n");
    else
       printf("RGP FSERVER is FALSE \n");

    if (fServer)
    {
        // RGP StartRPCThreads();
        StartRPC();
    }

#ifdef ENABLE_WALLET
    // Mine proof-of-stake blocks in the background
    if (!GetBoolArg("staking", true))
        LogPrintf("Staking disabled\n");
    else if (pwalletMain)
        threadGroup.create_thread(boost::bind(&ThreadStakeMiner, pwalletMain));
#endif

    // ********************************************************* Step 12: finished

    uiInterface.InitMessage(_("Done loading"));

#ifdef ENABLE_WALLET
    if (pwalletMain)
    {
        // Add wallet transactions that aren't already in a block to mapTransactions
        pwalletMain->ReacceptWalletTransactions();

        // Run a thread to flush wallet periodically
        threadGroup.create_thread(boost::bind(&ThreadFlushWalletDB, boost::ref(pwalletMain->strWalletFile)));
    }

    // Generate coins using internal miner
    if (pwalletMain)
    {
        GeneratePoWcoins(GetBoolArg("gen", false), pwalletMain, false);
    }

    int xyz;

    // Run a thread to periodically check when to exit
    threadGroup.create_thread(boost::bind(&DetectShutdownThread, xyz ));

    //detectShutdownThread = new boost::thread(boost::bind(&DetectShutdownThread, threadGroup));
#endif


    return !fRequestShutdown;
}
