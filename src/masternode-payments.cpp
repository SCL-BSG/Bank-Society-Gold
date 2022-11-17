// Copyright (c) 2014-2015 The Dash developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode-payments.h"
#include "masternodeman.h"
#include "darksend.h"
#include "util.h"
#include "sync.h"
#include "spork.h"
#include "addrman.h"
#include <boost/lexical_cast.hpp>

CCriticalSection cs_masternodepayments;

/** Object for who's going to get paid on which blocks */
CMasternodePayments masternodePayments;
// keep track of Masternode votes I've seen
map<uint256, CMasternodePaymentWinner> mapSeenMasternodeVotes;

int CMasternodePayments::GetMinMasternodePaymentsProto() {
    return MIN_MASTERNODE_PAYMENT_PROTO_VERSION_1;
}

void ProcessMessageMasternodePayments(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{

    if(!darkSendPool.IsBlockchainSynced())
    {
        //LogPrintf("*** RGP ProcessMessageMasternodePayments darkSendPool IsBlockchainSynced Failed \n");
        return;
    }

    if (strCommand == "mnget") { //Masternode Payments Request Sync

        //LogPrintf("*** RGP ProcessMessageMasternodePayments mnget \n");

        if(pfrom->HasFulfilledRequest("mnget"))
        {
            LogPrintf("mnget - peer already asked me for the list\n");
            Misbehaving(pfrom->GetId(), 20);
            return;
        }

        pfrom->FulfilledRequest("mnget");
        masternodePayments.Sync(pfrom);
        LogPrintf("mnget - Sent Masternode winners to %s\n", pfrom->addr.ToString().c_str());
    }
    else if (strCommand == "mnw")
    { //Masternode Payments Declare Winner

        //LogPrintf("*** RGP ProcessMessageMasternodePayments mnw \n");

        LOCK(cs_masternodepayments);

        //this is required in litemode
        CMasternodePaymentWinner winner;
        vRecv >> winner;

        if(pindexBest == NULL) return;

        CTxDestination address1;
        ExtractDestination(winner.payee, address1);
        CSocietyGcoinAddress address2(address1);

        uint256 hash = winner.GetHash();
        if(mapSeenMasternodeVotes.count(hash)) {
            if(fDebug) LogPrintf("mnw - seen vote %s Addr %s Height %d bestHeight %d\n", hash.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.nBlockHeight < pindexBest->nHeight - 10 || winner.nBlockHeight > pindexBest->nHeight+20){
            LogPrintf("mnw - winner out of range %s Addr %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);
            return;
        }

        if(winner.vin.nSequence != std::numeric_limits<unsigned int>::max()){
            LogPrintf("mnw - invalid nSequence\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        //LogPrintf("mnw - winning vote - Vin %s Addr %s Height %d bestHeight %d\n", winner.vin.ToString().c_str(), address2.ToString().c_str(), winner.nBlockHeight, pindexBest->nHeight);

        if(!masternodePayments.CheckSignature(winner)){
            LogPrintf("mnw - invalid signature\n");
            Misbehaving(pfrom->GetId(), 100);
            return;
        }

        mapSeenMasternodeVotes.insert(make_pair(hash, winner));

        if(masternodePayments.AddWinningMasternode(winner)){
            masternodePayments.Relay(winner);
        }
    }

}


bool CMasternodePayments::CheckSignature(CMasternodePaymentWinner& winner)
{
    //note: need to investigate why this is failing
    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    std::string strPubKey = strMainPubKey ;
    CPubKey pubkey(ParseHex(strPubKey));

    //LogPrintf("*** RGP CMasternodePayments::CheckSignature strPubkey <%s> strmessage <%s> \n", strPubKey, strMessage  );


    std::string errorMessage = "";
    if(!darkSendSigner.VerifyMessage(pubkey, winner.vchSig, strMessage, errorMessage)){
        return false;
    }

    return true;
}

bool CMasternodePayments::Sign(CMasternodePaymentWinner& winner)
{
    uint256 blockhash;
    extern uint256 hashkeygen;

    std::string strMessage = winner.vin.ToString().c_str() + boost::lexical_cast<std::string>(winner.nBlockHeight) + winner.payee.ToString();

    CKey key2;
    CPubKey pubkey2;
    std::string errorMessage = "";


    //CMasternode* Related_MN = this->Find(winner.vin);

    // DASH code
    // std::string strMessage = vinMasternode.prevout.ToStringShort() + std::to_string(nBlockHeight) + payee.ToString();



    //GetBlockHash(blockhash, winner.nBlockHeight );



    //std::string strMessage = hashkeygen.ToString();

    //LogPrintf("*** CMasternodePayments::Sign strMessage::Sign strMessage <%s> \n", strMessage );

    //std::string strMessage = txNew.GetHash().ToString() + boost::lexical_cast<std::string>(sigTime);
    //std::string strError = "";
    //std::vector<unsigned char> vchSig;



    std::string debug_Message = winner.vin.ToString().c_str();    

    //LogPrintf("*** RGP CMasternodePayments::Sign winner vin  %s \n", debug_Message );
    //LogPrintf("*** RGP CMasternodePayments::Sign Private Key <%s>  \n", strMessage );

    //if(!darkSendSigner.SetKey(strMasterPrivKey, errorMessage, key2, pubkey2))
    if(!darkSendSigner.SetKey(strMessage, errorMessage, key2, pubkey2))
    {
        LogPrintf("CMasternodePayments::Sign - ERROR: Invalid Masternodeprivkey: '%s'\n", errorMessage.c_str());
        //LogPrintf("*** RGP darkSendSigner.SetKey PrivKey %s  \n", strMessage );
        return false;
    }
    else
        LogPrintf("CMasternodePayments::Sign SUCCESS \n");

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, winner.vchSig, key2)) {
        LogPrintf("CMasternodePayments::Sign - Sign message failed");
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubkey2, winner.vchSig, strMessage, errorMessage)) {
        LogPrintf("CMasternodePayments::Sign - Verify message failed");
        return false;
    }

    return true;
}

uint64_t CMasternodePayments::CalculateScore(uint256 blockHash, CTxIn& vin)
{
    uint256 n1 = blockHash;
    uint256 n2 = Hash(BEGIN(n1), END(n1));
    uint256 n3 = Hash(BEGIN(vin.prevout.hash), END(vin.prevout.hash));
    uint256 n4 = n3 > n2 ? (n3 - n2) : (n2 - n3);

    //printf(" -- CMasternodePayments CalculateScore() n2 = %d \n", n2.Get64());
    //printf(" -- CMasternodePayments CalculateScore() n3 = %d \n", n3.Get64());
    //printf(" -- CMasternodePayments CalculateScore() n4 = %d \n", n4.Get64());

    return n4.Get64();
}

bool CMasternodePayments::GetBlockPayee(int nBlockHeight, CScript& payee, CTxIn& vin)
{

    BOOST_FOREACH( CMasternodePaymentWinner& winner, vWinning ){

        if(winner.nBlockHeight == nBlockHeight) {
            payee = winner.payee;
            vin = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::GetWinningMasternode(int nBlockHeight, CTxIn& vinOut)
{
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning){
        if(winner.nBlockHeight == nBlockHeight) {
            vinOut = winner.vin;
            return true;
        }
    }

    return false;
}

bool CMasternodePayments::AddWinningMasternode(CMasternodePaymentWinner& winnerIn)
{
    uint256 blockHash = 0;
    if(!GetBlockHash(blockHash, winnerIn.nBlockHeight-576))
    {
        LogPrintf("CMasternodePayments::AddWinningMasternode GetBlockHash failed");
        return false;
    }

    winnerIn.score = CalculateScore(blockHash, winnerIn.vin);

    bool foundBlock = false;
    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if(winner.nBlockHeight == winnerIn.nBlockHeight)
        {
            foundBlock = true;
            if(winner.score < winnerIn.score){
                winner.score = winnerIn.score;
                winner.vin = winnerIn.vin;
                winner.payee = winnerIn.payee;
                winner.vchSig = winnerIn.vchSig;

                mapSeenMasternodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

                return true;
            }
        }
    }

    // if it's not in the vector
    if(!foundBlock){
        vWinning.push_back(winnerIn);
        mapSeenMasternodeVotes.insert(make_pair(winnerIn.GetHash(), winnerIn));

        return true;
    }

    return false;
}

void CMasternodePayments::CleanPaymentList()
{
    LOCK(cs_masternodepayments);

    if(pindexBest == NULL) return;

    int nLimit = std::max(((int)mnodeman.size())*((int)1.25), 1000);

    vector<CMasternodePaymentWinner>::iterator it;
    for(it=vWinning.begin();it<vWinning.end();it++){
        if(pindexBest->nHeight - (*it).nBlockHeight > nLimit){
            if(fDebug) LogPrintf("CMasternodePayments::CleanPaymentList - Removing old Masternode payment - block %d\n", (*it).nBlockHeight);
            vWinning.erase(it);
            break;
        }
    }
}


uint256 hashkeygen;

bool CMasternodePayments::ProcessBlock(int nBlockHeight)
{
    bool key_status;
    uint256 hash;
    extern uint256 hashkeygen;
    unsigned int nHash;
    int nMinimumAge;



    //LogPrintf("*** RGP CMasternodePayments::ProcessBlock Start \n");

    LOCK(cs_masternodepayments);

    /* ------------------------------------------------------------
       -- RGP, Check that the Masternode is Enabled and that the --
       --      blockheight is greater that the masternode last   --
       --      block height.                                     --
       ------------------------------------------------------------ */

    if ( nBlockHeight <= nLastBlockHeight )
    {
        return false;
    }

    //LogPrintf("*** RGP CMasternodePayments::ProcessBlock nBlockHeight %d nLastBlockHeight %d \n", nBlockHeight, nLastBlockHeight );

    /* RGP enabled is ALWAYS FALSE, this needs investigating */
    if ( !enabled )
    {
        LogPrintf("*** RGP CMasternodePayments::ProcessBlock NOT ENABLED!!! by default \n" );
        /* RGP this is only set in CMasternodePayments::SetPrivKey() if the MN is a default winner
               seems strange and I need to look at this behaviour.                                  */
        enabled = true;
        //return false;
    }

    CMasternodePaymentWinner newWinner;
    nMinimumAge = mnodeman.CountEnabled();

    CScript payeeSource;

    if( !GetBlockHash(hash, nBlockHeight-10))
    {
        //LogPrintf("*** RGP CMasternodePayments::ProcessBlock GetBlockHash FAILED!!! \n" );
        return false;
    }


    hashkeygen = hash;

    //LogPrintf("*** RGP Hash is <%$> \n", hash.ToString() );

    memcpy(&nHash, &hash, 2);

//LogPrintf("*** RGP nHash is <%h> \n", nHash );

    //LogPrintf("*** RGP ProcessBlock Start nHeight %d vin %s. \n", nBlockHeight, activeMasternode.vin.ToString().c_str() );

    std::vector<CTxIn> vecLastPayments;
    BOOST_REVERSE_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {

        //LogPrintf("*** RGP, Looking for last winner %s \n, winner.vin ");



        //if we already have the same vin - we have one full payment cycle, break
        if(vecLastPayments.size() > (unsigned int)nMinimumAge) break;
        vecLastPayments.push_back(winner.vin);



    }

    int vsigcheck;

    // pay to the oldest MN that still had no payment but its input is old enough and it was active long enough
    CMasternode *pmn = mnodeman.FindOldestNotInVec(vecLastPayments, nMinimumAge);
    if(pmn != NULL)
    {
        newWinner.score = 0;
        newWinner.nBlockHeight = nBlockHeight;
        newWinner.vin = pmn->vin;

        std::string vchPubKey(pmn->pubkey.begin(), pmn->pubkey.end());


        //LogPrintf("*** pmw reward percentage %d node %s pubkey string %s \n", pmn->rewardPercentage, pmn->addr.ToString() , vchPubKey );

        /* Fix for MN with 0% rewards */
        pmn->rewardPercentage = 40;

        if(pmn->rewardPercentage > 0 && (nHash % 100) <= (unsigned int)pmn->rewardPercentage)
        {

            newWinner.payee = pmn->rewardAddress;
            //LogPrintf("*** RGP payee 1 %s \n", newWinner.payee.ToString() );
        }
        else
        {

            newWinner.payee = GetScriptForDestination(pmn->pubkey.GetID());
            //LogPrintf("*** RGP payee 2 %s \n", newWinner.payee.ToString() );
        }


        //CScript key1;
        //pmn->pubkey.SetDestination(pmn->pubkey.GetID());
        //CTxDestination address_pub;
        //ExtractDestination(pmn->pubkey, address_pub);

        //LogPrintf("*** RGp pubkey 1 %s \n", address_pub.ToString() );


        //CSocietyGcoinAddress address2(address1);

        //LogPrintf("*** RGp pubkey 1 %s \n", address_pub.ToString( ) );




        //key_status =  CMasternodePayments::SetPrivKey( pmn->strKeyMasternode );

        payeeSource = GetScriptForDestination(pmn->pubkey.GetID());       
        //LogPrintf("*** RGP payee 3 %s \n", payeeSource.ToString() );

        //LogPrintf(" \n");
        //for ( vsigcheck = 0; vsigcheck < pmn->sigvchSig.size(); vsigcheck++ )
        //{
        //    LogPrintf("%x", vchSig[ vsigcheck ] );
        //}
        //LogPrintf("*** RGP vchSig end \n");

        //LogPrintf("*** RGP pubkey begin \n");
        //for ( vsigcheck = 0; vsigcheck < pmn->pubkey.size(); vsigcheck++ )
        //{
        //    LogPrintf("%x", pmn->pubkey.operator [] ( vsigcheck  ) );
        //}
        //const unsigned char &operator[](unsigned int pos) const { return vch[pos]; }
        //LogPrintf("*** RGP pubkey end \n");

        //LogPrintf("*** RGP pubkey2 begin \n");
        //for ( vsigcheck = 0; vsigcheck < pmn->pubkey2.size(); vsigcheck++ )
        //{
        //    LogPrintf("%x", pmn->pubkey2.operator [] ( vsigcheck  ) );
        //}
        //LogPrintf("*** RGP pubkey2 end \n");


    }


    //if we can't find new MN to get paid, pick first active MN counting back from the end of vecLastPayments list
    if(newWinner.nBlockHeight == 0 && nMinimumAge > 0)
    {
        //LogPrintf(" Find by reverse \n");

        BOOST_REVERSE_FOREACH(CTxIn& vinLP, vecLastPayments)
        {
            CMasternode* pmn = mnodeman.Find(vinLP);            
            if(pmn != NULL)
            {

                //Test_PubKey (pmn->pubkey.begin(), pmn->pubkey.end() );

                //LogPrintf("*** RGP Payments Pick first Active %s Pubkey %s \n",  pmn->addr.ToString(), Test_PubKey  );

                pmn->Check();
                if ( !pmn->IsEnabled() )
                    continue;

                newWinner.score = 0;
                newWinner.nBlockHeight = nBlockHeight;
                newWinner.vin = pmn->vin;

                if(pmn->rewardPercentage > 0 && (nHash % 100) <= (unsigned int)pmn->rewardPercentage)
                {
                    newWinner.payee = pmn->rewardAddress;
                } else {
                    newWinner.payee = GetScriptForDestination( pmn->pubkey.GetID() );
                }

                payeeSource = GetScriptForDestination(pmn->pubkey.GetID());

                break; // we found active MN
            }
            else
                LogPrintf("*** RGP, mno valid MN found Yet \n" );
        }
    }

    if(newWinner.nBlockHeight == 0)
        return false;

    //CTxDestination address1;
    //ExtractDestination(newWinner.payee, address1);
    //CSocietyGcoinAddress address2(address1);

    //CTxDestination address3;
    //ExtractDestination(payeeSource, address3);
    //CSocietyGcoinAddress address4(address3);

    /* address2 and address3 is the wallet address of the new winner */

    // LogPrintf("Winner payee %s nHeight %d vin source %s. \n", address2.ToString().c_str(), newWinner.nBlockHeight, address3.ToString().c_str());





    CScript pubkey_temp;
    pubkey_temp.SetDestination( pmn->pubkey.GetID());
    CTxDestination address1;
    ExtractDestination(pubkey_temp, address1);

    CSocietyGcoinAddress address2(address1);

    //CKey secret_key = GetKey();


    //LogPrintf("*** RGP address1 %s \n", address2.ToString().c_str() );

    if(Sign(newWinner))
    {
        if(AddWinningMasternode(newWinner))
        {
            Relay(newWinner);
            nLastBlockHeight = nBlockHeight;
            return true;
        }
    }

    return false;
}


void CMasternodePayments::Relay(CMasternodePaymentWinner& winner)
{
    CInv inv(MSG_MASTERNODE_WINNER, winner.GetHash());

    //LogPrintf("A");

    vector<CInv> vInv;
    vInv.push_back(inv);
    LOCK(cs_vNodes);
    BOOST_FOREACH(CNode* pnode, vNodes)
    {
        pnode->PushMessage("inv", vInv);
        //LogPrintf("*** Relay to Nodes %s \n", pnode->addr.ToString() );
    }
}

void CMasternodePayments::Sync(CNode* node)
{
    LOCK(cs_masternodepayments);

     //LogPrintf("");

    BOOST_FOREACH(CMasternodePaymentWinner& winner, vWinning)
    {
        if(winner.nBlockHeight >= pindexBest->nHeight-10 && winner.nBlockHeight <= pindexBest->nHeight + 20)
        {
            node->PushMessage("mnw", winner);
        }
    }
}


bool CMasternodePayments::SetPrivKey(std::string strPrivKey)
{
    CMasternodePaymentWinner winner;

    // Test signing successful, proceed
    //LogPrintf("*** RGP, CMasternodePayments::SetPrivKey of MN <%s>  \n", strPrivKey );

    strMasterPrivKey = strPrivKey;

    Sign(winner);

    if(CheckSignature(winner)){
        LogPrintf("CMasternodePayments::SetPrivKey - Successfully initialized as Masternode payments master\n");
        enabled = true;
        return true;
    } else {
        return false;
    }
}
