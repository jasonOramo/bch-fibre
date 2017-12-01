// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blockencodings.h"
#include "chainparams.h"
#include "config.h"
#include "consensus/consensus.h"
#include "consensus/validation.h"
#include "hash.h"
#include "random.h"
#include "streams.h"
#include "txmempool.h"
#include "util.h"
#include "validation.h"

#include <unordered_map>

#include <chrono>
#define to_millis_double(t) (std::chrono::duration_cast<std::chrono::duration<double, std::chrono::milliseconds::period> >(t).count())


CBlockHeaderAndShortTxIDs::CBlockHeaderAndShortTxIDs(const CBlock& block, bool fUseWTXID, bool fDeterministic) :
        nonce(fDeterministic ? block.GetHash().GetUint64(0) : GetRand(std::numeric_limits<uint64_t>::max())),
        shorttxids(block.vtx.size() - 1), prefilledtxn(1), header(block) {
    FillShortTxIDSelector();
    // TODO: Use our mempool prior to block acceptance to predictively fill more
    // than just the coinbase.
    prefilledtxn[0] = {0, block.vtx[0]};
    for (size_t i = 1; i < block.vtx.size(); i++) {
        const CTransaction &tx = *block.vtx[i];
        shorttxids[i - 1] = GetShortID(tx.GetHash());
    }
}

void CBlockHeaderAndShortTxIDs::FillShortTxIDSelector() const {
    CDataStream stream(SER_NETWORK, PROTOCOL_VERSION);
    stream << header << nonce;
    CSHA256 hasher;
    hasher.Write((uint8_t *)&(*stream.begin()), stream.end() - stream.begin());
    uint256 shorttxidhash;
    hasher.Finalize(shorttxidhash.begin());
    shorttxidk0 = shorttxidhash.GetUint64(0);
    shorttxidk1 = shorttxidhash.GetUint64(1);
}

uint64_t CBlockHeaderAndShortTxIDs::GetShortID(const uint256 &txhash) const {
    static_assert(SHORTTXIDS_LENGTH == 6,
                  "shorttxids calculation assumes 6-byte shorttxids");
    return SipHashUint256(shorttxidk0, shorttxidk1, txhash) & 0xffffffffffffL;
}

ReadStatus PartiallyDownloadedBlock::InitData(
    const bool fBench = LogAcceptCategory("bench");
    std::chrono::steady_clock::time_point start;
    if (fBench)
        start = std::chrono::steady_clock::now();

    const CBlockHeaderAndShortTxIDs &cmpctblock,
    const std::vector<std::pair<uint256, CTransactionRef>> &extra_txn) {
    if (cmpctblock.header.IsNull() ||
        (cmpctblock.shorttxids.empty() && cmpctblock.prefilledtxn.empty()))
        return READ_STATUS_INVALID;
    if (cmpctblock.shorttxids.size() + cmpctblock.prefilledtxn.size() >
        config->GetMaxBlockSize() / MIN_TRANSACTION_SIZE)
        return READ_STATUS_INVALID;

    assert(header.IsNull() && txn_available.empty());
    header = cmpctblock.header;
    txn_available.resize(cmpctblock.BlockTxCount());

    int32_t lastprefilledindex = -1;
    for (size_t i = 0; i < cmpctblock.prefilledtxn.size(); i++) {
        if (cmpctblock.prefilledtxn[i].tx->IsNull()) return READ_STATUS_INVALID;

        // index is a uint16_t, so can't overflow here.
        lastprefilledindex += cmpctblock.prefilledtxn[i].index + 1;
        if (lastprefilledindex > std::numeric_limits<uint16_t>::max())
            return READ_STATUS_INVALID;
        if ((uint32_t)lastprefilledindex > cmpctblock.shorttxids.size() + i) {
            // If we are inserting a tx at an index greater than our full list
            // of shorttxids plus the number of prefilled txn we've inserted,
            // then we have txn for which we have neither a prefilled txn or a
            // shorttxid!
            return READ_STATUS_INVALID;
        }
        txn_available[lastprefilledindex] = cmpctblock.prefilledtxn[i].tx;
    }
    prefilled_count = cmpctblock.prefilledtxn.size();

    // Calculate map of txids -> positions and check mempool to see what we have
    // (or don't). 
    std::chrono::steady_clock::time_point prefilled_filled;
    if (fBench)
        prefilled_filled = std::chrono::steady_clock::now();

    //Because well-formed cmpctblock messages will have a
    // (relatively) uniform distribution of short IDs, any highly-uneven
    // distribution of elements can be safely treated as a READ_STATUS_FAILED.
    std::unordered_map<uint64_t, uint16_t> shorttxids(
        cmpctblock.shorttxids.size());
    uint16_t index_offset = 0;
    for (size_t i = 0; i < cmpctblock.shorttxids.size(); i++) {
        while (txn_available[i + index_offset])
            index_offset++;
        shorttxids[cmpctblock.shorttxids[i]] = i + index_offset;
        // To determine the chance that the number of entries in a bucket
        // exceeds N, we use the fact that the number of elements in a single
        // bucket is binomially distributed (with n = the number of shorttxids
        // S, and p = 1 / the number of buckets), that in the worst case the
        // number of buckets is equal to S (due to std::unordered_map having a
        // default load factor of 1.0), and that the chance for any bucket to
        // exceed N elements is at most buckets * (the chance that any given
        // bucket is above N elements). Thus: P(max_elements_per_bucket > N) <=
        // S * (1 - cdf(binomial(n=S,p=1/S), N)). If we assume blocks of up to
        // 16000, allowing 12 elements per bucket should only fail once per ~1
        // million block transfers (per peer and connection).
        if (shorttxids.bucket_size(
                shorttxids.bucket(cmpctblock.shorttxids[i])) > 12)
            return READ_STATUS_FAILED;
    }
    // TODO: in the shortid-collision case, we should instead request both
    // transactions which collided. Falling back to full-block-request here is
    // overkill.
    if (shorttxids.size() != cmpctblock.shorttxids.size()) {
        // Short ID collision
        return READ_STATUS_FAILED;
    }

    std::vector<bool> have_txn(txn_available.size());

    std::chrono::steady_clock::time_point shortids_mapped;
    if (fBench)
        shortids_mapped = std::chrono::steady_clock::now();

    {
        LOCK(pool->cs);
        const std::vector<std::pair<uint256, CTxMemPool::txiter>> &vTxHashes =
            pool->vTxHashes;
        for (size_t i = 0; i < vTxHashes.size(); i++) {
            uint64_t shortid = cmpctblock.GetShortID(vTxHashes[i].first);
            //TODO: Put an ifdef guard around the prefetch here
            if (i + 1 < vTxHashes.size())
                __builtin_prefetch(vTxHashes[i + 1].first.begin());
            std::unordered_map<uint64_t, uint16_t>::iterator idit =
                shorttxids.find(shortid);
            if (idit != shorttxids.end()) {
                if (!have_txn[idit->second]) {
                    txn_available[idit->second] =
                        vTxHashes[i].second->GetSharedTx();
                    have_txn[idit->second] = true;
                    mempool_count++;
                } else {
                    // If we find two mempool txn that match the short id, just
                    // request it. This should be rare enough that the extra
                    // bandwidth doesn't matter, but eating a round-trip due to
                    // FillBlock failure would be annoying.
                    if (txn_available[idit->second]) {
                        txn_available[idit->second].reset();
                        mempool_count--;
                    }
                }
            }
            // Though ideally we'd continue scanning for the
            // two-txn-match-shortid case, the performance win of an early exit
            // here is too good to pass up and worth the extra risk.
            if (mempool_count == shorttxids.size()) break;
        }
    }

    for (size_t i = 0; i < extra_txn.size(); i++) {
        uint64_t shortid = cmpctblock.GetShortID(extra_txn[i].first);
        std::unordered_map<uint64_t, uint16_t>::iterator idit =
            shorttxids.find(shortid);
        if (idit != shorttxids.end()) {
            if (!have_txn[idit->second]) {
                txn_available[idit->second] = extra_txn[i].second;
                have_txn[idit->second] = true;
                mempool_count++;
                extra_count++;
            } else {
                // If we find two mempool/extra txn that match the short id,
                // just request it. This should be rare enough that the extra
                // bandwidth doesn't matter, but eating a round-trip due to
                // FillBlock failure would be annoying. Note that we dont want
                // duplication between extra_txn and mempool to trigger this
                // case, so we compare hashes first.
                if (txn_available[idit->second] &&
                    txn_available[idit->second]->GetHash() !=
                        extra_txn[i].second->GetHash()) {
                    txn_available[idit->second].reset();
                    mempool_count--;
                    extra_count--;
                }
            }
        }

        // Though ideally we'd continue scanning for the two-txn-match-shortid
        // case, the performance win of an early exit here is too good to pass
        // up and worth the extra risk.
        if (mempool_count == shorttxids.size()) break;
    }

    if (fBench) {
        std::chrono::steady_clock::time_point finished(std::chrono::steady_clock::now());
        LogPrintf("PartiallyDownloadedBlock::InitData took %lf %lf %lf ms\n", to_millis_double(prefilled_filled - start), to_millis_double(shortids_mapped - prefilled_filled), to_millis_double(finished - shortids_mapped));
    }

    LogPrint("cmpctblock", "Initialized PartiallyDownloadedBlock for block %s "
                           "using a cmpctblock of size %lu\n",
             cmpctblock.header.GetHash().ToString(),
             GetSerializeSize(cmpctblock, SER_NETWORK, PROTOCOL_VERSION));

    return READ_STATUS_OK;
}

bool PartiallyDownloadedBlock::IsTxAvailable(size_t index) const {
    assert(!header.IsNull());
    assert(index < txn_available.size());
    return txn_available[index] ? true : false;
}

ReadStatus PartiallyDownloadedBlock::FillBlock(
    CBlock &block, const std::vector<CTransactionRef> &vtx_missing) {
    assert(!header.IsNull());
    uint256 hash = header.GetHash();
    block = header;
    block.vtx.resize(txn_available.size());

    size_t tx_missing_offset = 0;
    for (size_t i = 0; i < txn_available.size(); i++) {
        if (!txn_available[i]) {
            if (vtx_missing.size() <= tx_missing_offset)
                return READ_STATUS_INVALID;
            block.vtx[i] = vtx_missing[tx_missing_offset++];
        } else
            block.vtx[i] = std::move(txn_available[i]);
    }

    // Make sure we can't call FillBlock again.
    header.SetNull();
    txn_available.clear();

    if (vtx_missing.size() != tx_missing_offset) return READ_STATUS_INVALID;

    CValidationState state;
    if (!CheckBlock(*config, block, state, Params().GetConsensus())) {
        // TODO: We really want to just check merkle tree manually here, but
        // that is expensive, and CheckBlock caches a block's "checked-status"
        // (in the CBlock?). CBlock should be able to check its own merkle root
        // and cache that check.
        if (state.CorruptionPossible()) {
            // Possible Short ID collision.
            return READ_STATUS_FAILED;
        }
        return READ_STATUS_CHECKBLOCK_FAILED;
    }

    LogPrint("cmpctblock", "Successfully reconstructed block %s with %lu txn "
                           "prefilled, %lu txn from mempool (incl at least %lu "
                           "from extra pool) and %lu txn requested\n",
             hash.ToString(), prefilled_count, mempool_count, extra_count,
             vtx_missing.size());
    if (vtx_missing.size() < 5) {
        for (const auto &tx : vtx_missing)
            LogPrint("cmpctblock", "Reconstructed block %s required tx %s\n",
                     hash.ToString(), tx->GetId().ToString());
    }

    return READ_STATUS_OK;
}


CBlockHeaderAndLengthShortTxIDs::CBlockHeaderAndLengthShortTxIDs(const CBlock& block, bool fDeterministic) :
        CBlockHeaderAndShortTxIDs(block, true, fDeterministic), txlens(shorttxids.size()) {
    int32_t lastprefilledindex = -1;
    uint16_t index_offset = 0;
    std::vector<PrefilledTransaction>::const_iterator prefilledit = prefilledtxn.begin();
    for (size_t i = 0; i < block.vtx.size(); i++) {
        if (prefilledit != prefilledtxn.end() && (uint32_t)(lastprefilledindex + prefilledit->index + 1) == i + index_offset) {
            lastprefilledindex += prefilledit->index + 1;
            prefilledit++;
            index_offset++;
        } else
            txlens[i - index_offset] = GetSerializeSize(TransactionCompressor(const_cast<CTransactionRef&>(block.vtx[i])), SER_NETWORK, PROTOCOL_VERSION);
    }
}

ReadStatus CBlockHeaderAndLengthShortTxIDs::FillIndexOffsetMap(std::map<size_t, size_t>& index_offsets) const {
    if (txlens.size() != shorttxids.size())
        return READ_STATUS_INVALID;

    std::multimap<size_t, size_t> indexes_left; // size -> index
    int32_t lastprefilledindex = -1;
    uint16_t index_offset = 0;
    std::vector<PrefilledTransaction>::const_iterator prefilledit = prefilledtxn.begin();
    for (size_t i = 0; i < txlens.size(); i++) {
        while (prefilledit != prefilledtxn.end() &&
                (uint32_t)(lastprefilledindex + prefilledit->index + 1) == i + index_offset) {
            lastprefilledindex += prefilledit->index + 1;
            prefilledit++;
            index_offset++;
        }
        indexes_left.insert(std::make_pair(txlens[i], i + index_offset));
    }

    size_t current_index = 0;
    while (!indexes_left.empty()) {
        std::multimap<size_t, size_t>::reverse_iterator lastit = indexes_left.rbegin();
        index_offsets[current_index] = lastit->second;
        current_index += lastit->first;
        lastit++; // base() returns next (ie prev of reverse) element
        indexes_left.erase(lastit.base());

        size_t size_left = FEC_CHUNK_SIZE - (current_index % FEC_CHUNK_SIZE);
        while (!indexes_left.empty() && size_left > indexes_left.begin()->first) {
            std::multimap<size_t, size_t>::iterator it = indexes_left.upper_bound(size_left);
            assert(it != indexes_left.begin()); it--; assert(it->first <= size_left);

            index_offsets[current_index] = it->second;
            current_index += it->first;
            size_left -= it->first;
            indexes_left.erase(it);
        }

        if (current_index > MAX_BLOCK_SERIALIZED_SIZE)
            return READ_STATUS_INVALID;
    }

    return READ_STATUS_OK;
}


#define DIV_CEIL(a, b) (((a) + (b) - 1) / (b))

ChunkCodedBlock::ChunkCodedBlock(const CBlock& block, const CBlockHeaderAndLengthShortTxIDs& headerAndIDs) {
    std::map<size_t, size_t> index_offsets;
    assert(headerAndIDs.FillIndexOffsetMap(index_offsets) == READ_STATUS_OK);

    codedBlock.reserve(MAX_BLOCK_SERIALIZED_SIZE * 1.2);
    VectorOutputStream stream(&codedBlock, SER_NETWORK, PROTOCOL_VERSION);
    for (std::map<size_t, size_t>::iterator it = index_offsets.begin(); it != index_offsets.end(); it++) {
        if (stream.pos() < it->first)
            stream.skip_bytes(it->first - stream.pos());
        assert(stream.pos() == it->first);
        stream << TransactionCompressor(const_cast<CTransactionRef&>(block.vtx[it->second]));
    }
    codedBlock.resize(DIV_CEIL(codedBlock.size(), FEC_CHUNK_SIZE) * FEC_CHUNK_SIZE);
}


static inline uint16_t get_txlens_index(const std::map<uint16_t, uint16_t>& txn_prefilled, uint16_t real_index) {
    if (txn_prefilled.empty())
        return real_index;
    std::map<uint16_t, uint16_t>::const_iterator it = txn_prefilled.upper_bound(real_index);
    it--;
    return real_index - it->second;
}

ReadStatus PartiallyDownloadedChunkBlock::InitData(const CBlockHeaderAndLengthShortTxIDs& comprblock, const std::vector<std::pair<uint256, CTransactionRef>>& extra_txn) {
    const bool fBench = LogAcceptCategory("bench");
    std::chrono::steady_clock::time_point start;
    if (fBench)
        start = std::chrono::steady_clock::now();

    if (comprblock.txlens.size() != comprblock.shorttxids.size())
        return READ_STATUS_INVALID;
    ReadStatus status;
    // We limit number of mempool txn iterated over because it costs a lot of time,
    // and a few extra transactions missed is just fine.
    status = PartiallyDownloadedBlock::InitData(comprblock, extra_txn);
    if (status != READ_STATUS_OK)
        return status;

    std::chrono::steady_clock::time_point base_data_initd;
    if (fBench)
        base_data_initd = std::chrono::steady_clock::now();

    *decoded_block = header;
    decoded_block->vtx.resize(txn_available.size());

    allTxnFromMempool = true;
    for (const std::shared_ptr<const CTransaction>& tx : txn_available)
        allTxnFromMempool &= tx ? true : false;
    if (allTxnFromMempool)
        return READ_STATUS_OK;

    status = comprblock.FillIndexOffsetMap(index_offsets);
    if (status != READ_STATUS_OK)
        return status;

    std::chrono::steady_clock::time_point index_offset_mapped;
    if (fBench)
        index_offset_mapped = std::chrono::steady_clock::now();

    int32_t prefilled_txn_offset = -1;
    for (size_t i = 0; i < comprblock.prefilledtxn.size(); i++) {
        prefilled_txn_offset += comprblock.prefilledtxn[i].index + 1;
        assert(txn_prefilled.insert(std::make_pair(prefilled_txn_offset, i + 1)).second);
    }

    if (index_offsets.size()) {
        size_t codedBlockSize = DIV_CEIL(
                                index_offsets.rbegin()->first +
                                comprblock.txlens[get_txlens_index(txn_prefilled, index_offsets.rbegin()->second)],
                            FEC_CHUNK_SIZE) * FEC_CHUNK_SIZE;
        chunksAvailable.resize(codedBlockSize / FEC_CHUNK_SIZE);
        remainingChunks = codedBlockSize / FEC_CHUNK_SIZE;
        codedBlock.resize(codedBlockSize);
    }

    fill_coding_index_offsets_it = index_offsets.begin();

    if (fBench) {
        std::chrono::steady_clock::time_point finished(std::chrono::steady_clock::now());
        LogPrintf("PartiallyDownloadedChunkBlock::InitData took %lf %lf %lf ms\n", to_millis_double(base_data_initd - start), to_millis_double(index_offset_mapped - base_data_initd), to_millis_double(finished - index_offset_mapped));
    }

    return READ_STATUS_OK;
}

bool PartiallyDownloadedChunkBlock::SerializeTransaction(VectorOutputStream& stream, std::map<size_t, size_t>::iterator it) {
    if (stream.pos() < it->first)
        stream.skip_bytes(it->first - stream.pos());
    assert(stream.pos() == it->first);

    // We're fine blindly serializing tx -> either it came from mempool and is fully valid,
    // or it was received over the wire, so it shouldn't be able to eat all our memory.
    const CTransactionRef& tx = PartiallyDownloadedBlock::txn_available[it->second];
    stream << TransactionCompressor(const_cast<CTransactionRef&>(tx));

    // While we've got it fresh in cache, copy into decoded_block
    decoded_block->vtx[it->second] = tx;

    it++;
    if (it == index_offsets.end())
        return true;
    else
        return stream.pos() <= it->first;
}

ReadStatus PartiallyDownloadedChunkBlock::DoIterativeFill(size_t& firstChunkProcessed) {
    std::map<size_t, size_t>::iterator current_it = fill_coding_index_offsets_it;
    size_t current_index = current_it->first;

    VectorOutputStream stream(&codedBlock, SER_NETWORK, PROTOCOL_VERSION, current_index);

    firstChunkProcessed = current_index / FEC_CHUNK_SIZE;

    for (; fill_coding_index_offsets_it != index_offsets.end(); fill_coding_index_offsets_it++) {
        if (fill_coding_index_offsets_it->first / FEC_CHUNK_SIZE == current_index / FEC_CHUNK_SIZE)
            haveChunk &= IsTxAvailable(fill_coding_index_offsets_it->second);
        else
            break;
    }

    // First process the chunk we were most recently in
    if (haveChunk) {
        for (; current_it != fill_coding_index_offsets_it; current_it++) {
            if (!SerializeTransaction(stream, current_it))
                return READ_STATUS_FAILED; // Could be a shorttxid collision
        }
        for (size_t i = current_index / FEC_CHUNK_SIZE; i < fill_coding_index_offsets_it->first / FEC_CHUNK_SIZE; i++) {
            if (!chunksAvailable[i])
                remainingChunks--;
            chunksAvailable[i] = true;
        }
    }//TODO else if (haveMostRecentlyCheckedTx && mostRecentlyCheckedTxFillsChunk(s)OnItsOwn
        //TODO: Handle chunk that spanned a border and filled up at least one chunk on its own
        // Note that the current FillIndexOffsetMap implementation will never use this

    haveChunk = true; // Next chunk gets a fresh start

    // If we're gonna try to process this chunk later...
    if (fill_coding_index_offsets_it != index_offsets.end() && IsTxAvailable(fill_coding_index_offsets_it->second)) {
        current_index = fill_coding_index_offsets_it->first;
        if (current_index % FEC_CHUNK_SIZE != 0) {
            // If we don't start on a chunk boundry, we assume the previous transaction
            // came into our chunk, as otherwise our packing algorithm is braindead
            assert(fill_coding_index_offsets_it != index_offsets.begin());
            std::map<size_t, size_t>::iterator previt = fill_coding_index_offsets_it; previt--;
            if (IsTxAvailable(previt->second)) {
                if (stream.pos() <= previt->first) { // If previt was not already encoded...
                    if (!SerializeTransaction(stream, previt))
                        return READ_STATUS_FAILED; // Could be a shorttxid collision
                }
            } else
                haveChunk = false; // I'm sorry, but its just not gonna work out - its not you, its me
        }
    }

    return READ_STATUS_OK;
}

bool PartiallyDownloadedChunkBlock::IsIterativeFillDone() const {
    return allTxnFromMempool || fill_coding_index_offsets_it == index_offsets.end();
}

uint256& PartiallyDownloadedChunkBlock::GetBlockHash() const {
    assert(!header.IsNull());
    if (block_hash.IsNull())
        block_hash = header.GetHash();
    return block_hash;
}

bool PartiallyDownloadedChunkBlock::IsBlockAvailable() const {
    assert(!header.IsNull());
    return allTxnFromMempool || !remainingChunks;
}

bool PartiallyDownloadedChunkBlock::AreChunksAvailable() const {
    return !header.IsNull() && !allTxnFromMempool;
}

ReadStatus PartiallyDownloadedChunkBlock::FinalizeBlock() {
    const bool fBench = LogAcceptCategory("bench");
    std::chrono::steady_clock::time_point start;
    if (fBench)
        start = std::chrono::steady_clock::now();

    assert(!header.IsNull());
    assert(IsBlockAvailable());

    CBlock& block = *decoded_block;

    for (size_t i = 0; i < txn_available.size(); i++) {
        if (block.vtx[i])
            continue;

        if (txn_available[i])
            block.vtx[i] = txn_available[i];
        else
            assert(!allTxnFromMempool);
    }
    if (allTxnFromMempool) {
        block_finalized = true;
        return READ_STATUS_OK;
    }

    std::chrono::steady_clock::time_point mempool_filled;
    if (fBench)
        mempool_filled = std::chrono::steady_clock::now();

    // TODO: This is really slow (like several ms)
    // We should migrate to keeping the partially-decoded block as a unique_ptr
    // and decode transactions as we go...this will not only save the deserialize
    // time we spend here, but by calling GetHash() at that time, save the
    // hashing time we'll spend later to check the hash of each transaction.
    VectorInputStream stream(&codedBlock, SER_NETWORK, PROTOCOL_VERSION | SERIALIZE_TRANSACTION_NO_CACHE);
    for (std::map<size_t, size_t>::const_iterator it = index_offsets.begin(); it != index_offsets.end(); it++) {
        if (block.vtx[it->second])
            continue;
        try {
            if (it->first < stream.pos()) // Last transaction was longer than expected
                return READ_STATUS_FAILED; // Could be a shorttxid collision
            stream.seek(it->first);
            stream >> REF(TransactionCompressor(block.vtx[it->second]));
        } catch (const std::ios_base::failure& e) {
            return READ_STATUS_FAILED; // Could be a shorttxid collision
        }
    }

    if (fBench) {
        std::chrono::steady_clock::time_point finished(std::chrono::steady_clock::now());
        LogPrintf("PartiallyDownloadedChunkBlock::FinalizeBlock took %lf %lf ms\n", to_millis_double(mempool_filled - start), to_millis_double(finished - mempool_filled));
    }

    block_finalized = true;

    return READ_STATUS_OK;
}

size_t PartiallyDownloadedChunkBlock::GetChunkCount() const {
    assert(AreChunksAvailable());
    return chunksAvailable.size();
}

bool PartiallyDownloadedChunkBlock::IsChunkAvailable(size_t chunk) const {
    assert(chunk < GetChunkCount());
    return chunksAvailable[chunk];
}

unsigned char* PartiallyDownloadedChunkBlock::GetChunk(size_t chunk) {
    assert(chunk < GetChunkCount());
    return &codedBlock[chunk * FEC_CHUNK_SIZE];
}

void PartiallyDownloadedChunkBlock::MarkChunkAvailable(size_t chunk) {
    assert(chunk < GetChunkCount());
    if (!chunksAvailable[chunk])
        remainingChunks--;
    chunksAvailable[chunk] = true;
}
