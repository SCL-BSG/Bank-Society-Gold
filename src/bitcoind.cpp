// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcserver.h"
#include "rpcclient.h"
#include "init.h"
#include <boost/algorithm/string/predicate.hpp>


void WaitForShutdown(boost::thread_group* threadGroup)
{
    bool fShutdown;
    int filter = 0;

    LogPrintf("*** RGP WaitForShutdown started \n");

    MilliSleep(500);

    fShutdown = ShutdownRequested();
    // Tell the main threads to shutdown.
    while (!fShutdown)
    {
        /* RGP */
        //MilliSleep(500);
        MilliSleep(200);

        fShutdown = ShutdownRequested();

        filter++;
        if ( filter > 20 )
        {
           //LogPrintf("*** RGP Waiting for Shutdown \n");
           filter = 0;
        }
    }

    LogPrintf("*** RGP Waiting for Shutdown, SHUTDOWN DETECTED \n");
    if (threadGroup)
    {
        LogPrintf("*** RGP calling Shutdown \n");

        threadGroup->interrupt_all();
        threadGroup->join_all();
    }
}


//////////////////////////////////////////////////////////////////////////////
//
// Start
//
bool AppInit(int argc, char* argv[])
{

    LogPrintf("************************** \n");
    LogPrintf("*** RGP BITCOIND started   \n");
    LogPrintf("************************** \n");

    /* RGP Test */

    boost::thread_group threadGroup;

    bool fRet = false;
    fHaveGUI = false;
    try
    {

        //
        // Parameters
        //
        // If Qt is used, parameters/bitcoin.conf are parsed in qt/bitcoin.cpp's main()
        ParseParameters(argc, argv);
        if (!boost::filesystem::is_directory(GetDataDir(false)))
        {
            fprintf(stderr, "Error: Specified directory does not exist\n");
            Shutdown();
        }
        ReadConfigFile(mapArgs, mapMultiArgs);

LogPrintf("RGP DEBUG Bitcoind.cpp 001 \n");
        if (mapArgs.count("-?") || mapArgs.count("--help"))
        {
            // First part of help message is specific to bitcoind / RPC client
            std::string strUsage = _("SocietyG version") + " " + FormatFullVersion() + "\n\n" +
                _("Usage:") + "\n" +
                  "  societyGd [options]                     " + "\n" +
                  "  societyGd [options] <command> [params]  " + _("Send command to -server or SocietyGd") + "\n" +
                  "  societyGd [options] help                " + _("List commands") + "\n" +
                  "  societyGd [options] help <command>      " + _("Get help for a command") + "\n";

            strUsage += "\n" + HelpMessage();

LogPrintf("RGP DEBUG Bitcoind.cpp 002 \n");

            fprintf(stdout, "%s", strUsage.c_str());
            return false;
        }

LogPrintf("RGP DEBUG Bitcoind.cpp 003 \n");

        // Command-line RPC
        for (int i = 1; i < argc; i++)
        {
LogPrintf("RGP bitcoind.cpp argv %s %d i\n", argv[i], i );        
            if (!IsSwitchChar(argv[i][0]) && !boost::algorithm::istarts_with(argv[i], "SocietyG:"))
                fCommandLine = true;
	}
LogPrintf("RGP DEBUG Bitcoind.cpp 005 \n");

        if (fCommandLine)
        {
LogPrintf("fCommandLine is true \n");
            if (!SelectParamsFromCommandLine())
            {
                fprintf(stderr, "Error: invalid combination of -regtest and -testnet.\n");
LogPrintf("RGP DEBUG Bitcoind.cpp 006 \n");
                return false;
            }
LogPrintf("RGP DEBUG Bitcoind.cpp 007 \n");            
            int ret = CommandLineRPC(argc, argv);
            exit(ret);
        }
#if !WIN32

LogPrintf("RGP DEBUG Bitcoind.cpp 010 \n");
        fDaemon = GetBoolArg("daemon", false);
        if (fDaemon)
        {
            // Daemonize
            pid_t pid = fork();
            if (pid < 0)
            {
                fprintf(stderr, "Error: fork() returned %d errno %d\n", pid, errno);
                return false;
            }
            if (pid > 0) // Parent process, pid is child process id
            {
                CreatePidFile(GetPidFile(), pid);
                return true;
            }
            // Child process falls through to rest of initialization

            pid_t sid = setsid();
            if (sid < 0)
                fprintf(stderr, "Error: setsid() returned %d errno %d\n", sid, errno);
        }
#endif
//threadGroup.create_thread(boost::bind(&ThreadCheckDarkSendPool));



        //threadGroup.create_thread(boost::bind(&WaitForShutdown));

        LogPrintf("*** RGP AppINit2 calling init, extra information \n");

        fRet = AppInit2(threadGroup);

        MilliSleep( 1000 );

        LogPrintf("*** RGP AppINit2 after init returned \n");
LogPrintf("RGP DEBUG Bitcoind.cpp 001.1 \n");
        Read_MN_Config();
LogPrintf("RGP DEBUG Bitcoind.cpp 001.2 \n");

    }
    catch (std::exception& e) {
        PrintException(&e, "AppInit()");
    } catch (...) {
        PrintException(NULL, "AppInit()");
    }




    if (!fRet)
    {
        LogPrintf("*** RGP Appinit2 failed!!!   \n");

        threadGroup.interrupt_all();
        // threadGroup.join_all(); was left out intentionally here, because we didn't re-test all of
        // the startup-failure cases to make sure they don't result in a hang due to some
        // thread-blocking-waiting-for-another-thread-during-startup case
    }
    else
    {
        LogPrintf("*** RGP calling WaitForShutdown   \n");
        WaitForShutdown(&threadGroup);
    }
    Shutdown();

    return fRet;
}

extern void noui_connect();
int main(int argc, char* argv[])
{
    bool fRet = false;

    // Connect bitcoind signal handlers
    noui_connect();

    LogPrintf("*** RGP main started, calling AppInit \n");

    fRet = AppInit(argc, argv);

    if (fRet && fDaemon)
        return 0;

    return (fRet ? 0 : 1);
}
