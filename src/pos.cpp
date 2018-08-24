// Copyright (c) 2014-2016 The BlackCoin Core developers
// Copyright (c) 2012-2013 The PPCoin developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "pos.h"

#include "chain.h"
#include "chainparams.h"
#include "clientversion.h"
#include "coins.h"
#include "hash.h"
#include "primitives/transaction.h"
#include "uint256.h"
#include "util.h"
#include "validation.h"
#include <stdio.h>
#include "consensus/consensus.h"
#include "wallet/wallet.h"

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
uint256 ComputeStakeModifier(const CBlockIndex* pindexPrev, const uint256& kernel)
{
    if (!pindexPrev)
        return uint256(); // genesis block's modifier is 0

    CHashWriter ss(SER_GETHASH, 0);
    ss << kernel << pindexPrev->nStakeModifier;
    return ss.GetHash();
}

// Check whether the coinstake timestamp meets protocol
bool CheckCoinStakeTimestamp(int64_t nTimeBlock, int64_t nTimeTx)
{
    const Consensus::Params& params = Params().GetConsensus();

    if(! (nTimeBlock == nTimeTx) && ((nTimeTx & params.nStakeTimestampMask) == 0)){
        return false;
    }else{
        return true;
    }
}

// Simplified version of CheckCoinStakeTimestamp() to check header-only timestamp
bool CheckStakeBlockTimestamp(int64_t nTimeBlock)
{
    return CheckCoinStakeTimestamp(nTimeBlock, nTimeBlock);
}

// BlackCoin kernel protocol v3
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.nTime + txPrev.vout.hash + txPrev.vout.n + nTime) < bnTarget * nWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coins one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier: scrambles computation to make it very difficult to precompute
//                   future proof-of-stake
//   txPrev.nTime: slightly scrambles computation
//   txPrev.vout.hash: hash of txPrev, to reduce the chance of nodes
//                     generating coinstake at the same time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   nTime: current timestamp
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(const CBlockIndex* pindexPrev, 
                            unsigned int nBits,  
                            CBlockIndex& blockFrom, 
                            const CCoins* txPrev, 
                            const COutPoint& prevout, 
                            unsigned int nTimeTx)
{
    // Weight
    int64_t nValueIn = txPrev->vout[prevout.n].nValue;
    if (nValueIn == 0)
        return false;
    if (blockFrom.GetBlockTime() + Params().GetConsensus().nStakeMinAge > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");
    // Base target
    arith_uint256 bnTarget;
    bnTarget.SetCompact(nBits);

    // Calculate hash
    CHashWriter ss(SER_GETHASH, 0);
    ss << pindexPrev->nStakeModifier << txPrev->nTime << prevout.hash << prevout.n << nTimeTx;
    uint256 hashProofOfStake = ss.GetHash();

    // Now check if proof-of-stake hash meets target protocol
    if (UintToArith256(hashProofOfStake) / nValueIn > bnTarget)
        return false;

    return true;
}

bool IsConfirmedInNPrevBlocks(const CDiskTxPos& txindex, const CBlockIndex* pindexFrom, int nMaxDepth, int& nActualDepth)
{
    for (const CBlockIndex* pindex = pindexFrom; pindex && pindexFrom->nHeight - pindex->nHeight < nMaxDepth; pindex = pindex->pprev) {
        if (pindex->nDataPos == txindex.nPos && pindex->nFile == txindex.nFile) {
            nActualDepth = pindexFrom->nHeight - pindex->nHeight;
            return true;
        }
    }

    return false;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(CBlockIndex* pindexPrev, const CTransaction& tx, unsigned int nBits, CValidationState& state)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn& txin = tx.vin[0];
    
    
    CTransactionRef txPrev;
    uint256 hashBlock = uint256();
    if (!GetTransaction(txin.prevout.hash, txPrev, Params().GetConsensus(), hashBlock, true))
        return error("CheckProofOfStake() : INFO: read txPrev failed %s",txin.prevout.hash.GetHex());
    

    CDiskTxPos txindex;

    // Verify signature
    if (!VerifySignature(*txPrev, tx, 0, SCRIPT_VERIFY_NONE, 0))
        return state.DoS(100, error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString()));
 

    if (mapBlockIndex.count(hashBlock) == 0)
        return fDebug? error("CheckProofOfStake() : read block failed") : false; // unable to read block of previous transaction

    
    CBlockIndex* pblockindex = mapBlockIndex[hashBlock];
    if (!CheckStakeKernelHash(pindexPrev, nBits, *pblockindex , new CCoins(*txPrev, pindexPrev->nHeight), txin.prevout, tx.nTime))
        return state.DoS(1, error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s", tx.GetHash().ToString())); // may occur during initial download or if behind on block chain sync

    return true;
}

bool VerifySignature(const CTransaction& txFrom, const CTransaction& txTo, unsigned int nIn, unsigned int flags, int nHashType)
{
    assert(nIn < txTo.vin.size());
    const CTxIn& txin = txTo.vin[nIn];
    if (txin.prevout.n >= txFrom.vout.size())
        return false;
    const CTxOut& txout = txFrom.vout[txin.prevout.n];

    if (txin.prevout.hash != txFrom.GetHash())
        return false;

    return VerifyScript(txin.scriptSig, txout.scriptPubKey, &txin.scriptWitness, flags, TransactionSignatureChecker(&txTo, nIn, txout.nValue), NULL);
}
 

bool CheckKernel(CBlockIndex* pindexPrev, unsigned int nBits, uint32_t nTime, const COutPoint& prevout, uint32_t* pBlockTime)
{
    uint256 hashProofOfStake, targetProofOfStake;
    
    CTransactionRef txPrev;
    uint256 hashBlock = uint256();
    if (!GetTransaction(prevout.hash, txPrev, Params().GetConsensus(), hashBlock, true)){
        LogPrintf("CheckKernel : Could not find previous transaction %s\n",prevout.hash.ToString());
        return false;
    }
    
    if (mapBlockIndex.count(hashBlock) == 0){
        LogPrintf("CheckKernel : Could not find block of previous transaction %s\n",hashBlock.ToString());
        return false;
    }

    CBlockIndex* pblockindex = mapBlockIndex[hashBlock];

    if (pblockindex->GetBlockTime() + Params().GetConsensus().nStakeMinAge > nTime)
        return false;

    if (pBlockTime)
        *pBlockTime = pblockindex->GetBlockTime();
    if (!pwalletMain->mapWallet.count(prevout.hash))
        return("CheckProofOfStake(): Couldn't get Tx Index");
    return CheckStakeKernelHash(pindexPrev, nBits,*pblockindex, new CCoins(*txPrev, pindexPrev->nHeight), prevout, nTime);
    
}

void CacheKernel(std::map<COutPoint, CStakeCache>& cache, const COutPoint& prevout)
{
    if (cache.find(prevout) != cache.end()) {
        //already in cache
        return;
    }
    
    CMutableTransaction tmpPrevTx;
    CDiskTxPos txindex;
    if (!ReadFromDisk(tmpPrevTx, txindex, *pblocktree, prevout))
        return;
    CTransaction txPrev(tmpPrevTx);
    // Read block
    CBlock block;
    const CDiskBlockPos& pos = CDiskBlockPos(txindex.nFile, txindex.nPos);
    if (!ReadBlockFromDisk(block, pos, Params().GetConsensus()))
        return;
    CStakeCache c(block, txindex, txPrev);
    cache.insert({prevout, c});
}
