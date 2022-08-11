Bank Society Coin Gold (Society) is a free open source decentralized project derived from Bitcoin.
It's an experimental project with the goal of providing a long-term energy-efficient scrypt-based crypto-currency.
You're using this software with no guarantees or warranty of any kind. Use it at your own risk!
Built on the foundations of Bitcoin, Litecoin, PeerCoin, NovaCoin, CraveProject, Dash Masternodes
XUVCoin, BATA, and Crypostle to help further advance the field of crypto-currency.

Adjustments based on network hashrate, previous block difficulty simulating real bullion mining: If the difficulty rate is low; using excessive work to produce low value blocks does not yield large return rates. When the ratio of difficulty adjusts and the network hashrate remains constant or declines: The reward per block will reach the maximum level, thus mining becomes very profitable.

This algorithm is intended to discourage >51% attacks, or malicous miners. It will also act as an automatic inflation adjustment based on network conditions.

- Dynamic Block Reward 3.0 (C) 2017 Crypostle
- Block rewards to be updated
- Block Spacing: 240 Seconds (4 minutes)
- Diff Retarget: 2 Blocks
- Maturity: 101 Blocks
- Stake Minimum Age: 1 Hour
- Masternode Collateral: 150 000 SOCG
- 30 MegaByte Maximum Block Size (30X Bitcoin Core)

Misc Features:

Society includes an Address Index feature, based on the address index API (searchrawtransactions RPC command) implemented in Bitcoin Core but modified implementation to work with the Society codebase (PoS coins maintain a txindex by default for instance).

Initialize the Address Index By Running with -reindexaddr Command Line Argument. It may take 10-15 minutes to build the initial index.

Main Network Information:

/* RGP */
nDefaultPort = 23980;
nRPCPort = 23981;
/* RGP Magic BYtes */
pchMessageStart[0] = 0x1c;
pchMessageStart[1] = 0x43;
pchMessageStart[2] = 0x45;
pchMessageStart[3] = 0x96;

- Port: 20060
- RPC Port: 20061
- Magic Bytes: 0x1a 0x33 0x25 0x88

Test Network Information:

/* RGP *
pchMessageStart[0] = 0xd5;
pchMessageStart[1] = 0xc5;
pchMessageStart[2] = 0x55;
pchMessageStart[3] = 0x79;
nDefaultPort = 23982;
nRPCPort = 23983;
strDataDir = "testnet";

- Port: 20062
- RPC Port: 20063
- Magic Bytes: 0x6b 0x33 0x25 0x75

Social Network:

- Github: 
- Forum: 
- Slack:
- Telegram: 
- Discord: 
