#include "client-api/server.h"
#include "client-api/protocol.h"
#include "sync.h"
#include "util.h"
#include "wallet/wallet.h"
#include <univalue.h>
#include <boost/signals2/signal.hpp>

static bool fAPIRunning = false;
static bool fAPIInWarmup = true;
static std::string apiWarmupStatus("API server started");
static CCriticalSection cs_apiWarmup;

static struct CAPISignals
{
    boost::signals2::signal<void ()> Started;
    boost::signals2::signal<void ()> Stopped;
    boost::signals2::signal<void (const CAPICommand&)> PreCommand;
    boost::signals2::signal<void (const CAPICommand&)> PostCommand;
} g_apiSignals;

bool IsAPIRunning() {
	return fAPIRunning;
}

bool StartAPI()
{
    LogPrintf("Starting API\n");
    fAPIRunning = true;
    g_apiSignals.Started();
    return true;
}

void InterruptAPI()
{
    LogPrint("api", "Interrupting API\n");
    // Interrupt e.g. running longpolls
    fAPIRunning = false;
}

void StopAPI()
{
    LogPrint("api", "Stopping API\n");
    g_apiSignals.Stopped();
}

void SetAPIWarmupStatus(const std::string& newStatus)
{
    LOCK(cs_apiWarmup);
    apiWarmupStatus = newStatus;
}

void SetAPIWarmupFinished()
{
    LOCK(cs_apiWarmup);
    assert(fAPIInWarmup);
    fAPIInWarmup = false;
}

bool APIIsInWarmup(std::string *outStatus)
{
    LOCK(cs_apiWarmup);
    if (outStatus)
        *outStatus = apiWarmupStatus;
    return fAPIInWarmup;
}

bool APIIsInWarmup()
{
    LOCK(cs_apiWarmup);
    return fAPIInWarmup;
}

static const CAPICommand vAPICommands[] =
{ //  category              name                      actor (function)       
  //  --------------------- ------------------------  -----------------------  
  //{ "addressindex",       "getaddressbalance",      &getaddressbalance    },
};

CAPITable::CAPITable()
{
	unsigned int vcidx;
    for (vcidx = 0; vcidx < (sizeof(vAPICommands) / sizeof(vAPICommands[0])); vcidx++)
    {
        const CAPICommand *pcmd;

        pcmd = &vAPICommands[vcidx];
        mapCommands[pcmd->collection] = pcmd;
    }
}



const CAPICommand *CAPITable::operator[](const std::string &name) const
{
	std::map<std::string, const CAPICommand*>::const_iterator it = mapCommands.find(name);
	if (it == mapCommands.end())
		return NULL;
	return (*it).second;
}

bool CAPITable::appendCommand(const std::string& name, const CAPICommand* pcmd)
{
    if (IsAPIRunning())
        return false;

    // don't allow overwriting for now
    std::map<std::string, const CAPICommand*>::const_iterator it = mapCommands.find(name);
    if (it != mapCommands.end())
        return false;

    mapCommands[name] = pcmd;
    return true;
}

void APIJSONRequest::parseType(std::string typeRequest)
{
    if(typeRequest=="none"){
        type = None;
    }
    if(typeRequest=="initial"){
        type = Initial;
    }
    else if(typeRequest=="create"){
        type = Create;
    }
    else if(typeRequest=="update"){
        type = Update;
    }
    else if(typeRequest=="delete"){
        type = Delete;
    }
    else {
       throw JSONAPIError(API_INVALID_REQUEST, "Invalid Type request string"); 
    }
}

void APIJSONRequest::parse(const UniValue& valRequest)
{
    // Parse request
    if (!valRequest.isObject())
        throw JSONAPIError(API_INVALID_REQUEST, "Invalid Request object");
    const UniValue& request = valRequest.get_obj();

    // Parse type
    UniValue valType = find_value(request, "type");
    string typeStr;
    if (valType.isNull()){
        typeStr = "none";
    }
    else {
        if (!valType.isStr())
            throw JSONAPIError(API_INVALID_REQUEST, "type must be a string");
        typeStr = valType.get_str();
    }
    parseType(typeStr);

    // Parse collection
    UniValue valCollection = find_value(request, "collection");
    if (valCollection.isNull())
        throw JSONAPIError(API_INVALID_REQUEST, "Missing collection in JSON request");
    if (!valCollection.isStr())
        throw JSONAPIError(API_INVALID_REQUEST, "collection must be a string");
    collection = valCollection.get_str();

    // Parse auth
    UniValue valAuth = find_value(request, "auth");
    if (valAuth.isObject()){
        auth = valAuth.get_obj();
        LogPrintf("API: auth is object\n ");
    }
    else if (valAuth.isNull())
        auth = UniValue(UniValue::VARR);
    else
        throw JSONAPIError(API_INVALID_REQUEST, "auth must be an object");

    // Parse data
    UniValue valData = find_value(request, "data");
    if (valData.isObject()){
        data = valData.get_obj();
        LogPrintf("API: data is object\n ");
    }
    else if (valData.isNull())
        data = UniValue(UniValue::VARR);
    else
        throw JSONAPIError(API_INVALID_REQUEST, "data must be an object");


}

UniValue CAPITable::execute(APIJSONRequest request, const bool authPort) const
{

    // Find method - TODO make tableAPI a 2 dim array and pass type as the first argument
    const CAPICommand *pcmd = tableAPI[request.collection];
    if (!pcmd)
        throw JSONAPIError(API_METHOD_NOT_FOUND, "Method not found");

    // Return if in warmup
    { 
        LOCK(cs_apiWarmup);
        //TODO add 'warmupOK' field or something like that and set true for apistatus
        if (fAPIInWarmup && !pcmd->warmupOk)
            throw JSONAPIError(API_IN_WARMUP, apiWarmupStatus);
    }

    // If on open port, fail if trying to execute an authenticated method.
    if(!authPort && pcmd->authPort){
        throw JSONAPIError(API_NOT_AUTHENTICATED, "Not authenticated for this method");
    }

    const CAPICommand *walletlock = tableAPI["lockwallet"];

    g_apiSignals.PreCommand (*pcmd);
    try
    {
        // If this method requires passphrase, lock and unlock the wallet accordingly
        if(pcmd->authPassphrase && (pwalletMain && pwalletMain->IsCrypted())){
            if(request.auth.isNull()){
                throw JSONAPIError(API_INVALID_PARAMETER, "Missing auth field");
            }
            // execute wallet unlock, call method, relock following call. 
            const CAPICommand *walletunlock = tableAPI["unlockwallet"];
            UniValue lock = walletunlock->actor(request.type, NullUniValue, request.auth, false);
            return pcmd->actor(request.type, request.data, NullUniValue, false);
            walletlock->actor(request.type, NullUniValue, NullUniValue, false);

        }
        return pcmd->actor(request.type, request.data, request.auth, false);
    }
    catch (const std::exception& e)
    {
        //walletlock->actor(request.type, NullUniValue, NullUniValue, false); //ensure to relock should an error occur
        throw JSONAPIError(API_MISC_ERROR, e.what());
    }

    g_apiSignals.PostCommand(*pcmd);
}

CAPITable tableAPI;
