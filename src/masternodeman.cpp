

#include "masternodeman.h"
#include "masternode.h"
#include "activemasternode.h"
#include "darksend.h"
#include "core.h"
#include "util.h"
#include "addrman.h"

#include "net.h"

#include <boost/lexical_cast.hpp>
#include <boost/filesystem.hpp>


/** Masternode manager */
static CMasternodeMan mnodeman;



// map to hold all MNs
static    std::vector<CMasternode> vMasternodes;
    // who's asked for the masternode list and the last time
static    std::map<CNetAddr, int64_t> mAskedUsForMasternodeList;
    // who we asked for the masternode list and the last time
static    std::map<CNetAddr, int64_t> mWeAskedForMasternodeList;
    // which masternodes we've asked for
static    std::map<COutPoint, int64_t> mWeAskedForMasternodeListEntry;

CCriticalSection cs_process_message;



struct CompareValueOnly
{
    bool operator()(const pair<int64_t, CTxIn>& t1,
                    const pair<int64_t, CTxIn>& t2) const
    {
        return t1.first < t2.first;
    }
};
struct CompareValueOnlyMN
{
    bool operator()(const pair<int64_t, CMasternode>& t1,
                    const pair<int64_t, CMasternode>& t2) const
    {
        return t1.first < t2.first;
    }
};




//
// CMasternodeDB
//

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir() / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}

bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    LogPrintf("*** RGP CMasternodeDB::Write Start \n");

    int64_t nStart = GetTimeMillis();

    // serialize addresses, checksum data up to that point, then append csum
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION);
    ssMasternodes << strMagicMessage; // masternode cache file specific magic message
    ssMasternodes << FLATDATA(Params().MessageStart()); // network specific magic number
    ssMasternodes << mnodemanToSave;
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "wb");
    CAutoFile fileout = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssMasternodes;
    }
    catch (std::exception &e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    FileCommit(fileout.Get());
    fileout.fclose();

    //LogPrintf("Written info to mncache.dat  %dms\n", GetTimeMillis() - nStart);
    //LogPrintf("  %s\n", mnodemanToSave.ToString());

    return true;
}

CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad)
{

    LogPrintf("*** RGP CMasternodeDB::Read Start \n");

    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE *file = fopen(pathMN.string().c_str(), "rb");
    CAutoFile filein = CAutoFile(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
    {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }
LogPrintf("*** RGP CMasternodeDB::Read Debug 001 \n");
    // use file size to size memory buffer
    int fileSize = boost::filesystem::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;
LogPrintf("*** RGP CMasternodeDB::Read Debug 002 \n");
    // read data and checksum from file
    try {
        filein.read((char *)&vchData[0], dataSize);
        filein >> hashIn;
    }
    catch (std::exception &e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();
LogPrintf("*** RGP CMasternodeDB::Read Debug 003 \n");
    CDataStream ssMasternodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());
    if (hashIn != hashTmp)
    {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }
LogPrintf("*** RGP CMasternodeDB::Read Debug 004 \n");
    unsigned char pchMsgTmp[4];
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header (masternode cache file specific magic message) and ..

        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp)
        {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }
LogPrintf("*** RGP CMasternodeDB::Read Debug 005 \n");
        // de-serialize file header (network specific magic number) and ..
        ssMasternodes >> FLATDATA(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp, Params().MessageStart(), sizeof(pchMsgTmp)))
        {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }

        // de-serialize address data into one CMnList object
        ssMasternodes >> mnodemanToLoad;
    }
    catch (std::exception &e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    mnodemanToLoad.CheckAndRemove(); // clean out expired
    LogPrintf("Loaded info from mncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrintf("  %s\n", mnodemanToLoad.ToString());

    return Ok;
}

void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    LogPrintf("*** RGP DumpMasternodes Start \n");

    CMasternodeDB mndb;
    CMasternodeMan tempMnodeman;

    LogPrintf("Verifying mncache.dat format...\n");
    CMasternodeDB::ReadResult readResult = mndb.Read(tempMnodeman);
    // there was an error and it was not an error on file openning => do not proceed
    if (readResult == CMasternodeDB::FileError)
        LogPrintf("Missing masternode list file - mncache.dat, will try to recreate\n");
    else if (readResult != CMasternodeDB::Ok)
    {
        LogPrintf("Error reading mncache.dat: ");
        if(readResult == CMasternodeDB::IncorrectFormat)
            LogPrintf("magic is ok but data has invalid format, will try to recreate\n");
        else
        {
            LogPrintf("file format is unknown or invalid, please fix it manually\n");
            return;
        }
    }
    LogPrintf("Writting info to mncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrintf("Masternode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CMasternodeMan::CMasternodeMan() {
    nDsqCount = 0;
}

bool CMasternodeMan::Add(CMasternode &mn)
{
    LOCK(cs);

    CMasternode testmn;

    LogPrintf("*** RGP CMasternodeMan::Add MN node %s Debug 001\n", mn.addr.ToString() );

    //CMasternode testmn = Find( mn.vin );

    LogPrintf("*** RGP CMasternodeMan::Add Debug before Find, Masternodes %d \n", vMasternodes.size() );

    /* -------------------------------------------------------------------
       -- RGP, if pwn is NULL, it's a new Masternode                    --
       --      changed to a bool, so if not found, add a new MasterNode --
       ------------------------------------------------------------------- */

    if ( !Find( mn.vin ) )
    {        
        LogPrintf("*** RGP CMasternodeMan::Add Debug MN node Masternode is added Debug 002\n" );

        vMasternodes.push_back(mn);

 //   CTxIn search_vin;
 //   CMasternode* pmn;
bool got_it;
LogPrintf("RGP CMasternodeman::Add() Masternode_Information size %d \n", Masternode_Information.size() );


    map < unsigned int, CMasternode >::iterator find_mn_vin;
    got_it = false;
    for ( find_mn_vin = Masternode_Information.begin(); find_mn_vin != Masternode_Information.end(); ++find_mn_vin )
    {
        if ( find_mn_vin->second.pubkey == mn.pubkey )
        {
            /* Masternode_Information has this entry, no action */
LogPrintf("*** RGP CMasternodeMan::Add ALREADY RECORDED Masternode_Information %d \n", Masternode_Information.size() );
            got_it = true;
        }
    }

    if ( !got_it )
        Masternode_Information.insert(std::make_pair( Masternode_Information.size() + 1, mn ));

LogPrintf("*** RGP CMasternodeMan::Add Debug END Masternode_Information %d \n", Masternode_Information.size() );

LogPrintf("*** RGP CMasternodeMan::Add Debug END Masternodes %d \n", vMasternodes.size() );

        return true;
    }

    LogPrintf("*** RGP CMasternodeMan::Add Debug END Masternodes %d \n", vMasternodes.size() );

    return false;
}

void CMasternodeMan::AskForMN(CNode* pnode, CTxIn &vin)
{

    //LogPrintf("*** RGP CMasternodeMan::AskForMN Start \n");

    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    if (i != mWeAskedForMasternodeListEntry.end())
    {
        int64_t t = (*i).second;
        if (GetTime() < t) return; // we've asked recently
    }

    // ask for the mnb info once from the node that sent mnp

    LogPrintf("CMasternodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.ToString());
    pnode->PushMessage("dseg", vin);
    int64_t askAgain = GetTime() + MASTERNODE_MIN_DSEEP_SECONDS;
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

void CMasternodeMan::Check()
{

    LOCK(cs);
//LogPrintf("RGP CMasternodeMan::Check() Start \n");
    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();

    }
//LogPrintf("RGP CMasternodeMan::Check() End \n");

}

void CMasternodeMan::CheckAndRemove()
{
    LOCK(cs);

    LogPrintf("*** RGP CMasternodeMan::ChckAndRemove Start \n");

    Check();

//LogPrintf("*** RGP CMasternodeMan::ChckAndRemove after Check() \n");

    //remove inactive
    vector<CMasternode>::iterator it = vMasternodes.begin();
    while(it != vMasternodes.end())
    {
        if((*it).activeState == CMasternode::MASTERNODE_REMOVE || (*it).activeState == CMasternode::MASTERNODE_VIN_SPENT || (*it).protocolVersion < nMasternodeMinProtocol)
        {

            LogPrint("masternode", "CMasternodeMan: Removing inactive masternode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);
//LogPrintf("RGP vMasternodes.erase(it) commented out RESOLVE LATER \n");
            //it = vMasternodes.erase(it);
        }
        else
        {
            ++it;
        }
    }

    // check who's asked for the masternode list
    map<CNetAddr, int64_t>::iterator it1 = mAskedUsForMasternodeList.begin();
    while(it1 != mAskedUsForMasternodeList.end()){
        if((*it1).second < GetTime()) 
        {
            mAskedUsForMasternodeList.erase(it1++);
        } else 
        {
            ++it1;
        }
    }

    // check who we asked for the masternode list
    it1 = mWeAskedForMasternodeList.begin();
    while(it1 != mWeAskedForMasternodeList.end()){
        if((*it1).second < GetTime()){
            mWeAskedForMasternodeList.erase(it1++);
        } else {
            ++it1;
        }
    }

    // check which masternodes we've asked for
    map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
    while(it2 != mWeAskedForMasternodeListEntry.end()){
        if((*it2).second < GetTime()){
            mWeAskedForMasternodeListEntry.erase(it2++);
        } else {
            ++it2;
        }
    }

//LogPrintf("*** RGP CMasternodeMan::ChckAndRemove End \n");

}

void CMasternodeMan::Clear()
{
    LOCK(cs);

    LogPrintf("*** RGP CMasternodeMan::Clear Start \n");

    vMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    nDsqCount = 0;
}

int CMasternodeMan::CountEnabled(int protocolVersion)
{
    int i = 0;
    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;    

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();
        if ( mn.protocolVersion < protocolVersion || !mn.IsEnabled() )
        {            
            continue;
        }

        i++;
    }

    return i;
}

int CMasternodeMan::CountMasternodesAboveProtocol(int protocolVersion)
{
    int i = 0;

   // LogPrintf("*** RGP CMasternodeMan::CountMasternodesAboveProtocol Start \n");

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        mn.Check();
        if(mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        i++;
    }

    return i;
}

void CMasternodeMan::DsegUpdate(CNode* pnode)
{
    LOCK(cs);

    LogPrintf("*** RGP CMasternodeMan::DsegUpdate Start \n");

    std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);
    if (it != mWeAskedForMasternodeList.end())
    {
        if (GetTime() < (*it).second)
        {
            //LogPrintf("dseg - we already asked for the list; skipping...\n");
            return;
        }
    }
    pnode->PushMessage("dseg", CTxIn());
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
}

bool CMasternodeMan::FindNull(const CTxIn &vin)
{
    LOCK(cs);

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {

        if(mn.vin.prevout == vin.prevout)
            return true;
    }

   // LogPrintf("*** RGP CMasternodeMan::Find Debug 2 \n");

    return false;
}


CMasternode *CMasternodeMan::Find(const CTxIn &vin)
{
LogPrintf("RGP mnodeman find vin in #1 %s \n", vin.ToString() ); 
    LOCK(cs);

    LogPrintf("RGP mnodeman find vin in %s \n", vin.ToString() ); 
    LogPrintf("RGP mnodeman find vin recorded Masternodes %d \n", vMasternodes.size() ); 

    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
LogPrintf("RGP mnodeman find search vin in %s stored vin %s \n", vin.ToString(), mn.vin.ToString() ); 

        if(mn.vin.prevout == vin.prevout)
            return &mn;
    }

    return NULL;
}

CMasternode* CMasternodeMan::FindOldestNotInVec(const std::vector<CTxIn> &vVins, int nMinimumAge)
{
    LOCK(cs);

    CMasternode *pOldestMasternode = NULL;

    BOOST_FOREACH(CMasternode &mn, vMasternodes)
    {   
        mn.Check();
        if(!mn.IsEnabled()) continue;

        if(mn.GetMasternodeInputAge() < nMinimumAge) continue;

        bool found = false;
        BOOST_FOREACH(const CTxIn& vin, vVins)
        {

            if(mn.vin.prevout == vin.prevout)
            {   
                found = true;
                break;
            }
        }
        if(found) continue;

        if(pOldestMasternode == NULL || pOldestMasternode->SecondsSincePayment() < mn.SecondsSincePayment())
        {
            pOldestMasternode = &mn;
        }
    }

    return pOldestMasternode;
}

CMasternode *CMasternodeMan::FindRandom()
{
    LOCK(cs);

    //LogPrintf("*** RGP CMasternodeMan::FindRandom Start \n");

    if(size() == 0) return NULL;

    return &vMasternodes[GetRandInt(vMasternodes.size())];
}

CMasternode *CMasternodeMan::Find(const CPubKey &pubKeyMasternode)
{
    LOCK(cs);

    LogPrintf("*** RGP CMasternodeMan::Find Start Masternodes count %d \n", vMasternodes.size() );



    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {
        if(mn.pubkey2 == pubKeyMasternode)
            return &mn;
    }
    return NULL;
}

CMasternode *CMasternodeMan::FindRandomNotInVec(std::vector<CTxIn> &vecToExclude, int protocolVersion)
{
    LOCK(cs);

    protocolVersion = protocolVersion == -1 ? masternodePayments.GetMinMasternodePaymentsProto() : protocolVersion;

     LogPrintf("*** RGP CMasternodeMan::FindRandomNotInVec Start \n");

    int nCountEnabled = CountEnabled(protocolVersion);
    //LogPrintf("CMasternodeMan::FindRandomNotInVec - nCountEnabled - vecToExclude.size() %d\n", nCountEnabled - vecToExclude.size());
    if(nCountEnabled - vecToExclude.size() < 1) return NULL;

    int rand = GetRandInt(nCountEnabled - vecToExclude.size());
    //LogPrintf("CMasternodeMan::FindRandomNotInVec - rand %d\n", rand);
    bool found;

    BOOST_FOREACH(CMasternode &mn, vMasternodes) {
        if(mn.protocolVersion < protocolVersion || !mn.IsEnabled()) continue;
        found = false;
        BOOST_FOREACH(CTxIn &usedVin, vecToExclude) {
            if(mn.vin.prevout == usedVin.prevout) {
                found = true;
                break;
            }
        }
        if(found) continue;
        if(--rand < 1) {
            return &mn;
        }
    }

    return NULL;
}

CMasternode* CMasternodeMan::GetCurrentMasterNode(int mod, int64_t nBlockHeight, int minProtocol)
{
    unsigned int score = 0;
    CMasternode* winner = NULL;

    LogPrintf("*** RGP GetCurrentMasterNode start  \n");

    /* RGP : 13th April 2021 update this line to build vMasternodes */
    std::vector<CMasternode> vMasternodes = mnodeman.GetFullMasternodeVector();

    if ( vMasternodes.empty() )
    {

        LogPrintf("*** RGP vMasternodes empty returning null winner \n");
        return winner;
    }

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes)
    {

        /* -------------------------------------------------------------------
           -- RGP, Patch for BSG-182, where rewards are updated, but old MN --
           -- are not replying. Using GHaydons MN at the moment for the     --
           -- rewards fix.                                                  --
           ------------------------------------------------------------------- */
        LogPrintf("*** RGP GetCurrentMasterNode loop MN addr %s \n", mn.addr.ToString() );

        //if ( mn.addr.ToString() == "143.198.227.129:23980" )
        //{
        //    /* RGP, this is GHayden's MN */
        //    // calculate the score for each masternode
        //    uint256 n = mn.CalculateScore(mod, nBlockHeight);
        //    unsigned int n2 = 0;
        //    memcpy(&n2, &n, sizeof(n2));
        //    winner = &mn;
        //    break;
        //}

        mn.Check();
        LogPrintf("*** RGP RGP GetCurrentMasterNode %s \n",  mn.addr.ToString() );
        if(mn.protocolVersion < minProtocol || !mn.IsEnabled())
        {
            LogPrintf("*** RGP GetCurrentMasterNode Protocol failure %d \n", mn.protocolVersion  );
            continue;

        }

        // calculate the score for each masternode
        uint256 n = mn.CalculateScore(mod, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        // determine the winner
        if(n2 > score)
        {
            score = n2;
            winner = &mn;

            LogPrintf("*** RGP MasterNode Winner of Stake %s \n", winner->addr.ToString() );

        }

    }

    LogPrintf("*** RGP GetCurrentMasterNode end winner node %s \n", winner->addr.ToString() );

    return winner;
}

int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

     //LogPrintf("*** RGP CMasternodeMan::GetMasternodeRank Start \n");

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return -1;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        if(mn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            mn.Check();
            if(!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(s.second == vin) {
            return rank;
        }
    }

    return -1;
}

std::vector<pair<int, CMasternode> > CMasternodeMan::GetMasternodeRanks(int64_t nBlockHeight, int minProtocol)
{
    std::vector<pair<unsigned int, CMasternode> > vecMasternodeScores;
    std::vector<pair<int, CMasternode> > vecMasternodeRanks;

    //make sure we know about this block
    uint256 hash = 0;
    if(!GetBlockHash(hash, nBlockHeight)) return vecMasternodeRanks;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        mn.Check();

        if(mn.protocolVersion < minProtocol) continue;
        if(!mn.IsEnabled()) {
            continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnlyMN());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CMasternode)& s, vecMasternodeScores){
        rank++;
        vecMasternodeRanks.push_back(make_pair(rank, s.second));
    }

    return vecMasternodeRanks;
}

CMasternode* CMasternodeMan::GetMasternodeByRank(int nRank, int64_t nBlockHeight, int minProtocol, bool fOnlyActive)
{
    std::vector<pair<unsigned int, CTxIn> > vecMasternodeScores;

    // scan for winner
    BOOST_FOREACH(CMasternode& mn, vMasternodes) {

        if(mn.protocolVersion < minProtocol) continue;
        if(fOnlyActive) {
            mn.Check();
            if(!mn.IsEnabled()) continue;
        }

        uint256 n = mn.CalculateScore(1, nBlockHeight);
        unsigned int n2 = 0;
        memcpy(&n2, &n, sizeof(n2));

        vecMasternodeScores.push_back(make_pair(n2, mn.vin));
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareValueOnly());

    int rank = 0;
    BOOST_FOREACH (PAIRTYPE(unsigned int, CTxIn)& s, vecMasternodeScores){
        rank++;
        if(rank == nRank) {
            return Find(s.second);
        }
    }

    return NULL;
}

void CMasternodeMan::ProcessMasternodeConnections()
{
    LOCK(cs_vNodes);

//LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Start \n");

    if(!darkSendPool.pSubmittedToMasternode)
    {
        LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Darksend submit failed \n");
//LogPrintf("RGP FIX LASTER \n");
        return;
    }
   
    // RGP THis section of code causes a core dump, let's fix later  
LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections coredumps TO BE RESOLVED vNodes %d \n", vNodes.size() );
    if ( vNodes.size() == 0 );
        return;

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        LogPrintf("*** RGP CMasternodeMan::ProcessMasternodeConnections Masternodes %s dk %s \n", pnode->addr.ToString(), darkSendPool.pSubmittedToMasternode->addr.ToString() );

        if(darkSendPool.pSubmittedToMasternode->addr == pnode->addr) 
        {

            continue;
        }

        if(pnode->fDarkSendMaster)
        {
            LogPrintf("Closing masternode connection %s \n", pnode->addr.ToString().c_str());
            pnode->CloseSocketDisconnect();
        }
    }
}

/* --------------------------------------------------------------------------
   -- RGP, ProcessMessage processes all messages from wallets to and from  --
   --      Masternodes.                                                    --
   --                                                                      --
   --      dsee  -> Darksend Election                                      --
   --      dsee+ -> Darksend Election +                                    --
   -------------------------------------------------------------------------- */



void CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
uint256 hashBlock = 0;
int time_to_synch, last_hash_time;
//extern int64_t Last_known_block_height;

    //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Start \n");

    //Normally would disable functionality, NEED this enabled for staking.
    if(fLiteMode) return;

    if(!darkSendPool.IsBlockchainSynced() )
    {
        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Darksend blockchain is not synched  \n");

        /* Get inventory for everything from current blockchain */
        PushGetBlocks( pfrom,  pindexBest, pindexBest->GetBlockHash());
        
        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage start height %d nBestHeight %d \n", pfrom->nStartingHeight, nBestHeight );

        if ( ( nBestHeight + 20 ) > pfrom->nStartingHeight )
 //       {
/* RGP some time requirement here, if we are one block out, then just let the MN start */
             time_to_synch = ( GetTime() - pindexBest->nTime );
        
            /* Allow MN to start */
 //       }
        else
            return;


    }

    LOCK(cs_process_message);

//LogPrintf("*** RGP CMasternodeMan::ProcessMessage Processing messages \n");

    if (strCommand == "dsee")
    {
        //DarkSend Election Entry

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage 'dsee' entry \n");

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        std::string strMessage;
        CScript rewardAddress = CScript();
        int rewardPercentage = 0;
        std::string strKeyMasternode;

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 1 \n");

        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion;

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 2 \n");

        //Invalid nodes check
        if (sigTime < 1511159400)
        {
            LogPrintf("dsee - Bad packet\n");
            return;
        }
        MilliSleep(1); /* RGP Optimise */
        
        LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 3 \n");


        if (sigTime > lastUpdated) 
        {
            LogPrintf("dsee - Bad node entry\n");
            return;
        }
        
        if (addr.GetPort() == 0) 
        {
            LogPrintf("dsee - Bad port\n");
            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) 
        {
            LogPrintf("dsee - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 4 \n");

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(RegTest()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion);    

        LogPrintf("*** RGP, CMasternodeMan::ProcessMessage strMessage %s \n", strMessage );

        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION)
        {
            LogPrintf("dsee - ignoring outdated masternode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }
	    MilliSleep(1); /* RGP Optimise */
	
        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("dsee - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 4a \n");

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("dsee - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 5 \n");

        if(!vin.scriptSig.empty()) {
            //LogPrintf("dsee - Ignore Not Empty ScriptSig %s\n",vin.ToString().c_str());
            return;
        }

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage DSEE Debug 6 \n");

        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage)){
            LogPrintf("dsee - Got bad masternode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }
	    MilliSleep(1); /* RGP Optimise */
	
       LogPrintf("*** RGP CMasternodeMan::ProcessMessage DSEE Debug 7 \n");

        //search existing masternode list, this is where we update existing masternodes with new dsee broadcasts
        CMasternode* pmn = this->Find(vin);
        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part

        if (  pmn == NULL )
            LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7a pwn is NULL \n");

        if ( activeMasternode.vin != CTxIn() )
        {
            LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7b pwn activeMasternode.vin %s  \n", activeMasternode.vin.ToString());
            CTxIn test_tx;
            test_tx == CTxIn();
            LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7c CTxIn() %s  \n", test_tx.ToString() );

        }

        if ( pubkey2 != activeMasternode.pubKeyMasternode )
        {
            LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7d pwn pubkey2.vin %s  \n", pubkey2.GetID().ToString() );
LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7e pwn  %s activeMasternode.pubKeyMasternode  \n", activeMasternode.pubKeyMasternode.GetID().ToString()  );
        }
LogPrintf("RGP CMasternodeMan::ProcessMessage DSEE Debug 7f addr %s \n", addr.ToString() );
        if(pmn != NULL && !(fMasterNode && activeMasternode.vin == CTxIn() && pubkey2 == activeMasternode.pubKeyMasternode))
        {
LogPrintf("*** RGP CMasternodeMan::ProcessMessage DSEE Debug 10 \n");
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
            if(count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS))
            {
LogPrintf("RPG CMasternodeMan::ProcessMessage DSEE Debug 7fa addr %s \n", addr.ToString() );
              pmn->UpdateLastSeen();
                if(pmn->sigTime < sigTime)
                { //take the newest entry
                    if (!CheckNode((CAddress)addr))
                    {
LogPrintf("*** RGP CMasternodeMan::ProcessMessage DSEE Debug 10.1 \n");

                        pmn->isPortOpen = false;
                    } else 
                    {
                        pmn->isPortOpen = true;
                        addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
                    }
                   // LogPrintf("dsee - Got updated entry for %s\n", addr.ToString().c_str());
                    pmn->pubkey2 = pubkey2;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->Check();
                    pmn->isOldNode = true;
                    if(pmn->IsEnabled())
                    {
LogPrintf("*** RGP CMasternodeMan::ProcessMessage DSEE Debug RelayOldMasternodeEntry 10.1 \n");
                        mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
                    }
                }
            }

            /* RGP Removed return  return; */
        }

	MilliSleep(1); /* RGP Optimise */
	
        LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 20 \n");

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) 
        {
            LogPrintf("dsee - Got mismatched pubkey and vin\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CValidationState state;

 LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 21 \n");

        CTransaction tx = CTransaction();

 LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 22 \n");

        CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
 LogPrintf("*** RGP CMasternodeMan::ProcessMessage Debug 23 \n");

        tx.vin.push_back(vin);
        tx.vout.push_back(vout); 
        
    // Find the block it claims to be in
    //map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    //if (mi == mapBlockIndex.end())
    //    return 0;
    //CBlockIndex* pindex = (*mi).second;
    //if (!pindex || !pindex->IsInMainChain())
    //    return 0;
    //CBlock block;
    //if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))


LogPrintf("RGP MN ProcessMessage Debug DSEE 25 MN_CTransaction_Map size %d \n", MN_CTransaction_Map.size() ); 

        if ( MN_CTransaction_Map.size() == 0 )
        {
            hashBlock = 0;  

            /* -----------------------------------------------------------------------------
               -- GetTransaction will only return a hashblock, if the TX has been written --
               ----------------------------------------------------------------------------- */ 

           if ( !GetTransaction(vin.prevout.hash, tx, hashBlock ) )
           {
             LogPrintf("Masternode dsee+  hash %s added to MN_CTransaction_Map DSEE \n", vin.prevout.hash.ToString() );
             MN_CTransaction_Map.insert(std::make_pair( vin.prevout.hash, tx ));
           } 
        }

        if ( MN_CTransaction_Map.find( vin.prevout.hash ) != MN_CTransaction_Map.end() )
        {
            hashBlock = 0;

           if ( !GetTransaction(vin.prevout.hash, tx, hashBlock ) )
           {
             LogPrintf("Masternode dsee+  hash %s added to MN_CTransaction_Map DSEE \n", vin.prevout.hash.ToString() );
             MN_CTransaction_Map.insert(std::make_pair( vin.prevout.hash, tx ));
           } 
        }
        else
        {
            LogPrintf("Something was found hash DSEE %s \n", vin.prevout.hash.ToString() );
        }

LogPrintf("RGP MN ProcessMessage Debug DSEE 300 \n"); 



        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        //CValidationState state;
        tx = CTransaction();
        vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);
        tx.vin.push_back(vin);
        tx.vout.push_back(vout);
        bool fAcceptable = false;
        {
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) return;
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
            if ( !fAcceptable );
            {
            	LogPrintf("RGP Looks like Collateral is not enough %s tx.vout.size() %d \n", tx.ToString(), tx.vout.size() );

                /* -- RGP
                   -- input looks like 149999.00, which good enough, as the cost to send reduces from 150,000 */

                for (unsigned int i = 0; i < tx.vout.size(); i++)
                {
                    LogPrintf("RGP MN ProcessMessage CTranaction.vout %s \n", tx.vout[i].ToString() );
                    if ( tx.vout[ i ].nValue >= 149999.00 )
                        fAcceptable = true;
                }
                
                 
                
                LogPrintf("RGP MN ProcessMessage CTranaction.vout %s \n", tx.vout[0].ToString() );
                if ( tx.vout[ 0 ].nValue > 149999.00 )
                    fAcceptable = true;
                
LogPrintf("RGP MN ProcessMessage Debug DSEE 301 \n"); 

            }
        }

        if(fAcceptable)
        {
            LogPrintf("masternode, dsee - Accepted masternode entry %i %i\n", count, current);

            if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                LogPrintf("dsee - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);
                /* RGP removed for debug */
                // Misbehaving(pfrom->GetId(), 20);
                //return;
            }

            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 SocietyG tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            GetTransaction(vin.prevout.hash, tx, hashBlock);
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
            {
                CBlockIndex* pMNIndex = (*mi).second; // block for 10000 SocietyG tx -> 1 confirmation
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1)); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    LogPrintf("dsee - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d)\n",
                              sigTime, addr.ToString(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    /* TO BE RESOLVED when transactions are resolved */
                    //return;
                }
            }
LogPrintf("masternode, dsee - addr node %s \n", addr.ToString() );
            // add our masternode
            CMasternode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, rewardAddress, rewardPercentage);
            mn.UpdateLastSeen(lastUpdated);

            if (!CheckNode((CAddress)addr))
            {
                mn.ChangePortStatus(false);
            } 
            else
            {
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
            }
            
            mn.ChangeNodeStatus(true);
            this->Add(mn);

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION){
                activeMasternode.EnableHotColdMasterNode(vin, addr);
                }

            if(count == -1 && !isLocal)
            {
LogPrintf("dsee - RelayOldMasternodeEntry masternode entry %s\n", addr.ToString().c_str());
                mnodeman.RelayOldMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
            }
        } 
        else 
        {
            LogPrintf("dsee - Rejected masternode entry %s\n", addr.ToString().c_str());

            int nDoS = 0;
            if (state.IsInvalid(nDoS))
            {
                LogPrintf("dsee - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
                    pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
                if (nDoS > 0)
                    Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }
    else if (strCommand == "dsee+") 
    { //DarkSend Election Entry+

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage 'dsee+' Message \n");

        CTxIn vin;
        CService addr;
        CPubKey pubkey;
        CPubKey pubkey2;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        int count;
        int current;
        int64_t lastUpdated;
        int protocolVersion;
        CScript rewardAddress;
        int rewardPercentage;
        std::string strMessage;
        std::string strKeyMasternode;
        int vsigcheck;

        if ( fMasterNode )
            LogPrintf("RGP, fMasterNode is TRUE \n");
        else
            LogPrintf("RGP, fMasterNode is FALSE \n");

        LogPrintf("*** RGP CMasternodeMan::ProcessMessage dsee+ read from another node \n") ;
	
	MilliSleep(1); /* RGP Optimise */
	
        // 70047 and greater
        vRecv >> vin >> addr >> vchSig >> sigTime >> pubkey >> pubkey2 >> count >> current >> lastUpdated >> protocolVersion >> rewardAddress >> rewardPercentage;        


        std::string vchPubKeyX1(pubkey.begin(), pubkey.end());
        std::string vchPubKeyX2(pubkey2.begin(), pubkey2.end());
        std::string vchsigtestX1( vchSig.begin(), vchSig.end() );

        LogPrintf(" \n");

        LogPrintf("*** RGP CTxin VIN Poutput %s \n", vin.prevout.ToString() );      /* the prevout.hash entry has the transaction to the MN collatoral */
        LogPrintf("*** RGP CTxin VIN scriptsig %s \n", vin.scriptSig.ToString() );
        LogPrintf("*** RGP CTxin VIN prevpubkey %s \n", vin.prevPubKey.ToString() );
        LogPrintf("*** RGP Node Addr %s \n",  addr.ToString() );
        LogPrintf("*** RGP vchSig %s \n",   vchsigtestX1 );
        LogPrintf("*** RGP Time NOW %d \n",  lastUpdated );
        

        LogPrintf("*** RGP pubkey %s \n", pubkey.GetID().ToString() );
        LogPrintf("*** RGP pubkey2 %s \n", pubkey2.GetID().ToString() );


        CTxDestination address1;
        //ExtractDestination(rewardAddress, address1);

        //LogPrintf("*** RGP Rewardaddress %s \n",   address1 );
        //LogPrintf("*** RGP Reeward Percent %d \n",   rewardPercentage );


        //Invalid nodes check
        if (sigTime < 1511159400) {
            LogPrintf("dsee+ - Bad packet\n");
            return;
        }
        
        if (sigTime > lastUpdated) {
            LogPrintf("dsee+ - Bad node entry\n");
            return;
        }
        
        if (addr.GetPort() == 0) {
            LogPrintf("dsee+ - Bad port\n");
            return;
        }
        
        // make sure signature isn't in the future (past is OK)
        if (sigTime > GetAdjustedTime() + 60 * 60) {
            LogPrintf("dsee+ - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        bool isLocal = addr.IsRFC1918() || addr.IsLocal();
        //if(RegTest()) isLocal = false;

        std::string vchPubKey(pubkey.begin(), pubkey.end());
        std::string vchPubKey2(pubkey2.begin(), pubkey2.end());

        strMessage = addr.ToString() + boost::lexical_cast<std::string>(sigTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(protocolVersion)  + rewardAddress.ToString() + boost::lexical_cast<std::string>(rewardPercentage);
        
        LogPrintf("RGP ProcessMessage strmessage %s \n", strMessage );


        if(rewardPercentage < 0 || rewardPercentage > 100){
            LogPrintf("dsee+ - reward percentage out of range %d\n", rewardPercentage);
            return;
        }


        if(protocolVersion < MIN_POOL_PEER_PROTO_VERSION) {
            LogPrintf("dsee+ - ignoring outdated masternode %s protocol version %d\n", vin.ToString().c_str(), protocolVersion);
            return;
        }
        
	    MilliSleep(1); /* RGP Optimise */
	
        CScript pubkeyScript;
        pubkeyScript.SetDestination(pubkey.GetID());

        if(pubkeyScript.size() != 25) {
            LogPrintf("dsee+ - pubkey the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        CScript pubkeyScript2;
        pubkeyScript2.SetDestination(pubkey2.GetID());

        if(pubkeyScript2.size() != 25) {
            LogPrintf("dsee+ - pubkey2 the wrong size\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        if(!vin.scriptSig.empty()) {
            LogPrintf("dsee+ - Ignore Not Empty ScriptSig %s\n",vin.ToString().c_str());
            return;
        }

        std::string errorMessage = "";
        if(!darkSendSigner.VerifyMessage(pubkey, vchSig, strMessage, errorMessage))
        {
            LogPrintf("dsee+ - Got bad masternode address signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

LogPrintf("RGP MN ProcessMessage Debug 100 \n");

	MilliSleep(1); /* RGP Optimise */

        /* ------------------------------------------------------------------------------------------------------------ 
           -- RGP search existing masternode list, in this case dsee+ is a new MN entry, Find should return no entry --
           -- Why is this code here then???
           ------------------------------------------------------------------------------------------------------------ */
        CMasternode* pmn = this->Find(vin);
        // if we are a masternode but with undefined vin and this dsee is ours (matches our Masternode privkey) then just skip this part
        if(pmn != NULL && !(fMasterNode && activeMasternode.vin == CTxIn() && pubkey2 == activeMasternode.pubKeyMasternode))
        {
LogPrintf("RGP MN ProcessMessage updating pmn structure \n");
            // count == -1 when it's a new entry
            //   e.g. We don't want the entry relayed/time updated when we're syncing the list
            // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
            //   after that they just need to match
LogPrintf("RGP MN ProcessMessage Debug 190.000.5 count is %d  MASTERNODE_MIN_DSEE_SECONDS %d \n", count, MASTERNODE_MIN_DSEE_SECONDS ); 
LogPrintf("RGP MN ProcessMessage Debug 190.000.6 pmn->pubkey %s pubkey %s \n", pmn->pubkey.GetID().ToString(), pubkey.GetID().ToString() );
            if(count == -1 && pmn->pubkey == pubkey && !pmn->UpdatedWithin(MASTERNODE_MIN_DSEE_SECONDS))
            {
                pmn->UpdateLastSeen();
LogPrintf("RGP MN ProcessMessage Debug 190.001 \n"); 
                if(pmn->sigTime < sigTime)
                { //take the newest entry

LogPrintf("RGP MN ProcessMessage Debug 190.002 addr %s \n", addr.ToString() ); 
                    if (!CheckNode((CAddress)addr))
                    {
LogPrintf("RGP MN ProcessMessage updating pmn structure PORT OPEN IS FALSE \n");
                        // fix up should be false
                        pmn->isPortOpen = true;
                        // fix up
                        addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
                    } 
                    else 
                    {
LogPrintf("RGP MN ProcessMessage Debug 190.003 \n"); 
                        pmn->isPortOpen = true;
                        addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
                    }
                    LogPrintf("dsee+ - Got updated entry for %s\n", addr.ToString().c_str());
                    pmn->pubkey2 = pubkey2;
                    pmn->sigTime = sigTime;
                    pmn->sig = vchSig;
                    pmn->protocolVersion = protocolVersion;
                    pmn->addr = addr;
                    pmn->rewardAddress = rewardAddress;
                    pmn->rewardPercentage = rewardPercentage;                    
                    pmn->Check();
                    pmn->isOldNode = false;
LogPrintf("RGP MN ProcessMessage Debug 190.004 \n"); 
                    if(pmn->IsEnabled())
                    {
                        LogPrintf("dsee+ - pmn is  enabled, RelayMasternodeEntry  \n");
                        mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage );
                    }
                    else
                    {
                         LogPrintf("dsee+ - pmn is not enabled \n");
                    }
                }
                else
LogPrintf("RGP MN ProcessMessage Debug 190.001Y \n"); 

            }
            else
LogPrintf("RGP MN ProcessMessage Debug 190.001x \n"); 
	    MilliSleep(1); /* RGP Optimise */
	    
            /* RGP remove return in order to store the MN TX info */
            //return;
        }
        else
        {
LogPrintf("RGP MN ProcessMessage Debug 190, the vin has no associated Masternode, which is correct \n"); 
        }

LogPrintf("RGP MN ProcessMessage Debug 200 \n"); 

        // make sure the vout that was signed is related to the transaction that spawned the masternode
        //  - this is expensive, so it's only done once per masternode
        if(!darkSendSigner.IsVinAssociatedWithPubkey(vin, pubkey)) 
        {
            LogPrintf("dsee+ - Got mismatched pubkey and vin\n");
            //Misbehaving(pfrom->GetId(), 100);
            return;
        }

        LogPrintf("masternode dsee+ Got NEW masternode entry %s\n", addr.ToString().c_str());

        // make sure it's still unspent
        //  - this is checked later by .check() in many places and by ThreadCheckDarkSendPool()

        /* RGP, note none of the transactions are in the mempool, this needs more investigation */

        CTransaction check_tx = CTransaction();

        if ( MN_CTransaction_Map.size() == 0 )
        {
            hashBlock = 0;

            /* -----------------------------------------------------------------------------
               -- GetTransaction will determine if this vin exists, if not, it will store --
               -- it for ManageStatus().                                                  --
               ----------------------------------------------------------------------------- */ 
           if ( Get_MN_Transaction(vin.prevout.hash, check_tx, hashBlock ) )
           {
             LogPrintf("Masternode dsee+  hash %s added to MN_CTransaction_Map DSEE+ \n", vin.prevout.hash.ToString() );
             MN_CTransaction_Map.insert(std::make_pair( vin.prevout.hash, check_tx ));
           }
           else 
           {
LogPrintf("RGP MN ProcessMessage Debug 200.5 Get_MN_Transaction FAILED \n"); 
                return;
           } 
        }


        if ( MN_CTransaction_Map.find( vin.prevout.hash ) != MN_CTransaction_Map.end() )
        {
            hashBlock = 0;

           if ( !Get_MN_Transaction(vin.prevout.hash, check_tx, hashBlock ) )
           {
             LogPrintf("Masternode dsee+  hash %s added to MN_CTransaction_Map DSEE+ \n", vin.prevout.hash.ToString() );
             MN_CTransaction_Map.insert(std::make_pair( vin.prevout.hash, check_tx ));
           } 
        }
        else
        {
            LogPrintf("Something was found hash DSEE+ %s \n", vin.prevout.hash.ToString() );
        }
LogPrintf("RGP MN ProcessMessage Debug DSEE+ 208 MN_CTransaction_Map.size() is %d \n", MN_CTransaction_Map.size() );

        CValidationState state;
        CTransaction tx = CTransaction();
        //CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight)-1)*COIN, darkSendPool.collateralPubKey);

/* RGP removed -1 from the GetMNCollateral amount, caused the value to be 149999 */
CTxOut vout = CTxOut((GetMNCollateral(pindexBest->nHeight) )*COIN, darkSendPool.collateralPubKey);

LogPrintf("RGP MN ProcessMessage Debug DSEE+ 210 Collatoral is %s \n", vout.ToString() );

        tx.vin.push_back(vin);
        tx.vout.push_back(vout); 
        
//LogPrintf("RGP MN ProcessMessage Debug DSEE+ 250 vout %s \n", tx.ToString() ); 

LogPrintf("RGP MN ProcessMessage Debug 300 \n\n"); 

        bool fAcceptable = false;
        {
            TRY_LOCK(cs_main, lockMain);
            if(!lockMain) return;
            fAcceptable = AcceptableInputs(mempool, tx, false, NULL);
            if ( !fAcceptable );
            {
            	LogPrintf("RGP Looks like Collateral is not enough %s tx.vout.size() %d \n", tx.ToString(), tx.vout.size() );

                /* -- RGP
                   -- input looks like 149999.00, which good enough, as the cost to send reduces from 150,000 

                   -- TWO pieces of the exact same code RESOLVE */

                for (unsigned int i = 0; i < tx.vout.size(); i++)
                {
                    LogPrintf("RGP MN ProcessMessage CTranaction.vout %s \n", tx.vout[i].ToString() );
                    if ( tx.vout[ i ].nValue > 149990.00 )
                        fAcceptable = true;
                }

            }
        }
        if( fAcceptable )
        {
            LogPrintf("masternode, dsee+ - Accepted masternode entry %i %i\n", count, current);
/* NOTE : Create a new GetInputAge_MN */
            if( GetInputAge( vin ) < MASTERNODE_MIN_CONFIRMATIONS)
            {
                LogPrintf("TO BE RESOLVED dsee+ - Input must have least %d confirmations\n", MASTERNODE_MIN_CONFIRMATIONS);

                // RGP removed for Debug 
                //Misbehaving(pfrom->GetId(), 20);

/* RGP transaction from vin is not in the mempool, we will need to fix later */

                // return;
            }

            LogPrintf("masternode, dsee+ DEBUG 001 %s \n", addr.ToString().c_str() );


            // verify that sig time is legit in past
            // should be at least not earlier than block when 10000 SocietyG tx got MASTERNODE_MIN_CONFIRMATIONS
            uint256 hashBlock = 0;
            //GetTransaction(vin.prevout.hash, tx, hashBlock);
            //GetTransaction(vin.prevout.hash, tx, hashBlock);
            Get_MN_Transaction(vin.prevout.hash, tx, hashBlock);

            LogPrintf("masternode, dsee+ DEBUG transaction hash 001 %s \n", vin.prevout.hash.ToString() );

            /* --------------------------------------------------------------------------
               -- RGP, as there are no transactions or even Mempool, the block that is --
               --      associated with the transaction cannot be found.                --
               --      The blockhash is linked to the transaction, so let's go through --
               --      mapblockIndex and find the transaction.                         --
               -------------------------------------------------------------------------- */

            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi != mapBlockIndex.end() && (*mi).second)
            {

                LogPrintf("masternode, dsee+ DEBUG 002 %s \n", addr.ToString().c_str() );

                CBlockIndex* pMNIndex = (*mi).second; // block for 150, 000 SocietyG tx -> 1 confirmation
                CBlockIndex* pConfIndex = FindBlockByHeight((pMNIndex->nHeight + MASTERNODE_MIN_CONFIRMATIONS - 1)); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
                if(pConfIndex->GetBlockTime() > sigTime)
                {
                    LogPrintf("dsee+ - Bad sigTime %d for masternode %20s %105s (%i conf block is at %d)\n",
                              sigTime, addr.ToString(), vin.ToString(), MASTERNODE_MIN_CONFIRMATIONS, pConfIndex->GetBlockTime());
                    return;
                }
            }

           LogPrintf("masternode, dsee+ DEBUG 003 %s \n", addr.ToString().c_str() );

            //doesn't support multisig addresses
            if(rewardAddress.IsPayToScriptHash())
            {
                rewardAddress = CScript();
                rewardPercentage = 0;
LogPrintf("masternode, dsee+ DEBUG 003.5 %s reward percentage %d \n", pfrom->addr.ToString(), rewardPercentage );
            }

            LogPrintf("masternode, dsee+ DEBUG 004 %s \n", addr.ToString().c_str() );

            // add our masternode
            CMasternode mn(addr, vin, pubkey, vchSig, sigTime, pubkey2, protocolVersion, rewardAddress, rewardPercentage);
            mn.UpdateLastSeen(lastUpdated);

            
            /* --------------------------------------------------------------------------------------------- 
               -- RGP, CheckNode tries to establish if the MN local node is in vNodes, as this is called  --
               --      for a new MN from the remote wallet, the code is a waste of time, as vNodes        --
               --      only has REMOTE notes.                                                             --
               --------------------------------------------------------------------------------------------- */

 //           if ( !CheckNode((CAddress)addr) )
 //           {
 //               LogPrintf("Masternode, dsee+ DEBUG 4.5 strMasterNodeAddr %s addr %s \n", strMasterNodeAddr, addr.ToString().c_str() );
 //
 //               /* If our own IP ignore as it never works */
 //               if ( addr.ToString().c_str() == strMasterNodeAddr )
 //               {
 //
 //                   /* RGP, INvalid check as the MN is this ipaddr */
 //                   LogPrintf("TO BE RESOLVED FIXUP masternode, dsee+ DEBUG Failed CheckNode 005 %s \n", addr.ToString().c_str() );
 //                   
 //
 //                   mn.ChangePortStatus(true);
 //                   addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
 //               }
 //           }
 //           else
 //           {
                LogPrintf("masternode, dsee+ DEBUG good CheckNode 006 %s \n", addr.ToString().c_str() );
                mn.ChangePortStatus(true);
                addrman.Add(CAddress(addr), pfrom->addr, 2*60*60); // use this as a peer
 //           }
            

            mn.ChangeNodeStatus(true);
            this->Add(mn);
            MilliSleep(1); /* RGP Optimise */
            
            //LogPrintf("masternode, dsee+ DEBUG 007a %s \n", addr.ToString().c_str() );

            // if it matches our masternodeprivkey, then we've been remotely activated
            if(pubkey2 == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION)
            {
                LogPrintf("masternode, dsee+ DEBUG 008 %s \n", addr.ToString().c_str() );

                activeMasternode.EnableHotColdMasterNode(vin, addr);
            }

            //LogPrintf("masternode, dsee+ DEBUG 008a %s \n", addr.ToString().c_str() );

            //if(count == -1 && !isLocal)
            if(count == -1 )
            {

                LogPrintf("*** RGP Process Message DSee+ from %s  Validity Checks strKeyMasterNode %s \n", addr.ToString().c_str(), strKeyMasternode.c_str() );
                mnodeman.RelayMasternodeEntry(vin, addr, vchSig, sigTime, pubkey, pubkey2, count, current, lastUpdated, protocolVersion, rewardAddress, rewardPercentage );

             }

	     MilliSleep(1); /* RGP Optimise */
            //LogPrintf("masternode, dsee+ DEBUG 008b %s \n", pfrom->addr.ToString() );

        }
        else
        {
            LogPrintf("dsee+ - Rejected masternode entry \n");

            int nDoS = 0;
            //if (state.IsInvalid(nDoS))
            //{
            //    LogPrintf("dsee+ - %s from %s %s was not accepted into the memory pool\n", tx.GetHash().ToString().c_str(),
            //        pfrom->addr.ToString().c_str(), pfrom->cleanSubVer.c_str());
            //    if (nDoS > 0)
            //        Misbehaving(pfrom->GetId(), nDoS);
            //}
        }

        LogPrintf("\n\n*** RGP Process Message DSee+ COMPLETED \n\n");

    }
    else if (strCommand == "dseep") 
    { //DarkSend Election Entry Ping

        CTxIn vin;
        vector<unsigned char> vchSig;
        int64_t sigTime;
        bool stop;
        vRecv >> vin >> vchSig >> sigTime >> stop;

        LogPrintf("dseep - Received: vin: %s sigTime: %lld stop: %s\n", vin.ToString().c_str(), sigTime, stop ? "true" : "false");

        if (sigTime > GetAdjustedTime() + 60 * 60) 
        {
            LogPrintf("dseep - Signature rejected, too far into the future %s\n", vin.ToString().c_str());
            return;
        }

        if (sigTime <= GetAdjustedTime() - 60 * 60) 
        {
            LogPrintf("dseep - Signature rejected, too far into the past %s - %d %d \n", vin.ToString().c_str(), sigTime, GetAdjustedTime());
            return;
        }

LogPrintf("RGP dseep Debug 100 \n");

        // see if we have this masternode
        //CMasternode* pmn = this->Find(vin);

        //if ( !this->Find(vin) )
        //    LogPrintf("*** RGP DSEEP command, vin not found, NEW MN \n");

        CMasternode* pmn = this->Find(vin);
	    MilliSleep(1); /* RGP Optimise */

        if(pmn != NULL && pmn->protocolVersion >= MIN_POOL_PEER_PROTO_VERSION)
        {

LogPrintf("RGP dseep Debug 200 \n");

            // LogPrintf("dseep - Found corresponding mn for vin: %s\n", vin.ToString().c_str());
            // take this only if it's newer
LogPrintf("RGP Process Message dseep LastDseep %d sigtime %d \n", pmn->lastDseep, sigTime );
            if(pmn->lastDseep <= sigTime)
            {
                std::string strMessage = pmn->addr.ToString() + boost::lexical_cast<std::string>(sigTime) + boost::lexical_cast<std::string>(stop);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("dseep - Got bad masternode address signature %s \n", vin.ToString().c_str());
                    //Misbehaving(pfrom->GetId(), 100);
                    return;
                }

                pmn->lastDseep = sigTime;

                if(!pmn->UpdatedWithin(MASTERNODE_MIN_DSEEP_SECONDS))
                {
if(stop)
LogPrintf("RGP dseep Debug 201 stop is set \n");

                    if(stop) pmn->Disable();
                    else
                    {
LogPrintf("RGP dseep Debug 220 \n");
                        pmn->UpdateLastSeen();
                        pmn->Check();
                        if(!pmn->IsEnabled()) return;
                    }
                    mnodeman.RelayMasternodeEntryPing(vin, vchSig, sigTime, stop);
                }
            }
LogPrintf("RGP dseep Debug 230 \n");
            MilliSleep(1); /* RGP Optimise */
            return;
        }     
  
LogPrintf("RGP dseep Debug 240 \n");
        std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
        if (i != mWeAskedForMasternodeListEntry.end())
        {
            int64_t t = (*i).second;
            if (GetTime() < t) return; // we've asked recently
        }
LogPrintf("RGP dseep Debug 250 \n");
        // ask for the dseep info once from the node that sent dseep

        LogPrintf("dseep - Asking source node for missing entry %s\n", vin.ToString().c_str());
        pfrom->PushMessage("dseg", vin);
        int64_t askAgain = GetTime()+ MASTERNODE_MIN_DSEEP_SECONDS;
        mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
LogPrintf("RGP dseep Debug 260 \n");

    } 
    else if (strCommand == "mvote") 
    { //Masternode Vote

        CTxIn vin;
        vector<unsigned char> vchSig;
        int nVote;
        vRecv >> vin >> vchSig >> nVote;

LogPrintf("mvote - Received: \n");


        // see if we have this Masternode
        CMasternode* pmn = this->Find(vin);
        if(pmn != NULL)
        {
            if((GetAdjustedTime() - pmn->lastVote) > (60*60))
            {
                std::string strMessage = vin.ToString() + boost::lexical_cast<std::string>(nVote);

                std::string errorMessage = "";
                if(!darkSendSigner.VerifyMessage(pmn->pubkey2, vchSig, strMessage, errorMessage))
                {
                    LogPrintf("mvote - Got bad Masternode address signature %s \n", vin.ToString().c_str());
                    return;
                }

                pmn->nVote = nVote;
                pmn->lastVote = GetAdjustedTime();

                //send to all peers
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                    pnode->PushMessage("mvote", vin, vchSig, nVote);
            }
	        MilliSleep(1); /* RGP Optimise */
	    
            return;
        }

    } 
    else if (strCommand == "dseg") 
    { //Get masternode list or specific entry

        CTxIn vin;
        vRecv >> vin;

        if(vin == CTxIn()) { //only should ask for this once
            //local network
            if(!pfrom->addr.IsRFC1918() && Params().NetworkID() == CChainParams::MAIN)
            {
                std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);
                if (i != mAskedUsForMasternodeList.end())
                {
                    int64_t t = (*i).second;
                    if (GetTime() < t) {
                        Misbehaving(pfrom->GetId(), 34);
                        LogPrintf("dseg - peer already asked me for the list\n");
                        return;
                    }
                }

                int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
                mAskedUsForMasternodeList[pfrom->addr] = askAgain;
            }
        } //else, asking for a specific node which is ok

        int count = this->size();
        int i = 0;

        BOOST_FOREACH(CMasternode& mn, vMasternodes)
        {

            if(mn.addr.IsRFC1918()) continue; //local network

            if(mn.IsEnabled())
            {
                LogPrintf("masternode, dseg - Sending masternode entry - %s \n", mn.addr.ToString().c_str());
                if(vin == CTxIn())
                {
                    if (mn.isOldNode)
                    {
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    } 
                    else 
                    {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }
                } 
                else if (vin == mn.vin) 
                {
                    if (mn.isOldNode){
                        pfrom->PushMessage("dsee", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion);
                    } else {
                        pfrom->PushMessage("dsee+", mn.vin, mn.addr, mn.sig, mn.sigTime, mn.pubkey, mn.pubkey2, count, i, mn.lastTimeSeen, mn.protocolVersion, mn.rewardAddress, mn.rewardPercentage);
                    }
                    LogPrintf("dseg - Sent 1 masternode entries to %s\n", pfrom->addr.ToString().c_str());
                    return;
                }
                i++;
            }
            MilliSleep(1); /* RGP Optimise */
        }

        //LogPrintf("dseg - Sent %d masternode entries to %s\n", i, pfrom->addr.ToString().c_str());
    }

}

void CMasternodeMan::RelayOldMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig, const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count, const int current, const int64_t lastUpdated, const int protocolVersion)
{
    LOCK(cs_vNodes);

    //LogPrintf("*** RGP CMasternodeMan::RelayOldMasternodeEntry Start \n") ;

    BOOST_FOREACH(CNode* pnode, vNodes)
    {
       // LogPrintf("*** RGP CMasternodeMan::RelayOldMasternodeEntry to node \n" );

        pnode->PushMessage("dsee", vin, addr, vchSig, nNow, pubkey, pubkey2, count, current, lastUpdated, protocolVersion);
        MilliSleep(1); /* RGP Optimise */
    }
}


// RGP Info for debug
// mnodeman.RelayMasternodeEntry(vin, service, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyCollateralAddress,
//                              pubKeyMasternode, -1, -1, masterNodeSignatureTime,
//                              PROTOCOL_VERSION, rewardAddress, rewardPercentage );



void CMasternodeMan::RelayMasternodeEntry(const CTxIn vin, const CService addr, const std::vector<unsigned char> vchSig,
                                          const int64_t nNow, const CPubKey pubkey, const CPubKey pubkey2, const int count,
                                          const int current, const int64_t lastUpdated, const int protocolVersion,
                                          CScript rewardAddress, int rewardPercentage )
{
    LOCK(cs_vNodes);

    /*CTxIn vin;
    CService addr;
    CPubKey pubkey;
    CPubKey pubkey2;
    std::vector<unsigned char> sig;
    int activeState;
    int64_t sigTime; //dsee message times
    int64_t lastDseep;
    int64_t lastTimeSeen;
    int cacheInputAge;
    int cacheInputAgeBlock;
    bool unitTest;
    bool allowFreeTx;
    int protocolVersion;
    int64_t nLastDsq; //the dsq count from the last dsq broadcast of this node
    CScript rewardAddress;
    int rewardPercentage;
    int nVote;
    int64_t lastVote;
    int nScanningErrorCount;
    int nLastScanningErrorBlockHeight;
    int64_t nLastPaid;
    bool isPortOpen;
    bool isOldNode;
    std::string strKeyMasternode;
    */

LogPrintf("RGP CMasternodeMan::RelayMasternodeEntry Start \n");

    std::string vchPubKey(pubkey.begin(), pubkey.end());
    std::string vchPubKey2(pubkey2.begin(), pubkey2.end());
    std::string vchsigtest( vchSig.begin(), vchSig.end() );


    //(strMode == "pubkey")
    //CScript key1;
    //pubkey.SetDestination(mn.pubkey.GetID());
    //CTxDestination address1;
    //ExtractDestination(pubkey, address1);
    //CSocietyGcoinAddress address2(address1);

    //CScript key2;
    //pubkey.SetDestination(mn.pubkey2.GetID());
    //CTxDestination address3;
    //ExtractDestination(pubkey2, address3);
    //CSocietyGcoinAddress address4(address3);


    //ExtractDestination(mn.rewardAddress, address1);

    //LogPrintf("*** RGp pubkey 1 %s \n", address2.ToString() );
    //LogPrintf("*** RGp pubkey 2 %s \n", address4.ToString() );

    LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntry Start dsee+ message creation \n") ;

    //LogPrintf("*** RGP CTxin VIN %s \n",  vin.ToString() );
    //LogPrintf("*** RGP Node Addr %s \n",  addr.ToString() );
    //LogPrintf("*** RGP vchSig %s \n",   vchsigtest );
    //LogPrintf("*** RGP Time NOW %d \n",  nNow );
    //LogPrintf("*** RGP pubkey1 %s \n",   vchPubKey );
    //LogPrintf("*** RGP pubkey2 %s \n",   vchPubKey2 );



    //LogPrintf("*** RGP Rewardaddress %s \n",   rewardAddress );
    //LogPrintf("*** RGP Reeward Percent %d \n",   rewardPercentage );


    BOOST_FOREACH(CNode* pnode, vNodes)
    {

        LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntry to node %s \n", pnode->addr.ToString().c_str()  );
        LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntry vin %s rewardaddr %s \n", vin.ToString(), rewardAddress.ToString()  );
        

        pnode->PushMessage("dsee+",
                               vin,
                              addr,
                            vchSig,
                              nNow,
                            pubkey,
                           pubkey2,
                             count,
                           current,
                       lastUpdated,
                   protocolVersion,
                     rewardAddress,
                  rewardPercentage );
                  
        MilliSleep(1); /* RGP Optimise */
    }
}

void CMasternodeMan::RelayMasternodeEntryPing(const CTxIn vin, const std::vector<unsigned char> vchSig, const int64_t nNow, const bool stop)
{
LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntryPing START \n");

    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntryPing to node %s \n", pnode->addr.ToString().c_str() );
        //if ( pnode-> ) ??? RGP Removed until we know what this is doing, why stop?
        //pnode->PushMessage("dseep", vin, vchSig, nNow, stop);
        
        MilliSleep(1); /* RGP Optimise */
    }
LogPrintf("*** RGP CMasternodeMan::RelayMasternodeEntryPing STOP \n");
}

void CMasternodeMan::Remove(CTxIn vin)
{
    LOCK(cs);

    vector<CMasternode>::iterator it = vMasternodes.begin();
    while(it != vMasternodes.end())
    {
        if((*it).vin == vin){
            LogPrintf("masternode, CMasternodeMan: Removing Masternode %s - %i now\n", (*it).addr.ToString().c_str(), size() - 1);
LogPrintf("RGP vMasternodes.erase(it) commented out RESOLVE LATER \n");
            //vMasternodes.erase(it);
            break;
        } else 
        {
            ++it;
        }
        MilliSleep(1); /* RGP Optimise */
    }
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;

    info << "masternodes: " << (int)vMasternodes.size() <<
            ", peers who asked us for masternode list: " << (int)mAskedUsForMasternodeList.size() <<
            ", peers we asked for masternode list: " << (int)mWeAskedForMasternodeList.size() <<
            ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size() <<
            ", nDsqCount: " << (int)nDsqCount;

    return info.str();
}
