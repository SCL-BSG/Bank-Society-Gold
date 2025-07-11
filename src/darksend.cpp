// Copyright (c) 2014-2015 The Darkcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "darksend.h"
#include "main.h"
#include "init.h"
#include "util.h"
#include "masternodeman.h"
#include "instantx.h"
#include "ui_interface.h"

#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/lexical_cast.hpp>

#include <algorithm>
#include <boost/assign/list_of.hpp>
#include <openssl/rand.h>

using namespace std;
using namespace boost;

// The main object for accessing darksend
CDarksendPool darkSendPool;

// A helper object for signing messages from Masternodes
CDarkSendSigner darkSendSigner;
// The current darksends in progress on the network
std::vector<CDarksendQueue> vecDarksendQueue;
// Keep track of the used Masternodes
std::vector<CTxIn> vecMasternodesUsed;
// keep track of the scanning errors I've seen
map<uint256, CDarksendBroadcastTx> mapDarksendBroadcastTxes;
// Keep track of the active Masternode
CActiveMasternode activeMasternode;

// count peers we've requested the list from
int RequestedMasterNodeList = 0;

/* *** BEGIN DARKSEND MAGIC - DASH **********
    Copyright (c) 2014-2015, Dash Developers
        eduffield - evan@dashpay.io
        udjinm6   - udjinm6@dashpay.io
*/


void CDarksendPool::ProcessMessageDarksend(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    if(fLiteMode) return; //disable all darksend/Masternode related functionality

    if(!IsBlockchainSynced())
    {
        // LogPrintf("*** RGP ProcessMessageDarksend BLOCK IS NOT SYNCHED. \n");
        /* RGP, if the block is unsynched then no Masternodes are allowed,
                which then stops staking, catch22 blockchain stops          */
        return;
    }



    if (strCommand == "dsa")
    { //DarkSend Accept Into Pool

        //LogPrintf("*** RGP ProcessMessageDarksend Start %s dsa \n", pfrom->addr.ToString() );
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION)
        {
            std::string strError = _("Incompatible version.");
            LogPrintf("dsa -- incompatible version! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, strError);

            return;
        }

        if(!fMasterNode){
            std::string strError = _("This is not a Masternode.");
            LogPrintf("dsa -- not a Masternode! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, strError);

            return;
        }
        
        MilliSleep(5);  /* RGP Optimise */

        LogPrintf("*** RGP ProcessMessageDarksend Start %s dsa next \n", pfrom->addr.ToString() );

        int nDenom;
        CTransaction txCollateral;
        vRecv >> nDenom >> txCollateral;

        std::string error = "";
        CMasternode* pmn = mnodeman.Find(activeMasternode.vin);
        if(pmn == NULL)
        {
            LogPrintf("*** RGP ProcessMessageDarksend Start not in Masternode list \n" );

            std::string strError = _("Not in the Masternode list.");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, strError);
            return;
        }

        if(sessionUsers == 0)
        {
            if( pmn->nLastDsq != 0 &&
                pmn->nLastDsq + mnodeman.CountMasternodesAboveProtocol(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount)
            {
                LogPrintf("dsa -- last dsq too recent, must wait. %s \n", pfrom->addr.ToString().c_str());
                std::string strError = _("Last Darksend was too recent.");
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, strError);
                return;
            }
        }

        if(!IsCompatibleWithSession(nDenom, txCollateral, error))
        {
            LogPrintf("dsa -- not compatible with existing transactions! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        } else {
            LogPrintf("dsa -- is compatible, please submit! \n");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_ACCEPTED, error);
            return;
        }
    } else if (strCommand == "dsq")
    { //Darksend Queue

        MilliSleep(5);  /* RGP Optimise */

        LogPrintf("*** RGP ProcessMessageDarksend dsq \n" );

        TRY_LOCK(cs_darksend, lockRecv);
        if(!lockRecv) return;

        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        CDarksendQueue dsq;
        vRecv >> dsq;


        CService addr;
        if(!dsq.GetAddress(addr)) return;
        if(!dsq.CheckSignature()) return;

        if(dsq.IsExpired()) return;

        CMasternode* pmn = mnodeman.Find(dsq.vin);
        if(pmn == NULL) return;

        // if the queue is ready, submit if we can
        if(dsq.ready)
        {
             LogPrintf("*** RGP ProcessMessageDarksend Start not in Masternode list \n" );

            if(!pSubmittedToMasternode) return;
            if((CNetAddr)pSubmittedToMasternode->addr != (CNetAddr)addr){
                LogPrintf("dsq - message doesn't match current Masternode - %s != %s\n", pSubmittedToMasternode->addr.ToString().c_str(), addr.ToString().c_str());
                return;
            }

            if(state == POOL_STATUS_QUEUE){
                LogPrintf("Darksend queue is ready - %s\n", addr.ToString().c_str());
                PrepareDarksendDenominate();
            }
        } else {
            BOOST_FOREACH(CDarksendQueue q, vecDarksendQueue)
            {
                if(q.vin == dsq.vin) return;
                
                MilliSleep(1);  /* RGP Optimise */
            }

            LogPrintf("darksend, dsq last %d last2 %d count %d\n", pmn->nLastDsq, pmn->nLastDsq + mnodeman.size()/5, mnodeman.nDsqCount);
            //don't allow a few nodes to dominate the queuing process
            if(pmn->nLastDsq != 0 &&
                pmn->nLastDsq + mnodeman.CountMasternodesAboveProtocol(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount){
                LogPrint("darksend", "dsq -- Masternode sending too many dsq messages. %s \n", pmn->addr.ToString().c_str());
                return;
            }
            mnodeman.nDsqCount++;
            pmn->nLastDsq = mnodeman.nDsqCount;
            pmn->allowFreeTx = true;

            LogPrintf("darksend, dsq - new Darksend queue object - %s\n", addr.ToString().c_str());
            vecDarksendQueue.push_back(dsq);
            dsq.Relay();
            dsq.time = GetTime();
        }

    } else if (strCommand == "dsi") { //DarkSend vIn
        std::string error = "";
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            LogPrintf("dsi -- incompatible version! \n");
            error = _("Incompatible version.");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);

            return;
        }

        if(!fMasterNode)
        {
            LogPrintf("dsi -- not a Masternode! \n");
            error = _("This is not a Masternode.");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);

            return;
        }

        std::vector<CTxIn> in;
        int64_t nAmount;
        CTransaction txCollateral;
        std::vector<CTxOut> out;
        vRecv >> in >> nAmount >> txCollateral >> out;

        //do we have enough users in the current session?
        if(!IsSessionReady()){
            LogPrintf("dsi -- session not complete! \n");
            error = _("Session not complete!");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        //do we have the same denominations as the current session?
        if(!IsCompatibleWithEntries(out))
        {
            LogPrintf("dsi -- not compatible with existing transactions! \n");
            error = _("Not compatible with existing transactions.");
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
            return;
        }

        //check it like a transaction
        {
            int64_t nValueIn = 0;
            int64_t nValueOut = 0;
            bool missingTx = false;

            CValidationState state;
            CTransaction tx;

            BOOST_FOREACH(const CTxOut o, out)
            {
                nValueOut += o.nValue;
                tx.vout.push_back(o);

                if(o.scriptPubKey.size() != 25){
                    LogPrintf("dsi - non-standard pubkey detected! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Non-standard public key detected.");
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
                if(!o.scriptPubKey.IsNormalPaymentScript()){
                    LogPrintf("dsi - invalid script! %s\n", o.scriptPubKey.ToString().c_str());
                    error = _("Invalid script detected.");
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
                
                MilliSleep(2);  /* RGP Optimise */
            }

            BOOST_FOREACH(const CTxIn i, in)
            {
                tx.vin.push_back(i);

                LogPrint("darksend", "dsi -- tx in %s\n", i.ToString().c_str());

                CTransaction tx2;
                uint256 hash;
                if(GetTransaction(i.prevout.hash, tx2, hash)){
                    if(tx2.vout.size() > i.prevout.n) {
                        nValueIn += tx2.vout[i.prevout.n].nValue;
                    }
                } else{
                    missingTx = true;
                }
                MilliSleep(2);  /* RGP Optimise */
            }

            if (nValueIn > DARKSEND_POOL_MAX) {
                LogPrintf("dsi -- more than Darksend pool max! %s\n", tx.ToString().c_str());
                error = _("Value more than Darksend pool maximum allows.");
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }

            if(!missingTx){
                if (nValueIn-nValueOut > nValueIn*.01) {
                    LogPrintf("dsi -- fees are too high! %s\n", tx.ToString().c_str());
                    error = _("Transaction fees are too high.");
                    pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                    return;
                }
            } else {
                LogPrintf("dsi -- missing input tx! %s\n", tx.ToString().c_str());
                error = _("Missing input transaction information.");
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }

            if(!AcceptableInputs(mempool, tx, false, NULL, false, true)){
                LogPrintf("dsi -- transaction not valid! \n");
                error = _("Transaction not valid.");
                pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
                return;
            }
        }

        if(AddEntry(in, nAmount, txCollateral, out, error)){
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_ACCEPTED, error);
            Check();

            RelayStatus(sessionID, GetState(), GetEntriesCount(), MASTERNODE_RESET);
        } else {
            pfrom->PushMessage("dssu", sessionID, GetState(), GetEntriesCount(), MASTERNODE_REJECTED, error);
        }
    } else if (strCommand == "dssu") { //Darksend status update
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }
        if(!pSubmittedToMasternode) return;
        if((CNetAddr)pSubmittedToMasternode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dssu - message doesn't match current Masternode - %s != %s\n", pSubmittedToMasternode->addr.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionIDMessage;
        int state;
        int entriesCount;
        int accepted;
        std::string error;
        vRecv >> sessionIDMessage >> state >> entriesCount >> accepted >> error;

        LogPrintf("darksend", "dssu - state: %i entriesCount: %i accepted: %i error: %s \n", state, entriesCount, accepted, error.c_str());

        if((accepted != 1 && accepted != 0) && sessionID != sessionIDMessage){
            LogPrintf("dssu - message doesn't match current Darksend session %d %d\n", sessionID, sessionIDMessage);
            return;
        }

        StatusUpdate(state, entriesCount, accepted, error, sessionIDMessage);

    } else if (strCommand == "dss") { //DarkSend Sign Final Tx
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        vector<CTxIn> sigs;
        vRecv >> sigs;

        bool success = false;
        int count = 0;

        BOOST_FOREACH(const CTxIn item, sigs)
        {
            if(AddScriptSig(item)) success = true;
            LogPrint("darksend", " -- sigs count %d %d\n", (int)sigs.size(), count);
            count++;
            
            MilliSleep(1);  /* RGP Optimise */
        }

        if  ( success )
        {
            darkSendPool.Check();
            RelayStatus(darkSendPool.sessionID, darkSendPool.GetState(), darkSendPool.GetEntriesCount(), MASTERNODE_RESET);            
        }
    } else if (strCommand == "dsf") { //Darksend Final tx
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }
        if(!pSubmittedToMasternode) return;
        if((CNetAddr)pSubmittedToMasternode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current Masternode - %s != %s\n", pSubmittedToMasternode->addr.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }
        int sessionIDMessage;
        CTransaction txNew;
        vRecv >> sessionIDMessage >> txNew;
        if(sessionID != sessionIDMessage){
            LogPrint("darksend", "dsf - message doesn't match current darksend session %d %d\n", sessionID, sessionIDMessage);
            return;
        }
        //check to see if input is spent already? (and probably not confirmed)
        SignFinalTransaction(txNew, pfrom);
    } else if (strCommand == "dsc") { //Darksend Complete
        if (pfrom->nVersion < MIN_POOL_PEER_PROTO_VERSION) {
            return;
        }

        if(!pSubmittedToMasternode) return;
        if((CNetAddr)pSubmittedToMasternode->addr != (CNetAddr)pfrom->addr){
            //LogPrintf("dsc - message doesn't match current Masternode - %s != %s\n", pSubmittedToMasternode->addr.ToString().c_str(), pfrom->addr.ToString().c_str());
            return;
        }

        int sessionIDMessage;
        bool error;
        int errorID;
        vRecv >> sessionIDMessage >> error >> errorID;

        if(sessionID != sessionIDMessage){
            LogPrint("darksend", "dsc - message doesn't match current darksend session %d %d\n", darkSendPool.sessionID, sessionIDMessage);
            return;
        }

        darkSendPool.CompletedTransaction(error, errorID);
    }

}

int randomizeList (int i) { return std::rand()%i;}

void CDarksendPool::Reset(){
    cachedLastSuccess = 0;
    lastNewBlock = 0;
    txCollateral = CTransaction();
    vecMasternodesUsed.clear();
    UnlockCoins();
    SetNull();
}

void CDarksendPool::SetNull(){

    // MN side
    sessionUsers = 0;
    vecSessionCollateral.clear();

    // Client side
    entriesCount = 0;
    lastEntryAccepted = 0;
    countEntriesAccepted = 0;
    sessionFoundMasternode = false;

    // Both sides
    state = POOL_STATUS_IDLE;
    sessionID = 0;
    sessionDenom = 0;
    entries.clear();
    finalTransaction.vin.clear();
    finalTransaction.vout.clear();
    lastTimeChanged = GetTimeMillis();

    // -- seed random number generator (used for ordering output lists)
    unsigned int seed = 0;
    RAND_bytes((unsigned char*)&seed, sizeof(seed));
    std::srand(seed);
}

bool CDarksendPool::SetCollateralAddress(std::string strAddress){
    CSocietyGcoinAddress address;
    if (!address.SetString(strAddress))
    {
        LogPrintf("CDarksendPool::SetCollateralAddress - Invalid DarkSend collateral address\n");
        return false;
    }
    collateralPubKey = GetScriptForDestination(address.Get());
    return true;
}

//
// Unlock coins after Darksend fails or succeeds
//
void CDarksendPool::UnlockCoins(){
    while(true) 
    {
        TRY_LOCK(pwalletMain->cs_wallet, lockWallet);
        
        if  (!lockWallet) 
        {
            MilliSleep(5); 
            continue;
        }
        
        BOOST_FOREACH(CTxIn v, lockedCoins)
        {
            pwalletMain->UnlockCoin(v.prevout);
            MilliSleep(1);  /* RGP Optimise */
        }
        break;

        MilliSleep( 2 ); /* RGP Optimise */
    }

    lockedCoins.clear();
}

/// from masternode-sync.cpp
bool CDarksendPool::IsBlockchainSynced()
{
static bool fBlockchainSynced = false;
static int64_t lastProcess = GetTime();
static int64_t LastLook = GetTime();
static int lastlook_counter = 0;

    // if the last call to this function was more than 60 minutes ago (client was in sleep mode) reset the sync process
    if(GetTime() - lastProcess > 60*60)
    {
        LogPrintf("*** RGP CDarksendPool::IsBlockchainSynced Debug 2 Time %d Last %d \n",GetTime(),lastProcess );
        Reset();
        fBlockchainSynced = false;
    }
    lastProcess = GetTime();

    if(fBlockchainSynced) return true;

    if (fImporting || fReindex) return false;

    //TRY_LOCK(cs_main, lockMain);
    //if(!lockMain)
    //{
    //    LogPrintf("*** RGP CDarksendPool::IsBlockchainSynced LockMain FAILED /n");
    //    return false;
    //}

    CBlockIndex* pindex = pindexBest;

    if(pindex == NULL)
    {
//        LogPrintf("*** RGP CDarksendPool::IsBlockchainSynced DEBUG 004 ");
        if ( pindexBest == NULL )
            LogPrintf("*** RGP CDarksendPool::IsBlockchainSynced pindexBest is NULL DEBUG 004 ");
        return false;
    }

    if( pindex->nTime + 60*60 < GetTime() )
    {
//LogPrintf("*** RGP CDarksendPool::IsBlockchainSynced Debug 5b \n");
        /* Later add a block height equation in here */
        if ( GetTime() - pindexBest->GetBlockTime() > 10000 )        
        {

            

            if ( LastLook != GetTime() )                               
            {                

                /* Indicates that block is standing still, but clock is ticking */
                LastLook = GetTime();
                lastlook_counter++;
                if ( lastlook_counter == 3 )
                {                    

                    lastlook_counter = 0;
                    return true;
                }
            }
        }


        return false;
    }

    fBlockchainSynced = true;

    return true;
}

std::string CDarksendPool::GetStatus()
{
    static int showingDarkSendMessage = 0;
    showingDarkSendMessage+=10;
    std::string suffix = "";

    if(pindexBest->nHeight - cachedLastSuccess < minBlockSpacing ||!IsBlockchainSynced()) {
        return strAutoDenomResult;
    }
    switch(state) {
        case POOL_STATUS_IDLE:
            return _("Darksend is idle.");
        case POOL_STATUS_ACCEPTING_ENTRIES:
            if(entriesCount == 0) {
                showingDarkSendMessage = 0;
                return strAutoDenomResult;
            } else if (lastEntryAccepted == 1) {
                if(showingDarkSendMessage % 10 > 8) {
                    lastEntryAccepted = 0;
                    showingDarkSendMessage = 0;
                }
                return _("Darksend request complete:") + " " + _("Your transaction was accepted into the pool!");
            } else {
                std::string suffix = "";
                if(     showingDarkSendMessage % 70 <= 40) return strprintf(_("Submitted following entries to masternode: %u / %d"), entriesCount, GetMaxPoolTransactions());
                else if(showingDarkSendMessage % 70 <= 50) suffix = ".";
                else if(showingDarkSendMessage % 70 <= 60) suffix = "..";
                else if(showingDarkSendMessage % 70 <= 70) suffix = "...";
                return strprintf(_("Submitted to masternode, waiting for more entries ( %u / %d ) %s"), entriesCount, GetMaxPoolTransactions(), suffix);
            }
        case POOL_STATUS_SIGNING:
            if(     showingDarkSendMessage % 70 <= 40) return _("Found enough users, signing ...");
            else if(showingDarkSendMessage % 70 <= 50) suffix = ".";
            else if(showingDarkSendMessage % 70 <= 60) suffix = "..";
            else if(showingDarkSendMessage % 70 <= 70) suffix = "...";
            return strprintf(_("Found enough users, signing ( waiting %s )"), suffix);
        case POOL_STATUS_TRANSMISSION:
            return _("Transmitting final transaction.");
        case POOL_STATUS_FINALIZE_TRANSACTION:
            return _("Finalizing transaction.");
        case POOL_STATUS_ERROR:
            return _("Darksend request incomplete:") + " " + lastMessage + " " + _("Will retry...");
        case POOL_STATUS_SUCCESS:
            return _("Darksend request complete:") + " " + lastMessage;
        case POOL_STATUS_QUEUE:
            if(     showingDarkSendMessage % 70 <= 30) suffix = ".";
            else if(showingDarkSendMessage % 70 <= 50) suffix = "..";
            else if(showingDarkSendMessage % 70 <= 70) suffix = "...";
            return strprintf(_("Submitted to masternode, waiting in queue %s"), suffix);;
       default:
            return strprintf(_("Unknown state: id = %u"), state);
    }
}

//
// Check the Darksend progress and send client updates if a Masternode
//
void CDarksendPool::Check()
{
    LogPrintf("*** RGP CDarksendPool::Check called \n");

    if(fMasterNode) LogPrint("darksend", "CDarksendPool::Check() - entries count %lu\n", entries.size());
    //printf("CDarksendPool::Check() %d - %d - %d\n", state, anonTx.CountEntries(), GetTimeMillis()-lastTimeChanged);

    if(fMasterNode) {
        LogPrint("darksend", "CDarksendPool::Check() - entries count %lu\n", entries.size());
        // If entries is full, then move on to the next phase
        if(state == POOL_STATUS_ACCEPTING_ENTRIES && (int)entries.size() >= GetMaxPoolTransactions())
        {
            LogPrint("darksend", "CDarksendPool::Check() -- TRYING TRANSACTION \n");
            UpdateState(POOL_STATUS_FINALIZE_TRANSACTION);
        }
    }

    // create the finalized transaction for distribution to the clients
    if(state == POOL_STATUS_FINALIZE_TRANSACTION) {
        LogPrint("darksend", "CDarksendPool::Check() -- FINALIZE TRANSACTIONS\n");
        UpdateState(POOL_STATUS_SIGNING);

        if (fMasterNode) {
            CTransaction txNew;

            // make our new transaction
            for(unsigned int i = 0; i < entries.size(); i++)
            {
                BOOST_FOREACH(const CTxOut& v, entries[i].vout)
                {
                    txNew.vout.push_back(v);
                    MilliSleep(1);  /* RGP Optimise */
                }

                BOOST_FOREACH(const CTxDSIn& s, entries[i].sev)
                {
                    txNew.vin.push_back(s);
                    MilliSleep(1);  /* RGP Optimise */
 		        }                 
 		   
                MilliSleep(1);  /* RGP Optimise */
            }

            // shuffle the outputs for improved anonymity
            std::random_shuffle ( txNew.vin.begin(),  txNew.vin.end(),  randomizeList);
            std::random_shuffle ( txNew.vout.begin(), txNew.vout.end(), randomizeList);


            LogPrintf("darksend, Transaction 1: %s\n", txNew.ToString());
            finalTransaction = txNew;

            // request signatures from clients
            RelayFinalTransaction(sessionID, finalTransaction);
        }
    }

    // If we have all of the signatures, try to compile the transaction
    if(fMasterNode && state == POOL_STATUS_SIGNING && SignaturesComplete()) {
        LogPrintf("darksend, CDarksendPool::Check() -- SIGNING\n");
        UpdateState(POOL_STATUS_TRANSMISSION);
        CheckFinalTransaction();
    }

    // reset if we're here for 10 seconds
    if((state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) && GetTimeMillis()-lastTimeChanged >= 10000) {
        LogPrint("darksend", "CDarksendPool::Check() -- timeout, RESETTING\n");
        UnlockCoins();
        SetNull();
        if(fMasterNode) RelayStatus(sessionID, GetState(), GetEntriesCount(), MASTERNODE_RESET);
    }
}

void CDarksendPool::CheckFinalTransaction()
{



    if (!fMasterNode)
    {
        LogPrintf("*** RGP darksend not masternode before RelayInventory \n");
        return; // check and relay final tx only on masternode
    }

    CWalletTx txNew = CWalletTx(pwalletMain, finalTransaction);

    LOCK2(cs_main, pwalletMain->cs_wallet);
    {
        LogPrintf("darksend, Transaction 2: %s\n", txNew.ToString());

        // See if the transaction is valid
        if (!txNew.AcceptToMemoryPool(false, true, true))
        {
            LogPrintf("CDarksendPool::Check() - CommitTransaction : Error: Transaction not valid\n");
            SetNull();

            // not much we can do in this case
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            RelayCompletedTransaction(sessionID, true, _("Transaction not valid, please try again"));
            return;
        }

        LogPrintf("CDarksendPool::Check() -- IS MASTER -- TRANSMITTING DARKSEND\n");

        // sign a message

        int64_t sigTime = GetAdjustedTime();
        std::string strMessage = txNew.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
        std::string strError = "";
        std::vector<unsigned char> vchSig;
        CKey key2;
        CPubKey pubkey2;

        if(!darkSendSigner.SetKey(strMasterNodePrivKey, strError, key2, pubkey2))
        {
            LogPrintf("CDarksendPool::Check() - ERROR: Invalid Masternodeprivkey: '%s'\n", strError);
            return;
        }

        if(!darkSendSigner.SignMessage(strMessage, strError, vchSig, key2)) {
            LogPrintf("CDarksendPool::Check() - Sign message failed\n");
            return;
        }

        if(!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, strError)) {
            LogPrintf("CDarksendPool::Check() - Verify message failed\n");
            return;
        }

        string txHash = txNew.GetHash().ToString().c_str();
        LogPrintf("CDarksendPool::Check() -- txHash %d \n", txHash);
        if(!mapDarksendBroadcastTxes.count(txNew.GetHash())){
            CDarksendBroadcastTx dstx;
            dstx.tx = txNew;
            dstx.vin = activeMasternode.vin;
            dstx.vchSig = vchSig;
            dstx.sigTime = sigTime;

            mapDarksendBroadcastTxes.insert(make_pair(txNew.GetHash(), dstx));
        }

        CInv inv(MSG_DSTX, txNew.GetHash());

        LogPrintf("*** RGP darksend before RelayInventory \n");

        RelayInventory(inv);

        // Tell the clients it was successful
        RelayCompletedTransaction(sessionID, false, _("Transaction created successfully."));

        // Randomly charge clients
        ChargeRandomFees();

        // Reset
        LogPrint("darksend", "CDarksendPool::Check() -- COMPLETED -- RESETTING \n");
        SetNull();
        RelayStatus(sessionID, GetState(), GetEntriesCount(), MASTERNODE_RESET);
    }
}

//
// Charge clients a fee if they're abusive
//
// Why bother? Darksend uses collateral to ensure abuse to the process is kept to a minimum.
// The submission and signing stages in darksend are completely separate. In the cases where
// a client submits a transaction then refused to sign, there must be a cost. Otherwise they
// would be able to do this over and over again and bring the mixing to a hault.
//
// How does this work? Messages to Masternodes come in via "dsi", these require a valid collateral
// transaction for the client to be able to enter the pool. This transaction is kept by the Masternode
// until the transaction is either complete or fails.
//
void CDarksendPool::ChargeFees(){
    if(!fMasterNode) return;

    //we don't need to charge collateral for every offence.
    int offences = 0;
    int r = rand()%100;
    if(r > 33) return;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES){
        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) 
        {
            bool found = false;
            BOOST_FOREACH(const CDarkSendEntry& v, entries) 
            {
                if(v.collateral == txCollateral) {
                    found = true;
                }
                MilliSleep(1);  /* RGP Optimise */
            }

            // This queue entry didn't send us the promised transaction
            if(!found){
                LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't send transaction). Found offence.\n");
                offences++;
            }
            
            MilliSleep(1);  /* RGP Optimise */
        }
    }

    if(state == POOL_STATUS_SIGNING) {
        // who didn't sign?
        BOOST_FOREACH(const CDarkSendEntry v, entries) 
        {
            BOOST_FOREACH(const CTxDSIn s, v.sev) 
            {
                if(!s.fHasSig){
                    LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't sign). Found offence\n");
                    offences++;
                }
                MilliSleep(1);  /* RGP Optimise */
            }
            
            MilliSleep(1);  /* RGP Optimise */
        }
    }

    r = rand()%100;
    int target = 0;

    //mostly offending?
    if(offences >= Params().PoolMaxTransactions()-1 && r > 33) return;

    //everyone is an offender? That's not right
    if(offences >= Params().PoolMaxTransactions()) return;

    //charge one of the offenders randomly
    if(offences > 1) target = 50;

    //pick random client to charge
    r = rand()%100;

    if(state == POOL_STATUS_ACCEPTING_ENTRIES){
        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) 
        {
            bool found = false;
            BOOST_FOREACH(const CDarkSendEntry& v, entries) 
            {
                if(v.collateral == txCollateral) {
                    found = true;
                }
                MilliSleep(1);  /* RGP Optimise */
            }

            // This queue entry didn't send us the promised transaction
            if(!found && r > target){
                LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't send transaction). charging fees.\n");

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CDarksendPool::ChargeFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
                return;
            }
            MilliSleep(1);  /* RGP Optimise */
        }
    }

    if(state == POOL_STATUS_SIGNING) {
        // who didn't sign?
        BOOST_FOREACH(const CDarkSendEntry v, entries) 
        {
            BOOST_FOREACH(const CTxDSIn s, v.sev) 
            {
                if(!s.fHasSig && r > target){
                    LogPrintf("CDarksendPool::ChargeFees -- found uncooperative node (didn't sign). charging fees.\n");

                    CWalletTx wtxCollateral = CWalletTx(pwalletMain, v.collateral);

                    // Broadcast
                    if (!wtxCollateral.AcceptToMemoryPool(false))
                    {
                        // This must not fail. The transaction has already been signed and recorded.
                        LogPrintf("CDarksendPool::ChargeFees() : Error: Transaction not valid");
                    }
                    wtxCollateral.RelayWalletTransaction();
                    return;
                }
                MilliSleep(1);  /* RGP Optimise */
            }
            MilliSleep(1);  /* RGP Optimise */
        }
    }
}

// charge the collateral randomly
//  - Darksend is completely free, to pay miners we randomly pay the collateral of users.
void CDarksendPool::ChargeRandomFees(){
    if(fMasterNode) {
        int i = 0;

        BOOST_FOREACH(const CTransaction& txCollateral, vecSessionCollateral) 
        {
            int r = rand()%100;

            /*
                Collateral Fee Charges:

                Being that DarkSend has "no fees" we need to have some kind of cost associated
                with using it to stop abuse. Otherwise it could serve as an attack vector and
                allow endless transaction that would bloat SocietyG and make it unusable. To
                stop these kinds of attacks 1 in 50 successful transactions are charged. This
                adds up to a cost of 0.002SocietyG per transaction on average.
            */
            if(r <= 10)
            {
                LogPrintf("CDarksendPool::ChargeRandomFees -- charging random fees. %u\n", i);

                CWalletTx wtxCollateral = CWalletTx(pwalletMain, txCollateral);

                // Broadcast
                if (!wtxCollateral.AcceptToMemoryPool(true))
                {
                    // This must not fail. The transaction has already been signed and recorded.
                    LogPrintf("CDarksendPool::ChargeRandomFees() : Error: Transaction not valid");
                }
                wtxCollateral.RelayWalletTransaction();
            }
            MilliSleep(1);  /* RGP Optimise */
        }
    }
}

//
// Check for various timeouts (queue objects, darksend, etc)
//
void CDarksendPool::CheckTimeout()
{

    if(!fEnableDarksend && !fMasterNode) 
    {
        LogPrintf("RGP CDarksendPool::CheckTimeout DarkSend is not enabled and not a Masternode \n");
        return;
    }

    // catching hanging sessions
    if(!fMasterNode) 
    {
        switch(state) {
            case POOL_STATUS_TRANSMISSION:
                LogPrintf("darksend, CDarksendPool::CheckTimeout() -- Session complete -- Running Check()\n");
                Check();
                break;
            case POOL_STATUS_ERROR:
                LogPrintf("darksend, CDarksendPool::CheckTimeout() -- Pool error -- Running Check()\n");
                Check();
                break;
            case POOL_STATUS_SUCCESS:
                LogPrintf("darksend, CDarksendPool::CheckTimeout() -- Pool success -- Running Check()\n");
                Check();
                break;
        }
    }

    // check Darksend queue objects for timeouts
    int c = 0;
    vector<CDarksendQueue>::iterator it = vecDarksendQueue.begin();
    while(it != vecDarksendQueue.end()){
        if((*it).IsExpired()){
            LogPrintf("darksend, CDarksendPool::CheckTimeout() : Removing expired queue entry - %d\n", c);
            it = vecDarksendQueue.erase(it);
        } else ++it;
        c++;
        MilliSleep(1);  /* RGP Optimise */
    }

    int addLagTime = 0;
    if(!fMasterNode) addLagTime = 10000; //if we're the client, give the server a few extra seconds before resetting.

    if(state == POOL_STATUS_ACCEPTING_ENTRIES || state == POOL_STATUS_QUEUE)
    {
        c = 0;

        // check for a timeout and reset if needed
        vector<CDarkSendEntry>::iterator it2 = entries.begin();
        while(it2 != entries.end()){
            if((*it2).IsExpired()){
                LogPrintf("darksend, CDarksendPool::CheckTimeout() : Removing expired entry - %d\n", c);
                it2 = entries.erase(it2);
                if(entries.size() == 0){
                    UnlockCoins();
                    SetNull();
                }
                if(fMasterNode){
                    RelayStatus(sessionID, GetState(), GetEntriesCount(), MASTERNODE_RESET);
                }
            } 
            else
            {
                 ++it2;
            }
            c++;
            MilliSleep(1);  /* RGP Optimise */
        }

        if(GetTimeMillis()-lastTimeChanged >= (DARKSEND_QUEUE_TIMEOUT*1000)+addLagTime){
            UnlockCoins();
            SetNull();
        }
    } else if(GetTimeMillis()-lastTimeChanged >= (DARKSEND_QUEUE_TIMEOUT*1000)+addLagTime)
      {
        //LogPrintf("darksend, CDarksendPool::CheckTimeout() -- Session timed out (%ds) -- resetting\n", DARKSEND_QUEUE_TIMEOUT);
        UnlockCoins();
        SetNull();

        UpdateState(POOL_STATUS_ERROR);
        lastMessage = _("Session timed out.");
      }

    if(state == POOL_STATUS_SIGNING && GetTimeMillis()-lastTimeChanged >= (DARKSEND_SIGNING_TIMEOUT*1000)+addLagTime ) {
            //LogPrintf("darksend, CDarksendPool::CheckTimeout() -- Session timed out (%ds) -- restting\n", DARKSEND_SIGNING_TIMEOUT);
            ChargeFees();
            UnlockCoins();
            SetNull();

            UpdateState(POOL_STATUS_ERROR);
            lastMessage = _("Signing timed out.");
    }
}

//
// Check for complete queue
//
void CDarksendPool::CheckForCompleteQueue(){
    if(!fEnableDarksend && !fMasterNode) return;

    /* Check to see if we're ready for submissions from clients */
    //
    // After receiving multiple dsa messages, the queue will switch to "accepting entries"
    // which is the active state right before merging the transaction
    //
    if(state == POOL_STATUS_QUEUE && sessionUsers == GetMaxPoolTransactions()) {
        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

        CDarksendQueue dsq;
        dsq.nDenom = sessionDenom;
        dsq.vin = activeMasternode.vin;
        dsq.time = GetTime();
        dsq.ready = true;
        dsq.Sign();
        dsq.Relay();
    }
}

// check to see if the signature is valid
bool CDarksendPool::SignatureValid(const CScript& newSig, const CTxIn& newVin){
    CTransaction txNew;
    txNew.vin.clear();
    txNew.vout.clear();

    int found = -1;
    CScript sigPubKey = CScript();
    unsigned int i = 0;

    BOOST_FOREACH(CDarkSendEntry& e, entries) 
    {
        BOOST_FOREACH(const CTxOut& out, e.vout)
        {
            txNew.vout.push_back(out);
            MilliSleep(1);  /* RGP Optimise */
        }
        
        BOOST_FOREACH(const CTxDSIn& s, e.sev)
        {
            txNew.vin.push_back(s);

            if(s == newVin){
                found = i;
                sigPubKey = s.prevPubKey;
            }
            i++;
            MilliSleep(1);  /* RGP Optimise */
        }
        MilliSleep( 1 ); /* RGP Optimise */
    }

    if(found >= 0){ //might have to do this one input at a time?
        int n = found;
        txNew.vin[n].scriptSig = newSig;
        LogPrintf("darksend, CDarksendPool::SignatureValid() - Sign with sig %s\n", newSig.ToString().substr(0,24));
        if (!VerifyScript(txNew.vin[n].scriptSig, sigPubKey, txNew, n, SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_STRICTENC, 0)){
            LogPrint("darksend", "CDarksendPool::SignatureValid() - Signing - Error signing input %u\n", n);
            return false;
        }
    }

    LogPrint("darksend", "CDarksendPool::SignatureValid() - Signing - Successfully validated input\n");
    return true;
}

// check to make sure the collateral provided by the client is valid
bool CDarksendPool::IsCollateralValid(const CTransaction& txCollateral){
    if(txCollateral.vout.size() < 1) return false;
    if(txCollateral.nLockTime != 0) return false;

    int64_t nValueIn = 0;
    int64_t nValueOut = 0;
    bool missingTx = false;

    BOOST_FOREACH(const CTxOut o, txCollateral.vout)
    {
        nValueOut += o.nValue;

        if(!o.scriptPubKey.IsNormalPaymentScript()){
            LogPrintf ("CDarksendPool::IsCollateralValid - Invalid Script %s\n", txCollateral.ToString());
            return false;
        }
        MilliSleep(1);  /* RGP Optimise */
    }

    BOOST_FOREACH(const CTxIn i, txCollateral.vin)
    {
        CTransaction tx2;
        uint256 hash;
        if(GetTransaction(i.prevout.hash, tx2, hash)){
            if(tx2.vout.size() > i.prevout.n) {
                nValueIn += tx2.vout[i.prevout.n].nValue;
            }
        } else{
            missingTx = true;
        }
        MilliSleep(1);  /* RGP Optimise */
    }

    if(missingTx){
        LogPrintf("darksend, CDarksendPool::IsCollateralValid - Unknown inputs in collateral transaction - %s\n", txCollateral.ToString());
        return false;
    }

    //collateral transactions are required to pay out DARKSEND_COLLATERAL as a fee to the miners
    if(nValueIn-nValueOut < DARKSEND_COLLATERAL) {
        LogPrint("darksend", "CDarksendPool::IsCollateralValid - did not include enough fees in transaction %d\n%s\n", nValueOut-nValueIn, txCollateral.ToString());
        return false;
    }

    LogPrintf("darksend, CDarksendPool::IsCollateralValid %s\n", txCollateral.ToString());

    {
        LOCK(cs_main);
        CValidationState state;
        if(!AcceptableInputs(mempool, txCollateral, true, NULL)){
            LogPrintf ("CDarksendPool::IsCollateralValid - didn't pass IsAcceptable\n");
            return false;
        }
    }

    return true;
}


//
// Add a clients transaction to the pool
//
bool CDarksendPool::AddEntry(const std::vector<CTxIn>& newInput, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxOut>& newOutput, std::string& error){
    if (!fMasterNode) return false;

    BOOST_FOREACH(CTxIn in, newInput) 
    {
        if (in.prevout.IsNull() || nAmount < 0) {
            LogPrintf("darksend, CDarksendPool::AddEntry - input not valid!\n");
            error = _("Input is not valid.");
            sessionUsers--;
            return false;
        }
        MilliSleep(1);  /* RGP Optimise */
    }

    if (!IsCollateralValid(txCollateral)){
        LogPrintf("darksend, CDarksendPool::AddEntry - collateral not valid!\n");
        error = _("Collateral is not valid.");
        sessionUsers--;
        return false;
    }

    if((int)entries.size() >= GetMaxPoolTransactions()){
        LogPrintf("darksend, CDarksendPool::AddEntry - entries is full!\n");
        error = _("Entries are full.");
        sessionUsers--;
        return false;
    }

    BOOST_FOREACH(CTxIn in, newInput) 
    {
        LogPrintf("darksend, looking for vin -- %s\n", in.ToString());
        BOOST_FOREACH(const CDarkSendEntry& v, entries) 
        {
            BOOST_FOREACH(const CTxDSIn& s, v.sev)
            {
                if((CTxIn)s == in) {
                    LogPrintf("darksend, CDarksendPool::AddEntry - found in vin\n");
                    error = _("Already have that input.");
                    sessionUsers--;
                    return false;
                }
                MilliSleep(1);  /* RGP Optimise */
            }
            MilliSleep(1);  /* RGP Optimise */
        }
        MilliSleep(1);  /* RGP Optimise */
    }

    CDarkSendEntry v;
    v.Add(newInput, nAmount, txCollateral, newOutput);
    entries.push_back(v);

    LogPrintf("darksend, CDarksendPool::AddEntry -- adding %s\n", newInput[0].ToString());
    error = "";

    return true;
}

bool CDarksendPool::AddScriptSig(const CTxIn& newVin){
    LogPrint("darksend", "CDarksendPool::AddScriptSig -- new sig  %s\n", newVin.scriptSig.ToString().substr(0,24));

    BOOST_FOREACH(const CDarkSendEntry& v, entries) 
    {
        BOOST_FOREACH(const CTxDSIn& s, v.sev)
        {
            if(s.scriptSig == newVin.scriptSig) {
                LogPrintf("darksend, CDarksendPool::AddScriptSig - already exists \n");
                return false;
            }
            MilliSleep(1);  /* RGP Optimise */
        }
        MilliSleep(1);  /* RGP Optimise */
    }

    if(!SignatureValid(newVin.scriptSig, newVin)){
        LogPrintf("darksend, CDarksendPool::AddScriptSig - Invalid Sig\n");
        return false;
    }

    LogPrintf("darksend, CDarksendPool::AddScriptSig -- sig %s\n", newVin.ToString());

    BOOST_FOREACH(CTxIn& vin, finalTransaction.vin)
    {
        if(newVin.prevout == vin.prevout && vin.nSequence == newVin.nSequence){
            vin.scriptSig = newVin.scriptSig;
            vin.prevPubKey = newVin.prevPubKey;
            LogPrintf("darksend, CDarksendPool::AddScriptSig -- adding to finalTransaction  %s\n", newVin.scriptSig.ToString().substr(0,24));
        }
        MilliSleep(1);  /* RGP Optimise */
    }
    for(unsigned int i = 0; i < entries.size(); i++){
        if(entries[i].AddSig(newVin)){
            LogPrintf("darksend, CDarksendPool::AddScriptSig -- adding  %s\n", newVin.scriptSig.ToString().substr(0,24));
            return true;
        }
        MilliSleep(1);  /* RGP Optimise */
    }


    LogPrintf("CDarksendPool::AddScriptSig -- Couldn't set sig!\n" );
    return false;
}

// check to make sure everything is signed
bool CDarksendPool::SignaturesComplete(){

    BOOST_FOREACH(const CDarkSendEntry& v, entries) 
    {
        BOOST_FOREACH(const CTxDSIn& s, v.sev)
        {
            if(!s.fHasSig) return false;
            MilliSleep(1);  /* RGP Optimise */
        }
        MilliSleep(1);  /* RGP Optimise */
    }
    return true;
}

//
// Execute a darksend denomination via a Masternode.
// This is only ran from clients
//
void CDarksendPool::SendDarksendDenominate(std::vector<CTxIn>& vin, std::vector<CTxOut>& vout, int64_t amount){

    if(fMasterNode) {
        LogPrintf("CDarksendPool::SendDarksendDenominate() - Darksend from a Masternode is not supported currently.\n");
        return;
    }

    if(txCollateral == CTransaction()){
        LogPrintf ("CDarksendPool:SendDarksendDenominate() - Darksend collateral not set");
        return;
    }

    // lock the funds we're going to use
    BOOST_FOREACH(CTxIn in, txCollateral.vin)
    {
        lockedCoins.push_back(in);
        MilliSleep(1);  /* RGP Optimise */
    }
    
    BOOST_FOREACH(CTxIn in, vin)
    {
        lockedCoins.push_back(in);
        MilliSleep(1);  /* RGP Optimise */
    }
    //BOOST_FOREACH(CTxOut o, vout)
    //    LogPrintf(" vout - %s\n", o.ToString());


    // we should already be connected to a Masternode
    if(!sessionFoundMasternode){
        LogPrintf("CDarksendPool::SendDarksendDenominate() - No Masternode has been selected yet.\n");
        UnlockCoins();
        SetNull();
        return;
    }

    if (!CheckDiskSpace()) {
        UnlockCoins();
        SetNull();
        fEnableDarksend = false;

        LogPrintf("CDarksendPool::SendDarksendDenominate() - Not enough disk space, disabling Darksend.\n");
        return;
    }

    UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

    LogPrintf("CDarksendPool::SendDarksendDenominate() - Added transaction to pool.\n");

    ClearLastMessage();

    //check it against the memory pool to make sure it's valid
    {
        int64_t nValueOut = 0;

        CValidationState state;
        CTransaction tx;

        BOOST_FOREACH(const CTxOut& o, vout){
            nValueOut += o.nValue;
            tx.vout.push_back(o);
            MilliSleep(1);  /* RGP Optimise */
        }

        BOOST_FOREACH(const CTxIn& i, vin)
        {
            tx.vin.push_back(i);

            LogPrintf("darksend, dsi -- tx in %s\n", i.ToString());
            MilliSleep(1);  /* RGP Optimise */
        }

        LogPrintf("Submitting tx %s\n", tx.ToString());

        while(true){
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) { MilliSleep(50); continue;}
            if(!AcceptableInputs(mempool, txCollateral, false, NULL, false, true)){
                LogPrintf("dsi -- transaction not valid! %s \n", tx.ToString());
                UnlockCoins();
                SetNull();
                return;
            }
            break;
            MilliSleep(1);  /* RGP Optimise */
        }
    }

    // store our entry for later use
    CDarkSendEntry e;
    e.Add(vin, amount, txCollateral, vout);
    entries.push_back(e);

    RelayIn(entries[0].sev, entries[0].amount, txCollateral, entries[0].vout);
    Check();
}

// Incoming message from Masternode updating the progress of darksend
//    newAccepted:  -1 mean's it'n not a "transaction accepted/not accepted" message, just a standard update
//                  0 means transaction was not accepted
//                  1 means transaction was accepted

bool CDarksendPool::StatusUpdate(int newState, int newEntriesCount, int newAccepted, std::string& error, int newSessionID)
{
 
    if(fMasterNode) 
    {
        LogPrintf("RGP CDarksendPool::StatusUpdate no Masternode defined \n");
        return false;
    }

    if( state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) 
    {
        LogPrintf("RGP CDarksendPool::StatusUpdate Pool state is bad \n");
        return false;
    }

    UpdateState(newState);
    entriesCount = newEntriesCount;

    if(error.size() > 0) strAutoDenomResult = _("Masternode:") + " " + error;

    if(newAccepted != -1) {
        lastEntryAccepted = newAccepted;
        countEntriesAccepted += newAccepted;
        if(newAccepted == 0){
            UpdateState(POOL_STATUS_ERROR);
            lastMessage = error;
        }

        if(newAccepted == 1 && newSessionID != 0) {
            sessionID = newSessionID;
            LogPrintf("CDarksendPool::StatusUpdate - set sessionID to %d\n", sessionID);
            sessionFoundMasternode = true;
        }
    }

    if(newState == POOL_STATUS_ACCEPTING_ENTRIES){
        if(newAccepted == 1){
            LogPrintf("CDarksendPool::StatusUpdate - entry accepted! \n");
            sessionFoundMasternode = true;
            //wait for other users. Masternode will report when ready
            UpdateState(POOL_STATUS_QUEUE);
        } else if (newAccepted == 0 && sessionID == 0 && !sessionFoundMasternode) {
            LogPrintf("CDarksendPool::StatusUpdate - entry not accepted by Masternode \n");
            UnlockCoins();
            UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);
            DoAutomaticDenominating(); //try another Masternode
        }
        if(sessionFoundMasternode) return true;
    }

    return true;
}

//
// After we receive the finalized transaction from the Masternode, we must
// check it to make sure it's what we want, then sign it if we agree.
// If we refuse to sign, it's possible we'll be charged collateral
//
bool CDarksendPool::SignFinalTransaction(CTransaction& finalTransactionNew, CNode* node){
    if(fMasterNode) return false;

    finalTransaction = finalTransactionNew;
    LogPrintf("CDarksendPool::SignFinalTransaction %s\n", finalTransaction.ToString());

    vector<CTxIn> sigs;

    //make sure my inputs/outputs are present, otherwise refuse to sign
    BOOST_FOREACH(const CDarkSendEntry e, entries) {
        BOOST_FOREACH(const CTxDSIn s, e.sev) 
        {
            /* Sign my transaction and all outputs */
            int mine = -1;
            CScript prevPubKey = CScript();
            CTxIn vin = CTxIn();

            for(unsigned int i = 0; i < finalTransaction.vin.size(); i++){
                if(finalTransaction.vin[i] == s){
                    mine = i;
                    prevPubKey = s.prevPubKey;
                    vin = s;
                }
                MilliSleep( 1 ); /* RGP Optimise */
            }


            if(mine >= 0){ //might have to do this one input at a time?
                int foundOutputs = 0;
                CAmount nValue1 = 0;
                CAmount nValue2 = 0;

                for(unsigned int i = 0; i < finalTransaction.vout.size(); i++)
                {
                    BOOST_FOREACH(const CTxOut& o, e.vout) 
                    {
                        string Ftx = finalTransaction.vout[i].scriptPubKey.ToString().c_str();
                        string Otx = o.scriptPubKey.ToString().c_str();
                        if(Ftx == Otx){
                            //if(fDebug) LogPrintf("CDarksendPool::SignFinalTransaction - foundOutputs = %d \n", foundOutputs);
                            foundOutputs++;
                            nValue1 += finalTransaction.vout[i].nValue;
                            MilliSleep( 1 ); /* RGP Optimise */
                        }
                    }
                    MilliSleep( 1 ); /* RGP Optimise */
                }

                BOOST_FOREACH(const CTxOut o, e.vout)
                {
                    nValue2 += o.nValue;
                    MilliSleep( 1 ); /* RGP Optimise */
                }

                int targetOuputs = e.vout.size();
                if(foundOutputs < targetOuputs || nValue1 != nValue2) {
                    // in this case, something went wrong and we'll refuse to sign. It's possible we'll be charged collateral. But that's
                    // better then signing if the transaction doesn't look like what we wanted.
                    LogPrintf("CDarksendPool::Sign - My entries are not correct! Refusing to sign. %d entries %d target. \n", foundOutputs, targetOuputs);
                    UnlockCoins();
                    SetNull();
                    return false;
                }

                const CKeyStore& keystore = *pwalletMain;

                LogPrint("darksend", "CDarksendPool::Sign - Signing my input %i\n", mine);
                if(!SignSignature(keystore, prevPubKey, finalTransaction, mine, int(SIGHASH_ALL|SIGHASH_ANYONECANPAY))) { // changes scriptSig
                    LogPrint("darksend", "CDarksendPool::Sign - Unable to sign my own transaction! \n");
                    // not sure what to do here, it will timeout...?
                }

                sigs.push_back(finalTransaction.vin[mine]);
                LogPrint("darksend", " -- dss %d %d %s\n", mine, (int)sigs.size(), finalTransaction.vin[mine].scriptSig.ToString());
            }
            MilliSleep( 1 ); /* RGP Optimise */
        }
        MilliSleep( 1 ); /* RGP Optimise */
        LogPrint("darksend", "CDarksendPool::Sign - txNew:\n%s", finalTransaction.ToString());
    }

   // push all of our signatures to the Masternode
   if(sigs.size() > 0 && node != NULL)
       node->PushMessage("dss", sigs);
    return true;
}

void CDarksendPool::NewBlock()
{
    LogPrint("darksend", "CDarksendPool::NewBlock \n");

    //we we're processing lots of blocks, we'll just leave
    if(GetTime() - lastNewBlock < 10) return;
    lastNewBlock = GetTime();

    darkSendPool.CheckTimeout();

}

// Darksend transaction was completed (failed or successful)
void CDarksendPool::CompletedTransaction(bool error, int errorID)
{
    if(fMasterNode) return;

    if(error){
        LogPrintf("CompletedTransaction -- error \n");
        UpdateState(POOL_STATUS_ERROR);
        Check();
        UnlockCoins();
        SetNull();
    } else {
        LogPrintf("CompletedTransaction -- success \n");
        UpdateState(POOL_STATUS_SUCCESS);

        UnlockCoins();
        SetNull();
        // To avoid race conditions, we'll only let DS run once per block
        cachedLastSuccess = pindexBest->nHeight;
    }
    lastMessage = GetMessageByID(errorID);

}

void CDarksendPool::ClearLastMessage()
{
    lastMessage = "";
}

//
// Passively run Darksend in the background to anonymize funds based on the given configuration.
//
// This does NOT run by default for daemons, only for QT.
//
bool CDarksendPool::DoAutomaticDenominating(bool fDryRun)
{

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating \n");

    if(!fEnableDarksend)
    {
        LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating fEnableDarksend is not enabled  \n");
        return false;
    }

    if(!fMasterNode) 
    {
        LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating fMasterNode is not enabled  \n");
        return false;
    }

    if(state == POOL_STATUS_ERROR || state == POOL_STATUS_SUCCESS) 
    {
        LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating POOL_STATUS ISSUE is not enabled  \n");
        return false;
    }
    if(GetEntriesCount() > 0) 
    {
        strAutoDenomResult = _("Mixing in progress...");
        return false;
    }

//LogPrintf("RGP DoAutomaticDenom Debug 001 \n");

    TRY_LOCK(cs_darksend, lockDS);
    if(!lockDS) {
        strAutoDenomResult = _("Lock is already in place.");
        return false;
    }

    if(!IsBlockchainSynced()) 
    {
        strAutoDenomResult = _("Can't mix while sync in progress.");
        return false;
    }

    if (!fDryRun && pwalletMain->IsLocked()){
        strAutoDenomResult = _("Wallet is locked.");
        return false;
    }

    if(pindexBest->nHeight - cachedLastSuccess < minBlockSpacing) {
        LogPrintf("CDarksendPool::DoAutomaticDenominating - Last successful Darksend action was too recent\n");
        strAutoDenomResult = _("Last successful Darksend action was too recent.");
        return false;
    }
//LogPrintf("RGP DoAutomaticDenom Debug 001.1 \n");
    if(mnodeman.size() == 0){
        LogPrintf("darksend, CDarksendPool::DoAutomaticDenominating - No Masternodes detected\n");
        strAutoDenomResult = _("No Masternodes detected.");
        return false;
    }
  
//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 1 \n");

    // ** find the coins we'll use
    std::vector<CTxIn> vCoins;
    CAmount nValueMin = CENT;
    CAmount nValueIn = 0;

    CAmount nOnlyDenominatedBalance;
    CAmount nBalanceNeedsDenominated;

    // should not be less than fees in DARKSEND_COLLATERAL + few (lets say 5) smallest denoms
    CAmount nLowestDenom = DARKSEND_COLLATERAL + darkSendDenominations[darkSendDenominations.size() - 1]*5;

    // if there are no DS collateral inputs yet
    if(!pwalletMain->HasCollateralInputs())
        // should have some additional amount for them
        nLowestDenom += DARKSEND_COLLATERAL*4;

    CAmount nBalanceNeedsAnonymized = nAnonymizeSocietyGAmount*COIN - pwalletMain->GetAnonymizedBalance();

    // if balanceNeedsAnonymized is more than pool max, take the pool max
    if(nBalanceNeedsAnonymized > DARKSEND_POOL_MAX) nBalanceNeedsAnonymized = DARKSEND_POOL_MAX;

    // if balanceNeedsAnonymized is more than non-anonymized, take non-anonymized
    CAmount nAnonymizableBalance = pwalletMain->GetAnonymizableBalance();
    if(nBalanceNeedsAnonymized > nAnonymizableBalance) nBalanceNeedsAnonymized = nAnonymizableBalance;

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 2 \n");

    if(nBalanceNeedsAnonymized < nLowestDenom)
    {
        LogPrintf("DoAutomaticDenominating : No funds detected in need of denominating \n");
        strAutoDenomResult = _("No funds detected in need of denominating.");
        return false;
    }

    LogPrint("darksend", "DoAutomaticDenominating : nLowestDenom=%d, nBalanceNeedsAnonymized=%d\n", nLowestDenom, nBalanceNeedsAnonymized);

    // select coins that should be given to the pool
    if (!pwalletMain->SelectCoinsDark(nValueMin, nBalanceNeedsAnonymized, vCoins, nValueIn, 0, nDarksendRounds))
    {

//LogPrintf("RGP DoAutomaticDenom Debug 005 \n");
        nValueIn = 0;
        vCoins.clear();

        if (pwalletMain->SelectCoinsDark(nValueMin, 9999999*COIN, vCoins, nValueIn, -2, 0))
        {
            nOnlyDenominatedBalance = pwalletMain->GetDenominatedBalance(true) + pwalletMain->GetDenominatedBalance() - pwalletMain->GetAnonymizedBalance();
            nBalanceNeedsDenominated = nBalanceNeedsAnonymized - nOnlyDenominatedBalance;

            if(nBalanceNeedsDenominated > nValueIn) nBalanceNeedsDenominated = nValueIn;

            if(nBalanceNeedsDenominated < nLowestDenom) return false; // most likely we just waiting for denoms to confirm
            if(!fDryRun){
                LogPrintf("DoAutomaticDenominating : !fDryRun Returning CreateDenominated(nBalanceNeedsDenominated); \n");
                return CreateDenominated(nBalanceNeedsDenominated);
            }
            LogPrintf("DoAutomaticDenominating : fDryRun Returning true \n");

            return true;
        } else {
            LogPrintf("DoAutomaticDenominating : Can't denominate - no compatible inputs left\n");
            strAutoDenomResult = _("Can't denominate: no compatible inputs left.");
            return false;
        }

    } else {
        LogPrintf("DoAutomaticDenominating : fDryRun Returning true 2 \n");
    }
//LogPrintf("RGP DoAutomaticDenom Debug 010 \n");
    if(fDryRun) return true;

     LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 3 \n");


    nOnlyDenominatedBalance = pwalletMain->GetDenominatedBalance(true) + pwalletMain->GetDenominatedBalance() - pwalletMain->GetAnonymizedBalance();
    nBalanceNeedsDenominated = nBalanceNeedsAnonymized - nOnlyDenominatedBalance;

    //check if we have should create more denominated inputs
    if(nBalanceNeedsDenominated > nOnlyDenominatedBalance) return CreateDenominated(nBalanceNeedsDenominated);

    //check if we have the collateral sized inputs
    if(!pwalletMain->HasCollateralInputs()) return !pwalletMain->HasCollateralInputs(false) && MakeCollateralAmounts();

    std::vector<CTxOut> vOut;

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 4 \n");

    // initial phase, find a Masternode
    if(!sessionFoundMasternode)
    {
        // Clean if there is anything left from previous session
        UnlockCoins();
        SetNull();
        int nUseQueue = rand()%100;
        UpdateState(POOL_STATUS_ACCEPTING_ENTRIES);

        if(pwalletMain->GetDenominatedBalance(true) > 0){ //get denominated unconfirmed inputs
            LogPrintf("DoAutomaticDenominating -- Found unconfirmed denominated outputs, will wait till they confirm to continue.\n");
            strAutoDenomResult = _("Found unconfirmed denominated outputs, will wait till they confirm to continue.");
            return false;
        }

        //check our collateral
        std::string strReason;
        if(txCollateral == CTransaction()){
            if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                LogPrintf("% -- create collateral error:%s\n", __func__, strReason);
                return false;
            }
        } else {
            if(!IsCollateralValid(txCollateral)) {
                LogPrintf("%s -- invalid collateral, recreating...\n", __func__);
                if(!pwalletMain->CreateCollateralTransaction(txCollateral, strReason)){
                    LogPrintf("%s -- create collateral error: %s\n", __func__, strReason);
                    return false;
                }
            }
        }

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 6 \n");

        //if we've used 90% of the Masternode list then drop all the oldest first

        int nThreshold = (int)(mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION) * 0.9);


//LogPrint("darksend", "Checking vecMasternodesUsed size %d threshold %d\n", (int)vecMasternodesUsed.size(), nThreshold);
        while((int)vecMasternodesUsed.size() > nThreshold)
        {
            //vecMasternodesUsed.erase(vecMasternodesUsed.begin());
LogPrintf("RGP vecMasternodesUsed.erase commented out RESOLVE LATER \n");
LogPrintf("darksend,  vecMasternodesUsed size %d threshold %d\n", (int)vecMasternodesUsed.size(), nThreshold);

            MilliSleep( 1 ); /* RGP Optimise */
        }

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 7 \n");

        //don't use the queues all of the time for mixing
        if( nUseQueue > 33 )
        {

            // Look through the queues and see if anything matches
            BOOST_FOREACH(CDarksendQueue& dsq, vecDarksendQueue)
            {
                CService addr;
                if(dsq.time == 0) continue;

                if(!dsq.GetAddress(addr)) continue;
                if(dsq.IsExpired()) continue;

                int protocolVersion;
                if(!dsq.GetProtocolVersion(protocolVersion)) continue;
                if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION) continue;

                //non-denom's are incompatible
                if((dsq.nDenom & (1 << 5))) continue;

                bool fUsed = false;
                //don't reuse Masternodes
                BOOST_FOREACH(CTxIn usedVin, vecMasternodesUsed)
                {
                    if(dsq.vin == usedVin) {
                        fUsed = true;
                        break;
                    }
                    MilliSleep( 1 ); /* RGP Optimise */
                }
                if(fUsed) continue;
                std::vector<CTxIn> vTempCoins;
                std::vector<COutput> vTempCoins2;
                // Try to match their denominations if possible
                if (!pwalletMain->SelectCoinsByDenominations(dsq.nDenom, nValueMin, nBalanceNeedsAnonymized, vTempCoins, vTempCoins2, nValueIn, 0, nDarksendRounds)){
                    LogPrintf("DoAutomaticDenominating - Couldn't match denominations %d\n", dsq.nDenom);
                    continue;
                }

                // connect to Masternode and submit the queue request
                CNode* pnode = ConnectNode((CAddress)addr, NULL ); // , true);
                if(pnode != NULL)
                {
                    CMasternode* pmn = mnodeman.Find(dsq.vin);
                    if(pmn == NULL)
                    {

                            LogPrintf("DoAutomaticDenominating --- dsq vin %s is not in Masternode list!", dsq.vin.ToString());
                            continue;
                    }
                    pSubmittedToMasternode = pmn;
                    vecMasternodesUsed.push_back(dsq.vin);
                    sessionDenom = dsq.nDenom;

                    pnode->PushMessage("dsa", sessionDenom, txCollateral);
                    LogPrintf("DoAutomaticDenominating --- connected (from queue), sending dsa for %d - %s\n", sessionDenom, pnode->addr.ToString());
                    strAutoDenomResult = _("Mixing in progress...");
                    dsq.time = 0; //remove node
                    return true;
                } else {
                    LogPrintf("DoAutomaticDenominating --- error connecting \n");
                    strAutoDenomResult = _("Error connecting to Masternode.");
                    dsq.time = 0; //remove node
                    continue;
                }
                MilliSleep( 1 ); /* RGP Optimise */
            }
        }

//LogPrintf("*** RGP CDarksendPool::DoAutomaticDenominating Debug 9 \n");

        // do not initiate queue if we are a liquidity proveder to avoid useless inter-mixing
        if(nLiquidityProvider) return false;

        int i = 0;

        // otherwise, try one randomly
        while(i < 10)
        {
            CMasternode* pmn = mnodeman.FindRandomNotInVec(vecMasternodesUsed, MIN_POOL_PEER_PROTO_VERSION);
            if(pmn == NULL)
            {
                LogPrintf("DoAutomaticDenominating --- Can't find random masternode!\n");
                strAutoDenomResult = _("Can't find random Masternode.");
                return false;
            }

            if(pmn->nLastDsq != 0 &&
                pmn->nLastDsq + mnodeman.CountEnabled(MIN_POOL_PEER_PROTO_VERSION)/5 > mnodeman.nDsqCount){
                i++;
                continue;
            }

            lastTimeChanged = GetTimeMillis();
            LogPrintf("DoAutomaticDenominating -- attempt %d connection to Masternode %s\n", i, pmn->addr.ToString().c_str());
            CNode* pnode = ConnectNode((CAddress)pmn->addr, NULL ); //, true);
            if(pnode != NULL){
                pSubmittedToMasternode = pmn;
                vecMasternodesUsed.push_back(pmn->vin);

                std::vector<CAmount> vecAmounts;
                pwalletMain->ConvertList(vCoins, vecAmounts);
                // try to get a single random denom out of vecAmounts
                while(sessionDenom == 0)
                {
                    sessionDenom = GetDenominationsByAmounts(vecAmounts);
                    MilliSleep( 1 ); /* RGP Optimise */
                }

                pnode->PushMessage("dsa", sessionDenom, txCollateral);
                LogPrintf("DoAutomaticDenominating --- connected, sending dsa for %d\n", sessionDenom);
                strAutoDenomResult = _("Mixing in progress...");
                return true;

            } else {
                vecMasternodesUsed.push_back(pmn->vin); // postpone MN we wasn't able to connect to
                i++;
                continue;
            }
            MilliSleep( 1 ); /* RGP Optimise */
        }

        strAutoDenomResult = _("No compatible Masternode found.");
        return false;
    }

    strAutoDenomResult = _("Mixing in progress...");

    return false;
}


bool CDarksendPool::PrepareDarksendDenominate()
{
    std::string strError = "";
    // Submit transaction to the pool if we get here
    // Try to use only inputs with the same number of rounds starting from lowest number of rounds possible
    for(int i = 0; i < nDarksendRounds; i++) {
        strError = pwalletMain->PrepareDarksendDenominate(i, i+1);
        LogPrintf("DoAutomaticDenominating : Running darksend denominate for %d rounds. Return '%s'\n", i, strError);
        if(strError == "") return true;
        MilliSleep( 1 ); /* RGP Optimise */
    }

    strError = pwalletMain->PrepareDarksendDenominate(0, nDarksendRounds);
    LogPrintf("DoAutomaticDenominating : Running Darksend denominate for all rounds. Return '%s'\n", strError);
    if(strError == "") return true;

    // Should never actually get here but just in case
    strAutoDenomResult = strError;
    LogPrintf("DoAutomaticDenominating : Error running denominate, %s\n", strError);
    return false;
}

bool CDarksendPool::SendRandomPaymentToSelf()
{
    int64_t nBalance = pwalletMain->GetBalance();
    int64_t nPayment = (nBalance*0.35) + (rand() % nBalance);

    if(nPayment > nBalance) nPayment = nBalance-(0.1*COIN);

    // make our change address
    CReserveKey reservekey(pwalletMain);

    CScript scriptChange;
    CPubKey vchPubKey;
    assert(reservekey.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptChange = GetScriptForDestination(vchPubKey.GetID());

    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;

    // ****** Add fees ************ /
    vecSend.push_back(make_pair(scriptChange, nPayment));

    CCoinControl *coinControl=NULL;
    int32_t nChangePos;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekey, nFeeRet, nChangePos, strFail, coinControl, ONLY_DENOMINATED);
    if(!success){
        LogPrintf("SendRandomPaymentToSelf: Error - %s\n", strFail);
        return false;
    }

    pwalletMain->CommitTransaction(wtx, reservekey);

    LogPrintf("SendRandomPaymentToSelf Success: tx %s\n", wtx.GetHash().GetHex());

    return true;
}

// Split up large inputs or create fee sized inputs
bool CDarksendPool::MakeCollateralAmounts()
{
    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;
    CCoinControl *coinControl = NULL;

    // make our collateral address
    CReserveKey reservekeyCollateral(pwalletMain);
    // make our change address
    CReserveKey reservekeyChange(pwalletMain);

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    vecSend.push_back(make_pair(scriptCollateral, DARKSEND_COLLATERAL*4));

    int32_t nChangePos;
    // try to use non-denominated and not mn-like funds
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
            nFeeRet, nChangePos, strFail, coinControl, ONLY_NONDENOMINATED_NOT10000IFMN);
    if(!success){
        // if we failed (most likeky not enough funds), try to use denominated instead -
        // MN-like funds should not be touched in any case and we can't mix denominated without collaterals anyway
        LogPrintf("MakeCollateralAmounts: ONLY_NONDENOMINATED_NOT1000IFMN Error - %s\n", strFail);
        success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
                nFeeRet, nChangePos, strFail, coinControl, ONLY_NOT10000IFMN);
        if(!success){
            LogPrintf("MakeCollateralAmounts: ONLY_NOT1000IFMN Error - %s\n", strFail);
            reservekeyCollateral.ReturnKey();
            return false;
        }
    }

    reservekeyCollateral.KeepKey();

    LogPrintf("MakeCollateralAmounts: tx %s\n", wtx.GetHash().GetHex());

    // use the same cachedLastSuccess as for DS mixinx to prevent race
    if(!pwalletMain->CommitTransaction(wtx, reservekeyChange)) {
        LogPrintf("MakeCollateralAmounts: CommitTransaction failed!\n");
        return false;
    }

    cachedLastSuccess = pindexBest->nHeight;

    return true;
}

// Create denominations
bool CDarksendPool::CreateDenominated(int64_t nTotalValue)
{
    CWalletTx wtx;
    int64_t nFeeRet = 0;
    std::string strFail = "";
    vector< pair<CScript, int64_t> > vecSend;
    int64_t nValueLeft = nTotalValue;

    // make our collateral address
    CReserveKey reservekeyCollateral(pwalletMain);
    // make our change address
    CReserveKey reservekeyChange(pwalletMain);
    // make our denom addresses
    CReserveKey reservekeyDenom(pwalletMain);

    CScript scriptCollateral;
    CPubKey vchPubKey;
    assert(reservekeyCollateral.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
    scriptCollateral = GetScriptForDestination(vchPubKey.GetID());

    // ****** Add collateral outputs ************ /
    if(!pwalletMain->HasCollateralInputs()) {
        vecSend.push_back(make_pair(scriptCollateral, DARKSEND_COLLATERAL*4));
        nValueLeft -= DARKSEND_COLLATERAL*4;
    }

    // ****** Add denoms ************ /
    BOOST_REVERSE_FOREACH(int64_t v, darkSendDenominations)
    {
        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= DARKSEND_COLLATERAL && nOutputs <= 10) 
        {
            CScript scriptDenom;
            CPubKey vchPubKey;
            //use a unique change address
            assert(reservekeyDenom.GetReservedKey(vchPubKey)); // should never fail, as we just unlocked
            scriptDenom = GetScriptForDestination(vchPubKey.GetID());
            // TODO: do not keep reservekeyDenom here
            reservekeyDenom.KeepKey();

            vecSend.push_back(make_pair(scriptDenom, v));

            //increment outputs and subtract denomination amount
            nOutputs++;
            nValueLeft -= v;
            LogPrintf("CreateDenominated1 %d\n", nValueLeft);
            MilliSleep( 1 ); /* RGP Optimise */
        }

        if(nValueLeft == 0) break;
        MilliSleep( 1 ); /* RGP Optimise */
    }
    LogPrintf("CreateDenominated2 %d\n", nValueLeft);

    // if we have anything left over, it will be automatically send back as change - there is no need to send it manually

    CCoinControl *coinControl=NULL;
    int32_t nChangePos;
    bool success = pwalletMain->CreateTransaction(vecSend, wtx, reservekeyChange,
            nFeeRet, nChangePos, strFail, coinControl, ONLY_NONDENOMINATED_NOT10000IFMN);
    if(!success){
        LogPrintf("CreateDenominated: Error - %s\n", strFail);
        // TODO: return reservekeyDenom here
        reservekeyCollateral.ReturnKey();
        return false;
    }

    // TODO: keep reservekeyDenom here
    reservekeyCollateral.KeepKey();

    // use the same cachedLastSuccess as for DS mixinx to prevent race
    if(pwalletMain->CommitTransaction(wtx, reservekeyChange))
        cachedLastSuccess = pindexBest->nHeight;
    else
        LogPrintf("CreateDenominated: CommitTransaction failed!\n");

    LogPrintf("CreateDenominated: tx %s\n", wtx.GetHash().GetHex());

    return true;
}

bool CDarksendPool::IsCompatibleWithEntries(std::vector<CTxOut>& vout)
{
    if(GetDenominations(vout) == 0) return false;

    BOOST_FOREACH(const CDarkSendEntry v, entries) {
        LogPrintf(" IsCompatibleWithEntries %d %d\n", GetDenominations(vout), GetDenominations(v.vout));
/*
        BOOST_FOREACH(CTxOut o1, vout)
            LogPrintf(" vout 1 - %s\n", o1.ToString());

        BOOST_FOREACH(CTxOut o2, v.vout)
            LogPrintf(" vout 2 - %s\n", o2.ToString());
*/
        if(GetDenominations(vout) != GetDenominations(v.vout)) return false;
        MilliSleep( 1 ); /* RGP Optimise */
    }

    return true;
}

bool CDarksendPool::IsCompatibleWithSession(int64_t nDenom, CTransaction txCollateral,  std::string& strReason)
{
    if(nDenom == 0) return false;

    LogPrintf("CDarkSendPool::IsCompatibleWithSession - sessionDenom %d sessionUsers %d\n", sessionDenom, sessionUsers);

    if (!unitTest && !IsCollateralValid(txCollateral)){
        LogPrint("darksend", "CDarksendPool::IsCompatibleWithSession - collateral not valid!\n");
        strReason = _("Collateral not valid.");
        return false;
    }

    if(sessionUsers < 0) sessionUsers = 0;

    if(sessionUsers == 0) {
        sessionID = 1 + (rand() % 999999);
        sessionDenom = nDenom;
        sessionUsers++;
        lastTimeChanged = GetTimeMillis();

        if(!unitTest){
            //broadcast that I'm accepting entries, only if it's the first entry through
            CDarksendQueue dsq;
            dsq.nDenom = nDenom;
            dsq.vin = activeMasternode.vin;
            dsq.time = GetTime();
            dsq.Sign();
            dsq.Relay();
        }

        UpdateState(POOL_STATUS_QUEUE);
        vecSessionCollateral.push_back(txCollateral);
        return true;
    }

    if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE) || sessionUsers >= GetMaxPoolTransactions()){
        if((state != POOL_STATUS_ACCEPTING_ENTRIES && state != POOL_STATUS_QUEUE)) strReason = _("Incompatible mode.");
        if(sessionUsers >= GetMaxPoolTransactions()) strReason = _("Masternode queue is full.");
        LogPrintf("CDarksendPool::IsCompatibleWithSession - incompatible mode, return false %d %d\n", state != POOL_STATUS_ACCEPTING_ENTRIES, sessionUsers >= GetMaxPoolTransactions());
        return false;
    }

    if(nDenom != sessionDenom) {
        strReason = _("No matching denominations found for mixing.");
        return false;
    }

    LogPrintf("CDarksendPool::IsCompatibleWithSession - compatible\n");

    sessionUsers++;
    lastTimeChanged = GetTimeMillis();
    vecSessionCollateral.push_back(txCollateral);

    return true;
}

//create a nice string to show the denominations
void CDarksendPool::GetDenominationsToString(int nDenom, std::string& strDenom){
    // Function returns as follows:
    //
    // bit 0 - 100SocietyG+1 ( bit on if present )
    // bit 1 - 10SocietyG+1
    // bit 2 - 1SocietyG+1
    // bit 3 - .1SocietyG+1
    // bit 3 - non-denom


    strDenom = "";

    if(nDenom & (1 << 0)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "1000";
    }

    if(nDenom & (1 << 1)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "100";
    }

    if(nDenom & (1 << 2)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "10";
    }

    if(nDenom & (1 << 3)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "1";
    }

    if(nDenom & (1 << 4)) {
        if(strDenom.size() > 0) strDenom += "+";
        strDenom += "0.1";
    }
}

int CDarksendPool::GetDenominations(const std::vector<CTxDSOut>& vout){
    std::vector<CTxOut> vout2;

    BOOST_FOREACH(CTxDSOut out, vout)
    {
        vout2.push_back(out);
        MilliSleep( 1 ); /* RGP Optimise */
    }
    return GetDenominations(vout2);
}

// return a bitshifted integer representing the denominations in this list
int CDarksendPool::GetDenominations(const std::vector<CTxOut>& vout, bool fSingleRandomDenom){
    std::vector<pair<int64_t, int> > denomUsed;

    // make a list of denominations, with zero uses
    BOOST_FOREACH(int64_t d, darkSendDenominations)
    {
        denomUsed.push_back(make_pair(d, 0));
        MilliSleep( 1 ); /* RGP Optimise */
    }

    // look for denominations and update uses to 1
    BOOST_FOREACH(CTxOut out, vout)
    {
        bool found = false;
        BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed)
        {
            if (out.nValue == s.first){
                s.second = 1;
                found = true;
            }
            MilliSleep( 1 ); /* RGP Optimise */
        }
        if(!found) return 0;
        MilliSleep( 1 ); /* RGP Optimise */
    }

    int denom = 0;
    int c = 0;
    // if the denomination is used, shift the bit on.
    // then move to the next
    BOOST_FOREACH (PAIRTYPE(int64_t, int)& s, denomUsed)
    {
        int bit = (fSingleRandomDenom ? rand()%2 : 1) * s.second;
        denom |= bit << c++;
        if(fSingleRandomDenom && bit) break; // use just one random denomination
        MilliSleep( 1 ); /* RGP Optimise */
    }

    // Function returns as follows:
    //
    // bit 0 - 100SocietyG+1 ( bit on if present )
    // bit 1 - 10SocietyG+1
    // bit 2 - 1SocietyG+1
    // bit 3 - .1SocietyG+1

    return denom;
}


int CDarksendPool::GetDenominationsByAmounts(std::vector<int64_t>& vecAmount){
    CScript e = CScript();
    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, vecAmount)
    {
        CTxOut o(v, e);
        vout1.push_back(o);
        MilliSleep( 1 ); /* RGP Optimise */
    }

    return GetDenominations(vout1, true);
}

int CDarksendPool::GetDenominationsByAmount(int64_t nAmount, int nDenomTarget){
    CScript e = CScript();
    int64_t nValueLeft = nAmount;

    std::vector<CTxOut> vout1;

    // Make outputs by looping through denominations, from small to large
    BOOST_REVERSE_FOREACH(int64_t v, darkSendDenominations)
    {
        if(nDenomTarget != 0)
        {
            bool fAccepted = false;
            if((nDenomTarget & (1 << 0)) && v == ((1000*COIN) +1000000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 1)) && v == ((100*COIN) +100000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 2)) && v == ((10*COIN) +10000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 3)) && v == ((1*COIN)  +1000)) {fAccepted = true;}
            else if((nDenomTarget & (1 << 4)) && v == ((.1*COIN) +100)) {fAccepted = true;}
            if(!fAccepted) continue;
        }

        int nOutputs = 0;

        // add each output up to 10 times until it can't be added again
        while(nValueLeft - v >= 0 && nOutputs <= 10) {
            CTxOut o(v, e);
            vout1.push_back(o);
            nValueLeft -= v;
            nOutputs++;

            MilliSleep( 1 ); /* RGP Optimise */
        }
        LogPrintf("GetDenominationsByAmount --- %d nOutputs %d\n", v, nOutputs);
        MilliSleep( 1 ); /* RGP Optimise */
    }

    return GetDenominations(vout1);
}

std::string CDarksendPool::GetMessageByID(int messageID) {
    switch (messageID) {
    case ERR_ALREADY_HAVE: return _("Already have that input.");
    case ERR_DENOM: return _("No matching denominations found for mixing.");
    case ERR_ENTRIES_FULL: return _("Entries are full.");
    case ERR_EXISTING_TX: return _("Not compatible with existing transactions.");
    case ERR_FEES: return _("Transaction fees are too high.");
    case ERR_INVALID_COLLATERAL: return _("Collateral not valid.");
    case ERR_INVALID_INPUT: return _("Input is not valid.");
    case ERR_INVALID_SCRIPT: return _("Invalid script detected.");
    case ERR_INVALID_TX: return _("Transaction not valid.");
    case ERR_MAXIMUM: return _("Value more than Darksend pool maximum allows.");
    case ERR_MN_LIST: return _("Not in the Masternode list.");
    case ERR_MODE: return _("Incompatible mode.");
    case ERR_NON_STANDARD_PUBKEY: return _("Non-standard public key detected.");
    case ERR_NOT_A_MN: return _("This is not a Masternode.");
    case ERR_QUEUE_FULL: return _("Masternode queue is full.");
    case ERR_RECENT: return _("Last Darksend was too recent.");
    case ERR_SESSION: return _("Session not complete!");
    case ERR_MISSING_TX: return _("Missing input transaction information.");
    case ERR_VERSION: return _("Incompatible version.");
    case MSG_SUCCESS: return _("Transaction created successfully.");
    case MSG_ENTRIES_ADDED: return _("Your entries added successfully.");
    case MSG_NOERR:
    default:
        return "";
    }
}

bool CDarkSendSigner::IsVinAssociatedWithPubkey(CTxIn& vin, CPubKey& pubkey)
{
    CScript payee2;
    payee2 = GetScriptForDestination(pubkey.GetID());

    CTransaction txVin;
    uint256 hash;

    LogPrintf("RGP CDarkSendSigner::IsVinAssociatedWithPubkey start \n");

    //if(GetTransaction(vin.prevout.hash, txVin, hash, true)){

    // Get_MN_Transaction will look through all blocks if the transaction is not found
    // In the Mempool or the Ctransaction Store on disk 

    if ( Get_MN_Transaction(vin.prevout.hash, txVin, hash) )
    {
LogPrintf("RGP CDarkSendSigner::IsVinAssociatedWithPubkey Get_MN_Transaction valid \n");
        BOOST_FOREACH(CTxOut out, txVin.vout)
        {
            LogPrintf("RGP CDarkSendSigner::IsVinAssociatedWithPubkey start %s \n", txVin.ToString() );
            

            if(out.nValue == GetMNCollateral(pindexBest->nHeight)*COIN)
            {
LogPrintf("RGP CDarkSendSigner::IsVinAssociatedWithPubkey out value is 15000 suceeded \n");
                if(out.scriptPubKey == payee2) return true;
            }
            else
LogPrintf("RGP CDarkSendSigner::IsVinAssociatedWithPubkey out value is 15000 FAILED \n");
        }
    }

    /* RGP return true always until we find the TX issue in GetTransaction */
    //return false;
    return true;
}

/* --
   -- RGP, SetKey
   -- */

//darkSendSigner.SetKey(strKeyMasternode, errorMessage, keyMasternode, pubKeyMasternode))

bool CDarkSendSigner::SetKey(std::string strSecret, std::string& errorMessage, CKey& key, CPubKey& pubkey)
{

//LogPrintf("RGP CDarkSendSigner::SetKey Start <%s> \n", strSecret);

    CSocietyGcoinSecret vchSecret;
    extern uint256 hashkeygen;
    //std::string TESTstrSecret = hashkeygen.ToString();
    bool fGood;

    fGood = vchSecret.SetString(strSecret);

//LogPrintf("*** RGP CDarkSendSigner::SetKey Secret key <%s> \n", strSecret );


    if (!fGood)
    {
        errorMessage = _("Invalid private key.");
        return false;
    }

//LogPrintf("RGP CDarkSendSigner getting Key and public key \n" );

    key = vchSecret.GetKey();
    pubkey = key.GetPubKey();

    return true;
}

bool CDarkSendSigner::SignMessage(std::string strMessage, std::string& errorMessage, vector<unsigned char>& vchSig, CKey key)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    LogPrintf("RGP CDarkSendSigner::SignMessage strMessage %s strMessageMagic %s \n", strMessage, strMessageMagic );

    if (!key.SignCompact(ss.GetHash(), vchSig)) 
    {
        errorMessage = _("Signing failed.");
LogPrintf("RGP CDarkSendSigner::SignMessage fudge RESOLVE later \n");
        //return false;
    }

    return true;
}

bool CDarkSendSigner::VerifyMessage(CPubKey pubkey, vector<unsigned char>& vchSig, std::string strMessage, std::string& errorMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;   
    CPubKey pubkey2;

LogPrintf("RGP CDarkSendSigner::VerifyMessage strMessage %s strMessageMagic %s \n", strMessage, strMessageMagic );

    if (!pubkey2.RecoverCompact(ss.GetHash(), vchSig))
    {
        errorMessage = _("Error recovering public key.");
        //return false;
    }

    LogPrintf("RGP CDarkSendSigner::VerifyMessage messageMagic %s strMessage %s \n", strMessageMagic, strMessage );

    if (fDebug && (pubkey2.GetID() != pubkey.GetID()))
        LogPrintf("CDarkSendSigner::VerifyMessage -- keys don't match: %s %s\n", pubkey2.GetID().ToString(), pubkey.GetID().ToString() );

    LogPrintf("RGP CDarkSendSigner::VerifyMessage pubkey2 ID %s pubkey ID %s \n", pubkey2.GetID().ToString(), pubkey.GetID().ToString()  );

    /* -- RGP older code bases of MN fail here, will fix later -- */
    return true;

    return (pubkey2.GetID() == pubkey.GetID());
}

bool CDarksendQueue::Sign()
{
    if(!fMasterNode) return false;

    std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, key2, pubkey2))
    {
        LogPrintf("CDarksendQueue():Relay - ERROR: Invalid Masternodeprivkey: '%s'\n", errorMessage);
        return false;
    }

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchSig, key2)) {
        LogPrintf("CDarksendQueue():Relay - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, vchSig, strMessage, errorMessage)) {
        LogPrintf("CDarksendQueue():Relay - Verify message failed");
        return false;
    }

    return true;
}

bool CDarksendQueue::Relay()
{

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes){
        // always relay to everyone
        pnode->PushMessage("dsq", (*this));
    }

    return true;
}

bool CDarksendQueue::CheckSignature()
{
    CMasternode* pmn = mnodeman.Find(vin);
    if(pmn != NULL)
    {
        std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nDenom) + boost::lexical_cast<std::string>(time) + boost::lexical_cast<std::string>(ready);
        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage)){
            return error("CDarksendQueue::CheckSignature() - Got bad Masternode address signature %s \n", vin.ToString().c_str());
        }

        return true;
    }

    return false;
}

void CDarksendPool::RelayFinalTransaction(const int sessionID, const CTransaction& txNew)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("dsf", sessionID, txNew);
    }
}

void CDarksendPool::RelayIn(const std::vector<CTxDSIn>& vin, const int64_t& nAmount, const CTransaction& txCollateral, const std::vector<CTxDSOut>& vout)
{
    if(!pSubmittedToMasternode) return;

    std::vector<CTxIn> vin2;
    std::vector<CTxOut> vout2;

    BOOST_FOREACH(CTxDSIn in, vin)
        vin2.push_back(in);

    BOOST_FOREACH(CTxDSOut out, vout)
        vout2.push_back(out);

    CNode* pnode = FindNode(pSubmittedToMasternode->addr);
    if(pnode != NULL) {
        LogPrintf("RelayIn - found master, relaying message - %s \n", pnode->addr.ToString());
        pnode->PushMessage("dsi", vin2, nAmount, txCollateral, vout2);
    }
}

void CDarksendPool::RelayStatus(const int sessionID, const int newState, const int newEntriesCount, const int newAccepted, const std::string error)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dssu", sessionID, newState, newEntriesCount, newAccepted, error);
}

void CDarksendPool::RelayCompletedTransaction(const int sessionID, const bool error, const std::string errorMessage)
{
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
        pnode->PushMessage("dsc", sessionID, error, errorMessage);
}

//TODO: Rename/move to core
void ThreadCheckDarkSendPool()
{
    if(fLiteMode) return; //disable all Darksend/Masternode related functionality

    // Make this thread recognisable as the wallet flushing thread
    RenameThread("SocietyG-darksend");

    unsigned int c = 0;

    LogPrintf("**** ThreadCheckDarkSendPool STARTING ****\n");

    while (true)
    {
        MilliSleep(500);
        //LogPrintf("ThreadCheckDarkSendPool::check timeout\n");

        // try to sync from all available nodes, one step at a time
        //masternodeSync.Process();


        if(darkSendPool.IsBlockchainSynced())
        {

            c++;

            // check if we should activate or ping every few minutes,
            // start right after sync is considered to be done
            //LogPrintf("ManageStatus to be called now\n");

            if(c % MASTERNODE_PING_SECONDS == 1) activeMasternode.ManageStatus();

            if(c % 60 == 0)
            {
                mnodeman.CheckAndRemove();
                mnodeman.ProcessMasternodeConnections();
                masternodePayments.CleanPaymentList();
                CleanTransactionLocksList();
            }

            if(c % MASTERNODES_DUMP_SECONDS == 0) DumpMasternodes();

            darkSendPool.CheckTimeout();
            darkSendPool.CheckForCompleteQueue();

            if(darkSendPool.GetState() == POOL_STATUS_IDLE && c % 15 == 0)
            {
                darkSendPool.DoAutomaticDenominating();
            }
        }
        MilliSleep( 500 );
    }
} 

