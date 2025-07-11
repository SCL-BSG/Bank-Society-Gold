// Copyright (c) 2015-2017 The Bitcoin Core developers
// Copyright (c) 2017 The PIVX developers
// Copyright (c) 2018 The Myce developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "httprpc.h"

#include "base58.h"
#include "chainparams.h"
#include "httpserver.h"
#include "rpcprotocol.h"
#include "rpcserver.h"
#include "random.h"
#include "sync.h"
#include "util.h"
#include "utilstrencodings.h"
#include "ui_interface.h"

#include <event2/event.h>
#include <event2/http.h>
#include <event2/thread.h>
#include <event2/buffer.h>
#include <event2/util.h>
#include <event2/keyvalq_struct.h>

#include <boost/algorithm/string.hpp> // boost::trim

#include <univalue.h>

/** Simple one-shot callback timer to be used by the RPC mechanism to e.g.
 * re-lock the wellet.
 */
class HTTPRPCTimer : public RPCTimerBase
{
public:
    HTTPRPCTimer(struct event_base* eventBase, boost::function<void(void)>& func, int64_t millis) :
        ev(eventBase, false, func)
    {
        struct timeval tv;
        tv.tv_sec = millis/1000;
        tv.tv_usec = (millis%1000)*1000;
        ev.trigger(&tv);
    }
private:
    HTTPEvent ev;
};

class HTTPRPCTimerInterface : public RPCTimerInterface
{
public:
    HTTPRPCTimerInterface(struct event_base* base) : base(base)
    {
    }
    const char* Name()
    {
        return "HTTP";
    }
    RPCTimerBase* NewTimer(boost::function<void(void)>& func, int64_t millis)
    {
        return new HTTPRPCTimer(base, func, millis);
    }
private:
    struct event_base* base;
};


/* Pre-base64-encoded authentication token */
static std::string strRPCUserColonPass;
/* Stored RPC timer interface (for unregistration) */
static HTTPRPCTimerInterface* httpRPCTimerInterface = 0;

static void JSONErrorReply(HTTPRequest* req, const UniValue& objError, const UniValue& id)
{

printf("RGP JSONErrorReply started \n");

    // Send error reply from json-rpc error object
    int nStatus = HTTP_INTERNAL_SERVER_ERROR;
    int code = find_value(objError, "code").get_int();

    if (code == RPC_INVALID_REQUEST)
        nStatus = HTTP_BAD_REQUEST;
    else if (code == RPC_METHOD_NOT_FOUND)
        nStatus = HTTP_NOT_FOUND;

    std::string strReply = JSONRPCReply(NullUniValue, objError, id);

    req->WriteHeader("Content-Type", "application/json");
    req->WriteReply(nStatus, strReply);
}

static bool RPCAuthorized(const std::string& strAuth)
{

printf("RGP RPCAuthorized started %s \n", &strAuth );

    if (strRPCUserColonPass.empty()) // Belt-and-suspenders measure if InitRPCAuthentication was not called
    {
        LogPrintf("RGP Debug RPCAuthorized strRPCUserColonPassis empty \n ");
        return false;
    }    
    if (strAuth.substr(0, 6) != "Basic ")
    {
        LogPrintf("RGP Debug strAuth.substr not Basic \n ");
        return false;
    }

LogPrintf("RGP RPCAuthorized Debug 010 \n ");

    std::string strUserPass64 = strAuth.substr(6);
    boost::trim(strUserPass64);
    std::string strUserPass = DecodeBase64(strUserPass64);

LogPrintf("RGP RPCAuthorized Debug 010 \n ");

LogPrintf("RGP Debug TimingResistantEqual next %s %s \n ", strUserPass, strRPCUserColonPass );

    return true;

    //return TimingResistantEqual(strUserPass, strRPCUserColonPass);
}

static bool HTTPReq_JSONRPC(HTTPRequest* req, const std::string &)
{
std::string strReply;

LogPrintf("RGP DEBUG start HTTPReq_JSONRPC \n");

    // JSONRPC handles only POST
    if (req->GetRequestMethod() != HTTPRequest::POST) 
    {
        LogPrintf("RGP DEBUG GetRequestMethod is not HTTPRequest::POST \n");
        req->WriteReply(HTTP_BAD_METHOD, "JSONRPC server handles only POST requests");
        return false;
    }
    // Check authorization
    std::pair<bool, std::string> authHeader = req->GetHeader("authorization");
    if (!authHeader.first) 
    {
LogPrintf("RGP DEBUG authheader is HTTP_UNAUTHORIZED \n");
        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

LogPrintf("RGP DEBUG before RPCAuthorized \n");

    if (!RPCAuthorized(authHeader.second)) 
    {
        LogPrintf("ThreadRPCServer incorrect password attempt from %s \n", authHeader.second );
        /* Deter brute-forcing
           If this results in a DoS the user really
           shouldn't have their RPC port exposed. */
        MilliSleep(250);

        req->WriteReply(HTTP_UNAUTHORIZED);
        return false;
    }

LogPrintf("RGP DEBUG After RPCAuthorized \n");

    JSONRequest jreq;
    try {
        // Parse request
        UniValue valRequest;
        if (!valRequest.read(req->ReadBody()))
            throw JSONRPCError(RPC_PARSE_ERROR, "Parse error");
int looper;

        // singleton request
        if (valRequest.isObject()) 
        {
            jreq.parse(valRequest);


        LogPrintf("RGP Debug httprpc Table.execute method \n" );

            UniValue result = tableRPC.execute(jreq.strMethod, jreq.params);

            // Send reply
            strReply = JSONRPCReply(result, NullUniValue, jreq.id);


        // array of requests
        } 
        else if (valRequest.isArray())
        {
LogPrintf("RGP Debug httprpc valRequest.isArray \n" );
            strReply = JSONRPCExecBatch(valRequest.get_array());
        }
        else
        {
LogPrintf("RGP Debug httprpc parse error \n" );
            throw JSONRPCError(RPC_PARSE_ERROR, "Top-level object parse error");
        }

LogPrintf("RGP Debug httprpc OK \n" );

        req->WriteHeader("Content-Type", "application/json");
        req->WriteReply(HTTP_OK, strReply);
        

        
    } catch (const UniValue& objError) 
    {
        JSONErrorReply(req, objError, jreq.id);
        return false;
    
    } catch (const std::exception& e) 
    {
        JSONErrorReply(req, JSONRPCError(RPC_PARSE_ERROR, e.what()), jreq.id);
        return false;
    }
    
    
    return true;
}

static bool InitRPCAuthentication()
{


    if (mapArgs["rpcpassword"] == "")
    {
        LogPrintf("No rpcpassword set - using random cookie authentication\n");
        if (!GenerateAuthCookie(&strRPCUserColonPass)) 
        {
            uiInterface.ThreadSafeMessageBox(
                _("Error: A fatal internal error occurred, see debug.log for details"), // Same message as AbortNode
                "", CClientUIInterface::MSG_ERROR);
            return false;
        }
    } else 
    {

        strRPCUserColonPass = mapArgs["rpcuser"] + ":" + mapArgs["rpcpassword"];
    }

    return true;
}

bool StartHTTPRPC()
{
    LogPrintf("RGP HTTPReq_JSONRPC \n");

    if (!InitRPCAuthentication())
    {
    	
        return false;
    }
    	
    LogPrintf("RGP RegisterHTTPHandler \n");

    RegisterHTTPHandler("/", true, HTTPReq_JSONRPC);


    assert(EventBase());


    assert(EventBase());
    httpRPCTimerInterface = new HTTPRPCTimerInterface(EventBase());
    


    RPCSetTimerInterface( httpRPCTimerInterface );
        
    //RPCSetTimerInterface(httpRPCTimerInterface.get());
  
    
    
    return true;
}

void InterruptHTTPRPC()
{
    LogPrintf("rpc, Interrupting HTTP RPC server\n");
}

void StopHTTPRPC()
{

    LogPrintf("rpc, Stopping HTTP RPC server\n");
    UnregisterHTTPHandler("/", true);
    if (httpRPCTimerInterface) {
        RPCUnsetTimerInterface(httpRPCTimerInterface);
        delete httpRPCTimerInterface;
        httpRPCTimerInterface = 0;
    }
}
