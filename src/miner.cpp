// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2013 The NovaCoin developers
// Copyright (c) 2018 Profit Hunters Coin developers
// Copyright (c) 2022 Bank Society Gold developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "txdb.h"
#include "main.h"
#include "miner.h"
#include "kernel.h"
#include "masternodeman.h"
#include "masternode-payments.h"
#include "checkpoints.h"

#include "arith_uint256.h"

#include <string>

#include <stdlib.h>

#include "semaphore.h"

Semaphore thread_semaphore;
//extern std::mutex startup_lock;

using namespace std;

double dHashesPerSec = 0.0;

int64_t nHPSTimerStart = 0;

bool fDebugConsoleOutputMining = false;

std::string MinerLogCache;

/* RGP JIRA BSG-181 */
static int processed_stakes = 0;
unsigned int last_time_staked = 0;

//////////////////////////////////////////////////////////////////////////////
//
// Miner Functions
//

extern unsigned int nMinerSleep;

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;

    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;

    unsigned int bits = len * 8;

    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;

    return blocks;
}


static const unsigned int pSHA256InitState[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};


void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;

    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
    {
        LogPrintf("*** RGP miner loop 1 \n");
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);
    }

    for (int i = 0; i < 8; i++)
    {
        LogPrintf("*** RGP miner  loop 2 \n");
        ctx.h[i] = ((uint32_t*)pinit)[i];
    }

    SHA256_Update(&ctx, data, sizeof(data));

    for (int i = 0; i < 8; i++)
    {
        LogPrintf("*** RGP miner loop 3 \n");
        ((uint32_t*)pstate)[i] = ctx.h[i];
    }
}


// Some explaining would be appreciated
class COrphan
{
    public:

        CTransaction* ptx;
        set<uint256> setDependsOn;

        double dPriority;
        double dFeePerKb;

        COrphan(CTransaction* ptxIn)
        {
            ptx = ptxIn;
            dPriority = dFeePerKb = 0;
        }
};


uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;
int64_t nLastCoinStakeSearchInterval = 0;


// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority; class TxPriorityCompare
{
    bool byFee;

    public:

        TxPriorityCompare(bool _byFee) : byFee(_byFee)
        { }
        
        bool operator()(const TxPriority& a, const TxPriority& b)
        {
            if (byFee)
            {
                if (a.get<1>() == b.get<1>())
                {
                    return a.get<0>() < b.get<0>();
                }

                return a.get<1>() < b.get<1>();
            }
            else
            {
                if (a.get<0>() == b.get<0>())
                {
                    return a.get<1>() < b.get<1>();
                }

                return a.get<0>() < b.get<0>();
            }
        }
};


CBlock* CreateNewBlockWithKey(CReserveKey& reservekey, CWallet *pwallet)
{
    int64_t pFees = 0;

    CPubKey pubkey;
    if (!reservekey.GetReservedKey(pubkey))
        return NULL;

    CScript scriptPubKey = CScript() << ToByteVector(pubkey) << OP_CHECKSIG;

    // Create new block
    /* RGP, replaced auto_ptr with unique_ptr */
    /* auto_ptr<CBlock> pblock(new CBlock()); */
    unique_ptr<CBlock> pblock(new CBlock());

    if (!pblock.get())
    {
        return NULL;
    }

    CBlockIndex* pindexPrev = pindexBest;
    int nHeight = pindexPrev->nHeight + 1;

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);

    txNew.vout[0].scriptPubKey = scriptPubKey;


    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);

    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64_t nMinTxFee = MIN_TX_FEE;
    if (mapArgs.count("-mintxfee"))
    {
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);
    }

    pblock->nBits = GetNextTargetRequired(pindexPrev, false);

    // Collect memory pool transactions into the block
    int64_t nFees = 0;

    // Global Namespace Start
    {
        LOCK2(cs_main, mempool.cs);
        CTxDB txdb("r");

        //>SOCG<
        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {

            LogPrintf("*** RGP miner loop 5 \n");
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
            {
                continue;
            }

            COrphan* porphan = NULL;

            double dPriority = 0;
            int64_t nTotalIn = 0;

            bool fMissingInputs = false;

            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {

                LogPrintf("*** RGP miner loop 6 \n");
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        if (fDebug)
                        {
                            LogPrint("mempool", "%s : ERROR: mempool transaction missing input\n", __FUNCTION__);

                            assert("mempool transaction missing input" == 0);
                        }

                        fMissingInputs = true;

                        if (porphan)
                        {
                            vOrphan.pop_back();
                        }

                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }

                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    
                    continue;
                }

                int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            }

            if (fMissingInputs)
            {
                continue;
            }

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
            {
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
            }
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;

        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            LogPrintf("*** RGP miner loop 7 \n");

            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
            {
                continue;
            }

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
            {
                continue;
            }

            // Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (false && tx.nTime > pblock->vtx[0].nTime))
            {
                continue;
            }

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
            {
                continue;
            }

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee && ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;

            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
            {
                continue;
            }

            int64_t nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, mapInputs);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
            {
                continue;
            }

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true, MANDATORY_SCRIPT_VERIFY_FLAGS))
            {
                continue;
            }

            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority", false))
            {
                LogPrint("miner", "%s : priority %.1f feeperkb %.1f txid %s\n", __FUNCTION__, dPriority, dFeePerKb, tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    LogPrintf("*** RGP miner loop 10 \n");
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        if (fDebug && GetBoolArg("-printpriority", false))
        {
            LogPrint("miner", "%s : total size %u\n", __FUNCTION__, nBlockSize);
        }
        
        // >SOCG< POW

        pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(pindexPrev->nHeight + 1, nFees);


        if (pFees)
        {
            pFees = nFees;
        }

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->nTime          = max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());


        pblock->UpdateTime(pindexPrev);


        pblock->nNonce         = 0;
    }
    // Global Namespace End

    return pblock.release();

}


// CreateNewBlock: create new block (without proof-of-work/proof-of-stake)

/* RGP, Issue here is that this must start after the last know best block
        has been received, based on network there should be 240
        seconds between blocks, which should be enough time. However,
        timing is the key part of this, which is performed in the Stake Thread */



CBlock* CreateNewBlock(CReserveKey& reservekey, bool fProofOfStake, int64_t* pFees)
{
int nHeight;

    // Create new block
    /* RGP, replaced auto_ptr with unique_ptr */
    /* auto_ptr<CBlock> pblock(new CBlock()); */
    unique_ptr<CBlock> pblock(new CBlock());

    if (!pblock.get())
    {
        return NULL;
    }

    /* copy latest block to previous, as we need to create hash
       based on prev and current */

    CBlockIndex* pindexPrev = pindexBest;
    nHeight = pindexPrev->nHeight + 1;

    // Create coinbase tx
    CTransaction txNew;
    txNew.vin.resize(1);
    txNew.vin[0].prevout.SetNull();
    txNew.vout.resize(1);

    if (!fProofOfStake)
    {
        CPubKey pubkey;
        if (!reservekey.GetReservedKey(pubkey))
        {
            return NULL;
        }

        txNew.vout[0].scriptPubKey.SetDestination(pubkey.GetID());
    }
    else
    {
        // Height first in coinbase required for block.version=2
        txNew.vin[0].scriptSig = (CScript() << nHeight) + COINBASE_FLAGS;
        assert(txNew.vin[0].scriptSig.size() <= 100);

        txNew.vout[0].SetEmpty();
    }

    // Add our coinbase tx as first transaction
    pblock->vtx.push_back(txNew);

    // Largest block you're willing to create:
    unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);

    // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

    // How much of the block should be dedicated to high-priority transactions,
    // included regardless of the fees they pay
    unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", DEFAULT_BLOCK_PRIORITY_SIZE);
    nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

    // Minimum block size you want to create; block will be filled with free transactions
    // until there are no more or the block reaches this size:
    unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
    nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

    // Fee-per-kilobyte amount considered the same as "free"
    // Be careful setting this: if you set it to zero then
    // a transaction spammer can cheaply fill blocks using
    // 1-satoshi-fee transactions. It should be set above the real
    // cost to you of processing a transaction.
    int64_t nMinTxFee = MIN_TX_FEE;
    if (mapArgs.count("-mintxfee"))
    {
        ParseMoney(mapArgs["-mintxfee"], nMinTxFee);
    }

    pblock->nBits = GetNextTargetRequired(pindexPrev, fProofOfStake);

    // Collect memory pool transactions into the block
    int64_t nFees = 0;

    // Global Namespace Start
    {
        LOCK2(cs_main, mempool.cs);
        CTxDB txdb("r");

        //>SOCG<
        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {

            //LogPrintf("*** RGP miner loop 11 \n");
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || tx.IsCoinStake() || !IsFinalTx(tx, nHeight))
            {
                continue;
            }

            COrphan* porphan = NULL;

            double dPriority = 0;
            int64_t nTotalIn = 0;

            bool fMissingInputs = false;

            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                //LogPrintf("*** RGP miner loop 12 \n");
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        if (fDebug)
                        {
                            LogPrint("mempool", "%s : ERROR: mempool transaction missing input\n", __FUNCTION__);

                            assert("mempool transaction missing input" == 0);
                        }

                        fMissingInputs = true;

                        if (porphan)
                        {
                            vOrphan.pop_back();
                        }

                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }

                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    nTotalIn += mempool.mapTx[txin.prevout.hash].vout[txin.prevout.n].nValue;
                    
                    continue;
                }

                int64_t nValueIn = txPrev.vout[txin.prevout.n].nValue;
                nTotalIn += nValueIn;

                int nConf = txindex.GetDepthInMainChain();
                dPriority += (double)nValueIn * nConf;
            }

            if (fMissingInputs)
            {
                continue;
            }

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            double dFeePerKb =  double(nTotalIn-tx.GetValueOut()) / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
            {
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
            }
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64_t nBlockSize = 1000;
        uint64_t nBlockTx = 0;

        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            //LogPrintf("*** RGP miner loop 13 \n");
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
            {
                continue;
            }

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = GetLegacySigOpCount(tx);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
            {
                continue;
            }

            // Timestamp limit
            if (tx.nTime > GetAdjustedTime() || (fProofOfStake && tx.nTime > pblock->vtx[0].nTime))
            {
                continue;
            }

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
            {
                continue;
            }

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee && ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;

            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
            {
                continue;
            }

            int64_t nTxFees = tx.GetValueIn(mapInputs)-tx.GetValueOut();

            nTxSigOps += GetP2SHSigOpCount(tx, mapInputs);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
            {
                continue;
            }

            // Note that flags: we don't want to set mempool/IsStandard()
            // policy here, but we still have to ensure that the block we
            // create only contains transactions that are valid in new blocks.
            if (!tx.ConnectInputs(txdb, mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true, MANDATORY_SCRIPT_VERIFY_FLAGS))
            {
                continue;
            }

            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority", false))
            {
                LogPrint("miner", "%s : priority %.1f feeperkb %.1f txid %s\n", __FUNCTION__, dPriority, dFeePerKb, tx.GetHash().ToString());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    LogPrintf("*** RGP miner loop 14 \n");
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;

        if (fDebug && GetBoolArg("-printpriority", false))
        {
            LogPrint("miner", "%s : total size %u\n", __FUNCTION__, nBlockSize);
        }
        
        // >SOCG<
        
        if (!fProofOfStake)
        {
            pblock->vtx[0].vout[0].nValue = GetProofOfWorkReward(pindexPrev->nHeight + 1, nFees);
        }

        if (pFees)
        {
            *pFees = nFees;
        }

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->nTime          = max(pindexPrev->GetPastTimeLimit()+1, pblock->GetMaxTransactionTime());

        if (!fProofOfStake)
        {
            pblock->UpdateTime(pindexPrev);
        }

        pblock->nNonce         = 0;
    }
    // Global Namespace End

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }

    ++nExtraNonce;

    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2

    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;

        unsigned char pchPadding0[64];

        uint256 hash1;

        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
    {
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);
    }
    MilliSleep(1);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}



bool ProcessBlockStake(CBlock* pblock, CWallet& wallet)
{
bool status;

    status = false;


    LogPrintf("*** RGP ProcessBlockStake started at time  %d \n",  GetTime() );

    uint256 proofHash = 0, hashTarget = 0;
    uint256 hashBlock = pblock->GetHash();

    if(!pblock->IsProofOfStake())
    {
        MilliSleep(5); /* RGP Optimize */
        LogPrintf("*** RGP Miner not proof of stake returned \n");
        return status;
        //return error("%s : %s is not a proof-of-stake block", __FUNCTION__, hashBlock.GetHex());
    }

    // verify hash target and signature of coinstake tx
    if (!CheckProofOfStake(mapBlockIndex[pblock->hashPrevBlock], pblock->vtx[1], pblock->nBits, proofHash, hashTarget))
    {
        MilliSleep(5); /* RGP Optimize */
        LogPrintf("*** RGP Miner not check proof of stake returned \n");
        return status;
        //return error("%s : proof-of-stake checking failed", __FUNCTION__);
    }

    //if (fDebug)
    //{
        //// debug print
        LogPrintf("coinstake, %s : new proof-of-stake block found  \n  hash: %s \nproofhash: %s  \ntarget: %s\n", __FUNCTION__, hashBlock.GetHex(), proofHash.GetHex(), hashTarget.GetHex());
        LogPrintf("coinstake, %s : %s\n", __FUNCTION__, pblock->ToString());
        LogPrintf("coinstake, %s : out %s\n", __FUNCTION__, FormatMoney(pblock->vtx[1].GetValueOut()));
    //}

    status = false;

    // Global Namespace Start
    {
        // Found a solution

        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
        {
            MilliSleep(5); /* RGP Optimize */
            LogPrintf("*** RGP ProcessBlockStake stale block \n");
            return status;
            //return error("%s : generated block is stale", __FUNCTION__);
        }

        // Global Namespace Start
        {
            // Track how many getdata requests this block gets
            LOCK(wallet.cs_wallet);
            {
                LogPrintf("*** RGP ProcessBlockStake wallet lock SUCCESS \n");
                wallet.mapRequestCount[hashBlock] = 0;
            }


        }
        // Global Namespace End       

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
        {

            LogPrintf("*** RGP ProcessBlockStake after Process Block FAILED!! \n");

            MilliSleep(5); /* RGP Optimize */

            return status;
            //return error("%s : ProcessBlock, block not accepted", __FUNCTION__);
        }
        else
        {

            LogPrintf("*** RGP ProcessBlockStake after Process Block SUCESS!! \n");

            status = true;
            //ProcessBlock successful for PoS. now FixSpentCoins.
            int nMismatchSpent;

            CAmount nBalanceInQuestion;
            wallet.FixSpentCoins(nMismatchSpent, nBalanceInQuestion);

            if (nMismatchSpent != 0)
            {
                if (fDebug)
                {
                    LogPrint("coinstake", "%s : PoS mismatched spent coins = %d and balance affects = %d \n", __FUNCTION__, nMismatchSpent, nBalanceInQuestion);
                }
            }
        }
    }
    // Global Namespace End



    processed_stakes++;
    last_time_staked = pblock->nTime;

    LogPrintf("*** RGP ProcessBlockStake Created number of processed stakes %d nTime %d time to process %d \n",processed_stakes, last_time_staked, GetTime() );


    return status;
}

#define ThreadStake_TID         10
#define Nodes_for_Staking       4
#define STAKE_ADJUST            200         /* Seconds */
#define STAKE_BLOCK_TIME        240         /* Seconds */

//extern double GetPoSKernelPS();



bool SYNCH_Override = false;

void ThreadStakeMiner(CWallet *pwallet)
{
extern int Known_Block_Count;
extern volatile bool fRequestShutdown;
int64_t nFees;
extern bool BSC_Wallet_Synching; /* RGP defined in main.h */
int64_t Time_to_Last_block, Time_in_since_Last_Block, last_recorded_block_time, time_to_sleep, after_stake_success_timeout;
int64_t Last_known_block_time;

bool wait_for_best_time, first_time;
int ms_timeout, stake_timeout, lookforblock ;
bool fTryToSync;
//extern volatile bool Processing_Messages;
bool synch_status, ignore_check, blockchain_stuck, stake_status;
int  current_height, last_block_stake_check;
int64_t last_time_to_block, last_time_check, time_filter, synch_check;


    //SetThreadPriority(THREAD_PRIORITY_LOWEST);



    SetThreadPriority( 0 );

    // Make this thread recognisable as the mining thread
    RenameThread("SOCG-stake-miner");

    //startup_lock.lock();
    //LogPrintf("*** RGP ThreadStakeMiner atomic notified, GO! \n");
    //startup_lock.unlock();

    CReserveKey reservekey(pwallet);

    fTryToSync = false;


    MilliSleep(10000); /* 10 second delay to allow block variables to catchup */

    while ( fRequestShutdown == false )
    {
        LogPrintf("*** RGP ThreadStakeMiner, start SYNCH Loop \n");

        /* check wallet.h */

        while (pwallet->IsLocked())
        {
            nLastCoinStakeSearchInterval = 0;

           // LogPrintf("MinerStake wait 1 \n");
//            if ( Processing_Messages )
            {
                //thread_semaphore.wait ( ThreadStake_TID );

 //               LogPrintf("TSMg Wait Lock1 received  \n" );
            }
            //else
                MilliSleep(5000);

            if ( pwallet->IsLocked() )
            {
                LogPrintf("*** RGP ThreadStakeMiner wallet locked, ignoring, GO! \n");
                break;
            }


        }

        /* --------------------------------------------------------------------------------
           -- RGP check for at least 8 nodes. This has been reduced until there is more  --
           -- blockchain activity. Currently set to 4!                                   --
           -------------------------------------------------------------------------------- */

        /* Node for staking was 8, nothing defined except Bitcoin */
        while ( vNodes.size() < Nodes_for_Staking )
        {            
            MilliSleep(5000);

            if ( fRequestShutdown == true )
                return;

        }



        //LogPrintf("*** RGP ThreadStakeMiner wallet lock check! \n");

        /* If the wallet is not yet synched, then delay */
        Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
        Last_known_block_time = pindexBest->GetBlockTime();
        last_time_to_block = Time_to_Last_block;
        last_time_check = GetTime();
        time_filter = 0;
        ignore_check = false;
        blockchain_stuck = false;

        LogPrintf("*** RGP TimeCheck GETTIME() %d GETBLOCKTIME() %d \n", GetTime(),  pindexBest->GetBlockTime() );


        //while ( IsInitialBlockDownload() )
        while ( Time_to_Last_block > 240 )
        {          



            /* RGP, Otherwise, blocks are coming in and this may be a synching process */
            Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

            LogPrintf("*** RGP ThreadStakeMiner synch block loop check, time %d \n ", Time_to_Last_block);

            /* RGP Synch Loop,
               Remember, block time is physical time, but synching may be a lot faster
               Delay_factor is 10 instead of 1000 */

            LogPrintf("*** RGP ThreadStakeMiner delaying! %d \n",  Time_to_Last_block   );
            if ( Time_to_Last_block < 1000 )
            {
                /* Consider block may be stuck */
                MilliSleep( 1000 );
            }
            else
            {
                if ( Time_to_Last_block > 2000 )
                {
                    /* Block is up to date, eg gettime is 2000+ last received block */
                }
                else
                {
                    MilliSleep(  Time_to_Last_block * 100  );
                }
            }

            if ( fRequestShutdown )
                return;

            /* -------------------------------------------------------------------------
               -- RGP, Discovered scenario where the last block has been staked, but  --
               --      no other blocks have been staked. So perform test 3 time,      --
               --      if block has not changed, but time is increasing. Then bypass  --
               --      after 15 seconds from miner stake starting                     --
               ------------------------------------------------------------------------- */
            if ( Last_known_block_time < pindexBest->GetBlockTime() )
            {
                /* Blocks are coming in, which means that we are synching */
                LogPrintf("RGP Miner Stake synch loop, still syncing, blocks changing... \n");
                LogPrintf("RGP Last_known_block_time %d pindexBest->GetBlockTime %d \n", Last_known_block_time, pindexBest->GetBlockTime() );

                if ( time_filter > 0 )
                    time_filter--;
                MilliSleep( 5000 ); /* 5 second delay */
                /* RGP, Otherwise, blocks are coming in and this may be a synching process */
                Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

                LogPrintf("RGP CONFIRM block_time %d pindexBest->GetBlockTime %d Time_to_Last_block %d  \n", Time_to_Last_block, pindexBest->GetBlockTime(), Time_to_Last_block );

                /* ----------------------------------------------------------------
                   -- Update Last know block time ro determine if we are synched --
                   ---------------------------------------------------------------- */
                Last_known_block_time = pindexBest->GetBlockTime();
                continue;
            }
            else
            {
                /* Block time is not changing, could be waiting at end of the block chain */
                LogPrintf("RGP Miner Stake synch loop, blocks stuck ... \n");
                Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
                LogPrintf(" RGP stuck check time to gettime() %d ", Time_to_Last_block   );

                /* ---------------------
                   -- RGP JIRA BSG-93 --
                   --------------------------------------------------------------
                   -- Added condition to check if time difference between the  --
                   -- last blockchain received 'pindexBex->GetBlockTime()' and --  
                   -- GetTime() are close or far apart.                        --
                   -- BlockSpacing is 240 seconds, used this for the check     --                                                            
                   -------------------------------------------------------------- */
                if ( Time_to_Last_block > 240 )
                {
                    /* -------------------------------------------------------------------
                       --  Difference between 'pindexBex->GetBlockTime()' and GetTime() --
                       --  is more than 240 seconds, exit the loop and return to        --
                       --  Initial checks again.                                        --
                       ------------------------------------------------------------------- */ 

                    Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
                    if ( Time_to_Last_block > 480 )
                    {
                        /* something is wrong for code into next sequence */
                        break;
                    }
                    else
                    {
                       MilliSleep(5000);
                       continue;
                    }
                }
            }

            /* Check for scenario where the block time is not changing, but time is changing
               Block is not change, as we already checked this scenario above                  */

            /* Block has not changed, but time has moved on */
            if ( last_time_check != GetTime() )
            {
                /* Time moving forward, but block is not */
                time_filter++;
                LogPrintf("RGP Miner Stake synch loop, incremented time_filter %d \n", time_filter );
                MilliSleep(1000 * 10 );  /* wait 20 minutes, should be 25 minutes  */
                if ( time_filter > 5 )
                {
                     Time_to_Last_block = 0;  /* Force exit out of loop */
                     ignore_check = true;
                     fTryToSync = false;
                     blockchain_stuck = true;
                }

            }

        }

        LogPrintf("*** RGP Miner 3 checks \n");

        /* RGP, Ensure that stake mining is not allowed until after the wallet is synched */
        fTryToSync = false;
        while ( fTryToSync == false  && fRequestShutdown == false && ignore_check == false  )
        {

            // LogPrintf("*** RGP MIner 3 loop \n" );

            // RGP Get current height and test against last Checkpoint
            SYNCH_Override = false;

            Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
            /* Check if less than half way to next block */
            if ( Time_to_Last_block > 0 && Time_to_Last_block < 180  )
            {

                /* Not synched so wait for synch */

                //ms_timeout = ( STAKE_BLOCK_TIME - Time_to_Last_block  ) * 1000;

                // LogPrintf("*** RGP Miner 3 loop timeout is %d Time_to_Last_block %d \n", ms_timeout, Time_to_Last_block );

                if ( Time_to_Last_block > 220 && Time_to_Last_block < 240 )
                    ms_timeout = 100;
                else
                    ms_timeout = 5000;

                MilliSleep(ms_timeout);

                // LogPrintf("*** RGP MIner 3 continue \n" );

                /* If synch is in progress it could take a long time */

                Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
                if ( Time_to_Last_block < 200 )
                    continue;
                else
                {
                    /* between 200 and 240, ready to synch */
                    fTryToSync = true;
                }
            }
            else
            {
                /* -------------------------------------------------------------------------------
                   -- RGP Blocktime is now less than block space of 240 secs, it's now possible --
                   -- to stake.                                                                 --
                   ------------------------------------------------------------------------------- */

                fTryToSync = true;
            }
            
            if ( fRequestShutdown == true )
                return;

        }

        LogPrintf("*** RGP ThreadStakeMiner STARTED  Create NEW BLOCK *** \n"  );

        //
        // Create new block
        //


        if ( fRequestShutdown == true )
            return;

        /* --
           -- RGP, Signblock allows a coin maturity check every 15 seconds at present --
           --      THis is too short, this needs to be optimised to keep processes    --
           --      less busy.                                                         --
           ----------------------------------------------------------------------------- */

        stake_timeout = ( GetRandInt(5000) ); /* attempt to vary each wallet to avoid conflicted entries */
        LogPrintf("*** Stake timeout is %d msecs \n", stake_timeout);
        /* wait a random time before creating a new block */
        MilliSleep( stake_timeout );


        {

            /* -------------------------------------------------------------------
               -- RGP, now determine where we are in the next block generation, --
               --      as there should be 1 block every 240 seconds...          --
               --      Updated to consider both PoS and PoW blocks, as PoW may  --
               --      be quicker than 240 seconds.                             --
               --      if PoW is updating quicker than 240, then it needs to be --
               --      resolved.                                                --
               ------------------------------------------------------------------- */

            last_recorded_block_time = pindexBest->GetBlockTime();  /* Get the current block time */
            wait_for_best_time = true;
            first_time = true;
            do
            {

                if ( blockchain_stuck )
                {
                    LogPrintf("*** RGP BLOCKCHAIN stuck, just mint now\n");
                    break;
                }

                /* Calculate haw long to the next block and sleep */
                Time_in_since_Last_Block = GetTime() - pindexBest->GetBlockTime();
                LogPrintf("*** RGP Before Signblock, time between last block %d and now %d, time since last block stored %d \n", pindexBest->GetBlockTime(), GetTime(), Time_in_since_Last_Block );

                /* RGP Sleep until next block is created */
                /* REMOVED STAKE ADJUST */
                if ( Time_in_since_Last_Block > STAKE_BLOCK_TIME  )
                    Time_in_since_Last_Block = 100;

                LogPrintf("*** RGP time to sleep %d %d %d  \n", time_to_sleep, STAKE_BLOCK_TIME, Time_in_since_Last_Block  );

                time_to_sleep = ( ( STAKE_BLOCK_TIME - Time_in_since_Last_Block  ) * 1000 );


                if ( time_to_sleep  == 0 )
                {
                    LogPrintf("*** RGP time to sleep %d %d %d  \n", time_to_sleep, STAKE_BLOCK_TIME, Time_in_since_Last_Block  );
                    MilliSleep( GetRandInt(20000) );
                    wait_for_best_time = false;
                    continue;
                }
                else
                    MilliSleep( time_to_sleep  );

                first_time = false;

                LogPrintf("*** RGP Time check,  pindexBest->GetBlockTime %d and last_recorded_block_time %d \n", pindexBest->GetBlockTime(), last_recorded_block_time );


                if ( pindexBest->GetBlockTime() > last_recorded_block_time )
                {
                    //LogPrintf(" New block must have been seen... \n");
                    /* there must have been a new block generated, wait again */
                    if ( last_recorded_block_time != pindexBest->GetBlockTime() )
                        last_recorded_block_time = pindexBest->GetBlockTime();
                    /* ----------------------------------------------------------------------
                       -- RGP, logic dictates that if a new block just got created, try to --
                       --      create a new stake block, as the delay has been implemented --
                       ---------------------------------------------------------------------- */
                    //LogPrintf("Let's wait again... \n");
                    wait_for_best_time = false; /* RGP BIG TEST */
                }
                else
                {

                    /* if pindexBest->GetBlockTime() == last_recorded_block_time, no new
                        block has been generated yet, should we go for it? */

                    if ( pindexBest->GetBlockTime() == last_recorded_block_time )
                    {

                        /* RGP, Let's do one final check, as we may need to be creating a new
                                block now, as the clock is ticking and PoS needs to mint       */

                        if ( GetTime() > pindexBest->GetBlockTime() )
                        {
                            if ( (GetTime() - pindexBest->GetBlockTime() ) > 240 )
                            {
                                /* must be time to stake now */
                                last_recorded_block_time = pindexBest->GetBlockTime();      /* Exit the next while loop */
                                wait_for_best_time = false;
                                break;
                            }
                            else
                                wait_for_best_time = true;
                        }


                        //break;
                    }

                    if (  wait_for_best_time == false)
                    {
                        /* RGP, this is the tricky part, as we just woke up from the sleep
                            the logic above means that a new block DID GET CREATED
                            yet, should the algorithm now wait for the next block to
                            come in and then stake?                                     */

                        // last_recorded_block_time = pindexBest->GetBlockTime();
                        last_block_stake_check = 0;
                        while (  pindexBest->GetBlockTime() == last_recorded_block_time )
                        {
                            //LogPrintf("*** Wait for next block pindexBest->GetBlockTime %d and last_recorded_block_time %d \n", pindexBest->GetBlockTime(), last_recorded_block_time );

                            //MilliSleep( GetRandInt( 240 ) * 1000 );
MilliSleep( GetRandInt( 24 ) * 1000 );

                            /* RGP added code for the scenario where the current block is the last
                            block, this just loops forever and ever.                             */
                            last_block_stake_check++;
                            if ( last_block_stake_check > 3 )
                                break;
                        }

                        //LogPrintf(" exiting block stake time!! \n");
                        wait_for_best_time = false;
                    }
                }

                if ( fRequestShutdown == true )
                    return;


            } while ( wait_for_best_time  );


            //SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL);

            /* --------------------------------------------
               -- RGP, replaced auto_ptr with unique_ptr --
               -------------------------------------------- */

            /* auto_ptr<CBlock> pblock(CreateNewBlock(reservekey, true, &nFees)); */
            unique_ptr<CBlock> pblock(CreateNewBlock(reservekey, true, &nFees));
            if (!pblock.get())
            {
                LogPrintf("*** RGP Minerthread pblock.get failed!!! \n");
                MilliSleep( 2000 );
                continue;
            }

            //LogPrintf(" Next call Signblock!! \n");

            // Trying to sign a block
            if (pblock->SignBlock(*pwallet, nFees))
            {
                SetThreadPriority(THREAD_PRIORITY_ABOVE_NORMAL);

                stake_status = ProcessBlockStake(pblock.get(), *pwallet);
            
                SetThreadPriority(THREAD_PRIORITY_LOWEST);


                /* RGP, try to determine if the block was successful, using pindexBest
                        test hash of next few blocks, if not outs try again immediately */


                 if ( stake_status )
                 {
                        /* ProcessBlockStake() was successful and ProcessBlock() */
                        after_stake_success_timeout = ( ( ( 12 * 60 ) + GetRandInt( 100 ) ) * 1000 ) ;
                        LogPrintf("*** RGP new Mint stake delay after success timeout %d \n", after_stake_success_timeout);
//after_stake_success_timeout = 120000;
                        MilliSleep( after_stake_success_timeout );
                        //MilliSleep( 30000 );

                 }
                 else
                 {
                      LogPrintf("*** RGP Stake was NOT successfull, small delay and then go again!\n");
                      MilliSleep( 1000 );
                      break;

                 }
            }
            else
            {
                LogPrintf("*** RGP before delay %d \n", nMinerSleep);

                /* Failed, let's delay */
                MilliSleep(nMinerSleep);
            }

            pblock.reset();

        }


    }
}


//////////////////////////////////////////////////////////////////////////////
//
// 
// Internal Coin Miner
//


bool ProcessBlockFound(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hashBlock = pblock->GetHash();
    uint256 hashProof = pblock->GetPoWHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if(!pblock->IsProofOfWork())
    {
        return error("%s : %s is not a proof-of-work block", __FUNCTION__, hashBlock.GetHex());
    }

    if (hashProof > hashTarget)
    {
        return error("%s : proof-of-work not meeting target", __FUNCTION__);
    }

    if (fDebug)
    {
        //// debug print
        LogPrint("miner", "%s : new proof-of-work block found  \n  proof hash: %s  \ntarget: %s\n",  __FUNCTION__, hashProof.GetHex(), hashTarget.GetHex());
        LogPrint("miner", "%s : %s\n", __FUNCTION__, pblock->ToString());
        LogPrint("miner", "%s : generated %s\n", __FUNCTION__, FormatMoney(pblock->vtx[0].vout[0].nValue));
    }

    // Global Namespace Start
    {
        // Found a solution

        LOCK(cs_main);

        if (pblock->hashPrevBlock != hashBestChain)
        {
            return error("%s : generated block is stale", __FUNCTION__);
        }

        // Remove key from key pool
        reservekey.KeepKey();

        // Global Namespace Start
        {
            // Track how many getdata requests this block gets

            LOCK(wallet.cs_wallet);

            wallet.mapRequestCount[hashBlock] = 0;
        }
        // Global Namespace End

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
        {
            return error("%s : ProcessBlock, block not accepted", __FUNCTION__);
        }
    }
    // Global Namespace End

    return true;
}

void static InternalcoinMiner(CWallet *pwallet)
{
extern bool BSC_Wallet_Synching; /* RGP defined in main.h */
extern volatile bool fRequestShutdown;
int64_t Time_to_Last_block;

    std::string TempMinerLogCache;

    LogPrintf("SOCG-PoW-Miner - Started!\n");


    if (fDebug)
    {
        LogPrintf("SOCG-PoW-Miner - Started!\n");
    }

    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    RenameThread("SOCG-PoW-Miner");

    if ( BSC_Wallet_Synching )
        LogPrintf("NO SYNCHED - MINER!\n");
    else
        LogPrintf("SYNCHED - MINER!\n");


    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);

    unsigned int nExtraNonce = 0;

    try
    {
        while (true)
        {
            LogPrintf("*** RGP POW miner loop 19 \n");

            /* Wait if we are synching */

            Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();
            while ( Time_to_Last_block > 240  )
            {
                LogPrintf("*** RGP POW miner loop 19a \n");

                #ifdef WIN32
                    Sleep(5000);
                #else
                    MilliSleep(5000);
                #endif
                Time_to_Last_block = GetTime() - pindexBest->GetBlockTime();

                if ( fRequestShutdown )
                    return;
            }



                // Busy-wait for the network to come online so we don't waste time mining
                // on an obsolete chain. In regtest mode we expect to fly solo.
                do
                {
                    bool fvNodesEmpty;
                    {
                        LOCK(cs_vNodes);

                        fvNodesEmpty = vNodes.empty();
                    }

                    if (!fvNodesEmpty && !IsInitialBlockDownload() && !BSC_Wallet_Synching )
                    {
                        break;
                    }

                    MilliSleep(1000);
                }
                while (true);


            //
            // Create new block
            //
            unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();

            CBlockIndex* pindexPrev = pindexBest;
           
            /* RGP, replaced auto_ptr with unique_ptr */
            /* auto_ptr<CBlock> pblocktemplate(CreateNewBlockWithKey(reservekey, pwallet)); */

            unique_ptr<CBlock> pblocktemplate(CreateNewBlockWithKey(reservekey, pwallet));
            if (!pblocktemplate.get())
            {
                if (fDebugConsoleOutputMining)
                {
                    printf("Error in SOCG-PoW-Miner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                }

                if (fDebug)
                {
                    LogPrintf("Error in SOCG-PoW-Miner: Keypool ran out, please call keypoolrefill before restarting the mining thread\n");
                }

                return;
            }

            CBlock *pblock = pblocktemplate.get();

            IncrementExtraNonce(pblock, pindexPrev, nExtraNonce);

            if (fDebugConsoleOutputMining)
            {
                printf("Running SOCG-PoW-Miner with %u transactions in block (%u bytes)\n", (int)pblock->vtx.size(), ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));
            }

            if (fDebug)
            {
                LogPrintf("Running SOCG-PoW-Miner with %u transactions in block (%u bytes)\n", pblock->vtx.size(), ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));
            }

            //
            // Search
            //
            int64_t nStart = GetTime();
            uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            uint256 thash;

            while (true)
            {
                unsigned int nHashesDone = 0;
                char scratchpad[SCRYPT_SCRATCHPAD_SIZE];

                while(true)
                {
                    scrypt_1024_1_1_256_sp(BEGIN(pblock->nVersion), BEGIN(thash), scratchpad);

                    if (thash <= hashTarget)
                    {
                        // Found a solution
                        SetThreadPriority(THREAD_PRIORITY_NORMAL);
                        
                        if (ProcessBlockFound(pblock, *pwallet, reservekey))
                        {
                            TempMinerLogCache = "accepted:" + thash.GetHex();

                            if (MinerLogCache != TempMinerLogCache)
                            {
                                if (fDebugConsoleOutputMining)
                                {
                                    printf("SOCG-PoW-Miner: Proof-of-work found! (ACCEPTED) POW-Hash: %s Nonce: %d\n", thash.GetHex().c_str(), pblock->nNonce);
                                }

                                if (fDebug)
                                {
                                    LogPrintf("SOCG-PoW-Miner: Proof-of-work found! (ACCEPTED) POW-Hash: %s Nonce: %d\n", thash.GetHex(), pblock->nNonce);
                                }
                            }

                            MinerLogCache = "accepted:" + thash.GetHex();

                            MilliSleep(240000); /* Was 50 seconds, now 240 seconds */
                        }
                        else
                        {
                            TempMinerLogCache = "rejected:" + thash.GetHex();

                            if (MinerLogCache != TempMinerLogCache)
                            {
                                if (fDebugConsoleOutputMining)
                                {
                                    printf("SOCG-PoW-Miner: Proof-of-work found! (REJECTED) POW-Hash: %s Nonce: %d\n", thash.GetHex().c_str(), pblock->nNonce);
                                }

                                if (fDebug)
                                {
                                    LogPrintf("SOCG-PoW-Miner: Proof-of-work found! (REJECTED) POW-Hash: %s Nonce: %d\n", thash.GetHex(), pblock->nNonce);
                                }
                            }

                            MinerLogCache = "rejected:" + thash.GetHex();
                        }
                        
                        SetThreadPriority(THREAD_PRIORITY_LOWEST);

                        // In regression test mode, stop mining after a block is found.
                        /*
                        if (Params().MineBlocksOnDemand())
                            throw boost::thread_interrupted();
                        */

                        break;
                    }

                    pblock->nNonce += 1;
                    nHashesDone += 1;

                    if ((pblock->nNonce & 0xFF) == 0)
                    {
                        break;
                    }
                }

                // Meter hashes/sec
                static int64_t nHashCounter;

                if (nHPSTimerStart == 0)
                {
                    nHPSTimerStart = GetTimeMillis();
                    nHashCounter = 0;
                }
                else
                {
                    nHashCounter += nHashesDone;
                }

                if (GetTimeMillis() - nHPSTimerStart > 4000)
                {
                    static CCriticalSection cs;
                    {
                        LOCK(cs);

                        if (GetTimeMillis() - nHPSTimerStart > 4000)
                        {
                            dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                            nHPSTimerStart = GetTimeMillis();
                            nHashCounter = 0;

                            static int64_t nLogTime;

                            if (GetTime() - nLogTime > 30 * 60)
                            {
                                nLogTime = GetTime();

                                if (fDebugConsoleOutputMining)
                                {
                                    printf("SOCG-PoW-Miner: Hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                                }

                                if (fDebug)
                                {
                                    LogPrintf("SOCG-PoW-Miner: Hashmeter %6.0f khash/s\n", dHashesPerSec/1000.0);
                                }
                            }
                        }
                    }
                }

                // Check for stop or if block needs to be rebuilt
                boost::this_thread::interruption_point();

                // Regtest mode doesn't require peers
                if (vNodes.empty())
                {
                    break;
                }

                if (pblock->nNonce >= 0xffff0000)
                {
                    break;
                }

                if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                {
                    break;
                }

                if (pindexPrev != pindexBest)
                {
                    break;
                }

                // Update nTime every few seconds
                UpdateTime(*pblock, pindexPrev);

                /*
                if (Params().AllowMinDifficultyBlocks())
                {
                    // Changing pblock->nTime can change work required on testnet:
                    hashTarget.SetCompact(pblock->nBits);
                }
                */
            }
        }
    }
    catch (boost::thread_interrupted)
    {
        printf("SOCG-PoW-Miner terminated\n");

        if (fDebug)
        {
            LogPrintf("SOCG-PoW-Miner terminated\n");
        }

        throw;
    }
    catch (const std::runtime_error &e)
    {
        printf("SOCG-PoW-Miner runtime error: %s\n", e.what());

        if (fDebug)
        {
            LogPrintf("SOCG-PoW-Miner runtime error: %s\n", e.what());
        }

        return;
    }
}

void GeneratePoWcoins(bool fGenerate, CWallet* pwallet, bool fDebugToConsole)
{
    if (fDebugToConsole)
    {
        fDebugConsoleOutputMining = fDebugToConsole;
    }

    static boost::thread_group* minerThreads = NULL;

    int nThreads = GetArg("-genproclimit", -1);

    if (nThreads < 0)
    {
        nThreads = boost::thread::hardware_concurrency();
    }

    if (minerThreads != NULL)
    {
        minerThreads->interrupt_all();

        delete minerThreads;

        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
    {
        return;
    }

    minerThreads = new boost::thread_group();

    for (int i = 0; i < nThreads; i++)
    {
        minerThreads->create_thread(boost::bind(&InternalcoinMiner, pwallet));
    }
}
