Bank Society Coin Gold is a free open source decentralized project forked from Bitcoin.

It's an experimental project with the goal of providing a long-term energy-efficient scrypt-based crypto-currency.
You're using this software with no guarantees or warranty of any kind. Use it at your own risk!
Built on the foundations of Bitcoin, Litecoin, PeerCoin, NovaCoin, CraveProject, Dash Masternodes
XUVCoin, BATA, and Crypostle to help further advance the field of crypto-currency.

Adjustments based on network hashrate, previous block difficulty simulating real bullion mining: If the difficulty rate is low; using excessive work to produce low value blocks does not yield large return rates. When the ratio of difficulty adjusts and the network hashrate remains constant or declines: The reward per block will never go higher than 2 coins per block. It's much more profitable to stake in wallets using Proof of Stake.

This algorithm is intended to discourage >51% attacks, or malicous miners. It will also act as an automatic inflation adjustment based on network conditions.

- Dynamic Block Reward 3.0 (C) 2017 Crypostle
- Proof of Work Rewards
     Block 101 to 500 000              Reward = 2.0
     Block 500 001 to 1 000 000        Reward = 2.0
     Block 1 000 001 to 5 000 000      Reward = 1.0
     Block 5 000 001 to 20 000 000     Reward = 1.0
     Block 20 000 001 to 50 000 000    Reward = 1.0
     Block 50 000 001 to 75 000 000    Reward = 2.0
- Proof of Stake rewards
     Block 101 to 500 000              Reward 35%
     Block 500 001 to 1 000 000        Reward 30%
     Block 1 000 001 to 5 000 000      Reward 15%
     Block 5 000 001 to 20 000 000     Reward 10%
     Block 20 000 001 to 50 000 000    Reward 5%
     Block 50 000 000+                 Reward 2%
- Block Spacing: 240 Seconds (4 minutes)
- Diff Retarget: 2 Blocks
- Maturity: 101 Blocks
- Stake Minimum Age: 1 Hour
- Masternode Collateral: 150 000 SOCG   Reward is 40% of processed wallet stakes
- 30 MegaByte Maximum Block Size (30X Bitcoin Core)

Misc Features:

Society includes an Address Index feature, based on the address index API (searchrawtransactions RPC command) implemented in Bitcoin Core but modified implementation to work with the Society codebase (PoS coins maintain a txindex by default for instance).

Initialize the Address Index By Running with -reindexaddr Command Line Argument. It may take 10-15 minutes to build the initial index.

Main Network Information:

nDefaultPort = 23980;
nRPCPort = 23981;
/* RGP Magic Bytes */
pchMessageStart[0] = 0x1d;
pchMessageStart[1] = 0x44;
pchMessageStart[2] = 0x46;
pchMessageStart[3] = 0x97;

Test Network Information:

pchMessageStart[0] = 0xd6;
pchMessageStart[1] = 0xc6;
pchMessageStart[2] = 0x56;
pchMessageStart[3] = 0x7A;
nDefaultPort = 23982;
nRPCPort = 23983;
strDataDir = "testnet";

Social Network:


- Forum: 
- Telegram: 
- Discord: https://discord.gg/2DQfUBFGDz
