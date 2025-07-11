#include "masternode.h"
#include "masternodeman.h"
#include "darksend.h"
#include "core.h"
#include "main.h"
#include "sync.h"
#include "util.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>


CCriticalSection cs_masternodes;
// keep track of the scanning errors I've seen
map<uint256, int> mapSeenMasternodeScanningErrors;
// cache block hashes as we calculate them
std::map<int64_t, uint256> mapCacheBlockHashes;


struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};

//Get the last hash that matches the modulus given. Processed in reverse order
bool GetBlockHash(uint256& hash, int nBlockHeight)
{

    // LogPrintf("*** RGP Masternode::GetBlockHash Start \n");

    if (pindexBest == NULL) return false;

    if(nBlockHeight == 0)
        nBlockHeight = pindexBest->nHeight;

    if(mapCacheBlockHashes.count(nBlockHeight))
    {
        hash = mapCacheBlockHashes[nBlockHeight];
        return true;
    }

    const CBlockIndex *BlockLastSolved = pindexBest;
    const CBlockIndex *BlockReading = pindexBest;

    if (BlockLastSolved == NULL || BlockLastSolved->nHeight == 0 || pindexBest->nHeight+1 < nBlockHeight) return false;

    int nBlocksAgo = 0;
    if(nBlockHeight > 0) nBlocksAgo = (pindexBest->nHeight+1)-nBlockHeight;
    assert(nBlocksAgo >= 0);

    int n = 0;
    for (unsigned int i = 1; BlockReading && BlockReading->nHeight > 0; i++) {
        if(n >= nBlocksAgo){
            hash = BlockReading->GetBlockHash();
            mapCacheBlockHashes[nBlockHeight] = hash;

            return true;
        }
        n++;

        if (BlockReading->pprev == NULL) { assert(BlockReading); break; }
        BlockReading = BlockReading->pprev;
    }

    //LogPrintf("*** RGP GetBlockHash FAILED \n");

    return false;
}

CMasternode::CMasternode()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubkey = CPubKey();
    pubkey2 = CPubKey();
    sig = std::vector<unsigned char>();
    activeState = MASTERNODE_ENABLED;
    sigTime = GetAdjustedTime();
    lastDseep = 0;
    lastTimeSeen = 0;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = PROTOCOL_VERSION;
    nLastDsq = 0;
    rewardAddress = CScript();
    rewardPercentage = 0;
    nVote = 0;
    lastVote = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    //mark last paid as current for new entries
    nLastPaid = GetAdjustedTime();
    isPortOpen = true;
    isOldNode = true;

}

CMasternode::CMasternode(const CMasternode& other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubkey = other.pubkey;
    pubkey2 = other.pubkey2;
    sig = other.sig;
    activeState = other.activeState;
    sigTime = other.sigTime;
    lastDseep = other.lastDseep;
    lastTimeSeen = other.lastTimeSeen;
    cacheInputAge = other.cacheInputAge;
    cacheInputAgeBlock = other.cacheInputAgeBlock;
    unitTest = other.unitTest;
    allowFreeTx = other.allowFreeTx;
    protocolVersion = other.protocolVersion;
    nLastDsq = other.nLastDsq;
    rewardAddress = other.rewardAddress;
    rewardPercentage = other.rewardPercentage;
    nVote = other.nVote;
    lastVote = other.lastVote;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
    nLastPaid = other.nLastPaid;
    nLastPaid = GetAdjustedTime();
    isPortOpen = other.isPortOpen;
    isOldNode = other.isOldNode;

}

CMasternode::CMasternode(CService newAddr, CTxIn newVin, CPubKey newPubkey, std::vector<unsigned char> newSig, int64_t newSigTime, CPubKey newPubkey2, int protocolVersionIn, CScript newRewardAddress, int newRewardPercentage)
{
    LOCK(cs);
    vin = newVin;
    addr = newAddr;
    pubkey = newPubkey;
    pubkey2 = newPubkey2;
    sig = newSig;
    activeState = MASTERNODE_ENABLED;
    sigTime = newSigTime;
    lastDseep = 0;
    lastTimeSeen = 0;
    cacheInputAge = 0;
    cacheInputAgeBlock = 0;
    unitTest = false;
    allowFreeTx = true;
    protocolVersion = protocolVersionIn;
    nLastDsq = 0;
    rewardAddress = newRewardAddress;
    rewardPercentage = newRewardPercentage;
    nVote = 0;
    lastVote = 0;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    isPortOpen = true;
    isOldNode = true;

}

//
// Deterministically calculate a given "score" for a masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
uint256 CMasternode::CalculateScore(int mod, int64_t nBlockHeight)
{
    if(pindexBest == NULL) return 0;

    uint256 hash = 0;
    uint256 aux = vin.prevout.hash + vin.prevout.n;

    if(!GetBlockHash(hash, nBlockHeight)) return 0;

    uint256 hash2 = Hash(BEGIN(hash), END(hash));
    uint256 hash3 = Hash(BEGIN(hash), END(hash), BEGIN(aux), END(aux));

    uint256 r = (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);

    return r;
}

void CMasternode::Check()
{

LogPrintf("RGP CMasternode::Check() START \n");
    if ( ShutdownRequested() )
    {
//LogPrintf("RGP Check() Shutdown Requested \n");
        return;
    }
    //TODO: Random segfault with this line removed
    TRY_LOCK(cs_main, lockRecv);
    if(!lockRecv)
    {
//LogPrintf("*** RGP CMasterNode::Check LockRecv failed, exit! \n");
        //LogPrintf("*** RGP ignoring issue...\n");
        //return;
    }

//LogPrintf("RGP CMasternode::Check() Debug 001  \n");

    //once spent, stop doing the checks
    if(activeState == MASTERNODE_VIN_SPENT)
    {
        LogPrintf("*** RGP CMasterNode::Check spent, exit \n");
        //return;
    }

//LogPrintf("RGP CMasternode::Check() Debug 002  \n");

    if(!UpdatedWithin(MASTERNODE_REMOVAL_SECONDS))
    {
LogPrintf("*** RGP CMasterNode::Check UpdatedWithin failed, MASTERNODE_REMOVE set  \n");
        activeState = MASTERNODE_REMOVE;
        //return;
    }

//LogPrintf("RGP CMasternode::Check() Debug 003  \n");
    if(!UpdatedWithin(MASTERNODE_EXPIRATION_SECONDS))
    {
        LogPrintf("*** RGP CMasterNode::Check Debug 002 MASTERNODE_EXPIRED \n");

        activeState = MASTERNODE_EXPIRED;
        //return;
    }

//LogPrintf("RGP CMasternode::Check() Debug 004  \n");

    if( !unitTest )
    {
LogPrintf("*** RGP CMasterNode::Check Debug 005 Unit Test \n");
        CValidationState state;
        CTransaction tx = CTransaction();
        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);

        /* New routine for MN is required */
        if( !AcceptableInputs( mempool, tx, false, NULL ) )
        {            
LogPrintf("*** RGP CMasterNode::Check Debug 002 Unit Test MASTERNODE_VIN_SPENT - DISABLED RETURN, FIX LATER \n");
            //activeState = MASTERNODE_VIN_SPENT;
            activeState = MASTERNODE_ENABLED;
            //return;
        }
    }

//LogPrintf("RGP CMasternode::Check() Debug 006  \n");

    activeState = MASTERNODE_ENABLED; // OK

//LogPrintf("RGP CMasternode::Check() END \n");

}
