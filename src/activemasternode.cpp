// Copyright (c) 2009-2012 The Darkcoin developers
// Copyright (c) 2018-2024 The Bank Society Coin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "protocol.h"
#include "core.h"
#include "activemasternode.h"
#include "masternodeman.h"
#include <boost/lexical_cast.hpp>
#include "clientversion.h"

#include "masternodeconfig.h"

extern CMasternodeMan mnodeman;
static CMasternode* pmn;

static map < uint256, CTransaction > MN_CTransaction_Map; 

static map < unsigned int, CMasternode > Masternode_Information;

// map to hold all MNs
//static    std::vector<CMasternode> vMasternodes;
    // who's asked for the masternode list and the last time
//extern std::map<CNetAddr, int64_t> mAskedUsForMasternodeList;
    // who we asked for the masternode list and the last time
//extern  std::map<CNetAddr, int64_t> mWeAskedForMasternodeList;
    // which masternodes we've asked for
//extern   std::map<COutPoint, int64_t> mWeAskedForMasternodeListEntry;

   /* --------------------------------------------------------------------- 
       -- RGP, this routine will find the transaction on disk and confirm --
       --      the MasterNode Collatoral, it needs to only run once at    --
       --      startup issued from the controlling wallet.                --
       --------------------------------------------------------------------- */

bool CActiveMasternode::Extract_MN_Collatoral( CTxIn& vin, CPubKey& pubkey, CKey& secretKey )
{
uint256 block_hash = 0;
uint256 hashBlock = 0;


    /* RGP TO BE RESOLVED, Always the same for all masternodes */
    LogPrintf("RGP Extract_MN_Collatoral pubkey %s  \n", pubkey.GetID().ToString() );
    LogPrintf("RGP Extract_MN_Collatoral prevout passed in VIN %d \n", vin.prevout.ToString() );
    

    if ( vin.prevout.hash == 0 )
    {
        LogPrintf("RGP Extract_MN_Collatoral past in vin hash is invalid \n");
        return false;
    }

    /* RGP TO BE RESOLVED : The transaction is not in mempool, investigate this later... */

    CTransaction MN_Transaction = CTransaction();

    LogPrintf("RGP Extract_MN_Collatoral before Get_MN_Transaction \n" );

    Get_MN_Transaction(vin.prevout.hash, MN_Transaction, hashBlock); /* always fails as transactions are not on disk? */

    LogPrintf("RGP Extract_MN_Collatoral parameter past in vin hash %s %s hashblock \n", vin.prevout.hash.ToString(), hashBlock.ToString() );

    // ------------------------------------------------------------------------
    // -- RGP, look through all blocks, locate the block with the MN TX hash --
    // ------------------------------------------------------------------------

    map < uint256, CTransaction >::iterator test_data ;

LogPrintf("RGP Extract_MN_Collatoral before MN_CTransaction_Map map size %d \n", MN_CTransaction_Map.size() );

    for ( test_data = MN_CTransaction_Map.begin(); test_data != MN_CTransaction_Map.end(); ++test_data )
    {
LogPrintf("RGP Extract_MN_Collatoral top of loop  %s %s \n", test_data->first.ToString(), test_data->second.ToString() );
        /* check if we did not find anything */
        if ( test_data == MN_CTransaction_Map.end() ) 
        {
                /* no entry found */
                //LogPrintf("RGP Extract_MN_Collatoral MN_CTransaction_Map not found! %s %s \n", test_data->first.ToString(), test_data->second.ToString() );
                //uint256 hashBlock = 0;
                //GetTransaction(vin.prevout.hash, tx, hashBlock);

                //MN_CTransaction_Map.insert(std::make_pair( hashBlock, tx ));

                //note : transaction hash is here : tx.vin.prevout.hash
        }
        else
        {
                /* Something was found */

                //LogPrintf("RGP Extract_MN_Collatoral Something was found hash %s %s \n", test_data->first.ToString(), test_data->second.ToString() );
                return true;
        }


        MilliSleep( 1 ); /* RGP optimise */
    }

    return false;
}








// Bootup the masternode, look for a 150 000 SocietyG input and register on the network
//
void CActiveMasternode::ManageStatus()
{
    std::string errorMessage;

/*    if (fDebug) */
        LogPrintf("\n\n CActiveMasternode::ManageStatus() - Begin \n"); 

    /* -------------------------------------------------------
       -- RGP, fMasterNode is set in the societyG.conf file --
       ------------------------------------------------------- */
    if(!fMasterNode) 
    {
        LogPrintf("RGP CActiveMasternode::ManageStatus, fMasterNode is FALSE \n");
        return;
    }
std::vector<CMasternode> xxx = mnodeman.GetFullMasternodeVector();
LogPrintf("*** RGP CMasternodeMan::ManageStatus Start Masternodes count %d \n", xxx.size() );


    /* -------------------------------------------------------------------------------------------------------
       -- RGP, On determination that this is a MasterNode, status is always set to MASTERNODE_NOT_PROCESSED --
       ------------------------------------------------------------------------------------------------------- */

    //need correct adjusted time to send ping

    //LogPrintf("RGP CActiveMasternode::ManageStatus() Masternode_Information size is %d \n", Masternode_Information.size( ) );


    {
        bool fIsInitialDownload = IsInitialBlockDownload();
        if(fIsInitialDownload)
        {
            status = MASTERNODE_SYNC_IN_PROCESS;
            LogPrintf("CActiveMasternode::ManageStatus() - Sync in progress. Must wait until sync is complete to start masternode.\n");
            return;
        }
    }

//LogPrintf("*** RGP vMasternodes %d \n", vMasternodes.size() );
MilliSleep( 1 ); /* RGP optimise */

    if(status == MASTERNODE_INPUT_TOO_NEW || status == MASTERNODE_NOT_CAPABLE || status == MASTERNODE_SYNC_IN_PROCESS)
    {
        
        LogPrintf("RGP ManageStatus RGP DEBUG MASTERNODE_NOT_PROCESSED status is %d \n", status);
        switch( status )
        {
           case MASTERNODE_INPUT_TOO_NEW   : LogPrintf("RGP ManageStatus MASTERNODE_INPUT_TOO_NEW\n");
                                             break;
           case MASTERNODE_NOT_CAPABLE     : LogPrintf("RGP ManageStatus MASTERNODE_NOT_CAPABLE\n");
                                             break;
           case MASTERNODE_SYNC_IN_PROCESS : LogPrintf("RGP ManageStatus MASTERNODE_SYNC_IN_PROCESS\n");
                                             break;
        }
        status = MASTERNODE_NOT_PROCESSED;
    }

    if( status == MASTERNODE_NOT_PROCESSED )
    {


        if(strMasterNodeAddr.empty()) 
        {
            if(!GetLocal(service)) {
                notCapableReason = "Can't detect external address. Please use the masternodeaddr configuration option.";
                status = MASTERNODE_NOT_CAPABLE;
                // LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason.c_str());
                return;
            }
        } else
        {
        	service = CService(strMasterNodeAddr, true);
        }
MilliSleep( 1 ); /* RGP optimise */

//LogPrintf("RGP CActiveMasternode Masternode processing \n");
        if(strMasterNodeAddr.empty()) 
        {
//LogPrintf("RGP CActiveMasternode Masternode processing, missing IP Address \n");
            if(!GetLocal(service)) 
            {
//LogPrintf(" RGP DEBUG %s not cable on \n", strMasterNodeAddr.c_str()  );

                notCapableReason = "Can't detect external address %s . Please use the masternodeaddr configuration option.";
                status = MASTERNODE_NOT_CAPABLE;
                LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason.c_str());
                return;
            }
        }
        else
        {
LogPrintf("CActiveMasternode::ManageStatus() service is TRUE \n");
        	service = CService(strMasterNodeAddr, true);
        }

//LogPrintf("CActiveMasternode::ManageStatus() - Checking inbound connection to '%s'\n", service.ToString().c_str());


        if ( service.ToString().c_str() == strMasterNodeAddr )
        {
           LogPrintf("ignoring our own IP address %s \n", strMasterNodeAddr.c_str() );
           
       }
       else
       {
        
           LogPrintf("RGP Debug ActiveMasternode trying to ConnectNode %s \n", service.ToString().c_str() );

          if(!ConnectNode( (CAddress)service, service.ToString().c_str()))
          {
                notCapableReason = "Could not connect to " + service.ToString();
                status = MASTERNODE_NOT_CAPABLE;
                LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason.c_str());
                return;
          }
MilliSleep( 1 ); /* RGP optimise */
           
       }
        
//LogPrintf("RGP Debug ActiveMasternode before locked wallet check \n");
        if(pwalletMain->IsLocked()){
            notCapableReason = "Wallet is locked.";
            status = MASTERNODE_NOT_CAPABLE;
            LogPrintf("CActiveMasternode::ManageStatus() - not capable: %s\n", notCapableReason.c_str());
            return;
        }


//LogPrintf("RGP Debug ActiveMasternode before Remote enabled check \n");
        if (status != MASTERNODE_REMOTELY_ENABLED)
        {
            // Set defaults
            status = MASTERNODE_NOT_CAPABLE;
            notCapableReason = "Unknown. Check debug.log for more information.\n";

            // Choose coins to use
            CPubKey pubKeyCollateralAddress;
            CKey keyCollateralAddress;            

LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 002 \n");

        if ( MN_CTransaction_Map.size() == 0 )
        {
            LogPrintf("RGP Debug ActiveMasternode MN_CTransaction_Map is NULL \n");
LogPrintf("RGP Extract_MN_Collatoral before MN_CTransaction_Map map size %d \n", MN_CTransaction_Map.size() );
return;
        }
        
            LogPrintf("RGP Debug ActiveMasternode MN_CTransaction_Map is %d \n", MN_CTransaction_Map.size() ); 

/* RGP WRITE CODE TO USE THE MN_CTRANSACTION_MAP */
            if( CActiveMasternode::Extract_MN_Collatoral( vin, pubKeyCollateralAddress, keyCollateralAddress ) )
                LogPrintf("RGP Debug ActiveMasternode GetMNVinFromTransaction SUCCESS \n");
            else 
            {
                LogPrintf("RGP Debug ActiveMasternode GetMNVinFromTransaction FAILED \n");
                return;
            }

            if( GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress) )
                LogPrintf("RGP GetMasterNodeVin SUCCESS \n");
            else
            {
                LogPrintf("RGP GetMasterNodeVin FAILURE \n");
                return;
            }

            if( CActiveMasternode::Extract_MN_Collatoral( vin, pubKeyCollateralAddress, keyCollateralAddress ) )
            {
LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 003 \n");
                if(GetInputAge(vin) < MASTERNODE_MIN_CONFIRMATIONS)
                {
LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 004 \n");

                    notCapableReason = "Input must have least " + boost::lexical_cast<string>(MASTERNODE_MIN_CONFIRMATIONS) +
                                       " confirmations - " + boost::lexical_cast<string>(GetInputAge(vin)) + " confirmations";
                    LogPrintf("CActiveMasternode::ManageStatus() - %s\n", notCapableReason.c_str());
LogPrintf("RGP REMOVE COMMENT after Nodes are SYNCHING \n");
                    //status = MASTERNODE_INPUT_TOO_NEW;

LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 004 \n");

                    /* RESOLVE later RGP */
                    //return;
                }

                LogPrintf("CActiveMasternode::ManageStatus() - Is capable master node!\n");

                status = MASTERNODE_IS_CAPABLE;
                notCapableReason = "";
LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 005 \n");
                pwalletMain->LockCoin(vin.prevout);

                // send to all nodes
                CPubKey pubKeyMasternode;
                CKey keyMasternode;

                LogPrintf("*** RGPC ActiveMasternode::ManageStatus %s \n", strMasterNodePrivKey.c_str() );

                if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode))
                {

LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 006 \n");

                    LogPrintf("ActiveMasternode::Dseep() - Error upon calling SetKey [ManageStatus] : %s \n", errorMessage.c_str() );
                    return;
                }

                /* rewards are not supported in societyG.conf */
                CScript rewardAddress = CScript();
                int rewardPercentage = 0;

                if(!Register(vin, service, keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, rewardAddress, rewardPercentage, errorMessage ) )
                {
                   LogPrintf("CActiveMasternode::ManageStatus() - Error on Register: %s\n", errorMessage.c_str());
                   return;
                }
LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 008 \n");

/* Fix RGP to get this running */
status = MASTERNODE_REMOTELY_ENABLED;

                return;

            }
            else
            {

LogPrintf("RGP Debug ActiveMasternode before Remote enabled check 005 \n");

                    notCapableReason = "Could not find suitable coins!";
                    //LogPrintf("CActiveMasternode::ManageStatus() - Could not find suitable coins!\n");

             }

        }
        else
        {
            LogPrintf("CActiveMasternode::Extract_MN_Collatoral %s \n", vin.ToString() );
            return;
        }
    }

LogPrintf("*** RGP vMasternodes %d \n", vMasternodes.size() );


    //send to all peers
    if(!Dseep(errorMessage))
    {
        LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s\n", errorMessage.c_str());
    }
    MilliSleep(5); /* RGP Optimise */
}





// Send stop dseep to network for remote masternode
bool CActiveMasternode::StopMasterNode(std::string strService, std::string strKeyMasternode, std::string& errorMessage) {
	CTxIn vin;
    CKey keyMasternode;
    CPubKey pubKeyMasternode;

     //LogPrintf("*** RGP CActiveMasternode::StopMasternode Start 1 \n");

    if(!darkSendSigner.SetKey(strKeyMasternode, errorMessage, keyMasternode, pubKeyMasternode))
    {
        //LogPrintf("CActiveMasternode::StopMasterNode() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    if (GetMasterNodeVin(vin, pubKeyMasternode, keyMasternode)){
        // LogPrintf("MasternodeStop::VinFound: %s\n", vin.ToString());
    }
    MilliSleep(5); /* RGP Optimise */

    return StopMasterNode(vin, CService(strService, true), keyMasternode, pubKeyMasternode, errorMessage);
}

// Send stop dseep to network for main masternode
bool CActiveMasternode::StopMasterNode(std::string& errorMessage)
{
        //LogPrintf("*** RGP CActiveMasternode::StopMasternode Start 2 \n");

        if(status != MASTERNODE_IS_CAPABLE && status != MASTERNODE_REMOTELY_ENABLED) {
		errorMessage = "masternode is not in a running status";
        // LogPrintf("CActiveMasternode::StopMasterNode() - Error: %s\n", errorMessage.c_str());
		return false;
	}

	status = MASTERNODE_STOPPED;

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode))
    {
        // LogPrintf("Register::ManageStatus() - Error upon calling SetKey: %s\n", errorMessage.c_str());
    	return false;
    }
    MilliSleep(5); /* RGP Optimise */

    return StopMasterNode(vin, service, keyMasternode, pubKeyMasternode, errorMessage);
}

// Send stop dseep to network for any masternode
bool CActiveMasternode::StopMasterNode(CTxIn vin, CService service, CKey keyMasternode, CPubKey pubKeyMasternode, std::string& errorMessage)
{
        LogPrintf("*** RGP CActiveMasternode::StopMasternode Start 3 \n");

        pwalletMain->UnlockCoin(vin.prevout);
	return Dseep(vin, service, keyMasternode, pubKeyMasternode, errorMessage, true);
}

bool CActiveMasternode::Dseep(std::string& errorMessage) 
{
LogPrintf("*** RGP vMasternodes Dseep 001 %d \n", vMasternodes.size() );

	if(status != MASTERNODE_IS_CAPABLE && status != MASTERNODE_REMOTELY_ENABLED) 
    {
		errorMessage = "masternode is not in a running status";
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", errorMessage.c_str());

		return false;
	}

// Masternode is remotely enabled 
LogPrintf("RGP Debug Dseep() preparing for darksendsigner key %s \n", strMasterNodePrivKey.c_str() );

    CPubKey pubKeyMasternode;
    CKey keyMasternode;

    if(!darkSendSigner.SetKey(strMasterNodePrivKey, errorMessage, keyMasternode, pubKeyMasternode))
    {
        LogPrintf("CActiveMasternode::Dseep() - Error upon calling SetKey [stopmasternode]: %s\n", errorMessage.c_str());
    	return false;
    }

LogPrintf("*** RGP vMasternodes Dseep 002 %d \n", vMasternodes.size() );

	return Dseep(vin, service, keyMasternode, pubKeyMasternode, errorMessage, false);
}

bool CActiveMasternode::Dseep(CTxIn vin, CService service, CKey keyMasternode, CPubKey pubKeyMasternode, std::string &retErrorMessage, bool stop)
{
    std::string errorMessage;
    std::vector<unsigned char> vchMasterNodeSignature;
    std::string strMasterNodeSignMessage;
    int64_t masterNodeSignatureTime = GetAdjustedTime();

LogPrintf("*** RGP vMasternodes DSeep 003 %d \n", vMasternodes.size() );

    std::string strMessage = service.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + boost::lexical_cast<std::string>(stop);


    LogPrintf("*** RGP CActiveMasternode::dsEEP Start 2 MN Signature %s \n", strMessage );

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyMasternode)) {
    	retErrorMessage = "sign message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage.c_str());
        return false;
    }

    if(!darkSendSigner.VerifyMessage(pubKeyMasternode, vchMasterNodeSignature, strMessage, errorMessage)) {
    	retErrorMessage = "Verify message failed: " + errorMessage;
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage.c_str());
        return false;
    }

    // Update Last Seen timestamp in masternode list
LogPrintf("*** RGP CActiveMasternode::dsEEP vin %s \n", vin.ToString() );

LogPrintf("*** RGP vMasternodes Dseep 004 %d Masternodes \n", vMasternodes.size() );

    LogPrintf("*** RGP CActiveMasternode::Register calling mnodeman.FIND \n");

    CTxIn search_vin;

    
    /* It looks like mnodeman.Find() does not work here, as debug is not printed, 
       I'll use Masternode_Information to find the data now, fix it later          */
    map < unsigned int, CMasternode >::iterator find_mn_vin;

    if ( Masternode_Information.size() != 0 )
    {
        //LogPrintf("RGP DSEEP #2 Masternodes in the system %d \n", Masternode_Information.size() );
//CPubKey pubkey;
//CPubKey pubkey2;

        for ( find_mn_vin = Masternode_Information.begin(); find_mn_vin != Masternode_Information.end(); ++find_mn_vin )
        {

          LogPrintf("*** RGP pubkey2 %s service %s \n", find_mn_vin->second.pubkey2.GetID().ToString(), find_mn_vin->second.addr.ToString() );
          LogPrintf("*** RGP pubkey %s \n", find_mn_vin->second.pubkey.GetID().ToString() );
            LogPrintf("*** RGP pubkeymn %s \n", pubKeyMasternode.GetID().ToString() );
            //LogPrintf("*** RGP keyMasternode %s \n", keyMasternode.ToString() );

            if ( find_mn_vin->second.pubkey2 == pubKeyMasternode )
            {
//LogPrintf("RGP Found the VIN inside #2 Masternode_Information \n");

                search_vin = find_mn_vin->second.vin;
                pmn = &find_mn_vin->second;
                vin = search_vin;
                break;
            }
            else
            {
                // LogPrintf("RGP DSEEP #2 publike keys do not match for vin. Don't worry, as this is a for loop %s \n", find_mn_vin->second.vin.ToString() );
                continue;
            }

            MilliSleep( 1 ); /* RGP optimise */
        }
    }
    else
    {
        //LogPrintf("RGO DSEEP #2 no Masternode_Information! \n");
    }

LogPrintf("*** RGP vMasternodes Dseep 005 %d Masternodes \n", vMasternodes.size() );

    //CMasternode* pmn = mnodeman.Find(vin); // RGP, perhaps call this, debug later
    if( pmn != NULL )
    {
        if( stop )
        {
LogPrintf("DSEEP STOPPING \n");
            mnodeman.Remove(pmn->vin);
        }
        else
        {
            LogPrintf("DSEEP updating last seen \n");
status = MASTERNODE_REMOTELY_ENABLED;
    notCapableReason = "";
int64_t lastTime = GetAdjustedTime();
            //pmn->UpdateLastSeen();
            pmn->lastTimeSeen = lastTime;
          LogPrintf("DSEEP updating last seen %d \n", lastTime );
        }
//LogPrintf("*** RGP vMasternodes Dseep 006 %d Masternodes \n", vMasternodes.size() );
    }
    else 
    {
    	// Seems like we are trying to send a ping while the masternode is not registered in the network
    	retErrorMessage = "Darksend Masternode List doesn't include our masternode, Shutting down masternode pinging service! " + vin.ToString();
        LogPrintf("CActiveMasternode::Dseep() - Error: %s\n", retErrorMessage.c_str());
LogPrintf("RGP REMOVE AFTER RESOLVED \n");
        //status = MASTERNODE_NOT_CAPABLE;
        //notCapableReason = retErrorMessage;
//LogPrintf("*** RGP vMasternodes Dseep 007 Masternodes %d \n", vMasternodes.size() );
        return false;
    }

//LogPrintf("*** RGP vMasternodes Dseep 008 Masternodes %d \n", vMasternodes.size() );
    //send to all peers
    //LogPrintf("CActiveMasternode::Dseep() - RelayMasternodeEntryPing vin = %s\n", vin.ToString().c_str());
    mnodeman.RelayMasternodeEntryPing(vin, vchMasterNodeSignature, masterNodeSignatureTime, stop);

    MilliSleep(5); /* RGP Optimise */
    
    return true;
}

bool CActiveMasternode::Register(std::string strService, std::string strKeyMasternode, std::string txHash, std::string strOutputIndex, std::string strRewardAddress, std::string strRewardPercentage, std::string& errorMessage) {
    CTxIn vin;
    CPubKey pubKeyCollateralAddress;
    CKey keyCollateralAddress;
    CPubKey pubKeyMasternode;
    CKey keyMasternode;
    CScript rewardAddress = CScript();
    int rewardPercentage = 0;

    MilliSleep( 1 ); /* RGP optimise */

    LogPrintf("*** RGP CActiveMasternode::Register 1 Start \n" );


    LogPrintf("*** RGP CActiveMasternode::Register strService %s \n",strService );
    LogPrintf("*** RGP CActiveMasternode::Register strKeyMasternode %s \n", strKeyMasternode );
    LogPrintf("*** RGP CActiveMasternode::Register txHash %s \n", txHash );
    LogPrintf("*** RGP CActiveMasternode::Register strRewardAddress %s \n", strRewardAddress );
    LogPrintf("*** RGP CActiveMasternode::Register strOutputIndex %s \n", strOutputIndex );
    LogPrintf("*** RGP CActiveMasternode::Register strRewardPercentage %s \n", strRewardPercentage );

    if(!darkSendSigner.SetKey(strKeyMasternode, errorMessage, keyMasternode, pubKeyMasternode))
    {
        //LogPrintf("*** RGP CActiveMasternode::Register Debug 1 \n" );
        // LogPrintf("CActiveMasternode::Register() - Error upon calling SetKey: %s\n", errorMessage.c_str());
        return false;
    }


    bool GetVINreturn = GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress, txHash, strOutputIndex);
    if ( !GetVINreturn )
    {
    //if(!GetMasterNodeVin(vin, pubKeyCollateralAddress, keyCollateralAddress, txHash, strOutputIndex)) {
        errorMessage = "could not allocate vin";
        LogPrintf("CActiveMasternode::Register() - Error: %s\n", errorMessage.c_str());
        return false;
    }

    LogPrintf("*** RGP CActiveMasternode::Register Debug 3 \n" );

    CSocietyGcoinAddress address;
    if (strRewardAddress != "")
    {
        LogPrintf("*** RGP CActiveMasternode::Register Debug 4 \n" );

        if(!address.SetString(strRewardAddress))
        {
            LogPrintf("ActiveMasternode::Register - Invalid Reward Address\n");
            return false;
        }
        rewardAddress.SetDestination(address.Get());

        LogPrintf("*** RGP CActiveMasternode::Register Debug 5 \n" );

        try {
            rewardPercentage = boost::lexical_cast<int>( strRewardPercentage );
        } catch( boost::bad_lexical_cast const& ) {
            LogPrintf("ActiveMasternode::Register - Invalid Reward Percentage (Couldn't cast)\n");
            return false;
        }

        LogPrintf("*** RGP CActiveMasternode::Register Debug 6 \n" );

        if( rewardPercentage < 0 || rewardPercentage > 100)
        {
            LogPrintf("ActiveMasternode::Register - Reward Percentage Out Of Range\n");
            return false;
        }

        LogPrintf("*** RGP CActiveMasternode::Register Debug 7 \n" );

    }

    LogPrintf("*** RGP CActiveMasternode::Register Debug 8 Private key %s \n", strKeyMasternode );

    
    MilliSleep(5); /* RGP Optimise */
    
    return Register(vin, CService(strService, true), keyCollateralAddress, pubKeyCollateralAddress, keyMasternode, pubKeyMasternode, rewardAddress, rewardPercentage, errorMessage );

}

bool CActiveMasternode::Register(CTxIn vin, CService service, CKey keyCollateralAddress, CPubKey pubKeyCollateralAddress, CKey keyMasternode, CPubKey pubKeyMasternode, CScript rewardAddress, int rewardPercentage, std::string &retErrorMessage )
{
    std::string errorMessage;
    std::vector<unsigned char> vchMasterNodeSignature;
    std::string strMasterNodeSignMessage;
    int64_t masterNodeSignatureTime = GetAdjustedTime();

    std::string vchPubKey(pubKeyCollateralAddress.begin(), pubKeyCollateralAddress.end());
    std::string vchPubKey2(pubKeyMasternode.begin(), pubKeyMasternode.end());

    std::string strMessage = service.ToString() + boost::lexical_cast<std::string>(masterNodeSignatureTime) + vchPubKey + vchPubKey2 + boost::lexical_cast<std::string>(PROTOCOL_VERSION) + rewardAddress.ToString() + boost::lexical_cast<std::string>(rewardPercentage);

MilliSleep( 1 ); /* RGP optimise */

    LogPrintf("*** RGP CActiveMasternode::Register 2 start \n");

    // LogPrintf("*** RGP data is vchPubKey %s vchPubKey2 %s strMessage \n", vchPubKey, vchPubKey2, strMessage  );

    if(!darkSendSigner.SignMessage(strMessage, errorMessage, vchMasterNodeSignature, keyCollateralAddress)) 
    {
		retErrorMessage = "sign message failed: " + errorMessage;
                LogPrintf("CActiveMasternode::Register() - Error: %s\n", retErrorMessage.c_str());
		return false;
    }

    if(!darkSendSigner.VerifyMessage(pubKeyCollateralAddress, vchMasterNodeSignature, strMessage, errorMessage)) {
		retErrorMessage = "Verify message failed: " + errorMessage;
                LogPrintf("CActiveMasternode::Register() - Error: %s\n", retErrorMessage.c_str());
		return false;
	}


    LogPrintf("*** RGP CActiveMasternode::Register calling mnodeman.FIND \n");

    CTxIn search_vin;


    map < unsigned int, CMasternode >::iterator find_mn_vin;

    if ( Masternode_Information.size() != 0 )
    {
        for ( find_mn_vin = Masternode_Information.begin(); find_mn_vin != Masternode_Information.end(); ++find_mn_vin )
        {
            if ( find_mn_vin->second.pubkey == pubKeyMasternode )
            {
                search_vin = find_mn_vin->second.vin;
                pmn = mnodeman.Find(search_vin);
            }
            else
                continue;

            MilliSleep( 1 ); /* RGP optimise */
            
        }
    }
    else
    {
LogPrintf("RGP CActiveMasternode::Register mo Masternode_information defined \n");
        pmn = mnodeman.Find( vin );
    }

    LogPrintf("*** RGP CActiveMasternode::Register calling mnodeman.FIND returned \n");

    if(pmn == NULL)
    {
        LogPrintf("*** RGP CActiveMasternode::Register Debug 1 \n");
        LogPrintf("CActiveMasternode::Register() - Adding to masternode list service: %s - vin: %s\n", service.ToString().c_str(), vin.ToString().c_str());

        LogPrintf("*** RGP CActiveMasternode::Register Debug 2 \n");

        CMasternode mn(service, vin, pubKeyCollateralAddress, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyMasternode, PROTOCOL_VERSION, rewardAddress, rewardPercentage);

        LogPrintf("*** RGP CActiveMasternode::Register Debug 3 \n");

        mn.ChangeNodeStatus(true); /* RGP */
        mn.UpdateLastSeen(masterNodeSignatureTime);

        LogPrintf("*** RGP CActiveMasternode::Register Debug 4 \n");



        mnodeman.Add(mn);

        Masternode_Information.insert(std::make_pair( Masternode_Information.size() + 1, mn )); /* new Entry */

        LogPrintf("*** RGP CActiveMasternode::Register Debug 5 \n");
    }

    LogPrintf("*** RGP CActiveMasternode::Register calling RelayMasternodeEntry \n");

    //send to all peers
    LogPrintf("CActiveMasternode::Register() - RelayElectionEntry vin = %s  \n", vin.ToString().c_str()  );
    mnodeman.RelayMasternodeEntry(vin, service, vchMasterNodeSignature, masterNodeSignatureTime, pubKeyCollateralAddress,
                                  pubKeyMasternode, -1, -1, masterNodeSignatureTime,
                                  PROTOCOL_VERSION, rewardAddress, rewardPercentage );
                                  
    MilliSleep(5); /* RGP Optimise */

    return true;
}







bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey) 
{
LogPrintf("*** RGP vMasternodes 00001 %d \n", vMasternodes.size() );
    LogPrintf("RGP Debug GetMasterNodeVin start \n");

	return GetMasterNodeVin(vin, pubkey, secretKey, "", "");
}

bool CActiveMasternode::GetMasterNodeVin(CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex) 
{
    CScript pubScript;
LogPrintf("*** RGP vMasternodes 000002 %d \n", vMasternodes.size() );
LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 001 \n");

    // Find possible candidates
    vector<COutput> possibleCoins = SelectCoinsMasternode();

LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 001.25 \n");

    COutput *selectedOutput;

LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 001.5 \n");

    // Find the vin
	if(!strTxHash.empty())
    {
LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 002 hash %s outputIndex %s \n", strTxHash, strOutputIndex );
		// Let's find it
		uint256 txHash(strTxHash);
        int outputIndex = boost::lexical_cast<int>(strOutputIndex);
		bool found = false;
		BOOST_FOREACH(COutput& out, possibleCoins) 
		{
			if(out.tx->GetHash() == txHash && out.i == outputIndex)
			{
				selectedOutput = &out;
				found = true;
				break;
			}
			MilliSleep(1); /* RGP Optimise */
		}
		if(!found) 
        {
LogPrintf("*** RGP vMasternodes 000004 %d \n", vMasternodes.size() );
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate valid vin\n");
			return false;
		}
	} 
    else 
    {
LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 003 \n");

		// No output specified,  Select the first one
		if(possibleCoins.size() > 0) 
        {
			selectedOutput = &possibleCoins[0];
		} 
        else 
        {
            LogPrintf("CActiveMasternode::GetMasterNodeVin - Could not locate specified vin from possible list\n");
LogPrintf("*** RGP vMasternodes 00000006 %d \n", vMasternodes.size() );
			return false;
		}
    }

LogPrintf("RGP DEBUG Check GetMasterNodeVin Check 004 \n");

    MilliSleep(5); /* RGP Optimise */
	// At this point we have a selected output, retrieve the associated info
	return GetVinFromOutput(*selectedOutput, vin, pubkey, secretKey);
}

bool CActiveMasternode::GetMasterNodeVinForPubKey(std::string collateralAddress, CTxIn& vin, CPubKey& pubkey, CKey& secretKey) {
	return GetMasterNodeVinForPubKey(collateralAddress, vin, pubkey, secretKey, "", "");
}

bool CActiveMasternode::GetMasterNodeVinForPubKey(std::string collateralAddress, CTxIn& vin, CPubKey& pubkey, CKey& secretKey, std::string strTxHash, std::string strOutputIndex) {
    CScript pubScript;

LogPrintf("RGP GetMasterNodeVinForPubKey collateralAddress %s  strTxHash %s strOutputIndex  \n", collateralAddress, strTxHash, strOutputIndex    );

    // Find possible candidates
    vector<COutput> possibleCoins = SelectCoinsMasternodeForPubKey(collateralAddress);
    COutput *selectedOutput;

    LogPrintf("\n *** RGP GetMasterNodeVinForPubKey collateralAddress %s \n", collateralAddress );

    // Find the vin
	if(!strTxHash.empty()) 
    {
LogPrintf("RGP DEBUG Check GetMasterNodeVin Check strTxHash is EMPTY!!! \n");
		// Let's find it
		uint256 txHash(strTxHash);
        int outputIndex = boost::lexical_cast<int>(strOutputIndex);
		bool found = false;
		BOOST_FOREACH(COutput& out, possibleCoins) 
		{
			if(out.tx->GetHash() == txHash && out.i == outputIndex)
			{
				selectedOutput = &out;
				found = true;
				break;
			}
			MilliSleep(1); /* RGP Optimise */
		}
		if(!found) 
        {
            LogPrintf("CActiveMasternode::GetMasterNodeVinForPubKey - Could not locate valid vin\n");
			return false;
		}
	} else {
		// No output specified,  Select the first one
		if(possibleCoins.size() > 0) {
			selectedOutput = &possibleCoins[0];
		} else 
        {
            LogPrintf("CActiveMasternode::GetMasterNodeVinForPubKey - Could not locate specified vin from possible list\n");
			return false;
		}
    }
    MilliSleep(5); /* RGP Optimise */
    
    // At this point we have a selected output, retrieve the associated info
    return GetVinFromOutput(*selectedOutput, vin, pubkey, secretKey);
}

// Extract masternode vin information from output
bool CActiveMasternode::GetVinFromOutput(COutput out, CTxIn& vin, CPubKey& pubkey, CKey& secretKey) {

    CScript pubScript;
LogPrintf("*** RGP vMasternodes 0000056 %d \n", vMasternodes.size() );
    vin = CTxIn(out.tx->GetHash(),out.i);
    pubScript = out.tx->vout[out.i].scriptPubKey; // the inputs PubKey

    CTxDestination address1;
    ExtractDestination(pubScript, address1);
    CSocietyGcoinAddress address2(address1);

    CKeyID keyID;
    if (!address2.GetKeyID(keyID)) {
        LogPrintf("CActiveMasternode::GetMasterNodeVin - Address does not refer to a key\n");
        return false;
    }

    if (!pwalletMain->GetKey(keyID, secretKey)) {
        LogPrintf ("CActiveMasternode::GetMasterNodeVin - Private key for address is not known\n");
        return false;
    }

    pubkey = secretKey.GetPubKey();
    return true;
}

// get all possible outputs for running masternode
vector<COutput> CActiveMasternode::SelectCoinsMasternode()
{
    vector<COutput> vCoins;
    vector<COutput> filteredCoins;

LogPrintf("RGP Debug SelectCoinsMasternode Check 1 \n" );

    // Retrieve all possible outputs
    pwalletMain->AvailableCoinsMN(vCoins);

    // Filter
    BOOST_FOREACH(const COutput& out, vCoins)
    {

LogPrintf("RGP DEBUG SelectCoinsMasternode %s \n ", out.ToString() );

        /* RGP, should return 150,000, defined in main.h */
        if(out.tx->vout[out.i].nValue == GetMNCollateral( pindexBest->nHeight ) *COIN ) 
        { //exactly

            LogPrintf("RGP DEBUG SelectCoinsMasternode 150 000 coins found %s \n ", out.ToString() );

        	filteredCoins.push_back(out);
        }
        MilliSleep(1); /* RGP Optimise */
    }

    LogPrintf("RGP Debug SelectCoinsMasternode Check 2 \n" );

    return filteredCoins;
}

// get all possible outputs for running masternode for a specific pubkey
vector<COutput> CActiveMasternode::SelectCoinsMasternodeForPubKey(std::string collateralAddress)
{
    CSocietyGcoinAddress address(collateralAddress);
    CScript scriptPubKey;
    scriptPubKey.SetDestination(address.Get());
    vector<COutput> vCoins;
    vector<COutput> filteredCoins;

    // Retrieve all possible outputs
    pwalletMain->AvailableCoins(vCoins);

    // Filter
    BOOST_FOREACH(const COutput& out, vCoins)
    {
        if(out.tx->vout[out.i].scriptPubKey == scriptPubKey && out.tx->vout[out.i].nValue == GetMNCollateral(pindexBest->nHeight)*COIN) { //exactly
        	filteredCoins.push_back(out);
        }
        MilliSleep(1); /* RGP Optimise */
    }
    return filteredCoins;
}

// when starting a masternode, this can enable to run as a hot wallet with no funds
bool CActiveMasternode::EnableHotColdMasterNode(CTxIn& newVin, CService& newService)
{
    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() Start \n");

    if(!fMasterNode)
    {
        LogPrintf("CActiveMasternode::EnableHotColdMasterNode() fMasterNode is false!!! \n");
        return false;
    }

    status = MASTERNODE_REMOTELY_ENABLED;
    notCapableReason = "";

    //The values below are needed for signing dseep messages going forward
    this->vin = newVin;
    this->service = newService;

    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}
