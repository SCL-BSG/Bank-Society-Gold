#ifndef CLIENTVERSION_H
#define CLIENTVERSION_H

/* RGP : Changed on 4th May 2023 */

/* -------------------------------------------------------------
   -- client versioning V2.0.6.0 --                           --
   -----------------------                                    --
   -- This release includes fixes for wallet crash during     --
   -- stakes, POW miner delays to meet 240sec block spacing   --
   -- and issues with stake delays using embedded random      --
   -- function calls in MilliSecond().                        --
   -- JIRA BTS-8 issue resolved.                              --
   -- JIRA BSG-10 OpenSSL 1.1.1g implemented.                 --
   ------------------------------------------------------------- */
 
// These need to be macros, as version.cpp's and bitcoin-qt.rc's voodoo requires it
#define CLIENT_VERSION_MAJOR       3
#define CLIENT_VERSION_MINOR       0
#define CLIENT_VERSION_REVISION    0
#define CLIENT_VERSION_BUILD       0


static const int CLIENT_VERSION_RPC =
    1000000 * CLIENT_VERSION_MAJOR  ///
    + 10000 * CLIENT_VERSION_MINOR  ///
    + 100 * CLIENT_VERSION_REVISION ///
    + 1 * CLIENT_VERSION_BUILD;



// Set to true for release, false for prerelease or test build
#define CLIENT_VERSION_IS_RELEASE  true

// Converts the parameter X to a string after macro replacement on X has been performed.
// Don't merge these into one macro!
#define STRINGIZE(X) DO_STRINGIZE(X)
#define DO_STRINGIZE(X) #X

#endif // CLIENTVERSION_H
