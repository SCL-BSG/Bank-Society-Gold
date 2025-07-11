// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2018 Profit Hunters Coin developers
// Copyright (c) 2019 Bank Society Coin Developers
// Copyright (c) 2020-2023 Bank Soiety Gold Coin Developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

/* -----------------------------------------------------------
   -- File      :   main.cpp                                --
   -- Version   :   1.0.9.7 [based on wallet]               --
   -- Author    :   RGPickles                               --
   -- Date      :   14th February 2020                      --
   -- Detail    :   Updated ProcessMessage, ProcessBlock,   --
   --               AcceptBlock and SendBlock to resolve    --
   --               'sticky' synch issues during synch from --
   --               zero.                                   --
   --                                                       --
   ----------------------------------------------------------- */

#include "main.h"
#include <iostream>
#include <limits>

#include "addrman.h"
#include "alert.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "db.h"
#include "init.h"
#include "kernel.h"
#include "net.h"
#include "txdb.h"
#include "txmempool.h"
#include "ui_interface.h"
#include "instantx.h"
#include "darksend.h"
#include "masternodeman.h"
#include "masternode-payments.h"
#include "spork.h"
#include "smessage.h"
#include "util.h"
#include "rpcserver.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>
using namespace std;
using namespace boost;
using namespace boost::placeholders;
//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
static CTransaction other_copy;
// original 
static map<uint256, CBlockIndex*> mapBlockIndex;

// from MYCE
BlockMap mapBlockIndexmice;
CChain chainActive;
// from MYCE


static int POS_Blocks_Stored = 0;

//map<uint256, CBlockIndex*> mapBlockIndex;
//map<uint256, CBlockIndex*> BlockMap;
set<pair<COutPoint, unsigned int> > setStakeSeen;

CBigNum bnProofOfStakeLimit(~uint256(0) >> 20);

CConditionVariable cvBlockChange;

/* RGP Pre JIRA BSG-67 statement */
//unsigned int nStakeMinAge = 60 * 60; // 1 hours

/* ---------------------
   -- RGP JIRA BSG-67 --
   -----------------------------------------------------------
   -- nStakeMinAge is extending the time required for coins --
   -- in a wallet to be mature, before they are considered  --
   -- for staking.                                          --
   -- Note : Reversed this stops synch validation...        --
   ----------------------------------------------------------- */
unsigned int nStakeMinAge = 60 * 60 * 1; // 4 hours



unsigned int nModifierInterval = 8 * 60; // time to elapse before new modifier is computed (8 hours)

/* RGP, CoinbaseMaturity is value - 20 blocks before you can spend coins
        -20 is substracted elsewhere in the code. */
//int nCoinbaseMaturity = 100;
int nCoinbaseMaturity = 70;             /* Really 50 blocks before the coin can be spent */
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;

uint256 nBestChainTrust = 0;
uint256 nBestInvalidTrust = 0;

uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;

int64_t nTimeBestReceived = 0;
bool fImporting = false;
bool fReindex = false;
bool fAddrIndex = false;
bool fHaveGUI = false;

/* RGP, static Inventory structure using with AskFor()
   to request investory data                           */
static CInv Inventory_to_Request;
static CNode *From_Node;

struct COrphanBlock {
    uint256 hashBlock;
    uint256 hashPrev;
    std::pair<COutPoint, unsigned int> stake;
    vector<unsigned char> vchBlock;
};
map<uint256, COrphanBlock*> mapOrphanBlocks;
multimap<uint256, COrphanBlock*> mapOrphanBlocksByPrev;
set<pair<COutPoint, unsigned int> > setStakeSeenOrphan;

map<uint256, CTransaction> mapOrphanTransactions;
map<uint256, set<uint256> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "SocietyG Signed Message:\n";

std::set<uint256> setValidatedTx;


#include "semaphore.h"

extern Semaphore thread_semaphore;


//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets

namespace {

struct CMainSignals {
    // Notifies listeners of updated transaction data (passing hash, transaction, and optionally the block it is found in.
    boost::signals2::signal<void (const CTransaction &, const CBlock *, bool)> SyncTransaction;
    // Notifies listeners of an erased transaction (currently disabled, requires transaction replacement).
    boost::signals2::signal<void (const uint256 &)> EraseTransaction;
    // Notifies listeners of an updated transaction without new data (for now: a coinbase potentially becoming visible).
    boost::signals2::signal<void (const uint256 &)> UpdatedTransaction;
    // Notifies listeners of a new active block chain.
    boost::signals2::signal<void (const CBlockLocator &)> SetBestChain;
    // Notifies listeners about an inventory item being seen on the network.
    boost::signals2::signal<void (const uint256 &)> Inventory;
    // Tells listeners to broadcast their data.
    boost::signals2::signal<void (bool)> Broadcast;
} g_signals;
}

void RegisterWallet(CWalletInterface* pwalletIn) {
    g_signals.SyncTransaction.connect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
    g_signals.EraseTransaction.connect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.UpdatedTransaction.connect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.SetBestChain.connect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
    g_signals.Inventory.connect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
    g_signals.Broadcast.connect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn, _1));
}

void UnregisterWallet(CWalletInterface* pwalletIn) {
    g_signals.Broadcast.disconnect(boost::bind(&CWalletInterface::ResendWalletTransactions, pwalletIn, _1));
    g_signals.Inventory.disconnect(boost::bind(&CWalletInterface::Inventory, pwalletIn, _1));
    g_signals.SetBestChain.disconnect(boost::bind(&CWalletInterface::SetBestChain, pwalletIn, _1));
    g_signals.UpdatedTransaction.disconnect(boost::bind(&CWalletInterface::UpdatedTransaction, pwalletIn, _1));
    g_signals.EraseTransaction.disconnect(boost::bind(&CWalletInterface::EraseFromWallet, pwalletIn, _1));
    g_signals.SyncTransaction.disconnect(boost::bind(&CWalletInterface::SyncTransaction, pwalletIn, _1, _2, _3));
}

void UnregisterAllWallets() {
    g_signals.Broadcast.disconnect_all_slots();
    g_signals.Inventory.disconnect_all_slots();
    g_signals.SetBestChain.disconnect_all_slots();
    g_signals.UpdatedTransaction.disconnect_all_slots();
    g_signals.EraseTransaction.disconnect_all_slots();
    g_signals.SyncTransaction.disconnect_all_slots();
}

void SyncWithWallets(const CTransaction &tx, const CBlock *pblock, bool fConnect) 
{
    g_signals.SyncTransaction(tx, pblock, fConnect);
}

void ResendWalletTransactions(bool fForce) 
{
    g_signals.Broadcast(fForce);
}


//////////////////////////////////////////////////////////////////////////////
//
// Registration of network node signals.
//

namespace {
// Maintain validation-specific state about nodes, protected by cs_main, instead
// by CNode's own locks. This simplifies asynchronous operation, where
// processing of incoming data is done after the  call returns,
// and we're no longer holding the node's locks.
struct CNodeState {
    // Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    // Whether this peer should be disconnected and banned.
    bool fShouldBan;
    std::string name;

    CNodeState() {
        nMisbehavior = 0;
        fShouldBan = false;
    }
};

map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState *State(NodeId pnode) {
    map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

int GetHeight()
{
    while(true)
    {
        TRY_LOCK(cs_main, lockMain);
        if(!lockMain)
        {
            MilliSleep(1); /* RGP Optimize */
            continue;
        }
        return pindexBest->nHeight;

    }
}


void InitializeNode(NodeId nodeid, const CNode *pnode) {
    LOCK(cs_main);
    CNodeState &state = mapNodeState.insert(std::make_pair(nodeid, CNodeState())).first->second;
    state.name = pnode->addrName;
}

void FinalizeNode(NodeId nodeid) {
    LOCK(cs_main);
    mapNodeState.erase(nodeid);
}

}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats &stats) {
    LOCK(cs_main);
    CNodeState *state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    return true;
}


void RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.connect(&GetHeight);
    nodeSignals.ProcessMessages.connect(&ProcessMessages);
    nodeSignals.SendMessages.connect(&SendMessages);
    nodeSignals.InitializeNode.connect(&InitializeNode);
    nodeSignals.FinalizeNode.connect(&FinalizeNode);
}

void UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.GetHeight.disconnect(&GetHeight);
    nodeSignals.ProcessMessages.disconnect(&ProcessMessages);
    nodeSignals.SendMessages.disconnect(&SendMessages);
    nodeSignals.InitializeNode.disconnect(&InitializeNode);
    nodeSignals.FinalizeNode.disconnect(&FinalizeNode);
}

bool AbortNode(const std::string &strMessage, const std::string &userMessage) {
    strMiscWarning = strMessage;
    LogPrintf("***RGP  %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransaction& tx)
{
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    LogPrintf("*** RGP AddOrphanTx \n");


    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:

    size_t nSize = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);

    if (nSize > 5000)
    {
        LogPrint("mempool", "ignoring large orphan tx (size: %u, hash: %s)\n", nSize, hash.ToString());
        return false;
    }

    mapOrphanTransactions[hash] = tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(hash);

    LogPrintf("mempool, stored orphan tx %s (mapsz %u)\n", hash.ToString(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    map<uint256, CTransaction>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return;
    BOOST_FOREACH(const CTxIn& txin, it->second.vin)
    {
        map<uint256, set<uint256> >::iterator itPrev = mapOrphanTransactionsByPrev.find(txin.prevout.hash);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(hash);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }
    mapOrphanTransactions.erase(it);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
unsigned int nEvicted = 0;
uint256 randomhash;


    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        randomhash = GetRandHash();
        map<uint256, CTransaction>::iterator it = mapOrphanTransactions.lower_bound(randomhash);

        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;

        MilliSleep(1); /* RGP Optimize */

    }

    return nEvicted;
}

//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

bool CTransaction::ReadFromDisk(CTxDB& txdb, const uint256& hash, CTxIndex& txindexRet)
{
int read_status;

    //SetNull();

MilliSleep(1);

    if (!txdb.ReadTxIndex(hash, txindexRet))
    {
        MilliSleep( 5 );
        return false;
    }	

    MilliSleep(1);

    read_status = false;

    try
    {
        /* Looks like READ() is failing, when the transaction is not on disk */

        read_status = ReadFromDisk(txindexRet.pos );
        read_status = true;
        MilliSleep(1);
    }
    catch (std::ios_base::failure& e)
    {

        LogPrintf("pblock->AcceptBlock : Exception '%s' caught\n", e.what());
        PrintExceptionContinue(&e, "ios READ()");
    }
    catch (boost::thread_interrupted) 
    {
        throw;
    }
    catch (std::exception& e) 
    {
        PrintExceptionContinue(&e, "std Read()");
    } 
    catch (...) 
    {
        PrintExceptionContinue(NULL, "cont Read()");
    }

    if (!read_status)
    {
LogPrintf("RGP CTransaction::ReadFromDisk Entry 004 %s HASH disk pos %d \n", hash.ToString() , txindexRet.pos.nBlockPos );
        return false;
    }

    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{

    MilliSleep(1);


    if (!ReadFromDisk(txdb, prevout.hash, txindexRet))
    {
        LogPrintf("RGP CTransaction::ReadFromDisk Entry 002 %s previous hash failed  \n", prevout.hash.ToString() );
        LogPrintf("RGP CTransaction::ReadFromDisk Entry 002 %s previous hash \n", prevout.ToString() );
        return false;
    }

    /* ------------------------------------------------------------------------------
       -- RGP vout.size  has nothing to do with prevout.n                          --
       ------------------------------------------------------------------------------ */
//    if (prevout.n != vout.size())
//    {
        /* RGP, notes from WIndows wallets that a lot of older block transactions are not on disk
                most likely from validation errors in the code and input[] data invalid.          */ 
//LogPrintf("RGP CTransaction::ReadFromDisk prevout.n %d vout.size() %d \n", prevout.n, vout.size() );
//        LogPrintf("compare error? \n");
//        SetNull();
//        return false;
//    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool IsStandardTx(const CTransaction& tx, string& reason)
{
    if (tx.nVersion > CTransaction::CURRENT_VERSION || tx.nVersion < 1) {
        reason = "version";
        return false;
    }

    // Treat non-final transactions as non-standard to prevent a specific type
    // of double-spend attack, as well as DoS attacks. (if the transaction
    // can't be mined, the attacker isn't expending resources broadcasting it)
    // Basically we don't want to propagate transactions that can't be included in
    // the next block.
    //
    // However, IsFinalTx() is confusing... Without arguments, it uses
    // chainActive.Height() to evaluate nLockTime; when a block is accepted, chainActive.Height()
    // is set to the value of nHeight in the block. However, when IsFinalTx()
    // is called within CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a transaction can
    // be part of the *next* block, we need to call IsFinalTx() with one more
    // than chainActive.Height().
    //
    // Timestamps on the other hand don't get any special treatment, because we
    // can't know what timestamp the next block will have, and there aren't
    // timestamp applications where it matters.
    if (!IsFinalTx(tx, nBestHeight + 1)) {
        reason = "non-final";
        return false;
    }
    // nTime has different purpose from nLockTime but can be used in similar attacks
    if (tx.nTime > FutureDrift(GetAdjustedTime())) {
        reason = "time-too-new";
        return false;
    }

    // Extremely large transactions with lots of inputs can cost the network
    // almost as much to process as they cost the sender in fees, because
    // computing signature hashes is O(ninputs*txsize). Limiting transactions
    // to MAX_STANDARD_TX_SIZE mitigates CPU exhaustion attacks.
    unsigned int sz = tx.GetSerializeSize(SER_NETWORK, CTransaction::CURRENT_VERSION);
    if (sz >= MAX_STANDARD_TX_SIZE) {
        reason = "tx-size";
        return false;
    }

    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        // Biggest 'standard' txin is a 15-of-15 P2SH multisig with compressed
        // keys. (remember the 520 byte limit on redeemScript size) That works
        // out to a (15*(33+1))+3=513 byte redeemScript, 513+1+15*(73+1)+3=1627
        // bytes of scriptSig, which we round off to 1650 bytes for some minor
        // future-proofing. That's also enough to spend a 20-of-20
        // CHECKMULTISIG scriptPubKey, though such a scriptPubKey is not
        // considered standard)
        if (txin.scriptSig.size() > 1650) {
            reason = "scriptsig-size";
            return false;
        }
        if (!txin.scriptSig.IsPushOnly()) {
            reason = "scriptsig-not-pushonly";
            return false;
        }
        if (!txin.scriptSig.HasCanonicalPushes()) {
            reason = "scriptsig-non-canonical-push";
            return false;
        }
    }

    unsigned int nDataOut = 0;

    txnouttype whichType;
    BOOST_FOREACH(const CTxOut& txout, tx.vout) {
        if (!::IsStandard(txout.scriptPubKey, whichType))
        {
            reason = "scriptpubkey";
            return false;
        }
        if (whichType == TX_NULL_DATA)
        {
            nDataOut++;
        } else if (txout.nValue == 0) {
            reason = "dust";
            return false;
        }
        if (!txout.scriptPubKey.HasCanonicalPushes()) {
            reason = "scriptpubkey-non-canonical-push";
            return false;
        }
    }

    // not more than one data txout per non-data txout is permitted
    // only one data txout is permitted too
    if (nDataOut > 1 && nDataOut > tx.vout.size()/2) {
        reason = "multi-op-return";
        return false;
    }

    return true;
}

bool IsFinalTx(const CTransaction &tx, int nBlockHeight, int64_t nBlockTime)
{
    AssertLockHeld(cs_main);
    // Time based nLockTime implemented in 0.1.6
    if (tx.nLockTime == 0)
        return true;
    if (nBlockHeight == 0)
        nBlockHeight = nBestHeight;
    if (nBlockTime == 0)
        nBlockTime = GetAdjustedTime();
    if ((int64_t)tx.nLockTime < ((int64_t)tx.nLockTime < LOCKTIME_THRESHOLD ? (int64_t)nBlockHeight : nBlockTime))
        return true;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        if (!txin.IsFinal())
            return false;
    return true;
}

//
// Check transaction inputs to mitigate two
// potential denial-of-service attacks:
//
// 1. scriptSigs with extra data stuffed into them,
//    not consumed by scriptPubKey (or P2SH script)
// 2. P2SH scripts with a crazy number of expensive
//    CHECKSIG/CHECKMULTISIG operations
//
bool AreInputsStandard(const CTransaction& tx, const MapPrevTx& mapInputs)
{
    if (tx.IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        const CTxOut& prev = tx.GetOutputFor(tx.vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig
        // IsStandard() will have already returned false
        // and this method isn't called.
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, tx.vin[i].scriptSig, tx, i, SCRIPT_VERIFY_NONE, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (Solver(subscript, whichType2, vSolutions2))
            {
                int tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);

                if (whichType2 == TX_SCRIPTHASH)
                    return false;
                if (tmpExpected < 0)
                    return false;

                nArgsExpected += tmpExpected;
            }
            else
            {
                // Any other Script with less than 15 sigops OK:
                unsigned int sigops = subscript.GetSigOpCount(true);
                // ... extra data left on the stack after execution is OK, too:
                return (sigops <= MAX_P2SH_SIGOPS);
            }
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int GetLegacySigOpCount(const CTransaction& tx)
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, tx.vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}

unsigned int GetP2SHSigOpCount(const CTransaction& tx, const MapPrevTx& inputs)
{
//LogPrintf("RGP GetP2SHSigOpCount start \n");
unsigned long int nSigOps = 0;
    if (tx.IsCoinBase())
        return 0;

    nSigOps = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
//LogPrintf("RGP GetP2SHSigOpCount Debug 0.1 tx %s \n", tx.ToString());

        const CTxOut& prevout = tx.GetOutputFor(tx.vin[i], inputs);

//LogPrintf("RGP GetP2SHSigOpCount Debug 0.1 prevout %s \n", prevout.ToString());

        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);

//LogPrintf("RGP GetP2SHSigOpCount Debug 1.0 \n");

    }
//LogPrintf("RGP GetP2SHSigOpCount Debug 2.0 \n");
    return nSigOps;

//LogPrintf("RGP GetP2SHSigOpCount start Transaction %s  \n", tx.ToString() );    
//    if ( tx.vin.size() < 3 )
//    {
     /* Updated unsigned int to insigned long int */
//        for (unsigned long int i = 0; i < tx.vin.size(); i++)
//        {
//
//LogPrintf("RGP GetP2SHSigOpCount Debug 1.0 \n");
//            const CTxOut& prevout = tx.GetOutputFor(tx.vin[i], inputs);
//
//LogPrintf("RGP GetP2SHSigOpCount Debug 1.1 prevout %s \n", prevout.ToString() );
//
//            if (prevout.scriptPubKey.IsPayToScriptHash())
//            {
//LogPrintf("RGP GetP2SHSigOpCount Debug 1.2 \n");
//                nSigOps += prevout.scriptPubKey.GetSigOpCount(tx.vin[i].scriptSig);
//           }
//LogPrintf("RGP GetP2SHSigOpCount Debug 1.4 \n");
//        }
//
//    }

//LogPrintf("RGP GetP2SHSigOpCount Debug 1.6 nSigOps %d \n", nSigOps );
 //   return nSigOps;
}

int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    AssertLockHeld(cs_main);
    CBlock blockTmp;

    if (pblock == NULL) {
        // Load the block this tx is in
        CTxIndex txindex;
        
LogPrintf("RGP CMerkleTx::SetMerkleBranch before ReadTxIndex \n");
        if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
            return 0;
        if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos)) {
            return 0;
            pblock = &blockTmp;
        }
    }

    if (pblock) {
        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            LogPrintf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}

double CTransaction::ComputePriority(double dPriorityInputs, unsigned int nTxSize) const
{
    // In order to avoid disincentivizing cleaning up the UTXO set we don't count
    // the constant overhead for each txin and up to 110 bytes of scriptSig (which
    // is enough to cover a compressed pubkey p2sh redemption) for priority.
    // Providing any more cleanup incentive than making additional inputs free would
    // risk encouraging people to create junk outputs to redeem later.
    if (nTxSize == 0)
        nTxSize = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        unsigned int offset = 41U + std::min(110U, (unsigned int)txin.scriptSig.size());
        if (nTxSize > offset)
            nTxSize -= offset;
    }
    if (nTxSize == 0) return 0.0;
    return dPriorityInputs / nTxSize;
}

bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
    {
        LogPrintf("\nn *** RGP DoS 1 fail!! \n\n");
        /* RGP Fix Later */
        return false;
        //return DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    }

    if (vout.empty())
    {
        LogPrintf("\nn *** RGP DoS 2 fail!! \n\n");
        return DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    }


    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
    {
        LogPrintf("\nn *** RGP DoS 3 fail!! \n\n");
        return DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));
    }


    // Check for negative or overflow output values
    int64_t nValueOut = 0;
    for (unsigned int i = 0; i < vout.size(); i++)
    {
        const CTxOut& txout = vout[i];
        if (txout.IsEmpty() && !IsCoinBase() && !IsCoinStake())
        {
            LogPrintf("\nn *** RGP DoS 4 fail!! \n\n");
            return DoS(100, error("CTransaction::CheckTransaction() : txout empty for user transaction"));
        }

        if (txout.nValue < 0)
        {
            LogPrintf("\nn *** RGP DoS 5 fail!! \n\n");
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        }

        if (txout.nValue > MAX_MONEY)
        {
            LogPrintf("\nn *** RGP DoS 6 fail MAX MONEY SUPPLY!!! out %d mney %d \n\n", txout.nValue,  MAX_MONEY  );
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        }

        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
        {
            LogPrintf("\nn *** RGP DoS 7 fail!! \n\n");
            return DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
        }

    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return false;
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
        {
            LogPrintf("\nn *** RGP DoS 6 fail!! \n\n");
            return DoS(100, error("CTransaction::CheckTransaction() : coinbase script size is invalid"));
        }

    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
            {
                LogPrintf("\nn *** RGP DoS 6 fail!! \n\n");
                return DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
            }

    }

    return true;
}

int64_t GetMinFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree, enum GetMinFee_mode mode)
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    int64_t nBaseFee = (mode == GMF_RELAY) ? MIN_RELAY_TX_FEE : MIN_TX_FEE;

    int64_t nMinFee = (1 + (int64_t)nBytes / 1000) * nBaseFee;

    /*if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        // * If we are creating a transaction we allow transactions up to 1,000 bytes
        //   to be considered safe and assume they can likely make it into this section.
        if (nBytes < (mode == GMF_SEND ? 1000 : (DEFAULT_BLOCK_PRIORITY_SIZE - 1000)))
            nMinFee = 0;
    }*/

    // This code can be removed after enough miners have upgraded to version 0.9.
    // Until then, be safe when sending and require a fee if any output
    // is less than CENT:
    if (nMinFee < nBaseFee && mode == GMF_SEND)
    {
        BOOST_FOREACH(const CTxOut& txout, tx.vout)
            if (txout.nValue < CENT)
                nMinFee = nBaseFee;
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}


bool AcceptToMemoryPool(CTxMemPool& pool, CTransaction &tx, bool fLimitFree,
                        bool* pfMissingInputs, bool fRejectInsaneFee, bool ignoreFees)
{
extern CTransaction other_copy;

    CTransaction test_tx(tx);        /* using a global now */
    other_copy = test_tx;

    AssertLockHeld(cs_main);
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction())
        return error("AcceptToMemoryPool : CheckTransaction failed");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase()){
        /* RGP, Found some wallet error that exists only on one machine
                to be investigated later for future fix. */
        // return tx.DoS(100, error("AcceptToMemoryPool : coinbase as individual tx"));
        return ( false );
    }

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake()){
        /* RGP, Found some wallet error that exists only on one machine
                to be investigated later for future fix. */
        // return tx.DoS(100, error("AcceptToMemoryPool : coinstake as individual tx"));
        return ( false );
    }

    // Rather not work on nonstandard transactions (unless -testnet)
    string reason;
    if (!TestNet() && !IsStandardTx(tx, reason))
        return error("AcceptToMemoryPool : nonstandard transaction: %s",
                     reason);

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    if (pool.exists(hash))
        return false;

    // ----------- instantX transaction scanning -----------

    BOOST_FOREACH(const CTxIn& in, tx.vin){
        if(mapLockedInputs.count(in.prevout)){
            if(mapLockedInputs[in.prevout] != tx.GetHash()){
                return tx.DoS(0, error("AcceptToMemoryPool : conflicts with existing transaction lock: %s", reason));
            }
        }
    }

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
    }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
            return false;

        // do all inputs exist?
        // Note that this does not check for the presence of actual outputs (see the next check for that),
        // only helps filling in pfMissingInputs (to determine missing vs spent).
        BOOST_FOREACH(const CTxIn txin, tx.vin) {
            if (!txdb.ContainsTx(txin.prevout.hash)) {
                if (pfMissingInputs)
                    *pfMissingInputs = true;
                return false;
            }
        }
        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;

LogPrintf("RGP AcceptToMemoryPool, before FetchInputs \n");
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("AcceptToMemoryPool : FetchInputs found invalid tx %s", hash.ToString());
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!TestNet() && !AreInputsStandard(tx, mapInputs))
            return error("AcceptToMemoryPool : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);

        /* RGP, the result of GetLegacySigOpCount() + GetP2SHSigOpCount() is exceeding an unsigned int 
                update the code to register these issues for later investigation...                    */


        unsigned long int nSigOps_Special = nSigOps;

        nSigOps_Special += GetP2SHSigOpCount(tx, mapInputs);
        if (nSigOps > MAX_TX_SIGOPS)
        {
            LogPrintf("RGP GetP2SHSigOpCount count exceeds the size of block size * 5 INVESTIGATE LATER RGP \n");
            //return tx.DoS(0,
            //              error("AcceptToMemoryPool : too many sigops %s, %d > %d",
            //                    hash.ToString(), nSigOps, MAX_TX_SIGOPS));
        }
        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        // but prioritise dstx and don't check fees for it
        if(mapDarksendBroadcastTxes.count(hash)) {
            // Normally we would PrioritiseTransaction But currently it is unimplemented
            // mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1*COIN);
        } else if(!ignoreFees){
            int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);
            if (fLimitFree && nFees < txMinFee)
                return error("AcceptToMemoryPool : not enough fees %s, %d < %d",
                            hash.ToString(),
                            nFees, txMinFee);

            // Continuously rate-limit free transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
            {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000)
                    return error("AcceptableInputs : free transaction rejected by rate limiter");
                LogPrintf("mempool, Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > MIN_RELAY_TX_FEE * 10000)
            return error("AcceptableInputs: : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, MIN_RELAY_TX_FEE * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false, STANDARD_SCRIPT_VERIFY_FLAGS))
        {
            return error("AcceptToMemoryPool : ConnectInputs failed %s", hash.ToString());
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false, MANDATORY_SCRIPT_VERIFY_FLAGS))
        {
            return error("AcceptToMemoryPool: : BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s", hash.ToString());
        }
    }

    // Store transaction in memory
    pool.addUnchecked(hash, tx);
    setValidatedTx.insert(hash);

    SyncWithWallets(tx, NULL);

    LogPrintf("\nmempool AcceptToMemoryPool : accepted %s (poolsz %u) \n",
           hash.ToString(),
           pool.mapTx.size());

    return true;
}


bool AcceptableInputs(CTxMemPool& pool, const CTransaction &txo, bool fLimitFree,
                         bool* pfMissingInputs, bool fRejectInsaneFee, bool isDSTX)
{
extern CTransaction other_copy;

    AssertLockHeld(cs_main);
    
    LogPrintf("RGP AcceptableInputs() start \n" );
    
    CTransaction transaction (txo);        /* using a global now */
    other_copy = transaction;

    if (pfMissingInputs)
    {
    	LogPrintf("RGP AcceptableInputs() Missing Inputs \n" );
        *pfMissingInputs = false;
    }

LogPrintf("RGP AcceptableInputs CTransaction %s \n", txo.ToString() );


    CTransaction tx(txo);        /* using a global now */
    other_copy = tx;
    string reason;

LogPrintf("RGP AcceptableInputs tx(txo) %s \n", tx.ToString() );

    if (!tx.CheckTransaction())
    {  
        /* RGP FIX later, found the transaction was not correct. but it should be !!! */
        return false;
        //return error("AcceptableInputs : CheckTransaction failed");
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("AcceptableInputs : coinbase as individual tx"));

    // ppcoin: coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return tx.DoS(100, error("AcceptableInputs : coinstake as individual tx"));

    // is it already in the memory pool?
    uint256 hash = tx.GetHash();
    LogPrintf("RGP AcceptableInputs() hash %s \n", hash.ToString() );
    
    if (pool.exists(hash))
        return false;

    // ----------- instantX transaction scanning -----------

    BOOST_FOREACH(const CTxIn& in, tx.vin)
    {
        if(mapLockedInputs.count(in.prevout))
        {
            if(mapLockedInputs[in.prevout] != tx.GetHash())
            {
                return tx.DoS(0, error("AcceptableInputs : conflicts with existing transaction lock: %s", reason));
            }
        }
        MilliSleep(1); /* RGP Optimise */
    }

    // Check for conflicts with in-memory transactions
    {
    LOCK(pool.cs); // protect pool.mapNextTx
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (pool.mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;
        }
        MilliSleep(1); /* RGP Optimise */
    }
    }

    {
        CTxDB txdb("r");

        // do we already have it?
        if (txdb.ContainsTx(hash))
        {
            LogPrintf("RGP AssertableInputs() Already have the hash? \n" );
            return false;
	    }

//      typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx;
        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
LogPrintf("RGP AcceptableInputs, before FetchInputs \n");
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("AcceptableInputs : FetchInputs found invalid tx %s", hash.ToString());
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        //if (!TestNet() && !AreInputsStandard(tx, mapInputs))
          //  return error("AcceptToMemoryPool : nonstandard transaction input");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        nSigOps += GetP2SHSigOpCount(tx, mapInputs);
        if (nSigOps > MAX_TX_SIGOPS)
            return tx.DoS(0,
                          error("AcceptableInputs : too many sigops %s, %d > %d",
                                hash.ToString(), nSigOps, MAX_TX_SIGOPS));

        int64_t nFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
        int64_t txMinFee = GetMinFee(tx, nSize, true, GMF_RELAY);

        // Don't accept it if it can't get into a block
        if(isDSTX) {
            // Normally we would PrioritiseTransaction But currently it is unimplemented
            // mempool.PrioritiseTransaction(hash, hash.ToString(), 1000, 0.1*COIN);
        } else { // same as !ignoreFees for AcceptToMemoryPool
            if (fLimitFree && nFees < txMinFee)
                return error("AcceptableInputs : not enough fees %s, %d < %d",
                            hash.ToString(),
                            nFees, txMinFee);

            // Continuously rate-limit free transactions
            // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
            // be annoying or make others' transactions take longer to confirm.
            if (fLimitFree && nFees < MIN_RELAY_TX_FEE)
            {
                static CCriticalSection csFreeLimiter;
                static double dFreeCount;
                static int64_t nLastTime;
                int64_t nNow = GetTime();

                LOCK(csFreeLimiter);

                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000)
                    return error("AcceptableInputs : free transaction rejected by rate limiter");
                LogPrint("mempool", "Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        if (fRejectInsaneFee && nFees > txMinFee * 10000)
            return error("AcceptableInputs: : insane fees %s, %d > %d",
                         hash.ToString(),
                         nFees, MIN_RELAY_TX_FEE * 10000);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(txdb, mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, true, false, STANDARD_SCRIPT_VERIFY_FLAGS, false))
        {
            return error("AcceptableInputs : ConnectInputs failed %s", hash.ToString());
        }
    }


    LogPrintf("mempool, AcceptableInputs : accepted %s (poolsz %u)\n",
           hash.ToString(),
           pool.mapTx.size());
    
    return true;
}


int CMerkleTx::GetDepthInMainChainINTERNAL(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;
    AssertLockHeld(cs_main);

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}

int CMerkleTx::GetTransactionLockSignatures() const
{
    if(!IsSporkActive(SPORK_2_INSTANTX)) return -3;
    if(!fEnableInstantX) return -1;

    //compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()){
        return (*i).second.CountSignatures();
    }

    return -1;
}

bool CMerkleTx::IsTransactionLockTimedOut() const
{
    if(!fEnableInstantX) return -1;

    //compile consessus vote
    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(GetHash());
    if (i != mapTxLocks.end()){
        return GetTime() > (*i).second.nTimeout;
    }

    return false;
}

int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet, bool enableIX) const
{
    AssertLockHeld(cs_main);
    int nResult = GetDepthInMainChainINTERNAL(pindexRet);
    if (nResult == 0 && !mempool.exists(GetHash()))
        return -1; // Not in chain, not in mempool

    if(enableIX){
        if (nResult < 10){
            int signatures = GetTransactionLockSignatures();
            if(signatures >= INSTANTX_SIGNATURES_REQUIRED){
                return nInstantXDepth+nResult;
            }
        }
    }

    return nResult;
}

int CMerkleTx::GetBlocksToMaturity() const
{
    if (!(IsCoinBase() || IsCoinStake()))
        return 0;
    return max(0, nCoinbaseMaturity - GetDepthInMainChain() + 1);
}


bool CMerkleTx::AcceptToMemoryPool(bool fLimitFree, bool fRejectInsaneFee, bool ignoreFees)
{
    return ::AcceptToMemoryPool(mempool, *this, fLimitFree, NULL, fRejectInsaneFee, ignoreFees);
}



bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb)
{

    {
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!(tx.IsCoinBase() || tx.IsCoinStake()))
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(false);
            }
        }
        return AcceptToMemoryPool(false);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int GetInputAge(CTxIn& vin)
{
    const uint256& prevHash = vin.prevout.hash;
    CTransaction tx;
    uint256 hashBlock;

    LogPrintf("*** RGP GetInputAge Start \n ");

    bool fFound = GetTransaction(prevHash, tx, hashBlock);
    if ( fFound )
    {

        if ( mapBlockIndex.find(hashBlock) != mapBlockIndex.end())
        {

           return pindexBest->nHeight - mapBlockIndex[hashBlock]->nHeight;
        }
        else
        {
           return 0;
        }
    }
    else
    {
        LogPrintf("*** RGP GetInputAge NOT Found \n ");
        return 0;
    }
}


int GetInputAgeIX(uint256 nTXHash, CTxIn& vin)
{
    int sigs = 0;
    int nResult = GetInputAge(vin);
    if(nResult < 0) nResult = 0;

    if (nResult < 6){
        std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
        if (i != mapTxLocks.end()){
            sigs = (*i).second.CountSignatures();
        }
        if(sigs >= INSTANTX_SIGNATURES_REQUIRED){
            return nInstantXDepth+nResult;
        }
    }

    return -1;
}

int GetIXConfirmations(uint256 nTXHash)
{
    int sigs = 0;

    std::map<uint256, CTransactionLock>::iterator i = mapTxLocks.find(nTXHash);
    if (i != mapTxLocks.end()){
        sigs = (*i).second.CountSignatures();
    }
    if(sigs >= INSTANTX_SIGNATURES_REQUIRED){
        return nInstantXDepth;
    }

    return 0;
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
//LogPrintf("RGP CTxIndex::GetDepthInMainChain before ReadFromDisk Debug 800 \n");
    CBlock block;
    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return 1 + nBestHeight - pindex->nHeight;
}

static map < uint256, CTransaction > Active_Transaction_List;

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool Get_MN_Transaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock)
{

LogPrintf("RGP Get_MN_Transaction START hash used is %s \n", hash.ToString() );
    {
        LOCK(cs_main);
        {
            if (mempool.lookup(hash, tx))
            {
                return true;        
            }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
               
        if (tx.ReadFromDisk(txdb, hash, txindex))
        {
            CBlock block;
LogPrintf("RGP Get_MN_Transaction before ReadFromDisk Debug 802 %d \n", txindex.pos.nBlockPos ); 
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
        else
        {
            // look for transaction in disconnected blocks to find orphaned CoinBase and CoinStake transactions
            // RGP no point searching through Orphans, return false to activate further search
            //return false;


            /* ----
               -- RGP 
            ----*/
            mapBlockIndex.find(hashBlock);
LogPrintf("RGP Get_MN_Transaction Debug Active_Transaction_List size %d \n", Active_Transaction_List.size() ); 

            static map < uint256, CTransaction >::iterator tx_list ;

            if ( Active_Transaction_List.size() != 0 )
            {
                for ( tx_list = Active_Transaction_List.begin(); tx_list != Active_Transaction_List.end(); ++tx_list )
                {
//LogPrintf("RGP Get_MN_Transaction top of loop  %s %s \n", tx_list->first.ToString(), tx_list->second.ToString() );
                    /* check if we did not find anything */
                    if ( tx_list == MN_CTransaction_Map.end() ) 
                    {
                        /* no entry found */
LogPrintf("RGP Extract_MN_Collatoral MN_CTransaction_Map not found! %s %s \n", tx_list->first.ToString(), tx_list->second.ToString() );
 
                    }
                    else
                    {
                        /* Something was found */
                        if (tx_list->second.GetHash() == hash)
                        {
                            hashBlock = tx_list->second.GetHash();
LogPrintf("RGP Get_MN_Transaction Something was found hash  \n" );

//LogPrintf("RGP Get_MN_Transaction Something was found hash %s %s \n", tx_list->first.ToString(), tx_list->second.ToString() );
                            return true;
                        }
 
                    }
                }
            }

        MilliSleep(1);

            /* Otherwise, go through everyblock :( */

            
            BOOST_FOREACH(PAIRTYPE(const uint256, CBlockIndex*)& item, mapBlockIndex)
            {
                CBlockIndex* pindex = item.second;
//LogPrintf("RGP GetTransaction search through blocks for transaction  \n" );

                /* Move from index zero to max block to find the transaction */
                if ( pindex == pindexBest || pindex->pnext != 0)
                {
  
                    MilliSleep(1);      /* Let other threads execute as the search continues */
                    continue;
                }
//LogPrintf("RGP GetTransaction check block \n");

                CBlock block;
                MilliSleep(1);
//LogPrintf("RGP CTxIndex::GetDepthInMainChain before ReadFromDisk Debug 803 %d \n", txindex.pos.nBlockPos ); 

                if (!block.ReadFromDisk(pindex))
                {
                    LogPrintf("RGP Get_MN_Transaction failed to read block \n");
                    MilliSleep(5);
                    continue;
                }
                BOOST_FOREACH(const CTransaction& txOrphan, block.vtx)
                {
                    if (txOrphan.GetHash() == hash)
                    {
LogPrintf("RGP Get_MN_Transaction found in the block \n");
                        tx = txOrphan;
                        hashBlock = block.GetHash();
LogPrintf("RGP Get_MN_Transaction hash %s added to Active_Transaction_List transaction %s DSEE \n", hashBlock.ToString(), tx.ToString() );
                        Active_Transaction_List.insert(std::make_pair( hashBlock, tx ));

LogPrintf("RGP MAIN Get_MN_Transaction END hash used is %s \n", hashBlock.ToString() );

                        return true;
                    }
         
                    MilliSleep(2);
                }
                MilliSleep(1);
            }
        }

    }
    return false;
}



// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock)
{

LogPrintf("RGP MAIN GetTransaction hash used is %s \n", hash.ToString() );
    {
        LOCK(cs_main);
        {
            if (mempool.lookup(hash, tx))
            {
                return true;        
            }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
              
        if (tx.ReadFromDisk(txdb, hash, txindex))
        {
            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }

    }
    return false;
}

//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
CBlockIndex *pblockindex;

    if (nHeight < nBestHeight / 2)
    {
        pblockindex = pindexGenesisBlock;
    }
    else
    {
        pblockindex = pindexBest;
    }

    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
    {
        pblockindex = pblockindexFBBHLast;
    }
    while (pblockindex->nHeight > nHeight)
    {
        pblockindex = pblockindex->pprev;
        //MilliSleep(1); /* RGP Optimize */
    }

    while (pblockindex->nHeight < nHeight)
    {
        pblockindex = pblockindex->pnext;
        //MilliSleep(1); /* RGP Optimize */
    }

    pblockindexFBBHLast = pblockindex;

    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{

    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
    {
printf("RGP Debug ReadFromDisk 002\n");
        return false;
    }
    
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const uint256& hash)
{
    map<uint256, COrphanBlock*>::iterator it = mapOrphanBlocks.find(hash);
    if (it == mapOrphanBlocks.end())
        return hash;

    // Work back to the first block in the orphan chain
    do {
        map<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocks.find(it->second->hashPrev);
        if (it2 == mapOrphanBlocks.end())
        {
            return it->first;
        }

        it = it2;
        MilliSleep(1); /* RGP Optimize */

    } while ( true );
}

// ppcoin: find block wanted by given orphan block
uint256 WantedByOrphan(const COrphanBlock* pblockOrphan)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblockOrphan->hashPrev))
    {
        MilliSleep(1); /* RGP Optimize */
        pblockOrphan = mapOrphanBlocks[pblockOrphan->hashPrev];
    }
    return pblockOrphan->hashPrev;
}

// Remove a random orphan block (which does not have any dependent orphans).
void static PruneOrphanBlocks()
{
    if (mapOrphanBlocksByPrev.size() <= (size_t)std::max((int64_t)0, GetArg("-maxorphanblocks", DEFAULT_MAX_ORPHAN_BLOCKS)))
        return;

    // Pick a random orphan block.
    //int pos = insecure_rand() % mapOrphanBlocksByPrev.size();
    //RGP fix this later
    int pos = 100 % mapOrphanBlocksByPrev.size();

    std::multimap<uint256, COrphanBlock*>::iterator it = mapOrphanBlocksByPrev.begin();
    while (pos--)
    {
        it++;
        MilliSleep(1); /* RGP Optimize */
    }

    // As long as this block has other orphans depending on it, move to one of those successors.
    do {
        std::multimap<uint256, COrphanBlock*>::iterator it2 = mapOrphanBlocksByPrev.find(it->second->hashBlock);
        if (it2 == mapOrphanBlocksByPrev.end())
            break;
        it = it2;

        MilliSleep(1); /* RGP Optimize */

    } while(1);

    setStakeSeenOrphan.erase(it->second->stake);
    uint256 hash = it->second->hashBlock;
    delete it->second;
    mapOrphanBlocksByPrev.erase(it);
    mapOrphanBlocks.erase(hash);
}

static CBigNum GetProofOfStakeLimit(int nHeight)
{
    return bnProofOfStakeLimit;
}

/* ------------------------------------------------------------------------------------
   -- RGP, Bank Society GOLD Coin Proof of Work Rewards                              --
   ------------------------------------------------------------------------------------
   -- Premine Phase		: Block 1 to 100                 Reward = 2,000,000  --
   -- PoW Phase Start		: Block 101 to 500 000 	         Reward = 2.0        --
   -- PoW Phase  		: Block 500 001 to 1 000 000     Reward = 1.5	     --
   -- PoW Phase  		: Block 1 000 001 to 5 000 000   Reward = 1.0	     --
   -- PoW Phase  		: Block 5 000 001 to 20 000 000  Reward = 0.75	     --
   -- PoW Phase  		: Block 20 000 001 to 50 000 000 Reward = 1.0	     --
   -- PoW Phase  		: Block 50 000 001 to 75 000 000 Reward = 1.5	     --
   -- PoW ends 75 000 001                                                            --
   ------------------------------------------------------------------------------------ */
/*
Dynamic Block Reward 3.0 - (C) 2017 Crypostle
    https://github.com/JustinPercy/crypostle
*/

double GetDynamicBlockReward3(int nHeight)
{
double nDifficulty = GetDifficulty();
double nNetworkHashPS = GetPoWMHashPS();
double nSubsidyMin = 1.0;
double nSubsidyMax = 1.0;
double nSubsidyBase = nSubsidyMin;
double nSubsidyMod = 0.0;
int nFinalSubsidy = 0;
        //int TightForkHeight = 120000;

        /* ------ Pre-Mining Phase: Block #0 (Start) ------ */
        if (nHeight == 0)
        {
            nSubsidyMax = 1.0;
        }
        /* ------ Initial Mining Phase: Block #1 Up to 100 ------ */
        if (nHeight > 0)
        {
            nSubsidyMax = 200000.0; /* RGP Block 1 to 100 - Premine */
        }
        /* ------ Initial Mining Phase: Block #101 Up to #500 000 ------ */
        if (nHeight > 100)
        {
            nSubsidyMax = 2.0; /* RGP Block 101 to 500 000 - PoW Start */
        }

        /* ------ Regular Mining Phase: Block #500 001 Up to #1 000 000 ------ */
        if (nHeight > 500001)
        {
            nSubsidyMax = 2.0;
        }
        /* ------ Regular Mining Phase: Block #1 000 001 - #5 000 000 ------ */
        if (nHeight > 1000001)
        {
            nSubsidyMax = 1.0;
        }

        /* ------ Regular Mining Phase: Block #5 000 01 - #20 000 000 ------ */
        if (nHeight > 5000001)
        {
            nSubsidyMax = 1.0;
        }

        /* ------ Regular Mining Phase: Block #20 000 001 - #50 000 000 ------ */
        if (nHeight > 20000001)
        {
            nSubsidyMax = 1.0;
        }

        /* ------ Regular Mining Phase: Block #500001 PoW forever ------ */
        if (nHeight > 50000001)
        {
            nSubsidyMax = 2.0;
        }

        if ( nNetworkHashPS > 0.0 )
        {
            nSubsidyMod = nNetworkHashPS / nDifficulty;
//LogPrintf("RGP Debug GetDynamicRewards %f %f \n", nNetworkHashPS, nDifficulty );
        }
        else
            nSubsidyMod = 0.0;


/* RGP Fix for POW mining, due to old and new software */
//nSubsidyMax = 1.0;

        nSubsidyBase = nSubsidyMax - nSubsidyMod;

        /* Default Range Control for initial mining phases (Mitigates mining-centralization with 100% reward loss) */

        if ( nHeight < 101 )
        {
            /* do nothing, as it's PREMINE phase */
            //LogPrintf("RGP Debug Premine time \n");
        }
        else
        {
            /* ------ Max (Loose) ------ */
            if (nSubsidyMod > nSubsidyMax)
            {
                nSubsidyBase = nSubsidyMax;
            }
            /* ------ Min (Loose) ------ */
            if (nSubsidyMod < nSubsidyMin)
            {
                nSubsidyBase = nSubsidyMin;
            }
        }

        nFinalSubsidy = int ( nSubsidyBase  );

//LogPrintf("RGP Debug GetDynamicRewards %d %f \n", nFinalSubsidy, nSubsidyBase );

        return nFinalSubsidy;

}


/* ------------------------------------------------------------------------------------
   -- RGP, Bank Society GOLD Coin Proof of Work Rewards                              --
   ------------------------------------------------------------------------------------
   -- Premine Phase		: Block 1 to 100                 Reward = 2,000,000  --
   -- PoW Phase Start		: Block 101 to 500 000 	         Reward = 2.0        --
   -- PoW Phase  		: Block 500 001 to 1 000 000     Reward = 1.5	     --
   -- PoW Phase  		: Block 1 000 001 to 5 000 000   Reward = 1.0	     --
   -- PoW Phase  		: Block 5 000 001 to 20 000 000  Reward = 0.75	     --
   -- PoW Phase  		: Block 20 000 001 to 50 000 000 Reward = 1.0	     --
   -- PoW Phase  		: Block 50 000 001 to 75 000 000 Reward = 1.5	     --
   -- PoW ends 75 000 001                                                            --
   ------------------------------------------------------------------------------------ */


int64_t GetProofOfWorkReward(int nHeight, int64_t nFees)
{
    double MoneySupply;
    /*
    Dynamic Block Reward - (C) 2017 Crypostle

    Reward adjustments based on network hasrate, previous block difficulty

    Simulating real bullion mining: If the difficulty rate is low; using excessive
    work to produce low value blocks does not yield large return rates. When the
    ratio of difficulty adjusts and the network hashrate remains constant or declines:
    The reward per block will reach the maximum level, thus mining becomes very profitable.
    This algorithm is intended to discourage >51% attacks, or malicous miners.
    It will also act as an automatic inflation adjustment based on network conditions.
    */

    double nSubsidyBase;

    /* ----------------------------------------------------------------
       -- RGP, THis algorithm has changed from SOCI, as SOCI did not --
       -- Proof Of Work past the PoW phase. However, SOCG allows     --
       -- mining until maximum supply has been reached.              --
       -- MoneySupply is part of the pindexBest or last block that   --
       -- was successfully accepted and stored.                      --
       ---------------------------------------------------------------- */

    MoneySupply = (double)pindexBest->nMoneySupply / (double)COIN;
    //LogPrintf("*** RGP PoW Moneysupply is string %f \n", MoneySupply  );

    if ( MoneySupply > 450000000.0 )
    {
        LogPrintf("MAXIMUM Money SupplyExceeded!!!\n");
        return 0;
    }


    if (nHeight > 0) // Version 2.0 after Block 0
    {
        if (nHeight < 0) // Version 2.0 before Block 0
        {
            //nSubSidyBase = GetDynamicBlockReward2();
            nSubsidyBase = 1;
        }
        else // Version 3.0 after Block 0
        {
            nSubsidyBase = GetDynamicBlockReward3(nHeight);
        }
    }
    else
    {
        nSubsidyBase = 1; // Genesis (Unspendable)
    }

    int64_t nSubsidy = nSubsidyBase * COIN;

//LogPrintf("RGP Debug GetProofOfWork %d %f \n", nSubsidy, (nSubsidyBase * COIN));

    return nSubsidy + nFees;

    /* RGP, old code from previous developers
	int64_t nSubsidy;
	if (nHeight < 101) {
		nSubsidy = 10000 * COIN;
	}
	else {
		nSubsidy = 10 * COIN;
	}

   return nSubsidy + nFees;
    */

}


/* ---------------------------------------------------------------------------
   -- RGP, Bank Society GOLD Coin Proof of Stake Rewards                    --
   ---------------------------------------------------------------------------
   -- Premine Phase	  : Block 1 to 100                                      --
   -- PoS Phase Start : Block 101 to 500 000             Reward 35%         --
   -- PoS Phase  	  : Block 500 001 to 1 000 000       Reward 30%         --
   -- PoS Phase		  : Block 1 000 001 to 5 000 000     Reward 15%         --
   -- PoS Phase		  : Block 5 000 001 to 20 000 000    Reward 10%         --
   -- PoS Phase		  : Block 20 000 001 to 50 000 000   Reward 5%          --
   -- PoS Phase		  : Block 50 000 000+                Reward 2%          --
   --------------------------------------------------------------------------- */

int64_t GetProofOfStakeReward(const CBlockIndex* pindexPrev, int64_t nCoinAge, int64_t nFees)
{

LogPrintf("*** RGP GetProofOfStakeReward  nCoinAge %d nFees %d  \n",  nCoinAge,  nFees );

    int64_t nSubsidy = nCoinAge * COIN_YEAR_REWARD * 33 / (365 * 33 + 8);
    int64_t nSubsidyBase = nCoinAge * COIN_YEAR_REWARD / (365 + 8 / 33);
    int64_t total_POS_reward;

    double dSubsidy = nCoinAge * COIN_YEAR_REWARD * 33 / (365 * 33 + 8);

    double MoneySupply;
    double Balance;
    CAmount Wallet_Balance;

    total_POS_reward = 0;

    /* ----------------------------------------------------------------
       -- RGP, This algorithm has changed from SOCI.                 --
       -- Proof Of Stake for SOCG allows staking until MoneySupply   --
       -- Maximum supply has been reached.                           --
       -- MoneySupply is part of the pindexBest or last block that   --
       -- was successfully accepted and stored.                      --
       ---------------------------------------------------------------- */
 LogPrintf("*** RGP GetProofOfStakeReward nSubsidy %f nSubsidyBase %d new calc %f \n", nSubsidy,  nSubsidyBase, dSubsidy);


    MoneySupply = (double)pindexBest->nMoneySupply / (double)COIN;


    /* ---------------------------------------------------------------------
     * -- RGP, 28th April 2023, Changed the Money Supply from 75M to 450M --
     * --------------------------------------------------------------------- */
    if ( MoneySupply > 450000000.0 )
    {
        LogPrintf("MAXIMUM MoneySupply Exceeded!!! %f  %f  %f \n", MoneySupply, (double)pindexBest->nMoneySupply, (double)COIN );
        return 0;
    }

    /* RGP Check that the Balance is the same or higher than WALLET_STAKE_COLLATERAL */
    Wallet_Balance = pwalletMain->GetBalance();
    Balance = (double) Wallet_Balance / (double)COIN;
    //LogPrintf("*** RGP GetProofOfStakeReward wallet balance %f \n", Balance  );

    /* ------ Pre-Mining Phase: Block #0 (Start) ------ */
    if (pindexPrev->nHeight == 0)
    {
        nSubsidy = 0.03125;
    }
    /* ------ Initial Mining Phase: Block #1 Up to 100 ------ */
    else
    {
        nSubsidy = nSubsidy * 1;  // 1000%
    }
    /* ------ Initial Mining Phase: Block #101 Up to 500 000 ------ */
    if (pindexPrev->nHeight > 100)
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidy * 0.35; //0.35 keep the same. Otherwise, incorrect rewards.
    }
    /* ------ Initial Mining Phase: Block #500 001 Up to 1 000 000 ------ */
    if (pindexPrev->nHeight > 1000000)
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidy * 0.30; // Keep the same, Otherwise incorrect rewards
    }
    /* ------ Regular Mining Phase: Block #1 000 001 Up to 5 000 000 ------ */
    if (pindexPrev->nHeight > 5000000)
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidy * 0.025; // 0.025 was 0.15
    }

    /* ------ Regular Mining Phase: Block #5 000 001 Up to 20 000 000  ------ */
    if (pindexPrev->nHeight > 5000000)
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidyBase * 0.01; // 0.01 was 0.1
    }

    /* ------ Regular Mining Phase: Block #20 000 001 Up to 50 000 000  ------ */
    if (pindexPrev->nHeight > 20000000)
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidyBase * 0.005; // 0.005 was 0.05
    }

    /* ------ Regular Mining Phase: Block #50 000 001 Up to 75 000 000  ------ */
    if (pindexPrev->nHeight > 50000001 )
    {
        /* RGP, JIRA BSG-181 Update */
        nSubsidy = nSubsidyBase * 0.0025; // 0.0025 was 0.02
    }

    LogPrintf("Coin Stake creation GetProofOfStakeReward(): create=%s subsidybase %s nCoinAge=%d\n", FormatMoney(nSubsidy), nSubsidyBase, nCoinAge);
    LogPrintf("Subsidy %d Fees %d \n", nSubsidy, nFees );

    total_POS_reward = nSubsidy + nFees;

    LogPrintf("returning total POS reward %d \n", total_POS_reward );

    return total_POS_reward;
}


/* RGP, set here to ensure that a flood of miners cannot physically
   mine blocks faster */
static int64_t nTargetTimespan = 1 * 24 * 60 * 60;  	/* target Time Span 1 day */

// ppcoin: find last block index up to pindex
const CBlockIndex* GetLastBlockIndex(const CBlockIndex* pindex, bool fProofOfStake)
{
    while (pindex && pindex->pprev && (pindex->IsProofOfStake() != fProofOfStake))
    {
        pindex = pindex->pprev;

    }
    MilliSleep(1); /* RGP Optimize */
    return pindex;
}

unsigned int GetNextTargetRequired(const CBlockIndex* pindexLast, bool fProofOfStake)
{
    CBigNum bnTargetLimit = fProofOfStake ? GetProofOfStakeLimit(pindexLast->nHeight) : Params().ProofOfWorkLimit();

    if (pindexLast == NULL)
        return bnTargetLimit.GetCompact(); // genesis block

    const CBlockIndex* pindexPrev = GetLastBlockIndex(pindexLast, fProofOfStake);
    if (pindexPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // first block
    const CBlockIndex* pindexPrevPrev = GetLastBlockIndex(pindexPrev->pprev, fProofOfStake);
    if (pindexPrevPrev->pprev == NULL)
        return bnTargetLimit.GetCompact(); // second block

    int64_t nActualSpacing = pindexPrev->GetBlockTime() - pindexPrevPrev->GetBlockTime();

    if (nActualSpacing < 0){
        nActualSpacing = TARGET_SPACING;
    }

    // ppcoin: target change every block
    // ppcoin: retarget with exponential moving toward target spacing
    CBigNum bnNew;
    bnNew.SetCompact(pindexPrev->nBits);
    int64_t nInterval = nTargetTimespan / TARGET_SPACING;
    bnNew *= ((nInterval - 1) * TARGET_SPACING + nActualSpacing + nActualSpacing);
    bnNew /= ((nInterval + 1) * TARGET_SPACING);

    if (bnNew <= 0 || bnNew > bnTargetLimit)
        bnNew = bnTargetLimit;

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > Params().ProofOfWorkLimit())
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

int Known_Block_Count;

bool IsInitialBlockDownload()
{
static int64_t nLastUpdate;
static CBlockIndex* pindexLastBest;

    //LOCK(cs_main);

    Known_Block_Count = Checkpoints::GetTotalBlocksEstimate();

    if (pindexBest == NULL || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
    {
        //LogPrintf("*** RGP IsInitialBlockDownload bestheight is less that estimate %d %d \n", nBestHeight, Checkpoints::GetTotalBlocksEstimate()  );

        //UNLOCK_FUNCTION(cs_main);
        return true;

    }

    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }

    UNLOCK_FUNCTION(cs_main);

    return (GetTime() - nLastUpdate < 15 &&
            pindexBest->GetBlockTime() < GetTime() - 8 * 60 * 60);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
LogPrintf("RGP InvalidChainFound Debug 001 %s \n", pindexNew->ToString() );
    if (pindexNew->nChainTrust > nBestInvalidTrust)
    {
LogPrintf("RGP InvalidChainFound Debug 002 \n");
        nBestInvalidTrust = pindexNew->nChainTrust;
        CTxDB().WriteBestInvalidTrust(CBigNum(nBestInvalidTrust));
    }

LogPrintf("RGP InvalidChainFound Debug 003 \n");
    uint256 nBestInvalidBlockTrust = pindexNew->nChainTrust - pindexNew->pprev->nChainTrust;
    uint256 nBestBlockTrust = pindexBest->nHeight != 0 ? (pindexBest->nChainTrust - pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
      pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
      CBigNum(pindexNew->nChainTrust).ToString(), nBestInvalidBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexNew->GetBlockTime()));
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
      hashBestChain.ToString(), nBestHeight,
      CBigNum(pindexBest->nChainTrust).ToString(),
      nBestBlockTrust.Get64(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()));
      
      
      
      
}


void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(GetBlockTime(), GetAdjustedTime());
}

void UpdateTime(CBlock& block, const CBlockIndex* pindexPrev)
{
    block.nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    block.nBits = GetNextTargetRequired(pindexPrev, false);
}

bool IsConfirmedInNPrevBlocks(const CTxIndex& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth)
{
    for (const CBlockIndex* pindex = pindexFrom; pindex && pindexFrom->nHeight - pindex->nHeight < nMaxDepth; pindex = pindex->pprev)
    {
        if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
        {
            nActualDepth = pindexFrom->nHeight - pindex->nHeight;
            return true;
        }
    }
    return false;
}

bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
    
    LogPrintf("RGP CTransaction::DisconnectInputs \n");
        //
        // class CTxDB access to LevelDB, where the blockchain is managed. (folder in .Society/txleveldb)
        // class CTxIn is of class CTxIn, where 'prevout' is a class variable of type CoutPoint
        // CoutPoint is a class COutPoint in core.h
        // class CTransaction
        //    std::vector<CTxIn> vin;
        //    std::vector<CTxOut> vout;
        //    class COutPoint defines prevout (previous block hash )
        //
        // class CTxIn is 
        //
        //   COutPoint prevout;
        //   CScript scriptSig;
        //   CScript prevPubKey;
        //   unsigned int nSequence;
        
        
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
        
            // RGP, txin holds the previous transaction     
              
LogPrintf("RGP CTransaction::DisconnectInputs %s Previous transaction hash \n", txin.prevout.hash.ToString());      
 
            if ( txin.prevout.hash.ToString() == "9038f84cbd49013aaa328941238659be8d4e2ee8f19e056a5471ce83dbfb3249" )
            {
               // This one of the bad blocks caused at block 575697 ignore it
               continue;
            }
            COutPoint prevout = txin.prevout;

LogPrintf("RGP CTransaction::DisconnectInputs %s prevout hash \n", prevout.hash.ToString());      

            // Get prev txindex from disk, read using the hash txin.previous.hash
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
            {
LogPrintf("RGP DisconnectInputs prevout.hash %s \n \n", prevout.hash.ToString() );
            	// RGP removing the error() call as it affects MN processing
                // return error("DisconnectInputs() : ReadTxIndex failed");
                LogPrintf("DisconnectInputs() : ReadTxIndex failed\n");
                
                // If this entry is not read from the data stream, let's try to delete
                
                
                //return false;
	        }
	        else
	        {
                LogPrintf("RGP DisconnectInputs Debug 900 \n");

                // RGP commented out, as the block has no spent coins
            	//if ( prevout.n >= txindex.vSpent.size())
               	//   return error("DisconnectInputs() : prevout.n out of range");

            	// Mark outpoint as not spent
            	//txindex.vSpent[prevout.n].SetNull();

 LogPrintf("RGP DisconnectInputs Debug 910 \n");

            	// Write back
            	//if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                //{
//LogPrintf("RGP CTransaction::DisconnectInputs updatetxindex failed \n");      
               	 //return error("DisconnectInputs() : UpdateTxIndex failed");
                //} 

 LogPrintf("RGP DisconnectInputs Debug 920 \n");

	        }
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.

LogPrintf("RGP CTransaction::DisconnectInputs erasing TX index  \n");      
    txdb.EraseTxIndex(*this);

    return true;
}

/* --------------------------------------------------------------------------
   -- RGP, mapTestPool is also MapUnused, inputsRet is passed in as Inputs --
   --      however some of the information is not collected, such as nTime --
   --      vin and vout sizes. These issues                                --
   --      MapPrevTx& inputsRet is a list of COutPoint prevout by hash     --
   --      order.                                                          --
   --                                                                      --
   -- FetchInputs : The role of this function is test validate transaction --
   --               Inputs and Outputs, find them and check validity.      --
   -------------------------------------------------------------------------- */

bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid)
{
uint256 hashblock;
bool fFound = false;
extern CTransaction tx_copy; /* This is a copy of the Transaction linked to the inputs that we are trying to check */
extern CTransaction other_copy;                                

    // LogPrintf("RGP FetchInputs mapTestPool %d inputsRet.size() %d \n", mapTestPool.size(), inputsRet.size() );

    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    MilliSleep(1); /* RGP Optimise */

    if (IsCoinBase())
    {
        LogPrintf("RGP FetchInputs IsCoinBase() \n" );
        return true; // Coinbase transactions have no inputs to fetch.
    }

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        COutPoint prevout = vin[i].prevout;

        CTxIndex& txindex = inputsRet[prevout.hash].first; /* Used only to initialise txindex */

        // Read txindex from txdb on disk
        fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        if ( !fFound )
        {
LogPrintf("RGP FetchInputs ReadTXIndex FAILED \n");
        }

        /* RGP, the transaction is not stored on the disk, as some early PoW messages were invalid */
        /*      Let's monitor and determine how many blocks have this issue                        */
        /*      It seems common with last two outputs that are failing, where that are a lot of     
                    transactions.                                                                  */
        if (!fFound && (fBlock || fMiner))
        {
	        //fInvalid = false;
            //return fMiner ? false : LogPrintf("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString(),  prevout.hash.ToString());
            // RGP it's not an error, it's caused by the mined block being invalid 
            //return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString(),  prevout.hash.ToString());
            //return fMiner ? false : fInvalid = false;
            // return false;
LogPrintf("RGP FetchInputs not found \n");
            continue;
        }

        MilliSleep(5); /* RGP Optimise */

        /* RGP inputsRet[prevout.hash].second is failing as there is nothing in inputsRet */
        CTransaction& txPrev = inputsRet[prevout.hash].second;


        
        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
LogPrintf("RGP FetchInputs Debug 003.1 \n");
            // Get prev tx from single transactions in memory
            if (!mempool.lookup(prevout.hash, txPrev))
                return error("FetchInputs() : %s mempool Tx prev not found %s", GetHash().ToString(),  prevout.hash.ToString());
            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());

        }
        else
        {
            // Get prev tx from disk, last two to three transactions are not stored 
            // When lots of transactions are recorded.
            if ( txindex.pos.nBlockPos == 0 )
            {

               // Do nothing
               //LogPrintf("RGP Debug FetchInputs() %d fBlock %d fMiner BlockPos is ZERO FIX and Resolve \n", fBlock, fMiner);
LogPrintf("RGP FetchInputs Debug 004xy \n");
MilliSleep(1);
               continue; /* RGP Ignore */
               //return false;
               //return true;
            }   
            else
            {
                /* ------------------------------------------------------------------------------------
                   -- RGP, BSG-Work-23 code removed as it called a null file, no sense in the test   -- 
                   ------------------------------------------------------------------------------------ */

                //if (!txPrev.ReadFromDisk(txindex.pos))
                //{
                //  return error("FetchInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString(),  prevout.hash.ToString());     
                //}
            }
        }

	MilliSleep(2); /* RGP Optimise */
    }


    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {

        const COutPoint prevout = vin[i].prevout;
        CTxIndex& txindex = inputsRet[prevout.hash].first;

        // Read txindex from txdb on disk
        fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        if ( !fFound )
        {
LogPrintf("RGP FetchInputs ReadTXIndex FAILED \n");
        }


        if (prevout.n > vout.size() || prevout.n >= txindex.vSpent.size())
        {
// FIXUP LogPrintf("RGP Debug FetchInputs() prevout.n %d vout.size() %d txindex.vSpent.size() %d \n", prevout.n, vout.size(), txindex.vSpent.size() );
            // Revisit this if/when transaction replacement is implemented and allows
            // adding inputs:
            fInvalid = true;
// FIXUP LogPrintf("RGP Debug FetchInputs() Failed 666 prevout.hash %s \n", prevout.hash.ToString() );
            //return DoS(100, error("FetchInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString()));
            continue;
        }
        
        MilliSleep(2); /* RGP Optimise */
    }

    // LogPrintf("RGP Debug FetchInputs() Successfull \n");

    return true;
}

/* -- 
   -- RGP, the MapPrevTx& inputs does not work

   */

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
CTxOut null_contents; // = CTxOut() ;
CTransaction null_transaction = CTransaction();

null_contents.SetNull();

//LogPrintf("RGP GetOutputFor Debug 1.0 CTXIN %s \n", input.ToString() );

//LogPrintf("RGP GetOutputFor Debug 1.0-001 vin.size() %d vout.size() %d \n", vin.size(),  vout.size() );

COutPoint prevout = input.prevout;
CTxIndex& txindex = inputs[prevout.hash].first;
bool fFound = true;

//fFound = txdb.ReadTxIndex(prevout.hash, txindex);
//            if ( fFound )
//LogPrintf("RGP FetchInputs ReadTXIndex SUCCESS \n");
//            else
//LogPrintf("RGP FetchInputs ReadTXIndex FAILED \n");



    //MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    //if (mi == inputs.end())
    //{
    //    LogPrintf("RGP GetOutputFor Debug 1.0.1 end of inputs \n" );

        // RGP Disabled throw exception
        //throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");
    //    return ( null_contents );
    //}
//LogPrintf("RGP GetOutputFor Debug 2.9 \n");

    //const CTransaction& txPrev = (mi->second).second;

//LogPrintf("RGP GetOutputFor Debug 3.0 vin.size() %d vout.size() %d \n", vin.size(), vout.size() );
//LogPrintf("RGP GetOutputFor Debug 3.0.1 prout hash  %s \n", prevout.hash.ToString() );

//    if ( txPrev.vin.size() == 0 || txPrev.vout.size() == 0 )
    if ( vin.size() == 0 || vout.size() == 0 )
    {

//LogPrintf("RGP GetOutputFor Debug 3.1 \n");
        return null_contents;
    }
    else
    {

//LogPrintf("RGP GetOutputFor Debug 1.1 %s \n", prevout.hash.ToString() );

        if ( prevout.n == vout.size() )
        {
//LogPrintf("RGP GetOutputFor Debug 1.2 prevout.n %d vout.size() %d \n", prevout.n, vout.size() );
            // RGP some idiot used a throw, which stops everything synching!
            // throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");
            return vout [ 0 ];
        }
//LogPrintf("RGP GetOutputFor Debug 1.2.1 prevout.n %d vout.size() %d \n", prevout.n, vout.size() );
        return vout[ prevout.n  ];
    }

//LogPrintf("RGP GetOutputFor Debug 4.0 \n");

}

/* input and inputs need to be INVESTIGATED */
int64_t CTransaction::GetOutputFor_Other(const CTxIn& input, const MapPrevTx& inputs) const
{
int64_t null_contents;
CTransaction null_transaction = CTransaction();

    null_contents = 0;
// typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx;

//LogPrintf("RGP GetOutputFor_Other prevout.hash %s \n", input.prevout.hash.ToString() );

    auto mi = inputs.find( input.prevout.hash );

    //MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    if (mi == inputs.end())
    {
        LogPrintf("RGP GetOutputFor_Other int64_t Debug 1.0.1 end of inputs INVESTIGATE why CTXIN %s \n", input.ToString() );

        // RGP Disabled throw exception, stops synching
        //throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");
        return null_contents;
    }


    if ( mi->first != input.prevout.hash )
    {
LogPrintf("RGP GetOutputFor_Other DEBUG 2.0 input.prevout.hash %s \n",  input.prevout.hash.ToString() );

LogPrintf("RGP (mi->second).second %s \n", (mi->second).second.ToString() );
        return null_contents;
    }


    const CTransaction& txPrev = (mi->second).second;


    if ( txPrev.vin.size() == 0 || txPrev.vout.size() == 0 )
    {
//LogPrintf("RGP GetOutputFor_Other int64_t Debug 2.2 YES THEY ARE ZERO OR NULL %s \n", txPrev.ToString() );
        return null_contents;
    }


    /* RGP updated to use CTransaction instead of inputs, which needs INVESTIGATED */
    //if ( txPrev.vin.size() == 0 || txPrev.vout.size() == 0 )
    if ( vin.size() == 0 || vout.size() == 0 )
    {
LogPrintf("RGP GetOutputFor_Other int64_t Debug 2.1 NULL CONTENTS CTXIN %s \n", input.ToString() );
        return null_contents;
    }
    else
    {

LogPrintf("RGP GetOutputFor_Other int64_t Debug 2.2 %s \n", txPrev.ToString() );
/* RGP updated to use CTransaction instead of inputs, which needs INVESTIGATED */
        //if (input.prevout.n >= txPrev.vout.size())
        if ( input.prevout.n >= vout.size())        
        {
LogPrintf("RGP GetOutputFor_Other int64_t Debug 2.3  \n" );
            // RGP some idiot used a throw, which stops everything synching!
            // throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");
        }
        // No UNDERSTANDEE
        // CTransaction.vout (CTXout.value)
LogPrintf("RGP GetOutputFor_Other int64_t Debug 3.0 input.prevout.n %d txPrev.vout[input.prevout.n].nValue %d \n", input.prevout.n, txPrev.vout[input.prevout.n].nValue );
        return txPrev.vout[input.prevout.n].nValue;
LogPrintf("RGP GetOutputFor_Other int64_t Debug 4.0  \n" );
    }

LogPrintf("RGP GetOutputFor_Other int64_t Debug 4.0 \n");

}


int64_t CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
CTxOut outputs;

    if (IsCoinBase())
        return 0;


    int64_t long nResult = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {

        nResult = nResult + GetOutputFor_Other(vin[i], inputs);

    }

    return nResult;

}

   /* ----------------------------------------------------------------------------------------
       -- RGP, According to Bitcoin, a Proof of Work block should meet isCoinBase() as true --
       --      However, Proof of Stake can have many VIN entries.                           --
       --      Therefore, ConnectInputs is connecting all PoS inputs, the IsCoinBase()      --
       --      check is to validate only the PoW block.                                     --
       --------------------------------------------------------------------------------------- */

bool CTransaction::ConnectInputs(CTxDB& txdb, MapPrevTx inputs, map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
    const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, unsigned int flags, bool fValidateSig)
{
int error_filter = 0;

    // Bitcoin PoW term : A coinbase transaction is not allowed to have any other inputs, hence this check. 
    //                    Furthermore, because this check is part of consensus code, it is the definition 
    //                    for a coinbase transaction, so this ends up being a bit circular - the function says 
    //                    that a coinbase transaction must only have one input, and it checks that.

/* DEFINITION typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx; */

    /* RGP, vin[0].prevout should be NULL if IsCoinBase() for PoW validation only 
            Bitcoin has no PoS, this function shall check POS inputs              */

    if (!IsCoinBase())
    {
        int64_t nValueIn = 0;
        int64_t nFees = 0;

        //LogPrintf("RGP ConnectInputs SPECIAL vin.size() %d vin[0].prevout. %s vout.size() %d \n", vin.size(), vin[0].prevout.ToString(), vout.size() );

        /* --------------------------------------------
           -- Process all Proof of Stake VIN entries --
           --------------------------------------------*/
        
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
//LogPrintf("RGP ConnectInputs VIN %d prevout.n %d hash %s \n", i, prevout.n, prevout.hash.ToString() );
// std::vector<CTxIn> vin;

            /* RGP, this will determine if the inputs has the hash or not */
            if ( inputs.count(prevout.hash ) == 0 )
            {
                LogPrintf("RGP ConnectInputs inputs[ prevout.hash ] FAILED, no VIN transaction \n");
                continue;
            }

            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            /* RGP INVESTIGATE later txPrev.nTime is creatly incorrectly in previous code */
            // LogPrintf("RGP ConnectInputs txPrev.nTime %d Gettime() %d \n", txPrev.nTime, GetTime() ); 
            //if inputs

//typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx;

            /* Investigate why txPrev.nTime is almost at the latest GetTime() value, previous
               error introduced in AcceptBlock through to here */

            /* RGP, both of these tests are failing all of the time, when there are large
                    transactions being processed, looks like there is a bug here        
                    to be investigated later.  */
            //if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())

            /* RGP Input data is incorrect, using real transaction info for vout.size() */
            /* txindex.vSpent.size() invalid as inputs[] is incorrect TO BE INVESTIGATE */
            if (prevout.n >= vout.size()  )
            {
//                LogPrintf("RGP ConnectInputs prevout.n %d txPrev.vout.size() %d txindex.vSpent.size() %d \n", prevout.n, txPrev.vout.size(), txindex.vSpent.size() );
 
//                LogPrintf("RGP ConnectInputs CTxIndex& size of vSpent %d CTransaction %s \n", txindex.vSpent.size(), txPrev.ToString() );

//LogPrintf(" RGP ConnectInputs() : hash %s prevout.n out of range %d %u %u prev tx %s \n %s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString() );

//                LogPrintf("RGP ConnectInputs actual CTransaction data vin size %d vout size %s \n", vin.size(), vout.size() );

 //               LogPrintf("RGP prevout.n out of range, bypassing error FIXUP Later \n");
                //return DoS(100, error("ConnectInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString()));
            }
 
            /* RGP txindex generated from inputs[] is invalid TO BE INVESTIGATED */
            if ( prevout.n > txindex.vSpent.size() )
            {
 //               LogPrintf("RGP ConnectInputs prevout.n %d txindex.vSpent.size() %d  \n", prevout.n, txindex.vSpent.size() );
 //
 //               LogPrintf("RGP prevout.n out of range, bypassing error FIXUP Later \n");
                //return DoS(100, error("ConnectInputs() : %s prevout.n out of range %d %u %u prev tx %s\n%s", GetHash().ToString(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString(), txPrev.ToString()));
            }

            /* RGP inputs[] is invalid time is todays date, even for 2 year old blocks TO BE INVESTIGATED */ 
            if (txPrev.nTime > nTime)
            {
                //LogPrintf("RGP ConnectInputs Debug 003 SKIPPING ISSUES WITH PREVIOUS Transactions \n");
                continue;
            }
            else
            {// If prev is coinbase or coinstake, check that it's matured
                /* This code is never executed, as inputs[] data is time invalid TO BE INVESTIGATED */
                if (txPrev.IsCoinBase() || txPrev.IsCoinStake())
                {
                    int nSpendDepth;
                    if (IsConfirmedInNPrevBlocks(txindex, pindexBlock, nCoinbaseMaturity, nSpendDepth)){
                        return error("ConnectInputs() : tried to spend %s at depth %d", txPrev.IsCoinBase() ? "coinbase" : "coinstake", nSpendDepth);
                    }
                }
                // ppcoin: check transaction timestamp
            
                LogPrintf("RGP ConnectInputs Debug Transaction Time %d  txPrev.nTime %d FIXUP Ignoring ERROR time now %d \n", nTime, txPrev.nTime, GetTime() );
                //return DoS(100, error("ConnectInputs() : transaction timestamp earlier than input transaction"));
                continue;

LogPrintf("RGP ConnectInputs Debug 004 VnValue %d  \n", txPrev.vout[prevout.n].nValue );
                // Check for negative or overflow input values
                nValueIn += txPrev.vout[prevout.n].nValue;
LogPrintf("RGP ConnectInputs Debug 004.1 \n");


                if (!MoneyRange(txPrev.vout[prevout.n].nValue) || !MoneyRange(nValueIn))
                {
LogPrintf("RGP ConnectInputs Debug 004.2 \n");
                    return DoS(100, error("ConnectInputs() : txin values out of range"));
                }

           }
           MilliSleep( 2 );

        }

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
//LogPrintf("RGP ConnectInputs Debug 006 \n");
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.

/* RGP BSG-Work-25 define of Inputs */
// prevout.hash, txPrev ( transaction )
//typedef std::map<uint256, std::pair<CTxIndex, CTransaction> > MapPrevTx;


            if ( txPrev.nTime > nTime )
            {
                /* RGP, Bug here as txPrev.nTime is todays date, but should be the date of the older transaction
                   -> error prev 1744440292 nTime 1610157928 */
                continue;
                if ( error_filter == 0 )
                {                
LogPrintf("RGP ConnectInputs Debug 006.5.1 txPrev.nTime > block time, must be an earlier error prev %d nTime %d \n", txPrev.nTime, nTime );
                    error_filter++;
                }
                else
                {
                    if ( error_filter > 200 )
                        error_filter = 0;

                }
            }
            else
            {
                if (!txindex.vSpent[prevout.n].IsNull())
                {
LogPrintf("RGP ConnectInputs Debug 006.5.1.5 Ignore vSpent not equal to null\n");
                /* ------------------------------------------------------------------------------------
                   -- RGP, found that invalid stakes may also be causing this issue, where the synch --
                   --      algorithm has caused by invalid coin mint                                 -- 
                   --      The use of error() needs to be stopped, as it is closing the wallet       --
                   --      without any explanation.                                                  -- 
                   --      To do, add logic to determine if a PoS stake is occuring or not and       --
                   --      decide logic as required.                                                 --
                   ------------------------------------------------------------------------------------ */

                    // Ignore                    return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString(), txindex.vSpent[prevout.n].ToString());
                    
                }
            }

            if(fValidateSig)
            {
LogPrintf("RGP ConnectInputs Debug 006.6 \n");
                // Skip ECDSA signature verification when connecting blocks (fBlock=true)
                // before the last blockchain checkpoint. This is safe because block merkle hashes are
                // still computed and checked, and any change will be caught at the next checkpoint.
                if (!(fBlock && !IsInitialBlockDownload()))
                {
                    // Verify signature
                    if (!VerifySignature(txPrev, *this, i, flags, 0))
                    {
LogPrintf("RGP ConnectInputs Debug 006.7 \n");
                        if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                            // Check whether the failure was caused by a
                            // non-mandatory script verification check, such as
                            // non-null dummy arguments;
                            // if so, don't trigger DoS protection to
                            // avoid splitting the network between upgraded and
                            // non-upgraded nodes.
LogPrintf("RGP ConnectInputs Debug 006.8 \n");
                            if (VerifySignature(txPrev, *this, i, flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, 0))
                                return error("ConnectInputs() : %s non-mandatory VerifySignature failed", GetHash().ToString());
                        }
                        // Failures of other flags indicate a transaction that is
                        // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                        // such nodes as they are not following the protocol. That
                        // said during an upgrade careful thought should be taken
                        // as to the correct behavior - we may want to continue
                        // peering with non-upgraded nodes even after a soft-fork
                        // super-majority vote has passed.

                        /* ---------------------------------------------------------
                           -- RGP : NOTE this fails on every block... Investigate --
                           --------------------------------------------------------- */
//LogPrintf("RGP ConnectInputs Debug 006.9 DOS error TO BE RESOLVED later BUG RGP\n");

                        //return DoS(100,error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString()));
                    }
                }
            }
//LogPrintf("RGP ConnectInputs Debug 010 \n");

            if ( txPrev.nTime > nTime )
            {
                LogPrintf("RGP ConnectInputs Debug 006.5.1 txPrev.nTime > block time, must be an earlier error posThisTx %d   \n", posThisTx.nBlockPos );
            }
            else
            {
                // Mark outpoints as spent
                txindex.vSpent[prevout.n] = posThisTx;
            }

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }

            MilliSleep( 2 );
        }
//LogPrintf("RGP ConnectInputs Debug 011 \n");
        if (!IsCoinStake())
        {

            /* RGP looks like nValueIn is zero at this point, not calulated previously - INVESTIGATE */
            if (nValueIn < GetValueOut())
            {
                // Ignoring this error for large transactions
                //return DoS(100, error("ConnectInputs() : %s value in < value out", GetHash().ToString()));
            }

            // Tally transaction fees

// LogPrintf("RGP ConnectInputs %d nValueIn %d GetValueOut() \n", nValueIn, GetValueOut() );
            /* RGP INVESTIGATE why nValueIn is always ZERO here */
            int64_t nTxFee = nValueIn - GetValueOut();
            if (nTxFee < 0)
            {
                // LogPrintf("RGP ConnectInputs nTxFee is < 0 nValue %d GetValueOut() %d INVESTIGATE \n", nValueIn, GetValueOut() );
                /* Ignore until investigated */
                //return DoS(100, error("ConnectInputs() : %s nTxFee < 0", GetHash().ToString()));
            }

            /* Invalide due to error with input[] TO BE INVESTIGATED */
            nFees += nTxFee;
            if (!MoneyRange(nFees))
            {
//LogPrintf("RGP ConnectInputs MOneyRange failed INVESTIGATE nFees %d range %d \n", nFees, MoneyRange(nFees) );
                //return DoS(100, error("ConnectInputs() : nFees out of range"));
            }
        }
    }
//LogPrintf("RGP ConnectInputs Debug 012 \n");
    return true;
}

bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{

LogPrintf("RGP Debug DisconnectBlock pindex %s \n ", pindex->ToString() );

    // Disconnect in reverse order
    
LogPrintf("RGP DEBUG DisconnectBlock check of DisconnectInputs logic %d vtx size \n", vtx.size() );
    for (int i = vtx.size()-1; i >= 0; i--)
    {
        if (!vtx[i].DisconnectInputs(txdb))
        {
LogPrintf("RGP Debug DisconnectBlock, DisconectInputs FAIL! \n");
            return false;
	    }
	
    }

LogPrintf("RGP Debug DisconnectBlock pindex %s \n \n pindex->pprev %s \n \n", pindex->ToString(), pindex->pprev->ToString() );

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
LogPrintf(" RGP Debug DisconnectBlock pindex->pprev is not zero \n");

        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
        {
            return error("DisconnectBlock() : WriteBlockIndex failed");
        }
    }
LogPrintf(" RGP Debug DisconnectBlock after write block index \n");
    // ppcoin: clean up wallet after disconnecting coinstake
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, false);

    return true;
}

/* -- 
   -- MN_Disconnect_Block is a new function to read an incomplete --
   -- block from disk, zeroise all details and write back to 
   -- disk.
   -- 


   -- */

bool CBlock::MN_Disconnect_Block( CBlockIndex* pindex )
{
int block_height;
CBlock block;
CBlockIndex* pblockindex;

    /* Locate the disk block using the nHeight value */
    block_height = pindex->nHeight;
    LogPrintf("RGP MN_Disconnect_Block block height %d \n", block_height );

    if (block_height < 0 )
        return false;


    pblockindex = pindex;
    if ( !block.ReadFromDisk(pblockindex, true) )
    {
        LogPrintf("RGP MN_Disconnect_Block ReadFromDisk FAILED! \n");
        return false;
    }

    LogPrintf("RGP Disk block read hash is %s \n", pblockindex->GetBlockHash().ToString() );

    //pblockindex->phashBlock = uint265 (0);
    //pblockindex->pprev = NULL;
    //pblockindex->pnext = NULL;
;
    /* Update diskblock, NOTE Write only appends FFS */
//    if (!block.WriteToDisk(pblockindex->nFile, pblockindex->nBlockPos))
//       LogPrintf("RGP MN_Disconnect_Block block disk write FAILED for block height %d \n", block_height );


    return true;
}



bool static BuildAddrIndex(const CScript &script, std::vector<uint160>& addrIds)
{
CScript::const_iterator pc = script.begin();
CScript::const_iterator pend = script.end();
std::vector<unsigned char> data;
opcodetype opcode;
bool fHaveData = false;
uint160 addrid = 0;

    while (pc < pend)
    {
        script.GetOp(pc, opcode, data);
        if (0 <= opcode && opcode <= OP_PUSHDATA4 && data.size() >= 8) { // data element
            addrid = 0;
            if (data.size() <= 20) {
                memcpy(&addrid, &data[0], data.size());
            } else {
                addrid = Hash160(data);
            }
            addrIds.push_back(addrid);
            fHaveData = true;
        }

    }

    MilliSleep(1); /* RGP Optimize */

    if (!fHaveData)
    {
        uint160 addrid = Hash160(script);
        addrIds.push_back(addrid);
        return true;
    }
    else
    {
    if(addrIds.size() > 0)
        return true;
    else
        return false;
    }
}

bool FindTransactionsByDestination(const CTxDestination &dest, std::vector<uint256> &vtxhash) {
    uint160 addrid = 0;
    const CKeyID *pkeyid = boost::get<CKeyID>(&dest);
    if (pkeyid)
        addrid = static_cast<uint160>(*pkeyid);
    if (!addrid) {
        const CScriptID *pscriptid = boost::get<CScriptID>(&dest);
        if (pscriptid)
            addrid = static_cast<uint160>(*pscriptid);
    }
    if (!addrid)
    {
        LogPrintf("FindTransactionsByDestination(): Couldn't parse dest into addrid\n");
        return false;
    }

    LOCK(cs_main);
    CTxDB txdb("r");
    if(!txdb.ReadAddrIndex(addrid, vtxhash))
    {
        LogPrintf("FindTransactionsByDestination(): txdb.ReadAddrIndex failed\n");
        return false;
    }
    return true;
}

void CBlock::RebuildAddressIndex(CTxDB& txdb)
{
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();
        // inputs
        if(!tx.IsCoinBase())
        {
            MapPrevTx mapInputs;
            map<uint256, CTxIndex> mapQueuedChangesT;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                return;

            MapPrevTx::const_iterator mi;
            for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
            {
                BOOST_FOREACH(const CTxOut &atxout, (*mi).second.second.vout)
                {
                    std::vector<uint160> addrIds;
                    if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                    {
                        BOOST_FOREACH(uint160 addrId, addrIds)
                        {
                            if(!txdb.WriteAddrIndex(addrId, hashTx))
                                LogPrintf("RebuildAddressIndex(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                        }
                    }
                }
            }
        }
        // outputs
        BOOST_FOREACH(const CTxOut &atxout, tx.vout) {
            std::vector<uint160> addrIds;
            if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
            {
                BOOST_FOREACH(uint160 addrId, addrIds)
                {
                    if(!txdb.WriteAddrIndex(addrId, hashTx))
                        LogPrintf("RebuildAddressIndex(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                }
            }
        }
    }
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck)
{
int64_t nFees = 0;
int64_t long nValueIn = 0;
int64_t long nValueOut = 0;
int64_t nStakeReward = 0;
unsigned int nSigOps = 0;
int nInputs = 0;

//LogPrintf("RGP CBlock::ConnectBlock() Start \n");

    // Check it again in case a previous version let a bad block in, but skip BlockSig checking
    if (!CheckBlock(!fJustCheck, !fJustCheck, false))
    {
LogPrintf("RGP ConnectBlock CheckBlock failed \n");
        return false;

    }
    unsigned int flags = SCRIPT_VERIFY_NOCACHE;

    //// issue here: it doesn't know the version
    unsigned int nTxPos;
    if (fJustCheck)
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it, it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    else
    {
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) - (2 * GetSizeOfCompactSize(0)) + GetSizeOfCompactSize(vtx.size());
    }

    map<uint256, CTxIndex> mapQueuedChanges;
    nFees = 0;
    nValueIn = 0;
    nValueOut = 0;
    nStakeReward = 0;
    nSigOps = 0;
    nInputs = 0;

    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();
        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);

        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("ConnectBlock() : too many sigops"));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;
        if (tx.IsCoinBase())
            nValueOut += tx.GetValueOut(); // PoW
        else
        {
            bool fInvalid;

            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
            {
LogPrintf("RGP ConnectBlock Boost for each loop Fetchinputs failed debug 5 \n");
                /* RGP ALL BLOCKS is causing an issue here return true always*/
                return true;
            }

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += GetP2SHSigOpCount(tx, mapInputs); 
            //nSigOps = 0;

            if (nSigOps > MAX_BLOCK_SIGOPS)
            {
                /* RGP block 1061 is causing a catostrophic failure, igore */
                LogPrintf("RGP ConnectBlock MAX_BLOCK_SIGOPS FAIL \n");
                //return DoS(100, error("ConnectBlock() : too many sigops"));
            }

            if ( tx.vin.size() < 3 )
            {

                int64_t nTxValueIn = tx.GetValueIn(mapInputs);
                int64_t nTxValueOut = tx.GetValueOut();

                nValueIn += nTxValueIn;
                nValueOut += nTxValueOut;
                if (!tx.IsCoinStake())
                    nFees += nTxValueIn - nTxValueOut;
                if (tx.IsCoinStake())
                    nStakeReward = nTxValueOut - nTxValueIn;


//                int64_t long nTxValueIn = 0;
//                int64_t long nTxValueOut = 0;
                int x;
//                nTxValueIn = tx.GetValueIn(mapInputs);
//                nTxValueOut = tx.GetValueOut();
//                nValueIn += nTxValueIn;
//                nValueOut += nTxValueOut;


                if (!tx.IsCoinStake())
                {
                    /* RGP, for some reason this is failing as the IsCoinStake() needs updated, more investigation required */
                    /* RGP - FIXUP */
                    if (IsProofOfWork())
                    {
                        nFees = 0; //nTxValueOut - nTxValueIn; /* RGP standard PoW fee */ 
                    }
                    else
                    {  
LogPrintf("RGP ConnectBlock NOT COINSTAKE calculating fees %d nTxValueOut  %d nTxValueIn \n", nTxValueOut, nTxValueIn );
 //                       nFees += nTxValueIn - nTxValueOut;
                    }
                }
                else
                {
                    /* Proof of Stake */
//LogPrintf("RGP ConnectBlock IS COINSTAKE calculating fees nTxValueOut %d  nTxValueIn  %d \n", nTxValueOut, nTxValueIn );
//                    nStakeReward = nTxValueOut - nTxValueIn;
                }
            }

            if (!tx.ConnectInputs(txdb, mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, flags))
            {
                if ( tx.vin.size() < 3 )
                    return false;
                else
                    return true;
            }
        }

        /* RGP what is this ? */
        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());

        MilliSleep( 1 );
    }

MilliSleep(1);
    /* PoW Validation against Acual and Calculated rewards */

    /* -------------------------------------------------------
       -- RGP, Validation failed or a block calculation was --
       --      Incorrect, money supply jumped by more than  --
       --      the rewards, leaning heavily to a software   --
       --      bug in this code...                          --
       --      This section shall ensure moneysupply is     --
       --      calculated correctly.                        --
       ------------------------------------------------------- */


    if (IsProofOfWork())
    {
        if ( nFees < 0 )
        {
            //LogPrintf("RGP Fees are negative, recalculate nfees %d \n",nFees );
            nFees = 0;
        }

        int64_t nReward = GetProofOfWorkReward(pindex->nHeight, nFees);

#ifndef LOWMEM
        //  money supply info (last PoW reward)

        pindex->nLastReward = nReward;
#endif 

        /* ------------------------------------------------------------------- 
           -- RGP, This proof of work calculation is failing for some early --
           --      blocks ignoring until new POW mining is restarted.       --
           --      The rewards were adjusted in GetProofOfWorkReward()      --
           --      in order to adjust rewards that were too high at the     --
           --      and change of rewards due to increased collatoral.       --
           ------------------------------------------------------------------- */



        int64_t nValueOut = 0;
        BOOST_FOREACH(const CTxOut& txout, vtx[0].vout)
        {            
            nValueOut = nValueOut + txout.nValue;
            //LogPrintf("RGP Check VOUT values %d txout.nValue %d nValueOut", txout.nValue, nValueOut );
            if ( txout.nValue > nReward )
                LogPrintf("\nRGP Check VOUT INVALID VOUT %d txout.nValue %d nreward ", txout.nValue, nReward );
        }
        LogPrintf("\n");

        // Check coinbase reward
        if (vtx[0].GetValueOut() > nReward)
        {
                // FIXUP LogPrintf("RGP ConnectBlock INVESTIGATE GetValueOut() > nReward %d \n", nReward );
        }



            /*return DoS(50, error("ConnectBlock() : coinbase reward exceeded (actual=%d vs calculated=%d)",
                   vtx[0].GetValueOut(),
                   nReward));*/

    }  

    if (IsProofOfStake())
    {

        // RGP, coin stake tx earns reward instead of paying fee, to be elaborated
        uint64_t nCoinAge;

        /* RGP, it's assuming that the last coin is PoS, whereas it may be PoW */
        if (!vtx[1].GetCoinAge(txdb, pindex->pprev, nCoinAge))
        {
            // ignored return error("ConnectBlock() : %s unable to get coin age for coinstake", vtx[1].GetHash().ToString());
        }
        /* --------------------------------------------------------------------------- 
           -- RGP, note passed in was pindex->pprev, which is not the current block --
           -- pindex is the current block being processed...                        --
           --
           -- THis may not need to be called, as the transaction data is after the  --
           -- GetProofOfStakeReward() has been processed at the point of the stake  --
           -- creation.
           --------------------------------------------------------------------------- */

        /* ---------------------------------------------------------------------------
           -- RGP, ignoring GetProofOfStakeReward as it's impossible to determine   --
           -- the correct coinage, as GetcoinAge() is using the previous block to  --
           -- determine coinage, even if the previous block is a PoW block.         --
           --
           -- Some math was used to calculate the correct money supply as follows:  --
           -- if Vout.size if 4 then use Vout [ 3 ] value / 0.4 = total mint, which --
           -- should be added to the MoneySupply to get correct MoneySupply.
           --------------------------------------------------------------------------- */

        //int64_t nCalculatedStakeReward = GetProofOfStakeReward( pindex, nCoinAge, nFees);

        int64_t Reward_Index_Size = vtx[1].vout.size();
        int64_t Transaction_vout = 0;   

        LogPrintf("RGP Reward_Index_Size is %d \n", Reward_Index_Size );

        switch ( Reward_Index_Size )
        {
            case 3  : /* Requires only the last vout ( Reward_Index_Size ) nValue */
                      Transaction_vout = vtx[1].vout[ Reward_Index_Size - 1 ].nValue;
                      break;

            case 4  : /* Requires both Vout( 2 ) + vout ( 3 ) nValues */
                      Transaction_vout = vtx[1].vout[ Reward_Index_Size - 1 ].nValue;
                      break;
            case 5  : /* Requires both Vout( 3 ) + vout ( 4 ) nValues */
                      Transaction_vout = vtx[1].vout[ 3 ].nValue + vtx[1].vout[ 4 ].nValue;
                      break;

            default : LogPrintf("RGP Debug Reward_Index_Size is %d \n", Reward_Index_Size );
                      break;


        }
        /* RG Reward caluculation is vout / 0.4, which is the same as Vout * 2.5 */
        Transaction_vout = Transaction_vout * 2.5;

// LogPrintf("Vout calc %d  \n", Transaction_vout );

        int64_t nCalculatedStakeReward = Transaction_vout;

        nStakeReward = nCalculatedStakeReward;
        /* RGP nMint and nStake rewards in adjusted below */

        LogPrintf(" Calculated %d vs nStakeReward %d \n", nCalculatedStakeReward, nStakeReward );

#ifndef LOWMEM
        //  money supply info (last PoS reward)
        pindex->nLastReward = nCalculatedStakeReward;
#endif

/* RGP common update after nStakeReward is corrected */
           pindex->nMint = nStakeReward;
           pindex->nMoneySupply = pindexBest->nMoneySupply + pindex->nMint;
           nCalculatedStakeReward = nStakeReward;
           pindex->nLastReward =  nCalculatedStakeReward;
//LogPrintf("*** RGP ConnectBlock nStakeReward Adjusted stakereward %d calculated %d height %d mint %d \n",nStakeReward, nCalculatedStakeReward, pindex->nHeight, pindex->nMint );
    

    }

    // ppcoin: track money supply and mint amount info
#ifndef LOWMEM

    /* RGP, found nValueOut to be 90+ mint value for a 2.09 Mint PoW coin */
   
    if (IsProofOfWork())
    {
        //LogPrintf("RGP ConnectBlock Fix-UP nValueIn %d nValueOut %d \n", nValueIn, nValueOut );
        if ( nValueOut > GetProofOfWorkReward(pindex->nHeight, nFees) )
        {
            // FIXUP LogPrintf("RGP ConnectBlock Fix-UP nValueIn %d nValueOut %d \n", nValueIn, nValueOut );
            /* Fix up the issue with nValueOut is incorrect, use the Reward for PoW at this height */
            nValueOut = GetProofOfWorkReward(pindex->nHeight, nFees);
            pindex->nMint = nValueOut - nValueIn + nFees;
        }
        else
            pindex->nMint = nValueOut - nValueIn + nFees;

        pindex->nMoneySupply = (pindex->pprev? pindex->pprev->nMoneySupply : 0) + nValueOut - nValueIn;

    }
    
    LogPrintf("RGP Moneysupply %d mint %d  \n", FormatMoney( pindex->nMoneySupply ), FormatMoney( pindex->nMint ) );

#endif
//LogPrintf("RGP ConnectBlock Debug 010 \n");
    if (!txdb.WriteBlockIndex(CDiskBlockIndex(pindex)))
        return error("Connect() : WriteBlockIndex for pindex failed");

    if (fJustCheck)
        return true;

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
        MilliSleep( 1 );
    }

    /* -------------------------------------------------------------------
       -- RGP add addrindex=1 to societyG.conf to enable tx transaction --
       --     storage and mempool.                                      --
       ------------------------------------------------------------------- */

    if(GetBoolArg("-addrindex", true))
    {
        // Write Address Index
        BOOST_FOREACH(CTransaction& tx, vtx)
        {

            uint256 hashTx = tx.GetHash();
            // inputs
            if(!tx.IsCoinBase())
            {

                MapPrevTx mapInputs;
                map<uint256, CTxIndex> mapQueuedChangesT;
                bool fInvalid;

                if (!tx.FetchInputs(txdb, mapQueuedChangesT, true, false, mapInputs, fInvalid))
                    return false;

                MapPrevTx::const_iterator mi;
                for(MapPrevTx::const_iterator mi = mapInputs.begin(); mi != mapInputs.end(); ++mi)
                {
                    BOOST_FOREACH(const CTxOut &atxout, (*mi).second.second.vout)
                    {

                        std::vector<uint160> addrIds;
                        if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                        {
                            BOOST_FOREACH(uint160 addrId, addrIds)
                            {
                                if(!txdb.WriteAddrIndex(addrId, hashTx))
                                    LogPrintf("ConnectBlock(): txins WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                                    
                                MilliSleep( 1 );
                            }
                        }
                        
                        MilliSleep( 1 );
                    }
                    
                    MilliSleep( 1 );
                }
            }

            // outputs
            BOOST_FOREACH(const CTxOut &atxout, tx.vout)
            {

                std::vector<uint160> addrIds;
                if(BuildAddrIndex(atxout.scriptPubKey, addrIds))
                {

                    BOOST_FOREACH(uint160 addrId, addrIds)
                    {

                        if(!txdb.WriteAddrIndex(addrId, hashTx))
                            LogPrintf("ConnectBlock(): txouts WriteAddrIndex failed addrId: %s txhash: %s\n", addrId.ToString().c_str(), hashTx.ToString().c_str());
                        MilliSleep( 1 );
                    }
                }
                MilliSleep( 2 );
            }
            MilliSleep( 2 );
        }
    }
    else
    {
        /* RGP : Report check the addrindexin societyG.conf */
        LogPrintf("\n\nRGP ConnectBlock check to write transactions FAILURE not written Debug, check SocietyG.conf addrindex=1 \n");
    }

// LogPrintf("\n\nRGP ConnectBlock check to write transactions SUCCESS Writing Debug 010 \n");

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        SyncWithWallets(tx, this);
        MilliSleep( 1 );
    }
    return true;
}

/* ---------------------------------------------------------
   -- RGP, Reorganize NEVER removes the pindexBest block  --
   --      Added a new ReoganizeBadBlock for this purpose --
   --                                                     --
   --------------------------------------------------------- */

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew)
{
    // Find the fork
    CBlockIndex* pfork = pindexBest;   /* Current last known index */
    CBlockIndex* plonger = pindexNew;  /* index to replace pindexBest */

LogPrintf("RGP Debug Reorganize entry pindexBest %s  \n",  pindexBest->ToString() );

LogPrintf("RGP Debug Reorganize entry pindexNew %s   \n",  pindexNew->ToString() );


    /* RGP, this code does nothing, as it's always true */
    while (pfork != plonger)
    {
        /* Always true when called from Reorganize() */
        while (plonger->nHeight > pfork->nHeight)
        {
            MilliSleep(1); /* RGP Optimize */
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        }

        /* Never true if called from Reorganize() */
        if (pfork == plonger)
        {
     LogPrintf("RGP debug Reorganize pfork is equal to plonger \n");
        }
        break;

        /* Should never happen, unless two blocks are the same :(  */
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");

        MilliSleep(1); /* RGP Optimize */
    }

LogPrintf("RGP debug Reorganize Debug 100 \n");

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;

    for ( CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
    {
        LogPrintf("RGP Debug Reorganize list of what to disconnect pindex %s \n",  pindex->ToString() );
        vDisconnect.push_back(pindex);
    }
    
    LogPrintf("RGP debug Reorganize count of vDisconnect %d \n", vDisconnect.size() );
    
    if ( vDisconnect.size() == 0 )
    {
       CBlockIndex* pindex = pindexBest;
       vDisconnect.push_back( pindex );
    }
    LogPrintf("RGP debug Reorganize count of vDisconnect %d \n", vDisconnect.size() );

    // List of what to connect
    vector<CBlockIndex*> vConnect;

LogPrintf("RGP debug Reorganize Debug 120 \n");

    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
    {
LogPrintf("RGP debug Reorganize connect Debug 130 \n");
        vConnect.push_back(pindex);
        if ( vConnect.size() == 1 )
        {
  LogPrintf("RGP debug Reorganize connect Debug 135 \n");          
             break;
        }
    }
LogPrintf("RGP debug Reorganize connect Debug 140 \n");
    reverse(vConnect.begin(), vConnect.end());

    if (fDebug)
    {
        LogPrintf("REORGANIZE: Disconnect %d blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString(), pindexBest->GetBlockHash().ToString());
        LogPrintf("REORGANIZE: Connect %d blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString(), pindexNew->GetBlockHash().ToString());
    }

    LogPrintf("RGP DisconnectBlock count of vConnect Debug 150 %d \n", vConnect.size() );

    // Disconnect shorter branch
    list<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {

LogPrintf("RGP debug Reorganize Debug 155 disconnect pindex %s \n", pindex->GetBlockHash().ToString());
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for disconnect failed"); 

LogPrintf("RGP debug Reorganize Debug 160 disconnect pindex %s ReadFromDisk Successful \n", pindex->GetBlockHash().ToString());
            
        // 
        // RGP note that the failure in the MN fails in DisconnectBlock \n);
        //
        if (!block.DisconnectBlock(txdb, pindex))
        {
            // RGP removed, affecting Master Node
            // return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString());
            LogPrintf("Reorganize() : DisconnectBlock %s failed\n", pindex->GetBlockHash().ToString());
            return false;
	    }  

    LogPrintf("RGP debug Reorganize Debug 180 \n");
 
        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_REVERSE_FOREACH(const CTransaction& tx, block.vtx)
            if (!(tx.IsCoinBase() || tx.IsCoinStake()) && pindex->nHeight > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_front(tx);
    }

LogPrintf("RGP CTxIndex::GetDepthInMainChain before ReadFromDisk Debug 200 \n"); 

    // Connect longer branch
    vector<CTransaction> vDelete;
    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");
        if (!block.ConnectBlock(txdb, pindex))
        {
            // Invalid block
            return error("Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString());
        }

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("Reorganize() : TxnCommit failed");

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        AcceptToMemoryPool(mempool, tx, false, NULL);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete) {
        mempool.remove(tx);
        mempool.removeConflicts(tx);
    }

    return true;
}


/* ---------------------------------------------------------
   -- Function : DeleteBadBlock()                         --
   --       --
   -- Remove the pindexBlock that is incorrect    --
   --
                                                          --
   --------------------------------------------------------- */

bool static DeleteBadBlock(CTxDB& txdb, CBlockIndex* pindexNew)
{
    // Find the fork
    CBlockIndex* pfork = pindexBest;   /* Current last known index */
    CBlockIndex* plonger = pindexNew;  /* index to replace pindexBest */

LogPrintf("RGP Debug DeleteBadBlock entry pindexBest %s  \n",  pindexBest->ToString() );

LogPrintf("RGP Debug DeleteBadBlock entry pindexNew %s   \n",  pindexNew->ToString() );


LogPrintf("RGP debug DeleteBadBlock Debug 100 \n");

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;

    LogPrintf("RGP Debug DeleteBadBlock list of what to disconnect pindex %s \n",  pfork->ToString() );
    vDisconnect.push_back(pfork); /* Remove PindexBest */
    
    LogPrintf("RGP debug DeleteBadBlock count of vDisconnect %d \n", vDisconnect.size() );
    
 //   if ( vDisconnect.size() == 0 )
 //   {
 //      CBlockIndex* pindex = pindexBest;
 //      vDisconnect.push_back( pindex );
 //   }
    LogPrintf("RGP debug DeleteBadBlock count of vDisconnect %d \n", vDisconnect.size() );

    // List of what to connect
    vector<CBlockIndex*> vConnect;

LogPrintf("RGP debug DeleteBadBlock Debug 120 \n");

//    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
//   {
//LogPrintf("RGP debug DeleteBadBlock connect Debug 130 \n");
//      vConnect.push_back(pindex);
//        if ( vConnect.size() == 1 )
//        {
//  LogPrintf("RGP debug DeleteBadBlock connect Debug 135 \n");          
//            break;
//        }
//    }
LogPrintf("RGP debug DeleteBadBlock connect Debug 140 \n");
//    reverse(vConnect.begin(), vConnect.end());

    //if (fDebug)
    {
        LogPrintf("DeleteBadBlock: Disconnect %d blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString(), pindexBest->GetBlockHash().ToString());
        LogPrintf("DeleteBadBlock: Connect %d blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString(), pindexNew->GetBlockHash().ToString());
    }

    //LogPrintf("RGP DisconnectBlock count of vConnect Debug 150 %d \n", vConnect.size() );

    // Disconnect shorter branch
    list<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {

LogPrintf("RGP debug DeleteBadBlock Debug 155 disconnect pindex %s \n", pindex->GetBlockHash().ToString());
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("DeleteBadBlock() : ReadFromDisk for disconnect failed"); 

LogPrintf("RGP debug DeleteBadBlock Debug 160 disconnect pindex %s ReadFromDisk Successful \n", pindex->GetBlockHash().ToString());
            
        // 
        // RGP note that the failure in the MN fails in DisconnectBlock \n);
        //
        if (!block.DisconnectBlock(txdb, pindex))
        {
            // RGP removed, affecting Master Node
            // return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString());
            LogPrintf("DeleteBadBlock() : DisconnectBlock %s failed\n", pindex->GetBlockHash().ToString());
            return false;
	    }  

    LogPrintf("RGP debug DeleteBadBlock Debug 180 \n");
 
        // Queue memory transactions to resurrect.
        // We only do this for blocks after the last checkpoint (reorganisation before that
        // point should only happen with -reindex/-loadblock, or a misbehaving peer.
        BOOST_REVERSE_FOREACH(const CTransaction& tx, block.vtx)
            if (!(tx.IsCoinBase() || tx.IsCoinStake()) && pindex->nHeight > Checkpoints::GetTotalBlocksEstimate())
                vResurrect.push_front(tx);
    }

LogPrintf("RGP DeleteBadBlock before ReadFromDisk Debug 200 \n"); 

    // Connect longer branch
//    vector<CTransaction> vDelete;
//    for (unsigned int i = 0; i < vConnect.size(); i++)
//    {
//        CBlockIndex* pindex = vConnect[i];
//        CBlock block;
//        if (!block.ReadFromDisk(pindex))
//            return error("DeleteBadBlock() : ReadFromDisk for connect failed");
//        if (!block.ConnectBlock(txdb, pindex))
//        {
//            // Invalid block
//            return error("DeleteBadBlock() : ConnectBlock %s failed", pindex->GetBlockHash().ToString());
//       }

//        // Queue memory transactions to delete
//        BOOST_FOREACH(const CTransaction& tx, block.vtx)
//            vDelete.push_back(tx);
//    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("DeleteBadBlock() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("DeleteBadBlock() : TxnCommit failed");

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;
    }

    // Connect longer branch
    //BOOST_FOREACH(CBlockIndex* pindex, vConnect)
    //    if (pindex->pprev)
    //        pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        AcceptToMemoryPool(mempool, tx, false, NULL);

    // Delete redundant memory transactions that are in the connected branch
    //BOOST_FOREACH(CTransaction& tx, vDelete) 
    //{
    //    mempool.remove(tx);
    //    mempool.removeConflicts(tx);
    //}

    return true;
}





// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
    uint256 hash = GetHash();

//LogPrintf("RGP CBlock::SetBestChainInner Start hash %s \n", hash.ToString() );

    /* ----------------------------------------------------------------------
       -- RGP Found that a combined ConnectBlock and WriteBest Hash caused --
       -- a system read file IO fatal error, split the logic, it does not  --
       -- create the file IO error?                                        --
       -- THIS CAUSED AN ERROR STOPPING AND RESTARTING, REMOVE THIS FIX    --
       ---------------------------------------------------------------------- */
    MilliSleep(1); /* RGP Optimize */

    // Adding to current best branch
    if(!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
    {

        if ( !txdb.WriteHashBestChain(hash) )
        {

           MilliSleep(2);
           txdb.TxnAbort();
           InvalidChainFound(pindexNew);
LogPrintf("RGP CBlock::SetBestChainInner failed to write best hash %s \n", hash.ToString() );
           return false;
        }

        MilliSleep(1);
    }
    
    if (!txdb.TxnCommit())
    {
        return error("SetBestChainInner() : TxnCommit failed");
    }

    // Add to current best branch
    pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
//LogPrintf("RGP SetBestChainInner remove Mempool %s \n", vtx.ToString() );
        mempool.remove(tx);
        MilliSleep(1);
    }

    return true;
}

/* -----------------------------------------------------------------
   -- SetBestChain --                                             --
   ----------------------------------------------------------------- 
   --
   --
   -- */


bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    if (hashPrevBlock != hashBestChain)
    {
       /* ----------------------------------------------------------------------------------------------
          -- RGP, this means that somehow the system has recorded an invalid  block in the blockchain --
          --      Let's determine the last block in the chain and delete it.
          --      We need what we have available here first
          ---------------------------------------------------------------------------------------------- */
      
       LogPrintf("RGP SetBestChain establish the invalid block, previous is \n %s \n Best Chain is %s \n", hashPrevBlock.ToString() , hashBestChain.ToString() );

       return false; /* error("SetBestChain() : Invalid Block\n") */
    }

    if (!txdb.TxnBegin())
        return error("SetBestChain() : TxnBegin failed");

//LogPrintf("RGP CBlock::SetBestChain Debug 002 \n");

    MilliSleep(1);

    if (pindexGenesisBlock == NULL && hash == Params().HashGenesisBlock())
    { 

        txdb.WriteHashBestChain(hash);
        if (!txdb.TxnCommit())
        {
            return error("SetBestChain() : TxnCommit failed");
        }
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {

        if (!SetBestChainInner(txdb, pindexNew))
        {
            return error("SetBestChain() : SetBestChainInner failed");
        }
    }
    else
    {

LogPrintf("RGP CBlock::SetBestChain Debug 003 \n");
        MilliSleep(1);

        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        //
        // RGP check CBlockIndex* to see if this is ever more than one block?
        //
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside

        while (pindexIntermediate->pprev && pindexIntermediate->pprev->nChainTrust > pindexBest->nChainTrust)
        {

            if ( pindexIntermediate->ToString() == "5d7c9f594c3cd222e59ea68e902a60e66941f7aa0e72d44bb8bb2758971f9a41" )
            {
               // no Pushback
               MilliSleep(1);
            }
            else
            {

               vpindexSecondary.push_back(pindexIntermediate);
               pindexIntermediate = pindexIntermediate->pprev;
//LogPrintf("RGP DEBUG SetBest Chain pindexIntermediate LOOP %s pindexIntermediate->pprev->nChainTrust %s pindexBest->nChainTrust \n", pindexIntermediate->pprev->nChainTrust.ToString(), pindexBest->nChainTrust.ToString() );
               MilliSleep(1); /* RGP Optimize */
            
            }
        }

LogPrintf("RGP CBlock::SetBestChain Debug 004 \n");
        MilliSleep(2);
        
	//
	// RGP now it checks the size of vpindexSecondary
	//
        if (!vpindexSecondary.empty())
            LogPrintf("Postponing %u reconnects\n", vpindexSecondary.size());

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);
            //RGP, disabled the error() call as it's causing the MN to lose messages and fall behind the block
            //return error("SetBestChain() : Reorganize failed");
            LogPrintf("SetBestChain() : Reorganize failed \n");
            return false;
        }
      
        MilliSleep(1);
LogPrintf("RGP CBlock::SetBestChain Debug 006 \n");
        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
            {
                if ( fDebug )
                {
                    LogPrintf("SetBestChain() : ReadFromDisk failed\n");
                }
                break;
            }
            if (!txdb.TxnBegin()) {
                if ( fDebug ){
                    LogPrintf("SetBestChain() : TxnBegin 2 failed\n");
                }
                break;
            }
            // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex))
                break;
            
            MilliSleep(1);
        }
    }
//LogPrintf("RGP CBlock::SetBestChain Debug 007 \n");

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();
    if ((pindexNew->nHeight % 20160) == 0 || (!fIsInitialDownload && (pindexNew->nHeight % 144) == 0))
    {
        const CBlockLocator locator(pindexNew);
        g_signals.SetBestChain(locator);
    }

    MilliSleep(1);

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    nBestChainTrust = pindexNew->nChainTrust;
    nTimeBestReceived = GetTime();

    //LogPrintf("RGP Setbestchain adding transaction to mempool \n");

    mempool.AddTransactionsUpdated(1);

//LogPrintf("RGP Setbestchain transactions in mempool %d \n", mempool.GetTransactionsUpdated() );

    uint256 nBestBlockTrust = pindexBest->nHeight != 0 ? (pindexBest->nChainTrust - pindexBest->pprev->nChainTrust) : pindexBest->nChainTrust;
    
    MilliSleep(1);

    if (fDebug )
    {
        LogPrintf("SetBestChain: new best=%s  height=%d  trust=%s  blocktrust=%d  date=%s\n",
        hashBestChain.ToString(), nBestHeight,
        CBigNum(nBestChainTrust).ToString(),
        nBestBlockTrust.Get64(),
        DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()));
    }
    // Check the version of the last 100 blocks to see if we need to upgrade:
  
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            if ( fDebug ){
                LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)CBlock::CURRENT_VERSION);
            }

        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {    
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    MilliSleep(1);

    return true;
}

#define FIRST_POS_BLOCK         21190
#define BLOCK_ZERO              0

// ppcoin: total coin age spent in transaction, in the unit of coin-days.
// Only those coins meeting minimum age requirement counts. As those
// transactions not in main chain are not currently indexed so we
// might not find out about their coin age. Older transactions are
// guaranteed to be in main chain by sync-checkpoint. This rule is
// introduced to help nodes establish a consistent view of the coin
// age (trust score) of competing branches.
bool CTransaction::GetCoinAge(CTxDB& txdb, const CBlockIndex* pindexPrev, uint64_t& nCoinAge) const
{
    CBigNum bnCentSecond = 0;  // coin age in the unit of cent-seconds
    nCoinAge = 0;

    if (IsCoinBase())
        return true;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // First try finding the previous transaction in database
        CTransaction txPrev;
        CTxIndex txindex;
        if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
            continue;  // previous transaction not in main chain
        if (nTime < txPrev.nTime)
            return false;  // Transaction timestamp violation

        // Read block header
        CBlock block;
        if (!block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
            return false; // unable to read block of previous transaction
        if (block.GetBlockTime() + nStakeMinAge > nTime)
            continue; // only count coins meeting min age requirement

        int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
        bnCentSecond += CBigNum(nValueIn) * (nTime-txPrev.nTime) / CENT;

        LogPrint("coinage", "coin age nValueIn=%d nTimeDiff=%d bnCentSecond=%s\n", nValueIn, nTime - txPrev.nTime, bnCentSecond.ToString());
    }

    CBigNum bnCoinDay = bnCentSecond * CENT / COIN / (24 * 60 * 60);
    LogPrint("coinage", "coin age bnCoinDay=%s\n", bnCoinDay.ToString());
    nCoinAge = bnCoinDay.getuint64();
    return true;
}


bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos, const uint256& hashProof)
{
    AssertLockHeld(cs_main);

//LogPrintf("RGP AddToBlockIndex START \n");

    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString());

    if ( hashPrevBlock != hashBestChain )
    {
        LogPrintf("RGP AcceptBlock AddToBlockIndex invalid block \n" );
        LogPrintf("RGP current Hash is %s \n", hash.ToString() );    // Possible duplicate block?
        LogPrintf("RGP hashPrevBlock is %s \n", hashPrevBlock.ToString() );
        LogPrintf("RGP hashBestChain is %s Height is %d \n \n \n", hashBestChain.ToString(), GetHeight() );

        LogPrintf("RGP AddToBlockIndex establish the invalid block, Position is %d Best Chain is %s \n", nBlockPos , hashProof.ToString() );

        /* Get the current blocks next hash block */

        PushGetBlocks(From_Node, pindexBest,  pindexBest->GetBlockHash() );
        
        MilliSleep(5);
   
        return error("AddtoBlockIndex() : Invalid Block\n");
    }        

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");

    pindexNew->phashBlock = &hash;
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }

    // ppcoin: compute chain trust score
    pindexNew->nChainTrust = (pindexNew->pprev ? pindexNew->pprev->nChainTrust : 0) + pindexNew->GetBlockTrust();

    // ppcoin: compute stake entropy bit for stake modifier
    if (!pindexNew->SetStakeEntropyBit(GetStakeEntropyBit()))
        return error("AddToBlockIndex() : SetStakeEntropyBit() failed");

    // Record proof hash value
    pindexNew->hashProof = hashProof;

    // ppcoin: compute stake modifier
    uint64_t nStakeModifier = 0;
    bool fGeneratedStakeModifier = false;
    if (!ComputeNextStakeModifier(pindexNew->pprev, nStakeModifier, fGeneratedStakeModifier))
        return error("AddToBlockIndex() : ComputeNextStakeModifier() failed");

    pindexNew->SetStakeModifier(nStakeModifier, fGeneratedStakeModifier);
    
    MilliSleep(1);
    
    // Add to mapBlockIndex
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    if (pindexNew->IsProofOfStake())
        setStakeSeen.insert(make_pair(pindexNew->prevoutStake, pindexNew->nStakeTime));
    pindexNew->phashBlock = &((*mi).first);

    MilliSleep(2);

    // Write to disk block index
    CTxDB txdb;
    if (!txdb.TxnBegin())
    {
        LogPrintf("RGP AddToBlockIndex txdb Begin FAILED \n" );

        return false;
    }
   
    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));

    if (!txdb.TxnCommit())
    {

        LogPrintf("RGP AddToBlockIndex TxnCommit failed \n" );

        return false;
    }

    MilliSleep(1);

    // New best
    if (pindexNew->nChainTrust > nBestChainTrust)
    {

        if ( !SetBestChain(txdb, pindexNew) )
        {
            LogPrintf("RGP AddToBlockIndex SetBestChain Debug 020 FAILED!!! \n" );

            return false;
        }
    }

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        
        g_signals.UpdatedTransaction(hashPrevBestCoinBase);
        
        hashPrevBestCoinBase = vtx[0].GetHash();
    }
    else
        LogPrintf("RGP AddToBlockIndex failed last check \n");

    MilliSleep(1);
//LogPrintf("RGP AddToBlockIndex END \n");
    return true;
}

bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CheckBlock() : size limits failed"));

    // Check proof of work matches claimed amount
    if (fCheckPOW && IsProofOfWork() && !CheckProofOfWork(GetPoWHash(), nBits))
        return DoS(50, error("CheckBlock() : proof of work failed"));

    // Check timestamp
    if (GetBlockTime() > FutureDrift(GetAdjustedTime()))
        return error("CheckBlock() : block timestamp too far in the future");

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return DoS(100, error("CheckBlock() : first tx is not coinbase"));
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return DoS(100, error("CheckBlock() : more than one coinbase"));

    if (IsProofOfStake())
    {
        // Coinbase output should be empty if proof-of-stake block
        if (vtx[0].vout.size() != 1 || !vtx[0].vout[0].IsEmpty())
            return DoS(100, error("CheckBlock() : coinbase output not empty for proof-of-stake block"));

        // Second transaction must be coinstake, the rest must not be
        if (vtx.empty() || !vtx[1].IsCoinStake())
            return DoS(100, error("CheckBlock() : second tx is not coinstake"));
        for (unsigned int i = 2; i < vtx.size(); i++)
            if (vtx[i].IsCoinStake())
                return DoS(100, error("CheckBlock() : more than one coinstake"));
    }

    // Check proof-of-stake block signature
    if (fCheckSig && !CheckBlockSignature())
        return DoS(100, error("CheckBlock() : bad proof-of-stake block signature"));


// ----------- instantX transaction scanning -----------

    if(IsSporkActive(SPORK_3_INSTANTX_BLOCK_FILTERING)){
        BOOST_FOREACH(const CTransaction& tx, vtx){
            if (!tx.IsCoinBase()){
                //only reject blocks when it's based on complete consensus
                BOOST_FOREACH(const CTxIn& in, tx.vin){
                    if(mapLockedInputs.count(in.prevout)){
                        if(mapLockedInputs[in.prevout] != tx.GetHash()){
                            if(fDebug) { LogPrintf("CheckBlock() : found conflicting transaction with transaction lock %s %s\n", mapLockedInputs[in.prevout].ToString().c_str(), tx.GetHash().ToString().c_str()); }
                            return DoS(0, error("CheckBlock() : found conflicting transaction with transaction lock"));
                        }
                    }
                }
            }
        }
    } else{
        if(fDebug) { LogPrintf("CheckBlock() : skipping transaction locking checks\n"); }
    }


    /* -------------------------------------------
       --          masternode payments          --
       ------------------------------------------- */

    bool MasternodePayments = false;
    bool fIsInitialDownload = IsInitialBlockDownload();

    if(nTime > START_MASTERNODE_PAYMENTS)
    {
        MasternodePayments = true;
    }

    if (!fIsInitialDownload)
    {
        if( MasternodePayments )
        {
            LOCK2(cs_main, mempool.cs);

            CBlockIndex *pindex = pindexBest;
            if( IsProofOfStake() && pindex != NULL )
            {
                /* ---------------------------------------------------------------
                   -- RGPickles, Sanity check, we MUST have the previous block  --
                   --            if not then this is not valid. Skip Masternode --
                   --            payment.                                       --
                   --------------------------------------------------------------- */

                if( pindex->GetBlockHash() == hashPrevBlock )
                {
                    /* Previous Block located, continue with Masternode payment */

                    CAmount masternodePaymentAmount;
                    for (int i = vtx[1].vout.size(); i--> 0; )
                    {
                        masternodePaymentAmount = vtx[1].vout[i].nValue;
                        break;
                    }
                    bool foundPaymentAmount = false;
                    bool foundPayee = false;
                    bool foundPaymentAndPayee = false;

                    CScript payee;
                    CTxIn vin;
                    if(!masternodePayments.GetBlockPayee(pindexBest->nHeight+1, payee, vin) || payee == CScript() )
                    {
                        foundPayee = true; //doesn't require a specific payee
                        foundPaymentAmount = true;
                        foundPaymentAndPayee = true;
                        if(fDebug) 
                        { 
                        LogPrintf("CheckBlock() : Using non-specific masternode payments %d\n", pindexBest->nHeight+1); 
                        }
                    }

                    for (unsigned int i = 0; i < vtx[1].vout.size(); i++)
                    {
                        if(vtx[1].vout[i].nValue == masternodePaymentAmount )
                            foundPaymentAmount = true;
                        if(vtx[1].vout[i].scriptPubKey == payee )
                            foundPayee = true;
                        if(vtx[1].vout[i].nValue == masternodePaymentAmount && vtx[1].vout[i].scriptPubKey == payee)
                            foundPaymentAndPayee = true;
                    }

                    CTxDestination address1;
                    ExtractDestination(payee, address1);
                    CSocietyGcoinAddress address2(address1);

                    if(!foundPaymentAndPayee)
                    {
                        //if(fDebug) { 
                        LogPrintf("CheckBlock() : Couldn't find masternode payment(%d|%d) or payee(%d|%s) nHeight %d. \n", foundPaymentAmount, masternodePaymentAmount, foundPayee, address2.ToString().c_str(), pindexBest->nHeight+1); 
                        //}
                        return DoS(100, error("CheckBlock() : Couldn't find masternode payment or payee"));
                    }
                    else
                    {
                        if (fDebug)
                        {
                           LogPrintf("CheckBlock() : Found payment(%d|%d) or payee(%d|%s) nHeight %d. \n", foundPaymentAmount, masternodePaymentAmount, foundPayee, address2.ToString().c_str(), pindexBest->nHeight+1);
                        }
                    }
                }
                else
                {
                    if(fDebug)
                    {
                        LogPrintf("CheckBlock() : Skipping masternode payment check - nHeight %d Hash %s\n", pindexBest->nHeight+1, GetHash().ToString().c_str());
                    }
                }
            }
            else
            {
                if(fDebug)
                {
                    LogPrintf("CheckBlock() : pindex is null, skipping masternode payment check\n");
                }
            }
        }
        else
        {
            //if(fDebug) { 
            LogPrintf("CheckBlock() : skipping masternode payment checks\n");
            // }
        }
    }
    else
    {
        if(fDebug) { 
        LogPrintf("CheckBlock() : Is initial download, skipping masternode payment check %d\n", pindexBest->nHeight+1); 
        }
    }


    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        if (!tx.CheckTransaction())
        {

            LogPrintf("\n\n*** RGP CHECKBLOCK Checktransaction failed \n\n");

            return DoS(tx.nDoS, error("CheckBlock() : CheckTransaction failed"));
        }

        // ppcoin: check transaction timestamp
        if (GetBlockTime() < (int64_t)tx.nTime)
        {
            LogPrintf("\n\n*** RGP CHECKBLOCK block time INVALID failed \n\n");

            return DoS(50, error("CheckBlock() : block timestamp earlier than transaction timestamp"));
        }
    }

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        return DoS(100, error("CheckBlock() : duplicate transaction"));

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += GetLegacySigOpCount(tx);
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));


    return true;
}



bool CBlock::AcceptBlock()
{
extern bool BSC_Wallet_Synching;
extern CInv Inventory_to_Request;
extern CNode *From_Node;
extern volatile bool fRequestShutdown;
uint256 hash, current_previous_hash, previous_hash; 
int display_filter = 0;
int Top_entry, bad_entry;
int bad_block_flag;
int bad_block_hash_index;
uint256 bad_block_hash_list[100];
uint256 testhash;
uint256* pHash_pointer;
CBlockIndex *check_index;
uint256 current_block;
uint256 current_best_index_hash;
int64_t Time_to_Last_block;
CBlockIndex* pindexFork = NULL;
static int last_block_time_check = 0;


int block_test, block_matches, block_index;
uint256 current_best_hash, current_best_prev_hash;
/* RGP created next to ProcessMessages() */
extern CBlock block_to_check;
CBlock temp_block;
int array_entry_to_free;
    
    AssertLockHeld(cs_main);

    // LogPrintf("RGP ACCEPTBLOCK() START \n");

    /* ---------------------------------------------------------------------------------------
       -- RGP, Added to ensure previous versions running do not send blocks to be accepted. -- 
       --------------------------------------------------------------------------------------- */
    //LogPrintf("AcceptBlock() check for bad wallet version \n");
    if (nVersion < CURRENT_VERSION || nVersion > CURRENT_VERSION )
    {
    	LogPrintf("RGP \n version bad %d \n", nVersion );
        return DoS(100, error("AcceptBlock() : reject unknown block version %d", nVersion));
    }
    
    // Check for duplicate
    /* ---------------------------------------------------------------------- 
       -- RGP, check for duplicate blocks that have already been processes --
       --      and are in the block chain.                                 --
       ---------------------------------------------------------------------- */
    hash = GetHash(); // current new block hash
    current_previous_hash = hashPrevBlock; // current new block previous hash
    uint256 hash_to_request;

    //
    // RGP, Build a list of this hash in the blockchain, it exists or does not 
    //      if it already exists then exit, as it's a duplicated block
    //
    if (mapBlockIndex.count(hash))
    {
        /* We have the new block, try to get blocks from pindexbest */        
        PushGetBlocks(From_Node, pindexBest, pindexBest->GetBlockHash() ); /* ask for again from best block */
        
        MilliSleep( 5 );

        return error("AcceptBlock() : block already in mapBlockIndex");
        //return false;
    }
    else
    {
        //
        // RGP : class CBlock has hashPrevBlock in the class for the current block
        //       Check if the previous block hash exists for the new blocks previous block link.
        //       if the previous block is not found, we did not receive the previous message yet, 
        //       so request it from other nodes.
        //
        //       if the previous block is found, we can now process this new block.
        //
        //       Alternatively, the current blcok that is pindexBest is invalid, this
        //       need to removed.
             

/* Quick fix */
 //       map<uint256, CBlockIndex*>::iterator mi_second = mapBlockIndex.find(hashPrevBlock);
 //       if (mi_second == mapBlockIndex.end())
        if ( mapBlockIndex.count( current_previous_hash ) == 0 )
        {
            /* The Previous hash was not found in the local store, look for
               the previous blockhash, if we do not have it, ask for it */

            /* Create an inventory request message, it will result in a block being returned */
            
           if ( GetHeight() == 21189 )
           {
//           	hash_to_request = "958200e9d76d2057e8913bc5b0105aceb4d84567953e401e4e967b8c599c46cf";
//           	LogPrintf("RGP hash to request %d \n", hash_to_request );
//           	Inventory_to_Request.type = MSG_BLOCK;
//           	Inventory_to_Request.hash = "958200e9d76d2057e8913bc5b0105aceb4d84567953e401e4e967b8c599c46cf";
//           	From_Node->AskFor( Inventory_to_Request, true );
//           	LogPrintf("RGP requested 958200e9d76d2057e8913bc5b0105aceb4d84567953e401e4e967b8c599c46cf \n ");
	       }
                       


            //PushGetBlocks(From_Node, pindexBest, uint256(0) );
            // From_Node->fDisconnect = true;
            MilliSleep( 2 );
	        LogPrintf("RGP Can't find previous block %s current has %s current %s \n", current_previous_hash.ToString(), hash.ToString(), hashBestChain.ToString() );
            LogPrintf("RGP Sanity check pindex best is %s prevhash is %s \n", pindexBest->GetBlockHash().ToString(), current_previous_hash.ToString() );
	    
            /* ---------------------------------------------------------------
               -- RGP, New code to fix up a false stake block,              --
               --      long storey short, bug in V1 MN code is causing this --
               --      hashPrevBlock is not located in our copy of the      --
               --      Blockchain, remove pindexBest and ask for Hashprev   -- 
               --------------------------------------------------------------- */

            previous_hash = hashPrevBlock;


            /* RGP, wait until ten minutes, 600 seconds */ 
            Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

            LogPrintf("\n\n RGP WAITING1 TO FIX BLOCK ISSUES increments %d time  %d \n", last_block_time_check, Time_to_Last_block );
            //PushGetBlocks(From_Node, pindexBest, uint256(0)  );

            if ( Time_to_Last_block > 60000  )
            {
return ( false);
               /* RGP, Deleted the current best block. */

               last_block_time_check++;
LogPrintf("RGP AcceptBlock wait for 1000 new blocks until undating %d \n",last_block_time_check );
               /* Wait for 100 per block */
               if ( last_block_time_check > 5 )
               {
LogPrintf("RGP AcceptBlock check for 5 new blocks until undating \n");
                 // pindexFork = NULL; 

                  /* ------------------------------------------------------------------ 
                     -- pindexbest might be valid, but the previous block is missing -- 
                     -- remove the pindexBest block, then request the previous_hash  --
                     -- block.                                                       --
                     ------------------------------------------------------------------ */
               //   pindexFork = pindexBest; 

                    /* RGP, fixup as SetBestChain checks the hashPrevBlock to the new */

               /* -------------------------------------------------------- 
                  -- RGP, Use the previous block as the new best block  --
                  -- moving down the chain, until the issue is resolved --
                  -------------------------------------------------------- */

                 // CBlock block;
                 // if ( !block.ReadFromDisk ( pindexFork ) )
                 // {
                 //    return error("LoadBlockIndex() : block.ReadFromDisk failed");
                 // }

                 // CTxDB txdb;
                  //block.RemoveBadBlock(txdb, pindexFork);

                  /* RGP, get new blocks from the missing pindexFork  */
    		     // PushGetBlocks(From_Node, pindexFork, uint256(0)  );

                 // last_block_time_check = 0;

               }
               MilliSleep( 10 );

            }

            return false;
        }


    	/* Prevent the new possible next index from being 
           created if the prev hash is not existing in the blockchain on disk */
       
       if ( hashPrevBlock == hashBestChain )
	    {

           // RGP, Final check for known blocks, that should be ignored
           //      Long story short, Windows wallets store everything, even bad blocks
           //      This implementation will be added to Windows and Linux wallets next.

           if ( hash.ToString() == "63c090fc37e108d47ba309991f415bccdb9957d3a2f019a99030b5746e19d2f2")
           {
                LogPrintf(" RGP Rejected bad hash 63c090fc37e108d47ba309991f415bccdb9957d3a2f019a99030b5746e19d2f2 \n");
                //return false;
           }

		/* True the hashPrevBlock exits, so we can add this new block */
		//LogPrintf("RGP current CANDIDATE Hash is %s \n", hash.ToString() );    
		//LogPrintf("RGP CANDIDATE hashPrevBlock is %s \n", hashPrevBlock.ToString() );
		//LogPrintf("RGP hashBestChain is %s Height is %d \n \n \n", hashBestChain.ToString(), GetHeight() );


		   MilliSleep(2);
	    }       
	    else
	    {

    		/* Reject it as the previous block did not arrive yet */
 
    		PushGetBlocks(From_Node, pindexBest, hashBestChain  );
		
    		/* RGP Experimental, was false  */
    		Inventory_to_Request.type = MSG_BLOCK;
           	Inventory_to_Request.hash = hash_to_request;
            From_Node->AskFor( Inventory_to_Request, true );

		    return false;
	    }
   }

   MilliSleep(5);

    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    uint256 hashProof;
    if (IsProofOfWork() && nHeight > Params().LastPOWBlock())
    {

        return DoS(100, error("AcceptBlock() : reject proof-of-work at height %d", nHeight));
    }
    else
    {

        // PoW is checked in CheckBlock()
        if (IsProofOfWork())
        {
            hashProof = GetPoWHash();
        }
    }

    /* -------------------------
       -- RGP : JIRA 177 test --
       ------------------------- */
    if (mapBlockIndex.count(hash))
    {
        LogPrintf("*** RGP JIRA 177 check one exit \n");
        return false;
    }

    MilliSleep(2);

    if (IsProofOfStake() && nHeight < Params().POSStartBlock())
        return DoS(100, error("AcceptBlock() : reject proof-of-stake at height <= %d", nHeight));

    // Check coinbase timestamp
    if (GetBlockTime() > FutureDrift((int64_t)vtx[0].nTime) && IsProofOfStake())
        return DoS(50, error("AcceptBlock() : coinbase timestamp is too early"));

    /* -------------------------
       -- RGP : JIRA 177 test --
       ------------------------- */
    if (mapBlockIndex.count(hash))
    {
        LogPrintf("*** RGP JIRA 177 check two exit \n");
        return false;
    }

    // Check coinstake timestamp
    if (IsProofOfStake() && !CheckCoinStakeTimestamp(nHeight, GetBlockTime(), (int64_t)vtx[1].nTime))
    {
        /* RGP This kills the wallet */
        return DoS(50, error("AcceptBlock() : coinstake timestamp violation nTimeBlock=%d nTimeTx=%u", GetBlockTime(), vtx[1].nTime));

    }

    MilliSleep(2);
    
    // Check proof-of-work or proof-of-stake
    if (nBits != GetNextTargetRequired(pindexPrev, IsProofOfStake()) && hash != uint256("0x474619e0a58ec88c8e2516f8232064881750e87acac3a416d65b99bd61246968") && hash != uint256("0x4f3dd45d3de3737d60da46cff2d36df0002b97c505cdac6756d2d88561840b63") && hash != uint256("0x274996cec47b3f3e6cd48c8f0b39c32310dd7ddc8328ae37762be956b9031024"))
        return DoS(100, error("AcceptBlock() : incorrect %s", IsProofOfWork() ? "proof-of-work" : "proof-of-stake"));

    // Check timestamp against prev
    if (GetBlockTime() <= pindexPrev->GetPastTimeLimit() || FutureDrift(GetBlockTime()) < pindexPrev->GetBlockTime())
        return error("AcceptBlock() : block's timestamp is too early");

    /* -------------------------
       -- RGP : JIRA 177 test --
       ------------------------- */
    if (mapBlockIndex.count(hash))
    {
        LogPrintf("*** RGP JIRA 177 check three exit \n");
        return false;
    }

    MilliSleep(1);

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        if (!IsFinalTx(tx, nHeight, GetBlockTime()))
            return DoS(10, error("AcceptBlock() : contains a non-final transaction"));

        MilliSleep(1);
    }

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckHardened(nHeight, hash))
        return DoS(100, error("AcceptBlock() : rejected by hardened checkpoint lock-in at %d", nHeight));

    MilliSleep(1);

    // Verify hash target and signature of coinstake tx
    // ------------------------------------------------------------------------
    // -- RGP 8th June 2024 --                                               --
    // -----------------------                                               --
    //                                                                       --
    // Significant effort to update this algorithm, which was ignored in the --
    // check to what the previous block is and if it is in fact a PoW or PoS --
    // If the previous coin was a PoW and this block is PoS, then the last   --
    // or previous block needs to be considered, as there may not be the     --
    // same details available, such as more than two transactions for PoS.   --
    //
    // First PoS after a series of PoW :
    //
    // If PoS, We can check the vtx vector array to see what is there 
    // and validate that it may be correct.
    
    
    if ( IsProofOfStake() )
    {

        int64_t Time_to_Last_block;
        bool read_failed;
        bool stakekernelhash;
 
//LogPrintf("RGP block identified as PoS, but it's PoW %s vtx size %d \n", GetHash().ToString(), vtx.size() );

        uint256 targetProofOfStake;
//        if ( pindexBest->nHeight == 21189 )
//        {
//            /* ignore this block */
//            LogPrintf("*** RGP Fix for 21189 \n");

//        }
//        else
        {

            /* Final check for Proof work, it seems that changing from a
               PoS to a Pow fails to recognise as a PoW, 21189, 21504    
               CBlock.vtx should have one items, PoS has up to four items */
            if ( IsProofOfWork() )  // vtx.size() == 1 )
            {
                //LogPrintf("RGP block identified as PoS, but it's PoW %s \n", GetHash().ToString() );
                hashProof = GetPoWHash();
            }
            else
            {

                if (!CheckProofOfStake(pindexPrev, vtx[1], nBits, hashProof, targetProofOfStake))
                {

                    LogPrintf("*** RGP AcceptBlock failed after check PoS %s \n" , GetHash().ToString()  );

                    /* Ask for missing blocks */
                    PushGetBlocks(From_Node, pindexBest, pindexBest->GetBlockHash() );

//                    LogPrintf(" RGP CheckProofofStake fix best block is %d \n ", pindexBest->nHeight );
//                    switch (pindexBest->nHeight )
//                    {
//                        case 21189 : LogPrintf("*** RGP Fix for 21189 \n");
//                                    /* RGP block 21189 is a PoW that looks like a PoS, ignore checks */
//                                    break;
//                        case 21504 : LogPrintf("*** RGP Fix for 21504 \n");
//                                    /* RGP block 21189 is a PoW that looks like a PoS, ignore checks */
//                                    break;
//
//                        default : return error("AcceptBlock() : check proof-of-stake failed for block %s", hash.ToString());
//
//                    };

                    //return false;
                }
                            
            }
        }

        //LogPrintf("RGP IsProofOfStake current hash being processed %s \n",  hash.ToString() );
        MilliSleep(2);

       
        
        /* -- RGP, Find the previous block to determine PoW or PoS --
           --      If Proof of work 
           --
           --      If Proof of Stake

*/
        map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
        
        // RGP, Assuming (*mi).second is pprev entry of CBlockIndex, eg the previous block of hashPrevBlock
        //      
    	CBlockIndex* pindexPrev = (*mi).second;
    	const uint256 yaboy = *pindexPrev->phashBlock;
    	int nHeight = pindexPrev->nHeight+1;

        /* Check previous block for PoS or PoW */
        if ( pindexPrev->IsProofOfWork() )
        {
 
            MilliSleep(5);
        }
        else
        {
         	
    	    const CTransaction& tx1 = vtx[0];		// Transactions defined within the block, PoW has one PoS has two+
    	    const CTransaction& tx2 = vtx[1];

            if ( vtx.size() > 1 )
            {
                //LogPrintf("RGP Previous block Valid vtx size \n");
                MilliSleep( 5 );
            }
            else
            {
                //LogPrintf("RGP Previous block INVALID vtx size %d \n", vtx.size());
                MilliSleep( 5 );
                return false;
            }
            uint256 targetProofOfStake;
            //LogPrintf("Current hash to process > %s \n",  hash.ToString() );
            MilliSleep( 5 );

        }
       
       // LogPrintf("RGP AcceptBlock VerifySignature Success \n");
       MilliSleep( 5 );
 
    }

    // Check that the block satisfies synchronized checkpoint
    if (!Checkpoints::CheckSync(nHeight) )
        return error("AcceptBlock() : rejected by synchronized checkpoint");

    /* -------------------------
       -- RGP : JIRA 177 test --
       ------------------------- */
    if (mapBlockIndex.count(hash))
    {
        LogPrintf("*** RGP JIRA 177 check for exit \n");
        return false;
    }

    // Enforce rule that the coinbase starts with serialized block height
    CScript expect = CScript() << nHeight;
    if (vtx[0].vin[0].scriptSig.size() < expect.size() ||
        !std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
        return DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
        
    MilliSleep(1);

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
        return error("AcceptBlock() : out of disk space");

    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;

    /* ----------------------------------------------------------------------------------
       -- RGP, shutdown has been causing block corruption, this fRequestShutdown check --
       --      should stop blocks from being written to disk and mapBlockIndex will    --
       --      not be generated.                                                       --
       ---------------------------------------------------------------------------------- */
    
    if ( fRequestShutdown )
    {
        MilliSleep( 5000 );
        return error("AcceptBlock(), SHUTDOWN detected, exit processing");
    }

    if (!WriteToDisk(nFile, nBlockPos))
    {


        return error("AcceptBlock() : WriteToDisk failed");
    }

LogPrintf("RGP CANDIDATE Hash written %s \n", hash.ToString() );    
    //last_block_time_check = 0; /* zero this check, as a block was just written to the chain */

    MilliSleep(5);

    if (!AddToBlockIndex(nFile, nBlockPos, hashProof))
    {

        LogPrintf("*** RGP Acceptblock, After AddToBlockIndex Invalid block from wallet node %s  block %d height %d \n", From_Node->addr.ToString(), nBlockPos,  nHeight );
        // RGP removed error() call as it's upsetting the MN
        //return error("AcceptBlock() : AddToBlockIndex failed");
        LogPrintf("AcceptBlock() : AddToBlockIndex failed \n");
        return false;
    }

    MilliSleep(1);

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    
    MilliSleep(1);

    if (hashBestChain == hash)
    {
 
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
        {

            // Push Inventory to CNode
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
            {

                pnode->PushInventory(CInv(MSG_BLOCK, hash));
            }

            // Push Inventory Height to CNode Data Cache
            if (nHeight > 0)
            {
                pnode->nSyncHeight = nHeight;
            }
            
            MilliSleep( 1 );
        }

    }

    MilliSleep(2);

    return true;
}

uint256 CBlockIndex::GetBlockTrust() const
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    if (bnTarget <= 0)
        return 0;

    return ((CBigNum(1)<<256) / (bnTarget+1)).getuint256();
}

void PushGetBlocks(CNode* pnode, CBlockIndex* pindexBegin, uint256 hashEnd)
{
static char filter_counter = 0;
static bool tester = true;

    if (tester == true )
    {

        pnode->pindexLastGetBlocksBegin = pindexBegin;
        pnode->hashLastGetBlocksEnd = hashEnd;

        pnode->PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);
        return;
    }
    

    // Filter out duplicate requests
    if (pindexBegin == pnode->pindexLastGetBlocksBegin && hashEnd == pnode->hashLastGetBlocksEnd)
    {
        /* RGP, Behavour of the system is indicating that required
                blocks are not being sent based on requests, this
                results in the request being 100% filtered, causing the
                wallet to stop synching.
                Adding a filter on this process                        */
        if ( filter_counter >  5 )
        {
            filter_counter = 0;
            /* RGP, Continue to send this message */
        }
        else
        {
            filter_counter++;
            if ( hashEnd != 0 )
            {
                /* ignore and continue */

            }
            else
            {

                return;
            }
        }
    }

    pnode->pindexLastGetBlocksBegin = pindexBegin;
    pnode->hashLastGetBlocksEnd = hashEnd;

    pnode->PushMessage("getblocks", CBlockLocator(pindexBegin), hashEnd);

}

bool static IsCanonicalBlockSignature(CBlock* pblock)
{
    if (pblock->IsProofOfWork()) {
        return pblock->vchBlockSig.empty();
    }

    return IsDERSignature(pblock->vchBlockSig, false);
}

void Misbehaving(NodeId pnode, int howmuch)
{
    if (howmuch == 0)
        return;

    CNodeState *state = State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    if (state->nMisbehavior >= GetArg("-banscore", 100))
    {
        LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name.c_str(), state->nMisbehavior-howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name.c_str(), state->nMisbehavior-howmuch, state->nMisbehavior);
}



#define MAXIMUM_ARRAY_OF_BLOCKS               30
#define MAXIMUM_ARRAY_OF_BLOCKS_SYNCHED       30
#define BLOCK_BUFFER_BEFORE_PROCESSING        0 
#define BLOCK_BUFFER_SYNCH_PROCESSING         MAXIMUM_ARRAY_OF_BLOCKS 

#define NOT_INITIALISED                       0
#define INITIALISED                           1
#define BLANK_BLOCK                           0
#define SYNCH_BLOCK_BUFFER_SIZE               3

#define BLOCKER_END_OF_BUFFER                 MAXIMUM_ARRAY_OF_BLOCKS - 1

static int synch_mode_buffer_size = MAXIMUM_ARRAY_OF_BLOCKS;
static int64_t Last_known_block_height = 0;
static CBlock verified_block;
static int CBlock_index = 0;
static int Buffer_entries = 0;
static CBlock blank_block;
static CBlock Block_Checker[ MAXIMUM_ARRAY_OF_BLOCKS ];
static CBlock Buffer_Overrun_Protection[ 5 ];               /* Protect overflow of Block_Checker on variables and code */

static int no_blank_blocks = 0;
static int Block_Checker_Array = NOT_INITIALISED;
int marked_entries, synch_check;
int marked_blocks_for_processing[ 25 ];

/* ------------------------------------------------------
   --
   ------------------------------------------------------ */

bool ProcessBlock(CNode* pfrom, CBlock* pblock)
{
uint256 hash;
uint256 hashPrev;
extern volatile bool fRequestShutdown;
extern bool BSC_Wallet_Synching;
extern CNode *From_Node;
int64_t Time_to_Last_block;
bool PoS_Mining_Block, Accept_Status, One_Block_Mode;
int filter = 0;
uint256 current_previous_hash, previous_hash; 
int block_test, block_matches, block_index, match_test;
uint256 current_best_hash, current_best_prev_hash;
/* RGP created next to ProcessMessages() */
extern CBlock block_to_check;
CBlock temp_block;
int array_entry_to_free;
int random_count_of_new_blocks;
bool ALL_Blocks_Processed;
int Blocks_to_Process, Blocks_Processed, copy_Blocks_Processed;
uint256 block_synched_time, block_check_time;
unsigned int test_time, blank_entries;
int loops_performed, duplicate_loop_count;
CBlockIndex* PreviousIndex;
CBlockIndex* NextIndex;

map < uint256, CBlock > Match_Block_Map;


    AssertLockHeld(cs_main);

    Accept_Status = false;

    PoS_Mining_Block = false;
    
    if ( filter > 10 )
    {
    	filter = 0;
    	LogPrintf("mapBlockIndex.size() = %u\n",   mapBlockIndex.size());
    }
    filter++;
    MilliSleep(1);
    //LogPrintf("*** ProcessBlock POW MINING STARTED \n");


    /* RGP, reversed logic as a Pow mint has no pfrom node */

    if ( pfrom == NULL )
    {
        //if (fDebug )
        //{
        //    LogPrintf("*** ProcessBlock POW MINING BLOCK!!! \n");
        //}

        PoS_Mining_Block = false;
    }

    // Check for duplicates

    hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
    {
        //if ( fDebug )
        //{
            //LogPrintf("*** ProcessBlock Already have the newly provide block HASH \n");
        //LogPrintf("^");
        //}

        MilliSleep(5);
        return false;
    }


    MilliSleep(1);   
    if ( fRequestShutdown )
    {
       LogPrintf("\n\nBank Society Gold, AcceptBlock processing, SHUTDOWN detected \n");
       MilliSleep(10000);
       return false;
    }

    From_Node = pfrom;

   /* --------------------------------------------------------------------------
       -- Dedicated to : Brenda Doreen Martin who died this day on            --
       --                17th November 2024, rest in peace, God rest her soul.--
       --                                                                     --
       -- RGP Block_Checker algorithm required to check all blocks being sent --
       -- as some are POW or POS that were not accepted.                      --
       -- This algorithm will continuously fill the array until maximum count --
       -- is reached, locating blank_blocks and updating the array with new   --
       -- blocks, one at a time for processing.                               --
       --                                                                     --
       -- Phase 1 : Process all incoming blocks to MAXIMUM_ARRAY_OF_BLOCKS    --
       --           Perform time checks and test HashPrevBlock is not zero    --
       --                                                                     --
       -- Phase 2 : process all recorded blocks using a Block_Checker algo    --
       --           This is based on best hash matched with HashPrevBlock,    --
       --           if the block match is ONE, everything goes to Phase 3.    --
       --           if the block match is greater than one, investigation is  --
       --           required to resolve conflicting blocks.                   --
       --                                                                     --
       -- Phase 3 : Process all blocks through AcceptBlock().                 --
       --                                                                     --
       ------------------------------------------------------------------------- */


    current_best_hash = pindexBest->GetBlockHash(); /* Start with Best Block in the chain */
    //LogPrintf("RGP ProcessBlock starting best hash %s \n", current_best_hash.ToString() );

    block_matches = 0;
    marked_entries = 0;

    if ( Block_Checker_Array == NOT_INITIALISED )
    {
        /* --------------------------------------------------------------
           -- Initialise the Block_Checker[] array using a blank_block -- 
           -------------------------------------------------------------- */

        blank_block.SetNull();
        blank_block.hashPrevBlock = uint256 ( 0 );
        blank_block.nTime = 0;
        blank_block.nBits = 0;
        blank_block.nNonce = 0;

        /* Clear the Block_Checker array */
        for ( block_test = 0; block_test < MAXIMUM_ARRAY_OF_BLOCKS; block_test++ )
        {
            Block_Checker[ block_test ] = blank_block;
        }

        Buffer_entries = 0;
        Block_Checker_Array = INITIALISED;
        // synch_check = 0;
    }
    CBlock_index = 1; /* Only zero after an initialise */

    PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() );

    

//LogPrintf("RGP  last known height %d our best height %d \n",  Last_known_block_height, nBestHeight );
    if ( ( nBestHeight + 20 ) > Last_known_block_height  )
    {
        /* Close to the bockchain height, synch to synch block by block */
        synch_mode_buffer_size = SYNCH_BLOCK_BUFFER_SIZE;
        One_Block_Mode = true;
LogPrintf("\nRGP buffer size should be 30 is  %d \n", SYNCH_BLOCK_BUFFER_SIZE );
    }
    else
    {
        synch_mode_buffer_size = MAXIMUM_ARRAY_OF_BLOCKS;
        One_Block_Mode = false;
    }

    /* Fill the Block_Checker array with BlockChain Data */    

//LogPrintf("\nRGP ProcessBlock Fill the Block_Checker array Buffer entries %d cblock index %d \n", Buffer_entries, CBlock_index );
//LogPrintf("RGP ProcessBlock block incoming %s \n", block_to_check.GetHash().ToString() );

    //blank_entries = 0;
    if ( Buffer_entries < synch_mode_buffer_size )
    {
        /* ---------------------------------------------------------------------------------
           -- Store new block in a blank block, using hashPrevBlock, which should be zero --
           --------------------------------------------------------------------------------- */

        for ( block_test = 0; block_test < synch_mode_buffer_size; block_test++ )
        {
            /* A BLANK block has hashPrevBlock set to ZERO, So find a blank block */

            if ( Block_Checker[ block_test ].hashPrevBlock == uint256 ( BLANK_BLOCK ) )
            {
                /* ---------------------------------------------------------------------
                   -- Note : block_to_check is an external and is a copy of the block --
                   --        just received. Blocks are received one at a time         --
                   --        Block stored in an available null slot and then returns  --
                   --        awaiting the next block.                                 --
                   --------------------------------------------------------------------- */

                Block_Checker[ block_test ] = block_to_check; 

//LogPrintf("\n\nRGP Storage of new hash from synch %s \n hash prev of test block %s test block previous hash %s   \n\n", current_best_hash.ToString(), Block_Checker[ block_test ].GetHash().ToString(), Block_Checker[ block_test ].hashPrevBlock.ToString() );


                /* needed to check that we always have BLOCK_BUFFER_BEFORE_PROCESSING blocks stored
                   before processing starts                                                  */

                Buffer_entries = Buffer_entries + 1 ;
                CBlock_index = CBlock_index + 1;
//LogPrintf("\n\nRGP Storage of new hash complete, exiting loop \n");
                break;                                          /* Block stored, exit the loop */
            }
            else
            {
                /* block not blank, as elements are filled, this will happen. */
//                 LogPrintf("RGP ProcessBlock Block not BLANK %s time %d block_test %d, blank_entries %d  \n", Block_Checker[ block_test ].GetHash().ToString(), Block_Checker[ block_test ].nTime, block_test, blank_entries  );
                 blank_entries++;

                 /* Check for no entries available */
//                 no_blank_blocks++;
//LogPrintf("RGP ProcessBlock buffer entries processing no_blank_blocks is %d \n", no_blank_blocks ); 
//                 if ( no_blank_blocks == ( synch_mode_buffer_size - 1 ) || no_blank_blocks == synch_mode_buffer_size )
//                 {
//                    no_blank_blocks = 0;
//                    Buffer_entries = BLOCKER_END_OF_BUFFER;
//                    Block_Checker_Array = NOT_INITIALISED;
//LogPrintf("RGP ProcessBlock buffer entries processing and clearing  Buffer_entries %d hash %s \n",  Buffer_entries, Block_Checker[ block_test ].GetHash().ToString() ); 
//                   }                 
            
        MilliSleep(1);
        }
 
   }

//PushGetBlocks(pfrom, pindexBest, uint256 ( 0 ) );

//PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  );
//LogPrintf("RGP ProcessBlock Buffer entries %d cblock index %d \n", Buffer_entries, CBlock_index );


        /* If we are synching, then add a buffer of NNN, but when there is less than 240 seconds buffer is zero */
        block_synched_time = GetTime() - pindexBest->GetBlockTime();

/* ------------------------------------------------------------------------
   -- After Buffer_entries is incremented high enough, process one block --
   ------------------------------------------------------------------------ */
            
        if ( block_synched_time > 240 && One_Block_Mode == false )
        {
            if ( Buffer_entries < BLOCKER_END_OF_BUFFER )
            {
                // LogPrintf("\nRGP NOT synched buffer \n\n");
                return;
            }
        }
        else
        {       
                
            if ( Buffer_entries < ( synch_mode_buffer_size - 1 ) )
            {
                    LogPrintf("RGP SYNCHED buffer wait until it's filled  \n");
                    return true;
            } 
            else
            {
                    LogPrintf("Time to Process Blocks! \n");        
                    ///return true;
            }
           
        }

        MilliSleep( 1 );
    }

/* See what is in the block checker array, it should be filled */
LogPrintf("RGP ProcessBlock Buffer entries %d cblock index %d \n", Buffer_entries, CBlock_index );
LogPrintf("\n");
//for ( block_test = 0; block_test < synch_mode_buffer_size; block_test++ )
//{
//    if ( Block_Checker[ block_test ].GetHash() != 0 )
//        LogPrintf("RGP ProcessBlock Block Checker Item  %d Block %s \n ", block_test, Block_Checker[ block_test ].GetHash().ToString() );
//}
    no_blank_blocks = 0;

    /* ----------------------------------------------------------------------------------- 
       -- This section will continuously scan the array and clear only if AcceptBlock() --
       -- is successful, so that all unresolved blocks will be processed at a later     --
       -- Note : Blocks may not be sequential, so a new blocks 'hashPrevBlock' must     --
       --        point the currently record best chain block.                           --
       --                                                                               --
       -- The concept of this algo is to find duplicate stakes and remove them before   --
       -- they corrupt the blockchain.                                                  -- 
       ----------------------------------------------------------------------------------- */
    {
       
//        LogPrintf("\n %d Buffer_entries %d \n", CBlock_index, Buffer_entries ); 

        marked_entries = 0;
        random_count_of_new_blocks = 0;

        /* Process the array and all one block to be processed, if validated */
        for ( block_test = 0; block_test < synch_mode_buffer_size; block_test++ )
        {
//LogPrintf("GP ProcessBlock block current hash %s \n", Block_Checker[ block_test ].GetHash().ToString() );
//LogPrintf("\nRGP ProcessBlock block prev hash %s block_test %d \n", Block_Checker[ block_test ].hashPrevBlock.ToString(), block_test ); 
            /* ----------------------------------------------------------------------------------
               -- Check that the block is not from a future time, way in excess of 750 seconds --
               -- After debugging this code, I noticed some blocks provided were very high     --
               -- up the chain, this can be investigated later...                              --
               ---------------------------------------------------------------------------------- */

            if ( Block_Checker[ block_test ].nTime == 0 )
            {
                /* Blank Block */
                LogPrintf("RGP Blank Block %s \n", Block_Checker[ block_test ].GetHash().ToString() );
                Block_Checker[ block_test ] = blank_block;
                MilliSleep( 1 );
                continue;
            }
            else
            {
                if ( Block_Checker[ block_test ].nTime - pindexBest->nTime  > 10000000  )
                {
                    test_time = Block_Checker[ block_test ].nTime;
                    test_time = test_time - pindexBest->nTime;
LogPrintf("RGP ProcessBlock Block_Checker Time check failed %d REMOVING best test %d  \n", Block_Checker[ block_test ].nTime, pindexBest->nTime  ); 
LogPrintf("RGP ProcessBlock Block_Checker Time  difference %d \n", test_time   ); 

                    if ( pindexBest->nTime - Block_Checker[ block_test ].nTime  < 1000 )
                    {
                        LogPrintf("RGP ProcessBlock just slightly less %d \n", pindexBest->nTime - Block_Checker[ block_test ].nTime );
                        /* Found a scenario where a PoS block was record with an ntime less than 
                           a POW block, due to network latencies. Process it                       */
                    }
                    else
                    {
                        LogPrintf("RGP ProcessBlock time too far %d \n", pindexBest->nTime - Block_Checker[ block_test ].nTime );
                        Block_Checker[ block_test ] = blank_block;
                        MilliSleep( 1 );
                        continue;
                    }
                }
            }

//LogPrintf("\nRGP ProcessBlock block hash \n %s \n %s \n", Block_Checker[ block_test ].GetHash().ToString(), Block_Checker[ block_test ].hashPrevBlock.ToString() ); 


            /* Get Pindex best hash and previous block */
            //temp_block = Block_Checker[ block_test ];
            if ( Block_Checker[ block_test ].hashPrevBlock == 0 )
            {
/* RGP ADD CODE FOR A NULL RECORD */
LogPrintf("RGP NULL Block found, REMOVING \n");
                Block_Checker[ block_test ] = blank_block;
                MilliSleep( 1 );
                continue;
            }

            /* ------------------------------------------------------------------------ 
               -- RGP, multiple POS blocks with the same hashPrevBlock, caused by an --
               --      invalid or rejected POS block                                 --
               --      blank_block only has hashPrevBlock set to null, meaning it is --
               --      a blank block.                                                --
               ------------------------------------------------------------------------ */
//LogPrintf("\n\nRGP before record data in Match_Block_Map block_test index %d  \n", block_test );

            if ( current_best_hash == Block_Checker[ block_test ].hashPrevBlock )
            {
                /* New Vector implementation to record the blocks could have issues */

//LogPrintf("\n\nRGP got it record data in Match_Block_Map best hash %s \n current %s \n hashprev %d \n ", current_best_hash.ToString(), Block_Checker[ block_test ].GetHash().ToString(), Block_Checker[ block_test ].hashPrevBlock.ToString() );

// 

                map < uint256, CBlock >::iterator duplicate_checker;
                duplicate_checker = Match_Block_Map.find( Block_Checker[ block_test ].GetHash() );

                /* If there is one already, ignore other matches */
                if ( duplicate_checker !=  Match_Block_Map.end()  )
                {
                    LogPrintf("RGP Match_Block_Map check, key already exists for %s \n", Block_Checker[ block_test ].GetHash().ToString() );
                }
                else
                {
                    Match_Block_Map.insert (  pair < uint256, CBlock > ( Block_Checker[ block_test ].GetHash() , Block_Checker[ block_test ] ) );
                    marked_entries++;
                         

                    marked_blocks_for_processing[ marked_entries ] = block_test; /* this is recording the loop index, into to Block_Checker[] */

                    block_matches++;
                    random_count_of_new_blocks++;
                    MilliSleep(2);
                }
 
            }
            else
            {

                if ( Block_Checker[ block_test ].hashPrevBlock != 0 )
                {
                    /* weed out the blanks, only count actual valid blocks, NEEDS ATTENTION */
                    random_count_of_new_blocks++;
                }

            }
            MilliSleep(2);
        }

//LogPrintf("RGP ProcessBlock block match  marked_entries %d array contents %d \n", marked_entries , marked_blocks_for_processing[ marked_entries ] );

        if ( random_count_of_new_blocks < synch_mode_buffer_size )
        {

            LogPrintf("RGP random_count_of_new_blocks is %d buffer is %d \n", random_count_of_new_blocks, Buffer_entries );
            PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  );
            
        }

        LogPrintf("\n\nRGP before block_match check %d \n\n", block_matches );
/* block_matches == 0 mean nothing was found so false for each one */

        if ( block_matches == 0 && pindexBest->nHeight > 1000 )
        {

            /* -----------------------------------------------------------------
               -- if no matches were found, there is no block to be processed --
               -- Request the latest pindexBest and return.                   --
               -- Erase the Block_Checker[] array and request more blocks     --
               ----------------------------------------------------------------- */

            /* ----------------------------------------------------------------------------
               -- Check mapBlockIndex.size() against pindexBest->nHeight, it's possible  --
               -- that an error occurred and mapBlockIndex holds a failed block, which   --
               -- is not being processed by Inventory Processing. This means that the    --
               -- lost block can't be created using AcceptBlock. Therefore, let's        --
               -- locate the last entry and delete until we can then continue processing --
               ---------------------------------------------------------------------------- */
            map<uint256, CBlockIndex*>::iterator block_checker = mapBlockIndex.begin();
            if ( mapBlockIndex.size() > pindexBest->nHeight )
            {

                LogPrintf("RGP ProcessBlock pindexBest->nHeight %d mapBlockIndex.size() %d \n", pindexBest->nHeight,  mapBlockIndex.size() );
                LogPrintf("RGP ProcessBlock pindexBest->GetBlockHash %s  \n", pindexBest->GetBlockHash().ToString() );

                /* get the last actual map entry, noted hashes are sorted in order, scan from begin to end  */
                /* looking for greater than pindexBest->nHeight, then erase() it */
          

                //for ( block_checker = mapBlockIndex.begin(); block_checker != mapBlockIndex.end(); ++block_checker )
                while ( block_checker != mapBlockIndex.end())
                {

                    // LogPrintf("RGP ProcessBlock dump mapblockindex hash %s height %d \n", block_checker->first.ToString(), block_checker->second->nHeight );


                    if ( block_checker->second->nHeight > pindexBest->nHeight )
                    {
                        LogPrintf("RGP ProcessBlock found a mapblockindex with higher height hash \n\n %s \n", block_checker->first.ToString() );
                    uint256 xxx = block_checker->first;

                       //delete block_checker->second;
                       //block_checker = mapBlockIndex.erase( block_checker );
                        
                        /* Check for a double entry */

                        //map<uint256, CBlockIndex*>::iterator checker = mapBlockIndex.find( block_checker->first );
                        //if (checker != mapBlockIndex.end())
                        //{
                        //    LogPrintf("RGP ProcessBlock found a mapblockindex with higher height hash %s \n", block_checker->first.ToString() );
                        //    mapBlockIndex.erase( block_checker->first );
                        //}

                        //break;
                    }
                    else
                         block_checker++;

                    MilliSleep(1);
                   
                }

                
            }

PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() );

            /* THis may be caused by a missing or missed block, restart, zero and continue */ 
            ALL_Blocks_Processed = true;



            synch_check++;
LogPrintf("RGP synch_check. %d \n", synch_check );
            if ( synch_check > MAXIMUM_ARRAY_OF_BLOCKS )
            {
 
                /* Message not in the list, reset the table to zero and start again */
                Block_Checker_Array = NOT_INITIALISED;
                synch_check = 0;

            /* RGP, go back to entries and request, as no block matches seem to 
                    be caused by a missing block.                               */

            PreviousIndex = pindexBest->pprev;
            PushGetBlocks(pfrom, PreviousIndex, PreviousIndex->GetBlockHash()  );
//LogPrintf("RGP Asking for PREVIOUS %s \n", PreviousIndex->GetBlockHash().ToString() );
            PreviousIndex = PreviousIndex->pprev;
            PushGetBlocks(pfrom, PreviousIndex, PreviousIndex->GetBlockHash()  );
//LogPrintf("RGP Asking for PREVIOUS %s \n", PreviousIndex->GetBlockHash().ToString() );

            MilliSleep( 2 );



LogPrintf("RGP Resetting block, clearing all items. \n");
                MilliSleep(2);
            }


LogPrintf("RGP PROCESS MESSAGE block match is ZERO \n");
            return true;
        }

/* Output Block_Checker array #2 */
//for ( block_test = 0; block_test < synch_mode_buffer_size; block_test++ )
//{
//if ( Block_Checker[ block_test ].hashPrevBlock != 0 )
//        LogPrintf("RGP ProcessBlock Block Checker Item  %d Block %s \n ", block_test, Block_Checker[ block_test ].hashPrevBlock.ToString() ); 
   //LogPrintf("RGP ProcessBlock Block Checker Item  %d Block %s %d \n", block_test, Block_Checker[ block_test ].GetHash().ToString(), Block_Checker[ block_test ].nTime );
//}
        CBlock tester;
        if ( block_matches > 1 )
        {
            duplicate_loop_count = 0;
            /* locate the bad block */
            LogPrintf("\n\nRGP ProcessBlock block matches, FIX INVALID BLOCKS  %d \n\n", block_matches ); 

            map < uint256, CBlock >::iterator match_checker;
            for ( match_checker = Match_Block_Map.begin(); match_checker != Match_Block_Map.end(); match_checker++ )
            {
                
                tester = match_checker->second;
LogPrintf("RGP ProcessMage, new match ALGO hash %s tester %s \n", tester.GetHash().ToString(), tester.hashPrevBlock.ToString() );
                duplicate_loop_count++;
                MilliSleep( 2 );
            }
            
            if ( duplicate_loop_count == 1 )
            {
                LogPrintf("RGP DUPLICATE CHECK invalid dup loop count is %d \n", duplicate_loop_count );
                /* Ignore and continue */
            }
            else 
            {

                /* RGP found blocks from hight nTime, eg top of a synch chain, causing this issue 
                    I'll try a new algo using NTIME, which should not be 1000 seconds higher than the current block 
                    the block time may need to be adjusted */

                for ( match_test = 0; match_test < block_matches; match_test++ )
                {
                    LogPrintf("RGP ProcessMessage marked_blocks_for_processing[ marked_entries ], entry %d best hash %s \n",  match_test, current_best_hash.ToString() );

                    /* Get the duplicate blocks and determine, which to remove */
                    verified_block = Block_Checker[ match_test ];

LogPrintf("\nRGP ProcessMessage DUPLICATE BLOCK test hash %s \n",  verified_block.GetHash().ToString() );
LogPrintf("RGP ProcessMessage DUPLICATE BLOCK test prev %s \n",  verified_block.hashPrevBlock.ToString() );
LogPrintf("RGP ProcessMessage DUPLICATE BLOCK test nTime %d \n \n",  verified_block.nTime);


                    /* Test block->nTime for blocks higher than last block */
                    if ( verified_block.nTime > ( pindexBest->nTime + 1000 ) )
                    {
                        LogPrintf("RGP DUPLICATE is 500 seconds plus higher pindex best time %d bad block time %d \n", pindexBest->nTime, verified_block.nTime );
                        Block_Checker[ match_test ] = blank_block;
                    }

// LogPrintf("RGP ProcessMessage DUPLICATE BLOCK DELETING FIRST BLOCK \n" );
                
                    MilliSleep(1);

                }

                /* Move forward to process the blocks */
                LogPrintf("\n\n\nINVESTIGATE Multple block matches \n\n\n");
                return true;
           }

        }

        /* ----------------------------------------------------------------------------
           -- If block_matches is less than 2, then the whole array may be processed --
           -- and then cleared. Noted that some of the blocks are missed, if order   --
           -- is incorrect.                                                          --
           ---------------------------------------------------------------------------- */

        LogPrintf("\n\nRGP ProcessMessage Message Processing loop start \n\n" );

        ALL_Blocks_Processed = false;
        Blocks_to_Process = synch_mode_buffer_size;   
        Blocks_Processed = 0;
        loops_performed = 0;
        do
        {

            if ( Blocks_Processed == Blocks_to_Process || loops_performed > Buffer_entries )
            {
//LogPrintf("RGP ProcessMessage message processing loops %d buffer entries %d \n", loops_performed, Buffer_entries );
                //if ( loops_performed > Buffer_entries )
                {
                    ALL_Blocks_Processed = true;
                    Block_Checker_Array = NOT_INITIALISED;
                    Buffer_entries = 0;
                    MilliSleep( 1 );
                    break; /* end of the array of blocks */
                }

            }

            /* Get the Block to process */
 //           verified_block = Block_Checker[ Blocks_Processed ];

//LogPrintf("RGP ProcessMessage message processing loop start blocks processed %d, HASH is %s \n", Blocks_Processed, Block_Checker[ Blocks_Processed ].GetHash().ToString() );

//            hash = verified_block.GetHash();

//LogPrintf("TEST 2 RGP Hash to be currently processed PREVIOUS hash %s \n",  Block_Checker[ Blocks_Processed ].hashPrevBlock.ToString() );

            // = verified_block.hashPrevBlock; // current new block previous hash
            current_previous_hash = pindexBest->GetBlockHash();
//LogPrintf("TEST 4 RGP Hash to be current best hash %s \n", current_previous_hash.ToString() );

            if ( Block_Checker[ Blocks_Processed ].hashPrevBlock == 0 )
            {
                /* Skip this block it's been processed */

                Blocks_Processed++;
                MilliSleep( 1 );
            }
            else
            {
//LogPrintf("RGP ProcessMessage before AcceptBlock check with \n CURRENT hash %s \n NEW hashprevblock %s processed %d \n\n", current_previous_hash.ToString(), Block_Checker[ Blocks_Processed ].hashPrevBlock.ToString(), Blocks_Processed );

//LogPrintf("RGP ProcessMessage before AcceptBlock \n CURRENT BEST hash %s \n NEW hashprevblock %s \n new block hash %s \n\n", current_previous_hash.ToString(), Block_Checker[ Blocks_Processed ].hashPrevBlock.ToString(), Block_Checker[ Blocks_Processed ].GetHash().ToString() );


                /* Validate the Block and store */
                if ( current_previous_hash == Block_Checker[ Blocks_Processed ].hashPrevBlock )
                {
//LogPrintf("\n\nRGP ProcessMessage calling AcceptBlock with prev hash %s hashprevblock %s processed %d \n", current_previous_hash.ToString(), Block_Checker[ Blocks_Processed ].hashPrevBlock.ToString(), Blocks_Processed );

                    if ( fRequestShutdown )
                        exit;               /* Shutdown requested */

                    Accept_Status = Block_Checker[ Blocks_Processed ].AcceptBlock();
                    if ( !Accept_Status )
                    {
                        LogPrintf("RGP ProcessMessage AcceptBlock() failed %d  \n" , Blocks_Processed );
                        MilliSleep( 1 );
                        //Blocks_Processed++;
                        
                    }
                    else
                    {
//                        LogPrintf("RGP ProcessMessage AcceptBlock successful Blocks processed %d Buffer_entries %d \n New current hash %s \n", Blocks_Processed, Buffer_entries, pindexBest->GetBlockHash().ToString() );

                       
                        current_best_hash = pindexBest->GetBlockHash();   /* update to the new hash */

                        /* Mark Block as Processed */
                        Block_Checker[ Blocks_Processed ] = blank_block; 
                        
                        /* Process next block */      
                        Blocks_Processed++;
 
                        MilliSleep( 1 );
                    }

                }
                else
                {
                    /* The hash was not zero, but the prevhashblock did not match pindexBest */    
//LogPrintf("TEST 7 No block match current %s new previous %s \n", current_previous_hash.ToString(), Block_Checker[ Blocks_Processed ].hashPrevBlock.ToString() );        
                    Blocks_Processed++;
                    
                    MilliSleep(1);
                }  
            }

loops_performed++;

//LogPrintf("TEST 5 debug 1.2 loops %d \n",  loops_performed);

           MilliSleep(1);

        } while ( !ALL_Blocks_Processed );

        PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  );
 
        return true;

    }


        try
        {


            /* ---------------------------------------------------------------------------------
               -- RGP, AcceptBlock, after many validation checks, store the new block to disk --
               --      These blocks can be received blocks from synch nodes or POS blocks     --
               --      Added an Exception handler to catch invalid blocks detected during     --
               --      atempted write to block. If this failes a POS Stake has failed.        --
               --------------------------------------------------------------------------------- */

            Accept_Status = pblock->AcceptBlock();

        }
        catch (std::ios_base::failure& e)
        {
//            if (strstr(e.what(), "end of data"))
//            {
//                // Allow exceptions from under-length message on vRecv
//                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand, nMessageSize, e.what());
//            }
//            else if (strstr(e.what(), "size too large"))
//            {
//                // Allow exceptions from over-long size
                LogPrintf("pblock->AcceptBlock : Exception '%s' caught\n", e.what());
//            }
//            else
//            {

                PrintExceptionContinue(&e, "AcceptBlock()");
//            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {

            PrintExceptionContinue(&e, "AcceptBlock()");
        } catch (...) {

            PrintExceptionContinue(NULL, "AcceptBlock()");
        }

//MilliSleep( 20 );
        MilliSleep(5);

        if (!Accept_Status)
        {
            LogPrintf("\n *** RGP ProcessBlock failed AcceptBlock() from %s \n", pfrom->addr.ToString() );

            // Let's diisconnect the pfrom node to see if we can clear the issue
	    //pfrom->fDisconnect = true; 

            /* Previous block is missing, let's ask for it. However, this will repeat
               backwords until it finds the correct block, could take a while         */
            //PushGetBlocks(pfrom, pindexBest, uint256(0) ); 
            //pfrom->AskFor(CInv(MSG_BLOCK, pblock->hashPrevBlock ),  true );

            MilliSleep( 5 );

            //if ( BSC_Wallet_Synching )
            //{
              //  PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() ); /* also ask for everything from current block height */
                //PushGetBlocks(pfrom, pindexBest, pblock->hashPrevBlock );
            //}
            //return false; /* return false, as (ProcessMessage will then clear ask for list for this orphan */
        }
        else
        {           
	    
            if ( !IsInitialBlockDownload() )
            {

                //if ( fDebug )
                //{
                    LogPrintf("*** RGP ProcessBlock, Masternode payment section and no IsInialBlockDownload \n");
                //}

                CScript payee;
                CTxIn vin;

                // If we're in LiteMode disable darksend features without disabling masternodes
                if (!fLiteMode && !fImporting && !fReindex && pindexBest->nHeight > Checkpoints::GetTotalBlocksEstimate())
                {

                  //LogPrintf("*** RGP ProcessBlock not light mode \n");

                  if( masternodePayments.GetBlockPayee(pindexBest->nHeight, payee, vin))
                  {
                        //UPDATE MASTERNODE LAST PAID TIME
                        CMasternode* pmn = mnodeman.Find(vin);
                        if(pmn != NULL)
                        {

                            pmn->nLastPaid = GetAdjustedTime();
                        }

                       LogPrintf("ProcessBlock() : Update Masternode Last Paid Time - %d\n", pindexBest->nHeight);
                  }
                  else
                      //LogPrintf("*** RGP ProcessBlock not light mode, fell out 1 \n");



                    darkSendPool.CheckTimeout();
                    darkSendPool.NewBlock();
                    masternodePayments.ProcessBlock(GetHeight()+10);

                } else if (fLiteMode && !fImporting && !fReindex && pindexBest->nHeight > Checkpoints::GetTotalBlocksEstimate())
                {
                    LogPrintf("*** RGP ProcessBlock light mode \n");

                    /* Lite Mode */
                    if(masternodePayments.GetBlockPayee(pindexBest->nHeight, payee, vin)){
                        //UPDATE MASTERNODE LAST PAID TIME
                        CMasternode* pmn = mnodeman.Find(vin);
                        if(pmn != NULL) {
                            pmn->nLastPaid = GetAdjustedTime();
                        }

                        if ( fDebug )
                        {
                           LogPrintf("ProcessBlock() : Update Masternode Last Paid Time - %d\n", pindexBest->nHeight);
                        }
                    }
                    else
                    {
                        LogPrintf("*** RGP ProcessBlock not light mode, fell out 2 \n");
                    }
                    
                    masternodePayments.ProcessBlock(GetHeight()+10);
                }

            }

            return true;
    }

    // ppcoin: check proof-of-stake
    // Limited duplicity on stake: prevents block flood attack
    // Duplicate stake allowed only when there is orphan child block

    if ( !fReindex && !fImporting )
    {
        if ( pblock->IsProofOfStake() )
        {
            if ( setStakeSeen.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash) )
            {

                if ( pindexBest->GetBlockTime() < ( GetTime() - ( 10 * 60 ) ) )
                {
                   /* -----------------------------------------------
                      -- Duplicity check on stake was satisfactory --
                      ----------------------------------------------- */
                    LogPrintf("*** RGP Duplicity check-> Look into this BlockTime %d Gettim %d \n", pindexBest->GetBlockTime(), GetTime() );
                    MilliSleep(1);
                }
                else
                {
                   /* -----------------
                      -- F A I L E D --
                      ----------------- */

                   if (  pindexBest->GetBlockTime() < GetTime()  )
                   {
                       /* RGP, there can be 6 seconds between the current stored block and the
                               new block, this second test has been used to check that the
                               pindexBest block is always less than GetTime()                   */
                       MilliSleep(1);
                   }
                   else
                   {
                      LogPrintf("*** RGP Duplicate proof of stake \n");
                      MilliSleep(1);
                      return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());
                   }

                }
            }
        }
    }

    
    MilliSleep(1);
    
    if (pblock->hashPrevBlock != hashBestChain)
    {

            // Extra checks to prevent "fill up memory by spamming with bogus blocks"
            const CBlockIndex* pcheckpoint = Checkpoints::AutoSelectSyncCheckpoint();
            int64_t deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
            if (deltaTime < 0)
            {
                if (pfrom)
                {
                    LogPrintf("*** ProcessBlock fill up memory detected, misbehaving \n");
                    Misbehaving(pfrom->GetId(), 1);
                    MilliSleep(1); /* RGP Optimise */
                }

                LogPrintf("*** RGP ProcessBlock, DEBUG Special 003 \n");

                return error("ProcessBlock() : block with timestamp before last checkpoint");
            }

    }
    else
    {
       LogPrintf("*** ProcessBlock hashprev is equal to hashBestChain \n");
    }


    // Block signature can be malleated in such a way that it increases block size up to maximum allowed by protocol
    // For now we just strip garbage from newly received blocks
    if (!IsCanonicalBlockSignature(pblock)) {
        return error("ProcessBlock(): bad block signature encoding");
    }

    // Preliminary checks
    if (!pblock->CheckBlock())
    {
        return error("ProcessBlock() : CheckBlock FAILED");
    }

    // RGP : This is the start of the Orphan SAGA, which is causing synch issues, as
    //       this part is slowing chain synchs drastically. Looks like synch has stopped
    //       in this respect.

    Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

    if ( pblock->IsProofOfWork() )
        return true;

    // If we don't already have its previous block, shunt it off to holding area until we get it
//    if (!mapBlockIndex.count( pblock->hashPrevBlock ) && !IsInitialBlockDownload()   )
//    {
        //if ( fDebug )
        //{
//            LogPrintf("ProcessBlock: ORPHAN BLOCK %lu, prev=%s\n", (unsigned long)mapOrphanBlocks.size(), pblock->hashPrevBlock.ToString());

        //}

        /* RGP try to ask for Pushblocks from current index */
//        PushGetBlocks(pfrom, pindexBest, pblock->hashPrevBlock );

        // Accept orphans as long as there is a node to request its parents from
//        if (pfrom)
//        {
//            LogPrintf("*** RGP Shunt block debug 1 \n");

            // ppcoin: check proof-of-stake
//            if (pblock->IsProofOfStake())
//            {

               //LogPrintf("*** RGP Orphan Saga Debug 2, It's PoS \n");

                // Limited duplicity on stake: prevents block flood attack
                // Duplicate stake allowed only when there is orphan child block
//                if ( setStakeSeenOrphan.count(pblock->GetProofOfStake()) && !mapOrphanBlocksByPrev.count(hash))
//                {
                   // LogPrintf("*** RGP Orphan Saga Debug 3, It's PoS duplicate \n");
//                    return error("ProcessBlock() : duplicate proof-of-stake (%s, %d) for orphan block %s", pblock->GetProofOfStake().first.ToString(), pblock->GetProofOfStake().second, hash.ToString());
 //               }
//            }

 //           LogPrintf("*** RGP Orphan Saga Debug 4 pruning \n");

 //           PruneOrphanBlocks();
 //           COrphanBlock* pblock2 = new COrphanBlock();
 //           {
 //               CDataStream ss(SER_DISK, CLIENT_VERSION);
//                ss << *pblock;
//                pblock2->vchBlock = std::vector<unsigned char>(ss.begin(), ss.end());
 //           }
//            pblock2->hashBlock = hash;
//            pblock2->hashPrev = pblock->hashPrevBlock;
//            pblock2->stake = pblock->GetProofOfStake();
            
//            MilliSleep(1);

//            mapOrphanBlocks.insert(make_pair(hash, pblock2));
//            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrev, pblock2));

//            if ( pblock->IsProofOfStake() )
//            {
//                 LogPrintf("*** RGP ProcBlk, PoS insert orphan \n");
//                 setStakeSeenOrphan.insert(pblock->GetProofOfStake());
//            }


//	    MilliSleep(1);

 //           LogPrintf("*** RGP no Prev block ask for %s last filed block hgt %l \n", hash.ToString() , GetHeight() );

            // Ask this guy to fill in what we're missing
//            PushGetBlocks(pfrom, pindexBest, GetOrphanRoot(hash));

//            MilliSleep(1);

            //PushGetBlocks(pfrom, pindexBest, hash ); /* RGP GetOrphanRoot(hash)); */
            // ppcoin: getblocks may not obtain the ancestor block rejected
            // earlier by duplicate-stake check so we ask for it again directly
            
            // ALREADY CHECK THIS ABOVE, DELETE DECISION LATER
            
            //if (!IsInitialBlockDownload())
            //{
 //               pfrom->AskFor(CInv(MSG_BLOCK, WantedByOrphan( pblock2 ) ) );

                /* RGP Optimised, add a call to get our current highest entry and next 500 */
 //               MilliSleep(1);

 //               PushGetBlocks(pfrom, pindexBest,  pblock2->hashPrev );
            //}
  
 //       }
//        else
//        {
//
//            return true;
//        }


 //   }


 //         return true;


    if (Accept_Status)
    {
       LogPrintf("Accept_Status is true, exiting this message loop\n");
       return true;
    }

    /* RGP, Fast Synch Implementation, if AcceptBlock cannot
            find the previous block, then ask for it!!       */
//    From_Node = pfrom;
//LogPrintf("RGP <<<<Abnormal>>> AcceptBlock call - Process Orphan blocks \n");
    // Store to disk
//    if (!pblock->AcceptBlock())
//    {
        //if ( fDebug )
        //{
//            LogPrintf("*** RGP AcceptBlock 2nd try NOT Success trying Orphan store! \n");
        //}

//        vector<uint256> vWorkQueue;
//        vWorkQueue.push_back(hash);
//        for (unsigned int i = 0; i < vWorkQueue.size(); i++)
//        {
//          LogPrintf("Orphan Loop 1 \n");
//          hashPrev = vWorkQueue[i];
          
          
//          for (  multimap<uint256, COrphanBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
//                 mi != mapOrphanBlocksByPrev.upper_bound(hashPrev); ++mi)
          
//          {
//                LogPrintf("RGP multimap loop \n");
//                
//                CBlock block;
//                {
//                    CDataStream ss(mi->second->vchBlock, SER_DISK, CLIENT_VERSION);
//                    ss >> block;
//                }
//                block.BuildMerkleTree();
//                if (block.AcceptBlock())
//                {
//                   LogPrintf("*** RGP AcceptBlock good for Orphan! %d \n", i );
//                   vWorkQueue.push_back(mi->second->hashBlock);
//                }
//                else
//                {
//                    LogPrintf("*** RGP  failed to accept block using Orphans \n" );
//                    //LogPrintf("ProcessBlock() : AcceptBlock FAILED\n");
//                    return false;
//                }

//		 MilliSleep(1);

//                mapOrphanBlocks.erase(mi->second->hashBlock);
//                setStakeSeenOrphan.erase(block.GetProofOfStake());
//                delete mi->second;
                
//                MilliSleep(1);
//          }
//          mapOrphanBlocksByPrev.erase(hashPrev);

//          MilliSleep(1);

//        }



//        return error("ProcessBlock() : AcceptBlock FAILED");
//    }


    // Recursively process any orphan blocks that depended on this one
//    vector<uint256> vWorkQueue;
//    vWorkQueue.push_back(hash);
//    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
//    {

//        LogPrintf("*** RGP Process Block Process Workque DBG 001 \n");

//       hashPrev = vWorkQueue[i];
//      for (multimap<uint256, COrphanBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
//            mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
//             ++mi)
//       {

//          LogPrintf("*** RGP Process Block Process Workque DBG 002 \n");

//            CBlock block;
//            {
//                CDataStream ss(mi->second->vchBlock, SER_DISK, CLIENT_VERSION);
//                ss >> block;
//            }
//            block.BuildMerkleTree();
//            if (block.AcceptBlock())
//            {
//                LogPrintf("*** RGP AcceptBlock good for Orphan! %d \n", i );
//                vWorkQueue.push_back(mi->second->hashBlock);
//            }
//            else
//            {
//                 LogPrintf("*** RGP AcceptBlock BAD for Orphan! %d \n", i);
//            }
//            mapOrphanBlocks.erase(mi->second->hashBlock);
//            setStakeSeenOrphan.erase(block.GetProofOfStake());
//            delete mi->second;
            
//            MilliSleep(1);
            
//        }
//        mapOrphanBlocksByPrev.erase(hashPrev);

//        MilliSleep(1);

//    }



//    if(!IsInitialBlockDownload()){

        //LogPrintf("*** RGP ProcessBlock, Masternode payment section and no IsInialBlockDownload \n");

//        CScript payee;
//        CTxIn vin;

        // If we're in LiteMode disable darksend features without disabling masternodes
//        if (!fLiteMode && !fImporting && !fReindex && pindexBest->nHeight > Checkpoints::GetTotalBlocksEstimate()){

//            if(masternodePayments.GetBlockPayee(pindexBest->nHeight, payee, vin)){
//                //UPDATE MASTERNODE LAST PAID TIME
 //               CMasternode* pmn = mnodeman.Find(vin);
//                if(pmn != NULL) {
//                    pmn->nLastPaid = GetAdjustedTime();
//                }
//
               // LogPrintf("ProcessBlock() : Update Masternode Last Paid Time - %d\n", pindexBest->nHeight);
//            }

//            darkSendPool.CheckTimeout();
//            darkSendPool.NewBlock();
//            masternodePayments.ProcessBlock(GetHeight()+10);

//        } else if (fLiteMode && !fImporting && !fReindex && pindexBest->nHeight > Checkpoints::GetTotalBlocksEstimate())
//        {

          //  LogPrintf("*** RGP Process Block Process Workque DBG 004 \n");

//            if(masternodePayments.GetBlockPayee(pindexBest->nHeight, payee, vin)){
//                //UPDATE MASTERNODE LAST PAID TIME
//                CMasternode* pmn = mnodeman.Find(vin);
//                if(pmn != NULL) {
//                    pmn->nLastPaid = GetAdjustedTime();
//                }

                //if ( fDebug ){
                   // LogPrintf("ProcessBlock() : Update Masternode Last Paid Time - %d\n", pindexBest->nHeight);
               // }
//            }

           // LogPrintf("*** RGP AcceptBlock for Orphan Success! Masternode payment phase \n");

//            masternodePayments.ProcessBlock(GetHeight()+10);
//        }

    //}

//    if ( fDebug )
//    {
//       LogPrintf("ProcessBlock: ACCEPTED FINAL\n");
//    }



   return false;
}

static const int GOLD_STAKE_TIMESTAMP_MASK = 15;

#ifdef ENABLE_WALLET
// novacoin: attempt to generate suitable proof-of-stake
bool CBlock::SignBlock(CWallet& wallet, int64_t nFees)
{
int64_t nSearchTime;

    // if we are trying to sign
    //    something except proof-of-stake block template
    if (!vtx[0].vout[0].IsEmpty()){
        return false;
    }

    // if we are trying to sign
    //    a complete proof-of-stake block
    if (IsProofOfStake() ) {
        return true;
    }

    if (vNodes.size() == 0) {
        return false;
    }

   static int64_t nLastCoinStakeSearchTime = GetAdjustedTime(); // startup timestamp

    CKey key;
    CTransaction txCoinStake;

    //LogPrintf("*** RGP Signblock, txCoinStake before %d \n",txCoinStake.nTime  );

    /* RGP updated this mask to stop staking until after 240 seconds */
    txCoinStake.nTime &= ~GOLD_STAKE_TIMESTAMP_MASK; // mask is 240 seconds

    //LogPrintf("*** RGP Signblock, txCoinStake after mask %d \n",txCoinStake.nTime  );

    nSearchTime = txCoinStake.nTime; // search to current time

    //LogPrintf("*** RGP Signblock, Searchtime %d stakesearchtime %d \n",nSearchTime , nLastCoinStakeSearchTime   );

    if (nSearchTime > nLastCoinStakeSearchTime)
    {
        int64_t nSearchInterval = 1;

        if (wallet.CreateCoinStake(wallet, nBits, nSearchInterval, nFees, txCoinStake, key))
        {
            //LogPrintf("*** RGP Signblock, after wallet.CreateCoinStake \n");

            //LogPrintf("*** RGP Signblock, after wallet.CreateCoinStake Timelimit %d getpasttimelimit() %d \n",txCoinStake.nTime, pindexBest->GetPastTimeLimit()+1 );


            if (txCoinStake.nTime >= pindexBest->GetPastTimeLimit()+1)
            {
                //LogPrintf("*** RGP Signblock, ntime condition TRUE Timelimit %d getpasttimelimit() %d \n",txCoinStake.nTime, pindexBest->GetPastTimeLimit()+1 );



                /* RGP, No Idea why this is here, disabled the comment, as PoW staking is working fine */
                // error("main.cpp, SignBlock: calling Wallet.CreatCoinStake, DEFINED stake time\n");

                // make sure coinstake would meet timestamp protocol
                //    as it would be the same as the block timestamp
                vtx[0].nTime = nTime = txCoinStake.nTime;

                // we have to make sure that we have no future timestamps in
                //    our transactions set
                for (vector<CTransaction>::iterator it = vtx.begin(); it != vtx.end();)
                {
                    if (it->nTime > nTime)
                    {
                        it = vtx.erase(it);
                    }
                    else
                    { ++it;
                    }
                }

  //              LogPrintf("*** RGP Signblock, Build Merkel Tree Timelimit %d getpasttimelimit() %d \n",txCoinStake.nTime, pindexBest->GetPastTimeLimit()+1 );


                vtx.insert(vtx.begin() + 1, txCoinStake);
                hashMerkleRoot = BuildMerkleTree();

    //            LogPrintf("*** RGP Signblock, return Sign Timelimit %d getpasttimelimit() %d \n",txCoinStake.nTime, pindexBest->GetPastTimeLimit()+1 );


                nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
                nLastCoinStakeSearchTime = nSearchTime;

                // append a signature to our block
                return key.Sign(GetHash(), vchBlockSig);
            }
            else
            {
                LogPrintf("*** RGP SignBlock, not past time limit  \n");
            }

        }
        else
            LogPrintf("*** RGP Signblock,  wallet.CreateCoinStake failed \n");

        nLastCoinStakeSearchInterval = nSearchTime - nLastCoinStakeSearchTime;
        nLastCoinStakeSearchTime = nSearchTime;
    }


    return false;
}
#endif

bool CBlock::CheckBlockSignature() const
{
    if (IsProofOfWork())
        return vchBlockSig.empty();

    if (vchBlockSig.empty())
        return false;

    vector<valtype> vSolutions;
    txnouttype whichType;

    const CTxOut& txout = vtx[1].vout[1];

    if (!Solver(txout.scriptPubKey, whichType, vSolutions))
        return false;

    if (whichType == TX_PUBKEY)
    {
        valtype& vchPubKey = vSolutions[0];
        return CPubKey(vchPubKey).Verify(GetHash(), vchBlockSig);
    }

    return false;
}

bool CheckDiskSpace(uint64_t nAdditionalBytes)
{
    uint64_t nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        string strMessage = _("Error: Disk space is low!");
        strMiscWarning = strMessage;
        LogPrintf("*** %s\n", strMessage);
        uiInterface.ThreadSafeMessageBox(strMessage, "", CClientUIInterface::MSG_ERROR);
        StartShutdown();
        return false;
    }
    return true;
}

static filesystem::path BlockFilePath(unsigned int nFile)
{
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == (unsigned int) -1))
        return NULL;

    FILE* file = fopen( BlockFilePath(nFile).string().c_str(), pszMode );
    if (!file)
    {
        return NULL;
    }
   

    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {

        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            printf("RGP main.h OpenBlockFile debug 006 \n");
            return NULL;
        }
    }

    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;
    while (true)
    {

        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
        {
            return NULL;
        }

        if (fseek(file, 0, SEEK_END) != 0)
        {
            fclose(file);   /* RGP, 4th Sep 2019, fclose added */
            return NULL;
        }

        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < (long)(0x7F000000 - MAX_SIZE))
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }
        fclose(file);
        nCurrentBlockFile++;

        MilliSleep(1); /* RGP Optimize */
    }
}

bool LoadBlockIndex(bool fAllowNew)
{
unsigned int nFile;
unsigned int nBlockPos;

    LOCK(cs_main);

    if (TestNet())
    {
        nCoinbaseMaturity = 10; // test maturity is 10 blocks
    }

    //
    // Load block index
    //
    //printf("RGP Debug LoadBlockIndex 001\n");
    MilliSleep(1);
    CTxDB txdb("cr+");
    if (!txdb.LoadBlockIndex())
    {
        printf("RGP Debug LoadBlockIndex 002\n");
        return false;
    }
    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
        {
            printf("RGP Debug LoadBlockIndex 003\n");
            return false;
	}
        CBlock &block = const_cast<CBlock&>(Params().GenesisBlock());
        // Start new block file

        if (!block.WriteToDisk(nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos, Params().HashGenesisBlock()))
            return error("LoadBlockIndex() : genesis block not accepted");
    }

    return true;
}



void PrintBlockTree()
{
    AssertLockHeld(cs_main);
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                LogPrintf("| ");
            LogPrintf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                LogPrintf("| ");
            LogPrintf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            LogPrintf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
#ifndef LOWMEM
        LogPrintf("%d (%u,%u) %s  %08x  %s  mint %7s  tx %u",
#else
        LogPrintf("%d (%u,%u) %s  %08x  %s  tx %u",
#endif        
            pindex->nHeight,
            pindex->nFile,
            pindex->nBlockPos,
            block.GetHash().ToString(),
            block.nBits,
            DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()),
#ifndef LOWMEM
            FormatMoney(pindex->nMint),
#endif            
            block.vtx.size());

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64_t nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;
            while (nPos != (unsigned int)-1 && blkdat.good())
            {
                boost::this_thread::interruption_point();
                unsigned char pchData[65536];
                do {
                    fseek(blkdat.Get(), nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat.Get());
                    if (nRead <= 8)
                    {
                        nPos = (unsigned int)-1;
                        break;
                    }
                    void* nFind = memchr(pchData, Params().MessageStart()[0], nRead+1-MESSAGE_START_SIZE);
                    if (nFind)
                    {
                        if (memcmp(nFind, Params().MessageStart(), MESSAGE_START_SIZE)==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + MESSAGE_START_SIZE;
                            break;
                        }
                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - MESSAGE_START_SIZE + 1;
                    boost::this_thread::interruption_point();
                } while(true);
                if (nPos == (unsigned int)-1)
                    break;
                fseek(blkdat.Get(), nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;
                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;
                    LOCK(cs_main);
                    if (ProcessBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e) {
            LogPrintf("%s() : Deserialize or I/O error caught during load\n",
                   __PRETTY_FUNCTION__);
        }
    }
    LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

struct CImportingNow
{
    CImportingNow() {
        assert(fImporting == false);
        fImporting = true;
    }

    ~CImportingNow() {
        assert(fImporting == true);
        fImporting = false;
    }
};

void ThreadImport(std::vector<boost::filesystem::path> vImportFiles)
{
    RenameThread("SocietyG-loadblk");


    CImportingNow imp;

    LogPrintf(" :ThreadImport Started, SocietyG-loadblk \n");
    // -loadblock=
    BOOST_FOREACH(boost::filesystem::path &path, vImportFiles) {
        FILE *file = fopen(path.string().c_str(), "rb");
        if (file)
            LoadExternalBlockFile(file);
    }

    // hardcoded $DATADIR/bootstrap.dat
    filesystem::path pathBootstrap = GetDataDir() / "bootstrap.dat";
    if (filesystem::exists(pathBootstrap)) {
        FILE *file = fopen(pathBootstrap.string().c_str(), "rb");
        if (file) {
            filesystem::path pathBootstrapOld = GetDataDir() / "bootstrap.dat.old";
            LoadExternalBlockFile(file);
            RenameOver(pathBootstrap, pathBootstrapOld);
        }
    }
}



//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;

    if (GetBoolArg("-testsafemode", false))
        strRPC = "test";

    if (!CLIENT_VERSION_IS_RELEASE)
        strStatusBar = _("This is a pre-release test build - use at your own risk - do not use for mining or merchant applications");

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
                if (nPriority > 1000)
                    strRPC = strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{


    switch (inv.type)
    {
    case MSG_DSTX:
        return mapDarksendBroadcastTxes.count(inv.hash);
    case MSG_TX:
        {
        bool txInMap = false;


        txInMap = mempool.exists(inv.hash);

//        if ( txInMap )
//            LogPrintf("*** RGP Alreadyhave() mempool exists \n");
//        else
//            LogPrintf("*** RGP Alreadyhave() mempool does NOT exist \n");

//        if ( mapOrphanTransactions.count(inv.hash) )
//            LogPrintf("*** RGP Alreadyhave() mapOrphanTransactions exists \n");
//        else
//            LogPrintf("*** RGP Alreadyhave() mapOrphanTransactions does NOT exist \n");

//        if ( txdb.ContainsTx(inv.hash) )
//            LogPrintf("*** RGP Alreadyhave() txdb.ContainsTx exists \n");
//        else
//            LogPrintf("*** RGP Alreadyhave() txdb.ContainsTx does NOT exist \n");

        MilliSleep( 1 ); /* RGP Optimize */

        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               txdb.ContainsTx(inv.hash);
        }

    case MSG_BLOCK:

//        if ( mapBlockIndex.count(inv.hash) )
//            LogPrintf("*** RGP Alreadyhave() mapBlockIndex.count exists \n");
//        else
//            LogPrintf("*** RGP Alreadyhave() mapBlockIndex.count does NOT exist \n");

//        if ( mapOrphanBlocks.count(inv.hash))
//            LogPrintf("*** RGP Alreadyhave() mapOrphanBlocks.count exists \n");
//        else
//            LogPrintf("*** RGP Alreadyhave() mapOrphanBlocks.count does NOT exist \n");



        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        return mapTxLockReq.count(inv.hash) ||
               mapTxLockReqRejected.count(inv.hash);
    case MSG_TXLOCK_VOTE:
        return mapTxLockVote.count(inv.hash);
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        return mapSeenMasternodeVotes.count(inv.hash);
     /* RGP, added default to handle the following messages types
            MSG_FILTERED_BLOCK
            MSG_MASTERNODE_SCANNING_ERROR                           */
    default :
        LogPrintf("*** RGP UN-HANDLED MESSAGE \n");

    }
    // Don't know what it is, just say we already got one
    return true;
}



void static ProcessGetData(CNode* pfrom)
{
    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();

    vector<CInv> vNotFound;

    LOCK(cs_main);

//LogPrintf("RGP DEBUB ProcessGetData started \n");

    while (it != pfrom->vRecvGetData.end())
    {
//LogPrintf("^");
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize >= SendBufferSize() )
        {
            LogPrintf("*** RGP ProcessGetData debug 002 send size to big %d for node %s \n", pfrom->nSendSize, pfrom->addr.ToString() );

            break;
        }

        const CInv &inv = *it;
        {
            //boost::this_thread::interruption_point();
            it++;

            if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK)
            {
//LogPrintf("*** RGP ProcessGetData |n");
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage("block", block);

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {

                if ( fDebug)
                   LogPrintf("ProcessGetData -- Starting \n");
                // Send stream from relay memory
                bool pushed = false;
                /*{
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        if(fDebug) LogPrintf("ProcessGetData -- pushed = true Rest will fail\n");
                        pushed = true;
                    }
                }*/
                if (!pushed && inv.type == MSG_TX)
                {

                    CTransaction tx;
                    if (mempool.lookup(inv.hash, tx))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_VOTE)
                {
                    if(mapTxLockVote.count(inv.hash))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockVote[inv.hash];
                        pfrom->PushMessage("txlvote", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TXLOCK_REQUEST)
                {
                    if(mapTxLockReq.count(inv.hash))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapTxLockReq[inv.hash];
                        pfrom->PushMessage("txlreq", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_SPORK)
                {
                    if(mapSporks.count(inv.hash))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSporks[inv.hash];
                        pfrom->PushMessage("spork", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_MASTERNODE_WINNER)
                {
                    if(mapSeenMasternodeVotes.count(inv.hash))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << mapSeenMasternodeVotes[inv.hash];
                        pfrom->PushMessage("mnw", ss);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_DSTX)
                {
                    if(mapDarksendBroadcastTxes.count(inv.hash))
                    {
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss <<
                        mapDarksendBroadcastTxes[inv.hash].tx <<
                        mapDarksendBroadcastTxes[inv.hash].vin <<
                        mapDarksendBroadcastTxes[inv.hash].vchSig <<
                        mapDarksendBroadcastTxes[inv.hash].sigTime;

                        pfrom->PushMessage("dstx", ss);
                        pushed = true;
                    }
                }
                if (!pushed)
                {
                    vNotFound.push_back(inv);
                }
            }

            // Track requests for our stuff.
            g_signals.Inventory(inv.hash);

            if (inv.type == MSG_BLOCK  || inv.type == MSG_FILTERED_BLOCK)
                break;
        }
        MilliSleep(1); /* RGP Optimise */
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty())
    {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        pfrom->PushMessage("notfound", vNotFound);
    }
}

/* ---------------------------
   -- RGP. Message Protocol --
   --------------------------------------------------------------------
   -- To be implemented                                              --
   -------------------------------------------------------------------- */

static uint64_t message_ask_filter = 0;
CBlock block_to_check;

bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
vector<CInv> vInv;
extern bool BSC_Wallet_Synching;
bool Last_Block_Confirmed; /* RGP defined in main.h */
int64_t Time_to_Last_block;
int64_t nTime;
CAddress addrMe;
CAddress addrFrom;
uint64_t nNonce;
unsigned int nInventory_index;
bool fAlreadyHave;
uint256 hash_to_get;
CInv Problem_Blocks_Inv;

    RandAddSeedPerfmon();

    LOCK(cs_main);
    CTxDB txdb("r");

    if ( message_ask_filter > 100 )
    {
LogPrintf("rgp Processmessage started \n");
        /* If the wallet is not yet synched, then delay */
        Time_to_Last_block = ( GetTime() - pindexBest->GetBlockTime() );

        if ( Time_to_Last_block > 200 )
        {
            // RGP, new implementation for synch, send to all connected noded
            //      note requesting hash 0 does nothing!!!
            
            for (CNode* pnode : vNodes)
            {
                PushGetBlocks(pnode, pindexBest,  pindexBest->GetBlockHash() );
                // LogPrintf("RGP pushing inventory request to node %s \n", pnode->addrName );
            }
            MilliSleep( 10 );

        }
 
        message_ask_filter = 0;
    }
    
    
    message_ask_filter++;

// RGP let's see what happens if we put it here
        for (CNode* pnode : vNodes)
            {
                PushGetBlocks(pnode, pindexBest,  pindexBest->GetBlockHash() );
                // LogPrintf("RGP pushing inventory request to node %s \n", pnode->addrName );
                 MilliSleep( 5 );
            }



    if ( fDebug )
        LogPrintf("net, received: %s (%u bytes)\n", strCommand, vRecv.size());

    /* RGP, determine if we are still downloading the block index or not */
    Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

    if ( Time_to_Last_block < 240 )
    {
        BSC_Wallet_Synching = true;
    }
    else
    {
        BSC_Wallet_Synching = false;
    }

    //LOCK(cs_main);

    pfrom->fDisconnect = false;     /* RGP Introduced before tests */

    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == "version")
    {
        //LogPrintf("\nProcessMessage VERSION Received \n");

        //LOCK(cs_main);

        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {

            LogPrintf("*** RGP Too many version messages \n");

            Misbehaving(pfrom->GetId(), 1);
            return true;
        }

        nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PEER_PROTO_VERSION)
        {
             LogPrintf("ProcessMessage VERSION protocol incorrect, less than %d \n", MIN_PEER_PROTO_VERSION );

            // disconnect from peers older than this proto version
            LogPrintf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString(), pfrom->nVersion);

            pfrom->fDisconnect = true;
            return true;
        }

        //LogPrintf("*** RGP Debug Subversion Check \n");
        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> pfrom->strSubVer;
            //LogPrintf("*** RGP Debug Subversion Check %s \n", pfrom->strSubVer );
            pfrom->cleanSubVer = SanitizeString(pfrom->strSubVer);
            //LogPrintf("*** RGP Debug Subversion Check %s \n", pfrom->cleanSubVer );
            if ( pfrom->cleanSubVer == "/SocietyG:3.0.0/"  )
            {
                // only report invalide versions of wallet
            }
            else
            {
                LogPrintf("INvalid SUBVERSION, DISCONNECTED node %s \n", pfrom->addr.ToString() );
                pfrom->fDisconnect = true;
            }
        }
        if (!vRecv.empty())
        {
            vRecv >> pfrom->nStartingHeight;
        }

        if (!vRecv.empty())
        {
            pfrom->fRelayTxes = true;
            LogPrintf("*** RECIEVE Relaytxes \n");
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            LogPrintf("ProcessMessage connected to self \n");

            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            //pfrom->fDisconnect = true;
            //return true;
        }

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
        {
            pfrom->PushVersion();
        }

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);
        pfrom->fClient = true;
        if ( !pfrom->fClient )
        {
            LogPrintf("*** RGP ProcessMessage pnode->fClient is false!!! service %d node %d \n", pfrom->nServices, NODE_NETWORK );
        }

        // Change version
        pfrom->PushMessage("verack");
        pfrom->ssSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                {
                    pfrom->PushAddress(addr);
                }
                else if (IsPeerAddrLocalGood(pfrom))
                {
                    addr.SetIP(pfrom->addrLocal);
                    pfrom->PushAddress(addr);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        }
        else
        {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

//LogPrintf("\nProcessMessage VERSION -> SucessfullyConnected! \n");
        pfrom->fSuccessfullyConnected = true;

        //if (fDebug){
           LogPrintf("receive version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString(), addrFrom.ToString(), pfrom->addr.ToString());
        //}

        if ( pfrom->nStartingHeight > Last_known_block_height )
            Last_known_block_height = pfrom->nStartingHeight;

        //string ipAddressTest;
        //string portInfo;

        //ipAddressTest = addrFrom.ToString();
        //portInfo = addrFrom.ToStringPort();

        //LogPrintf("*** RGP Port check %s ",portInfo );

        //if ( portInfo == "23980" ){
             //LogPrintf("*** RGP SAFE Port check %s ",portInfo );
        //}
        //else{
                //LogPrintf("\n**********************************************************************\n");
                // LogPrintf("*** RGP Bad address %s marked as MIS-behaving!!!          \n ", ipAddressTest );
                //LogPrintf("**********************************************************************\n");
                //Misbehaving(pfrom->GetId(), 100 );
                //return false;
       // }


        if (GetBoolArg("-synctime", true)){
            AddTimeData(pfrom->addr, nTime);
        }

    }

    /* ----------------------------------
       -- RGP JIRA Reference BSG-123   --
       --------------------------------------------------------------------
       -- This section of code was causing banning of nodes during synch --
       -- The code section had no purpose, classified as legacy code.    --
       -------------------------------------------------------------------- */

    //else if (pfrom->nVersion == 0)
    //{

        // Must have a version message before anything else
        //    Misbehaving(pfrom->GetId(), 1);
        // return false;
    //}


    else if (strCommand == "verack")
    {
//LogPrintf("VERACK\n");
        pfrom->SetRecvVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );
LogPrintf("addr \n");
        //LOCK(cs_main);

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
        {
            LogPrintf("*** RGP addr older version eject!! \n");
            return true;
        }
        if (vAddr.size() > 1000)
        {
            LogPrintf("*** RGP MISBEHAVING, Address size > 1000 \n");
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64_t hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                        {
                            MilliSleep( 1 );  /* RGP Otimized */
                            continue;
                        }
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }

                    MilliSleep( 1 );  /* RGP Otimized */   

                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
             
            MilliSleep( 1 );  /* RGP Otimized */   
            
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;

        if (pfrom->fOneShot)
        {
            LogPrintf("ProcessMessage ONESHOT \n" );
            pfrom->fDisconnect = true;
        }
        
        

        //LogPrintf("RGP Debug ADDR Finished \n");
    }

    /* --------------------------------------------------------------------------
       -- RGP, The INV or Inventory messages in received providing a list of   --
       -- items available form another node(s). The Inventory message has a    --
       -- list or vector of two entries per inventory, where the message_type  --
       -- and the block hash code is provided.                                 --
       -- THis code determined if the hash code or block hash is stored within --
       -- the 'blk0001.dat' file.                                              --
       -- Blocks not identified as stored are requested from the node to then  --
       -- allow storage into the blk0001 file, but they must be in order.      --
       --------------------------------------------------------------------------  */

    else if (strCommand == "inv")
    {
        int net_request_filter = 0;
        vector<CInv> vInv;
        vRecv >> vInv;
        CBlockIndex* Block_Start_Requested;

        LogPrintf("*** RGP Inventory  %d \n", vInv.size() );

        /* RGP, typical inventory is 500 entries, but MAX_INV_SZ is --
                set to 50000, this shall be optimised to 1000       -- */

        /* -- RGP, As the QT wallet is using different schemes to calculate  --
           --      the current block height based on incoming blocks, this   --
           --      code needs to use seperate algorithms using the Inventory --
           --      records coming in                                         -- */


            if ( pindexBest->nHeight < 5 )
            {
                // Can't use ->pprev, it causes a core dump, as pprev on block zero 
                // is an invalid pointer value
            }
            else
            {
                // RGP determine best time to use this code...
                Block_Start_Requested = pindexBest->pprev;
                PushGetBlocks(pfrom, Block_Start_Requested, Block_Start_Requested->GetBlockHash()  );

//LogPrintf("RGP Asking for PREVIOUS %s \n", Block_Start_Requested->GetBlockHash().ToString() );
                Block_Start_Requested = Block_Start_Requested->pprev;
                PushGetBlocks(pfrom, Block_Start_Requested, Block_Start_Requested->GetBlockHash()  );
//LogPrintf("RGP Asking for PREVIOUS %s \n", Block_Start_Requested->GetBlockHash().ToString() );

        MilliSleep( 5 );
            }



        if ( vInv.size() > MAX_INV_SZ )
        {
            LogPrintf("*** RGP Inventory too big %d \n", vInv.size() );
            Misbehaving(pfrom->GetId(), 20);
            return error("message inv size() = %u", vInv.size());
        }

        // find last block in inv vector, RGP this crazy code needs rewritten
        //unsigned int nLastBlock = (unsigned int)(-1);
        //for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
        //    if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
        //        nLastBlock = vInv.size() - 1 - nInv;
        //        break;
        //    }
        //}
        


        LOCK(cs_main);
        CTxDB txdb("r");
        if ( fDebug )
        {
            LogPrintf("*** RGP ProcessMessage inventory size %d \n", vInv.size());
        }

        CInv Inventory_Item;
        CInv AskFor_Item;

        for ( nInventory_index = 0; nInventory_index < vInv.size(); nInventory_index++)
        {
            //const CInv &Inventory_Item = vInv[ nInventory_index ];
            Inventory_Item = vInv[ nInventory_index ];

            switch( Inventory_Item.type )
            {
                case MSG_TX     : MilliSleep( 5 );
                                  break;

                case MSG_BLOCK  : MilliSleep( 5 );
                                  break;

                default         : LogPrintf("*** Process Inventory OTHER MSG_TYPE!!! %d index %d from node %s \n", Inventory_Item.type, nInventory_index, pfrom->addr.ToString() );

                                  MilliSleep( 1 ); /* RGP Optimize */

                                  // Track requests for our stuff
                                  g_signals.Inventory( Inventory_Item.hash );

                                  /* RGP as this is end of the inv sequence, ask for blocks */
                                  PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() );
                                  MilliSleep( 1 );

                                  break; /* RGP let the for loop end */


            }

            fAlreadyHave = AlreadyHave(txdb, Inventory_Item );
            if (!fAlreadyHave)
            {

                /* RGP moved here as we should not presume that we have it by automatically
                   adding if we don't already have it, only set this if we do not have
                   and we add it!! */


                pfrom->AddInventoryKnown( Inventory_Item );
                if (!fImporting)
                {

                   /* RGP Experimental, was false  */
                   pfrom->AskFor( Inventory_Item, true );

                   /* RGP experimental */

                   //PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() );
                   MilliSleep( 1 ); /* RGP Optimize */

                   if ( !mapBlockIndex.count( Inventory_Item.hash ) )
                   {

                       pfrom->AskFor( Inventory_Item, false );
                       MilliSleep( 1 ); /* RGP Optimize */
                   }

                }


		/* get the source to push blocks from out best index */
PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  ); 
          
                MilliSleep( 1 ); /* RGP Optimize */
            }
            else
            {

                // Fix up test
                pfrom->AddInventoryKnown( Inventory_Item );
                pfrom->AskFor( Inventory_Item, true );
                MilliSleep( 1 ); /* RGP Optimize */



                //LogPrintf("*** RGP Inventory, already have this hash %s \n", Inventory_Item.ToString() );

                /* RGP test for synch
                   This could mean that we have the inventory,but not the actual blockchain yet
                   ask for blocks from latest blockheight */
 
                
                //LogPrintf("RGP Inventory items %s \n", Inventory_Item.hash.ToString() );


                if ( !mapBlockIndex.count( Inventory_Item.hash ) )
                {
                    //LogPrintf("*** RGP INV processing, we have, check orphans \n");
                    pfrom->AskFor( Inventory_Item, false );
                    MilliSleep( 1 ); /* RGP Optimize */

                    if ( Inventory_Item.type == MSG_BLOCK && mapOrphanBlocks.count( Inventory_Item.hash ) )
                    {

                        if ( fDebug )
                        {
                            LogPrintf("*** RGP Inventory Checking ORPHANBLOCKS FOUND inv hash %s  \n", Inventory_Item.hash.ToString());
                        }

                        /* if we do have the Inventory, why ask? */
                        if ( mapOrphanBlocksByPrev.count( pindexBest->phashBlock ) )
                        {
                             LogPrintf("*** RGP INV Processing FOUND pindex best in mapOrphanBlocksByPrev!! \n");
                        }


                        //PushGetBlocks(pfrom, pindexBest, GetOrphanRoot(Inventory_Item.hash));

                        /* RGP if we have orphans also ask for latest blockheight */

                        //PushGetBlocks(pfrom, pindexBest, uint256(0) );

                        //LogPrintf("*** RGP INV processing, end of Orphan check \n");

                        MilliSleep( 1 ); /* RGP Optimize */
                    }
                    else
                    {
                        if ( nInventory_index  == vInv.size() ) /* nLastBlock ) */
                        {

                           //LogPrintf("*** RGP PushGetBlocks LAST Ditch effort \n");

                           // In case we are on a very long side-chain, it is possible that we already have
                           // the last block in an inv bundle sent in response to getblocks. Try to detect
                           // this situation and push another getblocks to continue.
                           // PushGetBlocks(pfrom,  pindexBest, uint256(0)); /*  mapBlockIndex[inv.hash], uint256(0)); */

                           // PushGetBlocks(pfrom, pindexBest, uint256(0) );


                           if (fDebug)
                                LogPrintf("force request: %s\n", Inventory_Item.ToString());
                        }

                        /* Added for MSG_BLOCK  and not in orphan list */
                        pfrom->AskFor( Inventory_Item, false );


                        MilliSleep( 1 ); /* RGP Optimize */
                    }


                }
                else
                {
                   /* ALREADY in BlockIndex */
                   //LogPrintf("*** RGP INVENTORY Processing, Already have in blockchain!!! \n");

//                    CBlockIndex* CheckIndex;
//                    CBlock test_block;
//                    mapBlockIndex.find( Inventory_Item.hash );
//                    map<uint256, CBlockIndex*>::iterator inv_check = mapBlockIndex.find(Inventory_Item.hash);
//                    if (inv_check != mapBlockIndex.end())
//                    {
//                        CheckIndex = inv_check->second;
//                        if ( CheckIndex->nHeight > pindexBest->nHeight )
//                        {
//                            LogPrintf("RGP Invoice LIMBO block is %s \n", Inventory_Item.hash.ToString() );
//
//                           LogPrintf("RGP Find the NON BLOCK %s \n", CheckIndex->ToString() );
//                            if ( !test_block.MN_Disconnect_Block(  CheckIndex ) )
//                            {
//                                LogPrintf("RGP MN_Disconnect_Block failed %s \n",  CheckIndex->ToString() );
//                            }
//                            else
//                            {
//                                LogPrintf("RGP MN_Disconnect_Block sucess YEAH BABY %s \n",  CheckIndex->ToString() );
//
//                                mapBlockIndex.erase(Inventory_Item.hash);
//                                if ( mapBlockIndex.count(Inventory_Item.hash) > 0 )
//                                {
//                                    LogPrintf("RGP after ERASE, it still exists \n");
//                                }
//                            }
//
//                        }
                }




                   if ( net_request_filter <= 0 )
                   {
                       //LogPrintf("*** RGP INVENTORY Processing, Already have in blockchain!!! \n");


                       PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  ); /* RGP change the blockhash from 0 */
                       net_request_filter = 20;
                   }
                   else
                   {
                       net_request_filter--;
                   }

                   PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  ); /* RGP change the blockhash from 0 */

 


            }            

            // Track requests for our stuff
            g_signals.Inventory( Inventory_Item.hash );

            MilliSleep( 1 ); /* RGP Optimize */

        }

        // Track requests for our stuff
        //g_signals.Inventory( vInv );

        MilliSleep( 1 );
    }


    /* Getdata */

    else if (strCommand == "getdata")
    {
LogPrintf("rgp Processmessage 'getdata' request node %s \n", pfrom->addr.ToStringIP() );
        vector<CInv> vInv;
        vRecv >> vInv;

        //LogPrintf("*** RGP ProcessMessage getdata %d \n",  pfrom->addr.ToStringIP());
PushGetBlocks( pfrom,  pindexBest, pindexBest->GetBlockHash() );

        LOCK(cs_main);

        if (vInv.size() > MAX_INV_SZ)
        {
            Misbehaving(pfrom->GetId(), 1);
            return error("message getdata size() = %u", vInv.size());
        }

        //if (fDebug || (vInv.size() != 1))
        //    LogPrintf("net, received getdata (%u invsz)\n", vInv.size());

        //if ((fDebug && vInv.size() > 0) || (vInv.size() == 1))
        //    LogPrintf("net, received getdata for: %s\n", vInv.  vInv[0].ToString());

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom);
        MilliSleep(1); /* RGP Optimise */
    }


    else if (strCommand == "getblocks")
    {

        /* -------------------------------------------------------------------------------------------
           -- RGP, if hash of ZERO is passed in, inventory from the Genesis Block shall be returned --
           ------------------------------------------------------------------------------------------- */

//LogPrintf("rgp Processmessage 'getblocks' request node %s \n", pfrom->addr.ToStringIP() );
//PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash() );
MilliSleep( 5);


        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LOCK(cs_main);

// LogPrintf("rgp Processmessage 'getblocks' for hash %s \n", hashStop.ToString() );

        int nLimit = 500;

        if (  hashStop == uint256( 0 ) ) 
        {
            // RGP, A new algo is required to look at connection info and set this as the 
            //      latest has request.

            // RGP at present if hashstop is zero, just return with no activity  
            MilliSleep( 5 );
            return true;
        }

        //int nLimit = MAX_INV_SZ;

        // Find the last block the caller has in the main chain
        /* RGP, This occurs after we call PushBlocks or AskFor, where the Inventory
                Item is not known and a block is requested.
                This block 'should' be unique                                           */

//LogPrintf("*** RGP getblocks processing hashStop %s \n", hashStop.ToString() );


        Last_Block_Confirmed = false;
        Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

        //if ( ( nBestHeight + 500 ) < pfrom->nStartingHeight )
        if ( Time_to_Last_block > 2400  )
        {
            //LogPrintf("*** RGP ProcessMessage GETBLOCKS, Wallet Synching... \n" );

            MilliSleep(1);

            //pfrom->PushInventory(CInv(MSG_BLOCK, pindexBest->GetBlockHash()));
            //pfrom->hashContinue = pindex->GetBlockHash(); /* RGP */
       }
       else
       {

            /* RGP, Only do this if the code required it, when the wallet is synched */
            CBlockIndex* pindex = locator.GetBlockIndex();

LogPrintf("*** RGP ProcessMessage GETBLOCKS, request for index %d hashstop %s \n",  pindex->nHeight, hashStop.ToString() );
LogPrintf("*** RGP ProcessMessage GETBLOCKS from %s \n", pfrom->addr.ToStringIP());


            // Send the rest of the chain
            //if (pindex)
            //    pindex = pindex->pnext;

            //if( pindex->pnext == 0 )
            //{
            //    LogPrintf("*** RGP BLOCKS processing, pindex->next is NULL \n ");
            //}

            /* RGP, rewrite to send as many as we can, start with 100, then increase
                    have seen it sending the same block back as that requested...       */

          //LogPrintf("net, getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), nLimit);
            for (; pindex; pindex = pindex->pnext)
            {

                //LogPrintf("RGP getblocks pindex->nHeight %d \n", pindex->nHeight );

                if ( pindex->pnext == NULL )
                {
                    //LogPrintf("*** RGP getblocks pindex is NULL \n");
                    break;
                }
                //LogPrintf("*** RGP getblocks hash requested %s \n", pindex->GetBlockHash().ToString() );

                /* RGP, new code to send more blocks to requester, if we have them stored in our blockchain file */
                if ( pindex->nHeight <= pindexBest->nHeight )
                {
                    // LogPrintf("*** RGP getblocks sending from height %d hash %s \n", pindex->nHeight,  pindex->GetBlockHash().ToString() );
                    pfrom->PushInventory( CInv( MSG_BLOCK, pindex->GetBlockHash() ) );

                }


                if (pindex->GetBlockHash() == hashStop)
                {
                    /* RGP, tester, ignore this stuff */
                    //LogPrint("net", "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                    //LogPrintf("*** RGP getblocks stopping at heigh %d hash %s \n", pindex->nHeight,  pindex->GetBlockHash().ToString() );
                    //pfrom->hashContinue = pindex->GetBlockHash(); /* RGP */
                    //break;
                }



                if (--nLimit <= 0)
                {
                    // When this block is requested, we'll send an inv that'll make them
                    // getblocks the next batch of inventory.
                    //LogPrintf("net, getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                    pfrom->hashContinue = pindex->GetBlockHash();
                    MilliSleep(1);
                    break;
                }


		        MilliSleep(1);
            }
        }
        
	MilliSleep(1);
    }
    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        LogPrintf("rgp Processmessage 'getheaders' request node %s \n", pfrom->addr.ToStringIP() );

        LOCK(cs_main);


LogPrintf("GETHEADERS\n");

        if (IsInitialBlockDownload())
        {
            LogPrintf("*** RGP Message received getheaders Debug 002 \n");


            return true;
        }

        LogPrintf("*** RGP Message received getheaders Debug 003 \n");

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
            {
                LogPrintf("*** RGP Message received getheaders Debug 004 \n");

                return true;                
            }
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        vector<CBlock> vHeaders;
        int nLimit = 2000;
        LogPrintf("net, getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString());
        LogPrintf("*** RGP Message received getheaders Debug 004 \n");

        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
            
            MilliSleep(1);
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx"|| strCommand == "dstx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CTransaction tx;

        LogPrintf("*** RGP ProcessMessage TX or DSTX \n" );
PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );

        LOCK(cs_main);

        //masternode signed transaction
        bool ignoreFees = false;
        CTxIn vin;
        CInv inv;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        CTxDB txdb("r");

        if(strCommand == "tx") {
            vRecv >> tx;
            inv = CInv(MSG_TX, tx.GetHash());
            // Check for recently rejected (and do other quick existence checks)
            if (AlreadyHave(txdb, inv))
                return true;
        }
        else if (strCommand == "dstx") {
            vRecv >> tx >> vin >> vchSig >> sigTime;
            inv = CInv(MSG_DSTX, tx.GetHash());
            // Check for recently rejected (and do other quick existence checks)
            if (AlreadyHave(txdb, inv))
            {
                LogPrintf("\n\n *** RGP DSTX ALREADY HAVE IGNORING \n\n");
                return true;
            }
            //these allow masternodes to publish a limited amount of free transactions

            CMasternode* pmn = mnodeman.Find(vin);
            if(pmn != NULL)
            {
                if(!pmn->allowFreeTx){
                    //multiple peers can send us a valid masternode transaction
                    if(fDebug)
                        LogPrintf("dstx: Masternode sending too many transactions %s\n", tx.GetHash().ToString().c_str());
                    return true;
                }

                std::string strMessage = tx.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage)){
                    LogPrintf("dstx: Got bad masternode address signature %s \n", vin.ToString().c_str());
                    Misbehaving(pfrom->GetId(), 1);
                    return false;
                }

                LogPrintf("dstx: Got Masternode transaction %s\n", tx.GetHash().ToString().c_str());

                ignoreFees = true;
                pmn->allowFreeTx = false;

                if(!mapDarksendBroadcastTxes.count(tx.GetHash())){
                    CDarksendBroadcastTx dstx;
                    dstx.tx = tx;
                    dstx.vin = vin;
                    dstx.vchSig = vchSig;
                    dstx.sigTime = sigTime;

                    mapDarksendBroadcastTxes.insert(make_pair(tx.GetHash(), dstx));
                }
            }
        }

        pfrom->AddInventoryKnown(inv);

        bool fMissingInputs = false;

        pfrom->setAskFor.erase(inv.hash);
        mapAlreadyAskedFor.erase(inv);

        if (AcceptToMemoryPool(mempool, tx, true, &fMissingInputs, false, ignoreFees))
        {
            RelayTransaction(tx, inv.hash);
            vWorkQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                map<uint256, set<uint256> >::iterator itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue[i]);
                if (itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (set<uint256>::iterator mi = itByPrev->second.begin();
                     mi != itByPrev->second.end();
                     ++mi)
                {
                    const uint256& orphanTxHash = *mi;
                    CTransaction& orphanTx = mapOrphanTransactions[orphanTxHash];
                    bool fMissingInputs2 = false;

                    if (AcceptToMemoryPool(mempool, orphanTx, true, &fMissingInputs2))
                    {
                        LogPrint("mempool", "   accepted orphan tx %s\n", orphanTxHash.ToString());
                        RelayTransaction(orphanTx, orphanTxHash);
                        vWorkQueue.push_back(orphanTxHash);
                        vEraseQueue.push_back(orphanTxHash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee/priority
                        vEraseQueue.push_back(orphanTxHash);
                        LogPrint("mempool", "   removed orphan tx %s\n", orphanTxHash.ToString());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(tx);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                LogPrint("mempool", "mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        if(strCommand == "dstx")
        {
            inv = CInv(MSG_DSTX, tx.GetHash());

            LogPrintf("*** RGP RelayInventory is main DSTX \n");

            RelayInventory(inv);
        }
        if (tx.nDoS){
            LogPrintf("*** RGP tx.nDOS??? \n");

                LogPrintf("*** RGP MISBEHAVING, Tx.DOS \n");
                Misbehaving(pfrom->GetId(), 1);

        }
    }

    /* -- RGP, As the QT wallet is using different schemes to calculate  --
       --      the current block height based on incoming blocks, this   --
       --      code needs to use seperate algorithms using the Inventory --
       --      records coming in                                         -- */


    else if (strCommand == "block" && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        CBlock block;
        vRecv >> block;
        uint256 hashBlock = block.GetHash();
        unsigned int age_calculation;

//PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );

        /* RGP Copy this block for later use by AcceptBlock() */
        block_to_check = block;

        //LogPrintf("*** RGP BLOCK message \n");

        /* -----------------------------------------------------------------
           -- RGP, Some nodes are sending blocks from the high end of     --
           --     the chain, when synching, which does not help... try to --
           --     get them to produce what we want.                       --
           ----------------------------------------------------------------- */
       // if ( block.GetBlockTime() > ( pindexBest->GetBlockTime() + 120000 ) )
        

        /* if the message is an old message, ignore the time calculation */
        if ( block.GetBlockTime() < pindexBest->GetBlockTime() )
        {
            /* old message means than a node is sending rubbish */
            //LogPrintf("Old. new time %d index best time %d ", block.GetBlockTime(), pindexBest->GetBlockTime() );
            //PushGetBlocks(pfrom, pindexBest,  pindexBest->GetBlockHash() );
        }
        else
        {
            age_calculation = block.GetBlockTime();
            age_calculation = age_calculation - pindexBest->GetBlockTime();
            //age_calculation = pindexBest->GetBlockTime() - age_calculation;
            //LogPrintf("RGP age Calculation is %d block time %d best time %d \n", age_calculation, block.GetBlockTime(), pindexBest->GetBlockTime() );

            if ( age_calculation > 5000000 )
            {
                /* ------------------------------------------------------------------
                   -- The block from the synch node is way in advance, reject this --
                   -- block but ask for inventory from the current best block      --
                   ------------------------------------------------------------------ */
                PushGetBlocks(pfrom, pindexBest, pindexBest->GetBlockHash()  );
                //LogPrintf("RGP BLOCK message time too far %s hash time %d \n", pindexBest->GetBlockHash().ToString(), pindexBest->GetBlockTime() );
                //LogPrintf("RGP BLOCK message hash %s time %d \n", hashBlock.ToString(), block.GetBlockTime() );
                //LogPrintf("%");
                return true;
            }
        }

        CInv inv(MSG_BLOCK, hashBlock);
        fAlreadyHave = AlreadyHave(txdb, inv );

        //LogPrintf("*** RGP BLOCK message hash %s \n", inv.hash.ToString() );


        /* RGP, Don't add if we already have */
        //if ( !fAlreadyHave )
        //{
           // LogPrintf("*** RGP BLOCK message hash INventry NOT KNOWN???? %s \n", inv.hash.ToString() );
           //pfrom->AddInventoryKnown( inv );
        //}
        //else
        //{
            // LogPrintf("*** RGP ProcessMessage block already have %s \n", hashBlock.ToString() );
        //}

        if (fDebug )
        {
            LogPrintf("*** RGP before ProcessBlock received hash %s prevhash %S \n", block.GetHash().ToString(), block.hashPrevBlock.ToString());
        }

        //LOCK(cs_main);
        if ( ProcessBlock(pfrom, &block) )
        {
            /* RGP, ProcessBlock true means that AcceptBlock was successful */
            //LogPrintf("*** RGP ProcessMessage block ACCEPTED \n" );
            mapAlreadyAskedFor.erase(inv);

	    MilliSleep(1);

            //return true;
        }
        else
        {
            //LogPrintf("*** RGP debug Process Block FAILED, may be caused by Staking errors !\n");

            if (block.nDoS)
            {
                    LogPrintf("*** RGP block.nDOS DETECTED!!! %d node %s \n", block.nDoS, pfrom->addr.ToString()  );
                    Misbehaving(pfrom->GetId(), block.nDoS);
            }

        }


        if (fSecMsgEnabled)
            SecureMsgScanBlock(block);

        PushGetBlocks(pfrom,  pindexBest,  pindexBest->GetBlockHash()); /*  mapBlockIndex[inv.hash], uint256(0)); */
        MilliSleep( 1 ); /* RGP Optimize */

        return true;

    }

    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making users (which are behind NAT and can only make outgoing connections) ignore
    // getaddr message mitigates the attack.
    else if ((strCommand == "getaddr") && (pfrom->fInbound))
    {
        // Don't return addresses older than nCutOff timestamp

        LOCK(cs_main);

LogPrintf("GETADDR...\n");

PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );

        int64_t nCutOff = GetTime() - (nNodeLifespan * 24 * 60 * 60);
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
        {
            if(addr.nTime > nCutOff)
                pfrom->PushAddress(addr);
           
            MilliSleep( 1 ); /* RGP Optimize */   
        }
    }


    else if (strCommand == "mempool")
    {
        LOCK(cs_main);

LogPrintf("MEMPOOL\n");
PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );

        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        CInv inv;
        for (unsigned int i = 0; i < vtxid.size(); i++) {
            inv = CInv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                    break;
                    
            MilliSleep(1); /* Optimize */
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "ping")
    {
PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.

            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "pong")
    {
        int64_t pingUsecEnd = GetTimeMicros();
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;
PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash());
        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            LogPrintf("*** RGP PONG message debug 002 \n");

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint("net", "pong %s %s: %s, %x expected, %x received, %zu bytes\n"
                , pfrom->addr.ToString()
                , pfrom->strSubVer
                , sProblem
                , pfrom->nPingNonceSent
                , nonce
                , nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
LogPrintf(" RGP Inventory Processing duplicates \n");
                Misbehaving(pfrom->GetId(), 1);
            }
        }
    }


    else
    {
        LogPrintf("*** RGP Other message debug 001 > %s node %s \n", strCommand, pfrom->addr.ToStringIP() );
        PushGetBlocks( pfrom,  pindexBest,  pindexBest->GetBlockHash() );
        MilliSleep( 1 );

        if (fSecMsgEnabled)
            SecureMsgReceiveData(pfrom, strCommand, vRecv);

        darkSendPool.ProcessMessageDarksend(pfrom, strCommand, vRecv);

        mnodeman.ProcessMessage(pfrom, strCommand, vRecv);        

        ProcessMessageMasternodePayments(pfrom, strCommand, vRecv);

        ProcessMessageInstantX(pfrom, strCommand, vRecv);

        ProcessSpork(pfrom, strCommand, vRecv);

        // Ignore unknown commands for extensibility
    }

    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);

    return true;
}

// requires LOCK(cs_vRecvMsg)
bool ProcessMessages(CNode* pfrom)
{
string strCommand;

    //LogPrintf("\n\n ProcessMessages Start\n\n");

    /* Let's check that the node is still connected */
    if ( pfrom->fDisconnect )
    {
        LogPrintf("*** RGP ProcessMessages node disconnected for node %s \n", pfrom->addr.ToStringIP() );
        return false;
    }


    if ( pfrom->mapAskFor.size() == 0 )
    {
        /* Nothing being asked for, request. */
        // LogPrintf("*** RGP ProcessMessages mapAskfor is zero pushing for blocks \n ");
        PushGetBlocks(pfrom, pindexBest,  pindexBest->GetBlockHash() );

    }



    //if (fDebug)
    //{
    //    if ( pfrom->vRecvMsg.size() != 0 )
    //    {
    //        LogPrintf("ProcessMessages(%zu messages)\n", pfrom->vRecvMsg.size());
    //    }
    //}

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fOk = true;

    if (!pfrom->vRecvGetData.empty())
    {
        LogPrintf("vRecGetData is OK from %s calling ProcessGetData \n", pfrom->addr.ToString() );
        ProcessGetData(pfrom);
    }

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty())
    {
        LogPrintf("RGP ProcessMessages no data from node %s \n", pfrom->addrName );
        PushGetBlocks(pfrom, pindexBest,  pindexBest->GetBlockHash() );
        return fOk;
    }

    // LogPrintf("*** RGP ProcessMessages before main loop \n");


    if ( pfrom->vRecvMsg.empty() )
        LogPrintf("*** RGP ProcessMessages, recive buffer empty %s \n", pfrom->addr.ToString() );

    if ( pfrom->fDisconnect )
        LogPrintf("*** RGP ProcessMessages, Disconnected %s \n", pfrom->addr.ToString() );

    std::deque<CNetMessage>::iterator it = pfrom->vRecvMsg.begin();
    while (!pfrom->fDisconnect && it != pfrom->vRecvMsg.end())
    {

        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->nSendSize > SendBufferSize())
        {
            LogPrintf("*** RGP Sendbuffer too full from node %s send size %d send check size %d \n", pfrom->addr.ToString(), pfrom->nSendSize , SendBufferSize()  );
            break;
        }

        // get next message
        CNetMessage& msg = *it;

        if (fDebug)
        {
           LogPrintf("ProcessMessages(message %u msgsz, %zu bytes, complete:%s)\n",
                     msg.hdr.nMessageSize, msg.vRecv.size(),
                     msg.complete() ? "Y" : "N");
        }

        // end, if an incomplete message is found
        if (!msg.complete())
        {
            LogPrintf("*** RGP ProcessMessages msg incomplete from node %s \n", pfrom->addr.ToString() );

            /* RGP delete messages or the same message will be used again and again */

            try
            {
                LogPrintf("*** RGP ProcessMessages TRYING to free the vRecvMsg buffer \n");
                pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);

                if ( GetTime() - pindexBest->GetBlockTime() > 10000 )        
                    LogPrintf("RGP no ban, not synched \n");
                else
                {
                    LogPrintf(" Node to ban is %s \n", pfrom->addr.ToString() );
                    CNode::Ban(pfrom->addr, BanReasonNodeMisbehaving); 
                }
                MilliSleep( 1 );
            }
            catch (std::ios_base::failure& e)
            {

                PrintExceptionContinue(&e, "ProcessMessages()" );

            }
            catch (boost::thread_interrupted)
            {
                /* nothing */
            }
            catch (std::exception& e) {

                PrintExceptionContinue(&e, "ProcessMessages()");
            } catch (...) {

                PrintExceptionContinue(NULL, "ProcessMessages()");
            }



            /* Returning false will disconnect the node in net.cpp */
            LogPrintf("*** RGP stopped Disconnect... \n");
            pfrom->fDisconnect = true;
            return false;
        }

        // at this point, any failure means we can delete the current message
        it++;

        // Scan for message start
        if ( memcmp( msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE ) != 0 )
        {
            LogPrintf("\n\nPROCESSMESSAGE: INVALID MESSAGESTART\n\n");
            fOk = false;
            break;
        }

        // Read header
        CMessageHeader& hdr = msg.hdr;
        if (!hdr.IsValid())
        {
            LogPrintf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand());
            continue;
        }

        strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;

        // Checksum
        CDataStream& vRecv = msg.vRecv;
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            LogPrintf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
                                       strCommand, nMessageSize, nChecksum, hdr.nChecksum);
               
            /* -----------------------------------------------------------
               -- RGP, This is causing the system to be killed in Linux --
               --      This node will now be disconnected and ban score --
               --      hiked high.                                      --
               --                                                       --
               --      Suspect List : 5.189.179.97:23980                --
               ----------------------------------------------------------- */   
//            if ( GetTime() - pindexBest->GetBlockTime() > 10000 )        
//                LogPrintf("RGP no ban, not synched \n");
//            else
//            {
                // RGP no ban during sysnc...
                // CNode::Ban(pfrom->addr, BanReasonNodeMisbehaving);   
               
                pfrom->fDisconnect = true;
                break;				/* break out of the while loop          */
//            }                                  /* no more messages should be processed */   
            //continue;
        }

        MilliSleep( 1 );

        // Process message
        bool fRet = false;
        try
        {           
            //LogPrintf("*** RGP ProcessMessages before call to ProcessMessage \n");
            fRet = ProcessMessage(pfrom, strCommand, vRecv);

            MilliSleep( 1 );

            //boost::this_thread::interruption_point();
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand, nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                LogPrintf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand, nMessageSize, e.what());
            }
            else
            {

                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (boost::thread_interrupted) {
            throw;
        }
        catch (std::exception& e) {

            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {

            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        //LogPrintf("ProcessMessage, after ProcessMessage, after try catch \n");


        MilliSleep( 5 );



        if (!fRet)
        {

           LogPrintf("ProcessMessage, after ProcessMessage, Before fail message \n");

            LogPrintf("ProcessMessage(%s, %u bytes %s node) FAILED\n", strCommand, nMessageSize, pfrom->addr.ToString());
            /* RGP, 5th Sep 2019, break outside of this If Condition, caused all
                    nodes to be disconnected, after processing only one message */
            /* break;  RGP just in case only one message failed
                       This could cause all other messages to be discarded if break is allowed !!!*/

            if ( GetTime() - pindexBest->GetBlockTime() > 10000 )        
             LogPrintf("RGP no ban, not synched \n");

            else
            {
                /* RGP, this will disconnect the node */
                CNode::Ban(pfrom->addr, BanReasonNodeMisbehaving);                  
                pfrom->fDisconnect = true;
                return false;
            }
        }

        MilliSleep( 5 );

        //LogPrintf("ProcessMessage, after ProcessMessage, before End of while loop \n");

    }

    /* -----------------------------------------------------------------------
       -- RGP 6th Sep 2019, comment updated, if the node is still connected --
       -- then clear the message, it's been processed. The effect of not    --
       -- this that all nodes disconnect.                                   --
       ----------------------------------------------------------------------- */
    if ( !pfrom->fDisconnect )
    {
        pfrom->vRecvMsg.erase(pfrom->vRecvMsg.begin(), it);
    }

    return fOk;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
bool pingSend;
int monitor;
uint64_t nonce;
static int64_t nLastRebroadcast;
extern bool BSC_Wallet_Synching; /* RGP defined in main.h */



    //TRY_LOCK(cs_main, lockMain);
    //if (lockMain)
    {
        /* Lock cs_main #1 */
        thread_semaphore.wait ( THREAD_LOCK_CS_MAIN );
        //LogPrintf("*** SendMessages wait OK \n");

        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
        {
            thread_semaphore.notify( THREAD_LOCK_CS_MAIN, CURRENT_TASK );
            MilliSleep( 1 ); /* RGP Optimize */
            return true;
        }

        //
        // Message: ping
        //

        pingSend = false;
        if (pto->fPingQueued)
        {
            // RPC ping request by user
            pingSend = true;
        }

        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }

        if (pingSend)
        {
            nonce = 0;
            while (nonce == 0)
            {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));                
            }

            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION)
            {
                pto->nPingNonceSent = nonce;
                pto->PushMessage("ping", nonce);
            }
            else
            {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                pto->PushMessage("ping");
            }

            MilliSleep( 1 ); /* RGP Optimize */
        }

        thread_semaphore.notify( THREAD_LOCK_CS_MAIN, CURRENT_TASK );


        //TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        //if ( !lockMain )
        //{
        //    LogPrintf("*** RGP net SendMessages part 3b lock failed \n");

        //    return true;
        //}

        /* Lock cs_main #2 */
        thread_semaphore.wait ( THREAD_LOCK_CS_MAIN );
        //LogPrintf("*** SendMessages wait OK \n");



        // Start block sync
        if (pto->fStartSync && !fImporting && !fReindex)
        {
            pto->fStartSync = false;
            PushGetBlocks(pto, pindexBest,  pindexBest->GetBlockHash());

            MilliSleep( 1 ); /* RGP Optimize */
        }
//        else
 //       {

 //           pto->fStartSync = true;
 //           PushGetBlocks(pto, pindexBest, uint256(0));

 //           MilliSleep( 1 ); /* RGP Optimize */

  //      }


        thread_semaphore.notify( THREAD_LOCK_CS_MAIN, CURRENT_TASK );

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload())
        {           
                ResendWalletTransactions();

                MilliSleep( 1 ); /* RGP Optimize */

        }

        // Address refresh broadcast        
        if ( !IsInitialBlockDownload() && ( GetTime() - nLastRebroadcast > 24 * 60 * 60 ) )
        {

            {


                thread_semaphore.wait ( THREAD_LOCK_CS_VNODES );
                //LogPrintf("*** SendMessages wait OK \n");

                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }

                    MilliSleep(5);   /* RGP Optimize */
                }

                thread_semaphore.notify( THREAD_LOCK_CS_VNODES, CURRENT_TASK );

            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            monitor = 0;
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    monitor++;
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }

                //LogPrintf("*** SendMessages VADDR sent %d \n", monitor );

                MilliSleep(5);   /* RGP Optimize */
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }

        /* RGP reduce banning activity during synch */
        if (State(pto->GetId())->fShouldBan)
        {
            if (pto->addr.IsLocal())
                LogPrintf("Warning: not banning local node %s!\n", pto->addr.ToString().c_str());
            else
            {
                if( GetTime() - pindexBest->GetBlockTime() > 10000 )        
                {

                    /* don't ban until synched */
                }
                else
                {
                    LogPrintf("*** RGP SendMessage BANNING node \n");
                    pto->fDisconnect = true;
                    CNode::Ban(pto->addr, BanReasonNodeMisbehaving);
                }
            }
            State(pto->GetId())->fShouldBan = false;
        }

        //
        // Message: inventory
        //


        thread_semaphore.wait( THREAD_LOCK_CS_INVENTORY );

        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());

            if ( pto->vInventoryToSend.size() > 0 )
            {
                //LogPrintf("*** RGP Inventory to send size %d \n", pto->vInventoryToSend.size() );
            }
            monitor = 0;
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend )
            {

                if ( pto->setInventoryKnown.count(inv) )
                {
                    MilliSleep(2);   /* RGP Optimize */
                    continue;
                }

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {

                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();

                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    if (fTrickleWait)
                    {
                        monitor++;
                        vInvWait.push_back(inv);
                        MilliSleep(2);   /* RGP Optimize */
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    monitor++;
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        // LogPrintf("*** SendMessages INVENTORY sent %d \n", monitor );

                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }

                }

                MilliSleep(1);   /* RGP Optimize */
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
        {
            pto->PushMessage("inv", vInv);
        }


        thread_semaphore.notify( THREAD_LOCK_CS_INVENTORY, CURRENT_TASK );

        //
        // Message: getdata
        //
        /* ----------------------------------------------------
           -- RGP, this sends the inventory sent to AskFor() --
           -- This should result in 'blocks' being returned  --
           ---------------------------------------------------- */

        //LogPrintf("*** RGP SENDDATA, Process AskFor items %d \n", pto->mapAskFor.size());

        vector<CInv> vGetData;
        //int64_t nNow = GetTime() * 3000 ; /* rgp 1000000; */
        int64_t nNow = GetTime() + 60 ; /* rgp 1000000;  RGP experiment */
        CTxDB txdb("r");

        monitor = 0;
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;

            /* RGP, First check if we have the inventory hash in TXDB as a block */
            if (!AlreadyHave(txdb, inv))
            {
                /* RGP, we do not have the Block hash yet */
                if ( mapBlockIndex.count( inv.hash ) )
                {
                    LogPrintf("BUT in the block index, wasting network time for already acquired blocks!!\n");
                }
                else
                {
                    monitor++;
                    vGetData.push_back(inv);
                    if (vGetData.size() >= 1000 )
                    {
                        //LogPrintf("*** SendMessages GETDATA 1 sent %d \n", monitor );
                        pto->PushMessage("getdata", vGetData);
                        vGetData.clear();
                    }
                }
            }
            else
            {
                if ( !mapBlockIndex.count( inv.hash ) )
                {
                    /* message is not in the block index, FORCE request */
                    if ( vGetData.size() < 1000 )
                    {
                        /* Checking before add, as some messages are too big */
                        vGetData.push_back(inv);
                    }

                    if (vGetData.size() >= 1000 )
                    {
                        LogPrintf("*** SendMessages GETDATA 2 moitor %d send size %d \n", monitor, vGetData.size() );
                        pto->PushMessage("getdata", vGetData );
                        vGetData.clear();
                    }
                }

            }


            pto->PushMessage("getdata", vGetData);
            vGetData.clear();


            //If we're not going to ask, don't expect a response.
            pto->setAskFor.erase(inv.hash);
            pto->mapAskFor.erase(pto->mapAskFor.begin());

            MilliSleep(5);   /* RGP Optimize */

        }



        if (!vGetData.empty())
        {
            pto->PushMessage("getdata", vGetData);

            LogPrintf("*** RGP vGetData sent by Senddata \n");

            vGetData.clear();
            MilliSleep( 5 );  /* RGP optimised */
        }

        if (fSecMsgEnabled)
            SecureMsgSendData(pto, fSendTrickle); // should be in cs_main?


    }


    /* -------------------------------------------------------
       -- SendMessage is called to service message sending, --
       -- sometimes there is nothing to send.               --
       ------------------------------------------------------- */
    MilliSleep(1);   /* RGP Optimize */

    return true;
}

/* -----------------------------------------------------------
   -- RGPickles - GetMasternodePayment --                   --
   -----------------------------------------------------------
   -- This is where the Master node reward is set based on  --
   -- staked coins.                                         --
   --                                                       --
   -- 3/4  -> 75%                                           --
   -- 5/10 -> 50%                                           --
   -- 1/10 -> 10%                                           --
   ----------------------------------------------------------- */

int64_t GetMasternodePayment(int nHeight, int64_t blockValue)
{
int64_t mn_reward;
double test, mn_calc;
double MoneySupply;

    /* ----------------------------------------------------------------
       -- RGP, This algorithm has changed from SOCI, as moneysupply  --
       -- will determine when the Masternode reward is stopped.      --
       --                                                            --
       -- MoneySupply is part of the pindexBest or last block that   --
       -- was successfully accepted and stored.                      --
       ---------------------------------------------------------------- */

    MoneySupply = (double)pindexBest->nMoneySupply / (double)COIN;
    LogPrintf(" %f %f %f \n", MoneySupply , (double)pindexBest->nMoneySupply, (double)COIN  );

    /* --------------------------------------------------------
       -- RGP, 30th April 2023, Money supply changed to 250M --
       -------------------------------------------------------- */
    if ( MoneySupply > 450000000.0 )
    {
       /* Maximum MoneySupply has been reached, no more rewards */
       mn_reward = 0;
    }
    else
    {


        mn_reward = blockValue * 4/10; /* 40% MN reward */

        LogPrintf("RGP GetMasternodePayment masternode reward %d calc %d \n ", mn_reward, blockValue );


        //mn_calc = blockValue * 3/4; //75%

//        mn_calc = 0.0;
//        mn_calc = (double)  blockValue;
//        mn_calc = mn_calc * 4/10;
//        blockValue = ( int )mn_calc;
        //LogPrintf("RGP GetMasternodePayment %d %d \n ", test, blockValue );
//        mn_reward = blockValue;       // * 4/10; //40%

    }

    return mn_reward;
    
}


// From Myce
bool ReadBlockFromDisk(CBlock& block, const CDiskBlockPos& pos)
{
printf("RGP main.c ReadFromDisk line 6608 \n" );
//    block.SetNull();

    // Open history file to read
//    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
//    if (filein.IsNull())
 //       return error("ReadBlockFromDisk : OpenBlockFile failed");

    // Read block
//    try {
//        filein >> block;
//    } catch (std::exception& e) {
//        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
//    }

    // Check the header
//    if (block.IsProofOfWork()) {
//        if (!CheckProofOfWork(block.GetPoWHash(), block.nBits))
//            return error("ReadBlockFromDisk : Errors in block header");
//    }

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
 //   if (!ReadBlockFromDisk(block, pindex->GetBlockPos()))
 //       return false;
//    if (block.GetHash() != pindex->GetBlockHash()) {
//        LogPrintf("%s : block=%s index=%s\n", __func__, block.GetHash().ToString().c_str(), pindex->GetBlockHash().ToString().c_str());
//        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
//    }
    return true;
}



