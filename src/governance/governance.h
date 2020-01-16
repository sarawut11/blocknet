// Copyright (c) 2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKNET_GOVERNANCE_GOVERNANCE_H
#define BLOCKNET_GOVERNANCE_GOVERNANCE_H

#include <amount.h>
#include <chain.h>
#include <consensus/params.h>
#include <consensus/validation.h>
#include <hash.h>
#include <key_io.h>
#include <policy/policy.h>
#include <script/standard.h>
#include <shutdown.h>
#include <streams.h>
#include <txmempool.h>
#include <uint256.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validation.h>
#include <validationinterface.h>

#include <regex>
#include <string>
#include <utility>

#include <boost/algorithm/string.hpp>
#include <boost/thread.hpp>

/**
 * Governance namespace.
 */
namespace gov {

/**
 * Governance types are used with OP_RETURN to indicate how the messages should be processed.
 */
enum Type : uint8_t {
    NONE         = 0,
    PROPOSAL     = 1,
    VOTE         = 2,
};

static const uint8_t NETWORK_VERSION = 0x01;
static const CAmount VOTING_UTXO_INPUT_AMOUNT = 1 * COIN;
static const int VINHASH_SIZE = 12;
static const int PROPOSAL_USERDEFINED_LIMIT = 139;
typedef std::array<unsigned char, VINHASH_SIZE> VinHash;

/**
 * Create VinHash from vin prevout.
 * @param COutPoint
 * @return
 */
static VinHash makeVinHash(const COutPoint & prevout) {
    CHashWriter hw(SER_GETHASH, 0);
    hw << prevout;
    const auto & hwhash = hw.GetHash();
    const auto & v = ToByteVector(hwhash);
    VinHash r;
    for (int i = 0; i < VINHASH_SIZE; ++i)
        r[i] = v[i];
    return r;
}

/**
 * Return the CKeyID for the specified utxo.
 * @param utxo
 * @param keyid
 * @return
 */
static bool GetKeyIDForUTXO(const COutPoint & utxo, CTransactionRef & tx, CKeyID & keyid) {
    uint256 hashBlock;
    if (!GetTransaction(utxo.hash, tx, Params().GetConsensus(), hashBlock))
        return false;
    if (utxo.n >= tx->vout.size())
        return false;
    CTxDestination dest;
    if (!ExtractDestination(tx->vout[utxo.n].scriptPubKey, dest))
        return false;
    keyid = *boost::get<CKeyID>(&dest);
    return true;
}

/**
 * Returns the next superblock from the most recent chain tip by default.
 * If fromBlock is specified the superblock immediately after fromBlock
 * is returned.
 * @param params
 * @param fromBlock
 * @return
 */
static int NextSuperblock(const Consensus::Params & params, const int fromBlock = 0) {
    if (fromBlock == 0) {
        LOCK(cs_main);
        return chainActive.Height() - chainActive.Height() % params.superblock + params.superblock;
    }
    return fromBlock - fromBlock % params.superblock + params.superblock;
}

/**
 * Returns the previous superblock from the most recent chain tip by default.
 * If fromBlock is specified the superblock immediately preceeding fromBlock
 * is returned.
 * @param params
 * @param fromBlock
 * @return
 */
static int PreviousSuperblock(const Consensus::Params & params, const int fromBlock = 0) {
    const int nextSuperblock = NextSuperblock(params, fromBlock);
    return nextSuperblock - params.superblock;
}

/**
 * Encapsulates serialized OP_RETURN governance data.
 */
class NetworkObject {
public:
    explicit NetworkObject() = default;

    /**
     * Returns true if this network data contains the proper version.
     * @return
     */
    bool isValid() const {
        return version == NETWORK_VERSION;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
    }

    const uint8_t & getType() const {
        return type;
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{NONE};
};

/**
 * Proposals encapsulate the data required by the network to support voting and payments.
 * They can be created by anyone willing to pay the submission fee.
 */
class Proposal {
public:
    explicit Proposal(std::string name, int superblock, CAmount amount, std::string address,
                      std::string url, std::string description) : name(std::move(name)), superblock(superblock),
                                              amount(amount), address(std::move(address)), url(std::move(url)),
                                              description(std::move(description)) {}
    explicit Proposal(int blockNumber) : blockNumber(blockNumber) {}
    Proposal() = default;
    Proposal(const Proposal &) = default;
    Proposal& operator=(const Proposal &) = default;
    friend inline bool operator==(const Proposal & a, const Proposal & b) { return a.getHash() == b.getHash(); }
    friend inline bool operator!=(const Proposal & a, const Proposal & b) { return !(a.getHash() == b.getHash()); }
    friend inline bool operator<(const Proposal & a, const Proposal & b) { return a.getHash() < b.getHash(); }

    /**
     * Null check
     * @return
     */
    bool isNull() const {
        return superblock == 0;
    }

    /**
     * Valid if the proposal properties are correct.
     * @param params
     * @param failureReasonRet
     * @return
     */
    bool isValid(const Consensus::Params & params, std::string *failureReasonRet=nullptr) const {
        static std::regex rrname("^\\w+[\\w\\-_ ]*\\w+$");
        if (!std::regex_match(name, rrname)) {
            if (failureReasonRet) *failureReasonRet = strprintf("Proposal name %s is invalid, only alpha-numeric characters are accepted", name);
            return false;
        }
        if (superblock % params.superblock != 0) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad superblock number, did you mean %d", gov::NextSuperblock(params));
            return false;
        }
        if (!(amount >= params.proposalMinAmount && amount <= std::min(params.proposalMaxAmount, params.GetBlockSubsidy(superblock, params)))) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal amount, specify amount between %s - %s",
                    FormatMoney(params.proposalMinAmount), FormatMoney(std::min(params.proposalMaxAmount, params.GetBlockSubsidy(superblock, params))));
            return false;
        }
        if (!IsValidDestination(DecodeDestination(address))) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad payment address %s", address);
            return false;
        }
        if (type != PROPOSAL) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal type, expected %d", PROPOSAL);
            return false;
        }
        if (version != NETWORK_VERSION) {
            if (failureReasonRet) *failureReasonRet = strprintf("Bad proposal network version, expected %d", NETWORK_VERSION);
            return false;
        }
        CDataStream ss(SER_NETWORK, GOV_PROTOCOL_VERSION);
        ss << version << type << name << superblock << amount << address << url << description;
        const int maxBytes = MAX_OP_RETURN_RELAY-3; // -1 for OP_RETURN -2 for pushdata opcodes
        // If this protocol changes update PROPOSAL_USERDEFINED_LIMIT so that gui can understand that limit
        const int nonUserBytes = 14;
        const int packetBytes = 4;
        if (ss.size() > maxBytes) {
            if (failureReasonRet) *failureReasonRet = strprintf("Proposal input is too long, try reducing the description by %u characters. You can use a combined total of %u characters across proposal name, url, description, and payment address fields.", ss.size()-maxBytes, maxBytes-nonUserBytes-packetBytes);
            return false;
        }
        return true;
    }

    /**
     * Proposal name
     * @return
     */
    const std::string & getName() const {
        return name;
    }

    /**
     * Proposal superblock
     * @return
     */
    const int & getSuperblock() const {
        return superblock;
    }

    /**
     * Proposal amount
     * @return
     */
    const CAmount & getAmount() const {
        return amount;
    }

    /**
     * Proposal address
     * @return
     */
    const std::string & getAddress() const {
        return address;
    }

    /**
     * Proposal url (for more information)
     * @return
     */
    const std::string & getUrl() const {
        return url;
    }

    /**
     * Proposal description
     * @return
     */
    const std::string & getDescription() const {
        return description;
    }

    /**
     * Proposal block number
     * @return
     */
    const int & getBlockNumber() const {
        return blockNumber;
    }

    /**
     * Proposal hash
     * @return
     */
    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << name << superblock << amount << address << url << description;
        return ss.GetHash();
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
        READWRITE(superblock);
        READWRITE(amount);
        READWRITE(address);
        READWRITE(name);
        READWRITE(url);
        READWRITE(description);
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{PROPOSAL};
    std::string name;
    int superblock{0};
    CAmount amount{0};
    std::string address;
    std::string url;
    std::string description;

protected: // memory only
    int blockNumber{0}; // block containing this proposal
};

enum VoteType : uint8_t {
    NO      = 0,
    YES     = 1,
    ABSTAIN = 2,
};

/**
 * Votes can be cast on proposals and ultimately lead to unlocking funds for proposals that meet
 * the minimum requirements and minimum required votes.
 */
class Vote {
public:
    explicit Vote(const uint256 & proposal, const VoteType & vote,
                  const COutPoint & utxo, const VinHash & vinhash) : proposal(proposal),
                                                                     vote(vote),
                                                                     utxo(utxo),
                                                                     vinhash(vinhash) {
        loadKeyID();
    }
    explicit Vote(const uint256 & proposal, const VoteType & vote,
                  const COutPoint & utxo, const VinHash & vinhash,
                  const CKeyID & keyid, const CAmount & amount) : proposal(proposal),
                                                                  vote(vote),
                                                                  utxo(utxo),
                                                                  vinhash(vinhash),
                                                                  keyid(keyid),
                                                                  amount(amount) { }
    explicit Vote(const COutPoint & outpoint, const int64_t & time = 0, const int & blockNumber = 0) : outpoint(outpoint),
                                                                                                       time(time),
                                                                                                       blockNumber(blockNumber) {}
    Vote() = default;
    Vote(const Vote &) = default;
    Vote& operator=(const Vote &) = default;
    friend inline bool operator==(const Vote & a, const Vote & b) { return a.getHash() == b.getHash(); }
    friend inline bool operator!=(const Vote & a, const Vote & b) { return !(a.getHash() == b.getHash()); }
    friend inline bool operator<(const Vote & a, const Vote & b) { return a.getHash() < b.getHash(); }

    /**
     * Returns true if a valid vote string type was converted.
     * @param strVote
     * @param voteType Mutated with the converted vote type.
     * @return
     */
    static bool voteTypeForString(std::string strVote, VoteType & voteType) {
        boost::to_lower(strVote, std::locale::classic());
        if (strVote == "yes") {
            voteType = YES;
        } else if (strVote == "no") {
            voteType = NO;
        } else if (strVote == "abstain") {
            voteType = ABSTAIN;
        } else {
            return false;
        }
        return true;
    }

    /**
     * Returns the string representation of the vote type.
     * @param voteType
     * @param valid true if conversion was successful, otherwise false.
     * @return
     */
    static std::string voteTypeToString(const VoteType & voteType, bool *valid = nullptr) {
        std::string strVote;
        if (voteType == YES) {
            strVote = "yes";
        } else if (voteType == NO) {
            strVote = "no";
        } else if (voteType == ABSTAIN) {
            strVote = "abstain";
        } else {
            if (valid) *valid = false;
        }
        if (valid) *valid = true;
        return strVote;
    }

    /**
     * Null check
     * @return
     */
    bool isNull() {
        return utxo.IsNull();
    }

    /**
     * Returns true if the vote properties are valid and the utxo pubkey
     * matches the pubkey of the signature.
     * @return
     */
    bool isValid(const Consensus::Params & params) const {
        if (!(version == NETWORK_VERSION && type == VOTE && isValidVoteType(vote)))
            return false;
        if (amount < params.voteMinUtxoAmount) // n bounds checked in GetKeyIDForUTXO
            return false;
        // Ensure the pubkey of the utxo matches the pubkey of the vote signature
        if (keyid.IsNull())
            return false;
        if (pubkey.GetID() != keyid)
            return false;
        return true;
    }

    /**
     * Returns true if the vote properties are valid and the utxo pubkey
     * matches the pubkey of the signature as well as the added check
     * that the hash of the prevout matches the expected vin hash. This
     * check will prevent vote replay attacks by ensuring that the vin
     * associated with the vote matches the expected vin hash sent
     * with the vote's OP_RETURN data.
     * @param vinHashes Set of truncated vin prevout hashes.
     * @paramk params
     * @return
     */
    bool isValid(const std::set<VinHash> & vinHashes, const Consensus::Params & params) const {
        if (!isValid(params))
            return false;
        // Check that the expected vin hash matches an expected vin prevout
        return vinHashes.count(vinhash) > 0;
    }

    /**
     * Sign the vote with the specified private key.
     * @param key
     * @return
     */
    bool sign(const CKey & key) {
        signature.clear();
        if (!key.SignCompact(sigHash(), signature))
            return false;
        return pubkey.RecoverCompact(sigHash(), signature);
    }

    /**
     * Marks the vote utxo as being spent.
     * @params block Height of the block spending the vote utxo.
     * @params txhash Hash of transaction spending the vote utxo.
     * @return
     */
    void spend(const int & block, const uint256 & txhash) {
        spentBlock = block;
        spentHash = txhash;
    }

    /**
     * Unspends the vote. Returns true if the vote was successfully
     * unspent, otherwise returns false.
     * @params block Height of the block that spent the vote utxo.
     * @params txhash Hash of transaction that spent the vote utxo.
     * @return
     */
    bool unspend(const int & block, const uint256 & txhash) {
        if (spentBlock == block && spentHash == txhash) {
            spentBlock = 0;
            spentHash.SetNull();
            return true;
        }
        return false;
    }

    /**
     * Marks the vote utxo as being spent.
     * @param block
     * @return
     */
    bool spent() const {
        return spentBlock > 0;
    }

    /**
     * Proposal hash
     * @return
     */
    const uint256 & getProposal() const {
        return proposal;
    }

    /**
     * Proposal vote
     * @return
     */
    VoteType getVote() const {
        return static_cast<VoteType>(vote);
    }

    /**
     * Proposal vote signature
     * @return
     */
    const std::vector<unsigned char> & getSignature() const {
        return signature;
    }

    /**
     * Proposal utxo containing the vote
     * @return
     */
    const COutPoint & getUtxo() const {
        return utxo;
    }

    /**
     * Vote's vin hash (truncated vin prevout spending this vote).
     * @return
     */
    const VinHash & getVinHash() const {
        return vinhash;
    }

    /**
     * Vote hash
     * @return
     */
    uint256 getHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << proposal << utxo; // exclude vote from hash to properly handle changing votes
        return ss.GetHash();
    }

    /**
     * Vote signature hash
     * @return
     */
    uint256 sigHash() const {
        CHashWriter ss(SER_GETHASH, 0);
        ss << version << type << proposal << vote << utxo << vinhash;
        return ss.GetHash();
    }

    /**
     * Get the pubkey associated with the vote's signature.
     * @return
     */
    const CPubKey & getPubKey() const {
        return pubkey;
    }

    /**
     * Get the COutPoint of the vote. This is the outpoint of the OP_RETURN data
     * in the "voting" transaction. This shouldn't be confused with the vote's
     * utxo (the unspent transaction output representing the vote).
     * @return
     */
    const COutPoint & getOutpoint() const {
        return outpoint;
    }

    /**
     * Get the time of the vote.
     * @return
     */
    const int64_t & getTime() const {
        return time;
    }

    /**
     * Get the amount associated with the vote.
     * @return
     */
    const CAmount & getAmount() const {
        return amount;
    }

    /**
     * Vote block number
     * @return
     */
    const int & getBlockNumber() const {
        return blockNumber;
    }

    /**
     * Return the public key id associated with the vote's utxo.
     * @return
     */
    const CKeyID & getKeyID() const {
        return keyid;
    }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(version);
        READWRITE(type);
        READWRITE(proposal);
        READWRITE(vote);
        READWRITE(utxo);
        READWRITE(vinhash);
        READWRITE(signature);
        if (ser_action.ForRead()) { // assign memory only fields
            pubkey.RecoverCompact(sigHash(), signature);
            loadKeyID();
        }
    }

protected:
    /**
     * Returns true if the unsigned char is a valid vote type enum.
     * @param voteType
     * @return
     */
    bool isValidVoteType(const uint8_t & voteType) const {
        return voteType >= NO && voteType <= ABSTAIN;
    }

    /**
     * Load the keyid and amount.
     */
    void loadKeyID() {
        CTransactionRef tx;
        if (GetKeyIDForUTXO(utxo, tx, keyid))
            amount = tx->vout[utxo.n].nValue;
    }

protected:
    uint8_t version{NETWORK_VERSION};
    uint8_t type{VOTE};
    uint256 proposal;
    uint8_t vote{ABSTAIN};
    VinHash vinhash;
    std::vector<unsigned char> signature;
    COutPoint utxo; // voting on behalf of this utxo

protected: // memory only
    CPubKey pubkey;
    COutPoint outpoint; // of vote's OP_RETURN outpoint
    int64_t time{0}; // block time of vote
    CAmount amount{0}; // of vote's utxo (this is not the OP_RETURN outpoint amount, which is 0)
    CKeyID keyid; // CKeyID of vote's utxo
    int blockNumber{0}; // block containing this vote
    int spentBlock{0}; // block where this vote's utxo was spent (which invalidates it)
    uint256 spentHash; // tx hash where this vote's utxo was spent (which invalidates it)
};

/**
 * Check that utxo isn't already spent
 * @param vote
 * @param mempoolCheck Will check the mempool for spent votes
 * @return
 */
static bool IsVoteSpent(const Vote & vote, const bool & mempoolCheck = false) {
    Coin coin;
    if (mempoolCheck) {
        LOCK2(cs_main, mempool.cs);
        CCoinsViewMemPool view(pcoinsTip.get(), mempool);
        if (!view.GetCoin(vote.getUtxo(), coin) || mempool.isSpent(vote.getUtxo()))
            return true;
    } else {
        LOCK(cs_main);
        if (!pcoinsTip->GetCoin(vote.getUtxo(), coin))
            return true;
    }
    return false;
}

/**
 * ProposalVote associates a proposal with a specific vote.
 */
struct ProposalVote {
    Proposal proposal;
    VoteType vote{ABSTAIN};
    explicit ProposalVote() = default;
    explicit ProposalVote(const Proposal & proposal, const VoteType & vote) : proposal(proposal), vote(vote) {}
};
/**
 * Way to obtain all votes for a specific proposal
 */
struct Tally {
    Tally() = default;
    CAmount cyes{0};
    CAmount cno{0};
    CAmount cabstain{0};
    int yes{0};
    int no{0};
    int abstain{0};
    double passing() const {
        return static_cast<double>(yes) / static_cast<double>(yes + no);
    }
    int netyes() const {
        return yes - no;
    }
};
/**
 * Hasher used with unordered_map
 */
struct Hasher {
    size_t operator()(const uint256 & hash) const { return ReadLE64(hash.begin()); }
};

/**
 * Manages related servicenode functions including handling network messages and storing an active list
 * of valid servicenodes.
 */
class Governance : public CValidationInterface {
public:
    explicit Governance() = default;

    /**
     * Returns true if the proposal with the specified name exists.
     * @param name
     * @return
     */
    bool hasProposal(const std::string & name, const int & superblock) {
        LOCK(mu);
        for (const auto & item : proposals) {
            if (item.second.getSuperblock() == superblock && item.second.getName() == name)
                return true;
        }
        return false;
    }

    /**
     * Returns true if the proposal with the specified hash exists.
     * @param hash
     * @return
     */
    bool hasProposal(const uint256 & hash) {
        LOCK(mu);
        return proposals.count(hash) > 0;
    }

    /**
     * Returns true if the proposal with the specified hash exists and that it exists
     * prior to the specified block.
     * @param hash
     * @param blockNumber
     * @return
     */
    bool hasProposal(const uint256 & hash, const int & blockNumber) {
        LOCK(mu);
        return proposals.count(hash) > 0 && proposals[hash].getBlockNumber() < blockNumber;
    }

    /**
     * Returns true if the vote with the specified hash exists.
     * @param hash
     * @return
     */
    bool hasVote(const uint256 & hash) {
        LOCK(mu);
        return votes.count(hash) > 0;
    }

    /**
     * Returns true if the specified proposal and utxo matches a known vote.
     * @param proposal
     * @param voteType
     * @param utxo
     * @return
     */
    bool hasVote(const uint256 & proposal, const VoteType & voteType, const COutPoint & utxo) {
        LOCK(mu);
        if (!proposals.count(proposal))
            return false; // no proposal

        const auto & prop = proposals[proposal];
        if (!sbvotes.count(prop.getSuperblock()))
            return false; // no superblock proposal

        const auto & vs = sbvotes[prop.getSuperblock()];
        for (const auto & item : vs) {
            const auto & vote = item.second;
            if (vote.getUtxo() == utxo && vote.getProposal() == proposal && vote.getVote() == voteType)
                return true;
        }
        return false;
    }

    /**
     * Resets the governance state.
     * @return
     */
    bool reset() {
        LOCK(mu);
        proposals.clear();
        votes.clear();
        sbvotes.clear();
        return true;
    }

    /**
     * Loads the governance data from the blockchain ledger. It's possible to optimize
     * this further by creating a separate leveldb for goverance data. Currently, this
     * method will read every block on the chain and search for goverance data.
     * @return
     */
    bool loadGovernanceData(const CChain & chain, CCriticalSection & chainMutex,
                            const Consensus::Params & consensus, std::string & failReasonRet, const int & nthreads=0)
    {
        int blockHeight{0};
        {
            LOCK(chainMutex);
            blockHeight = chain.Height();
        }
        // No need to load any governance data if we on the genesis block
        // or if the governance system hasn't been enabled yet.
        if (blockHeight == 0 || blockHeight < consensus.governanceBlock)
            return true;

        boost::thread_group tg;
        Mutex mut; // manage access to shared data
        const auto cores = nthreads == 0 ? GetNumCores() : nthreads;
        std::map<COutPoint, std::pair<uint256, int>> spentPrevouts; // pair<txhash, blockheight>
        bool useThreadGroup{false};

        // Shard the blocks into num equivalent to available cores
        const int totalBlocks = blockHeight - consensus.governanceBlock;
        int slice = totalBlocks / cores;
        bool failed{false};

        auto p1 = [&spentPrevouts,&failed,&failReasonRet,&chain,&chainMutex,&mut,this]
                  (const int start, const int end, const Consensus::Params & consensus) -> bool
        {
            for (int blockNumber = start; blockNumber < end; ++blockNumber) {
                if (ShutdownRequested()) { // don't hold up shutdown requests
                    LOCK(mut);
                    failed = true;
                    return false;
                }

                CBlockIndex *blockIndex;
                {
                    LOCK(chainMutex);
                    blockIndex = chain[blockNumber];
                }
                if (!blockIndex) {
                    LOCK(mut);
                    failed = true;
                    failReasonRet += strprintf("Failed to read block index for block %d\n", blockNumber);
                    return false;
                }

                CBlock block;
                if (!ReadBlockFromDisk(block, blockIndex, consensus)) {
                    LOCK(mut);
                    failed = true;
                    failReasonRet += strprintf("Failed to read block from disk for block %d\n", blockNumber);
                    return false;
                }
                // Store all vins in order to use as a lookup for spent votes
                for (const auto & tx : block.vtx) {
                    LOCK(mut);
                    for (const auto & vin : tx->vin)
                        spentPrevouts[vin.prevout] = {tx->GetHash(), blockIndex->nHeight};
                }
                // Process block
                processBlock(&block, blockIndex, consensus, false);
            }
            return true;
        };

        for (int k = 0; k < cores; ++k) {
            const int start = consensus.governanceBlock + k*slice;
            const int end = k == cores-1 ? blockHeight+1 // check bounds, +1 due to "<" logic below, ensure inclusion of last block
                                         : start+slice;
            // try single threaded on failure
            try {
                if (cores > 1) {
                    tg.create_thread([start,end,consensus,&p1] {
                        RenameThread("blocknet-governance");
                        p1(start, end, consensus);
                    });
                    useThreadGroup = true;
                } else
                    p1(start, end, consensus);
            } catch (...) {
                try {
                    p1(start, end, consensus);
                } catch (std::exception & e) {
                    failed = true;
                    failReasonRet += strprintf("Failed to create thread to load governance data: %s\n", e.what());
                    return false; // fatal error
                }
            }
        }

        // Wait for all threads to complete
        if (useThreadGroup)
            tg.join_all();

        {
            LOCK(mu);
            if (votes.empty() || failed)
                return !failed;
        }

        // Now that all votes are loaded, check and remove any invalid ones.
        // Invalid votes can be evaluated using multiple threads since we
        // have the complete dataset in memory. Below the votes are sliced
        // up into shards and each available thread works on its own shard.
        std::vector<std::pair<uint256, Vote>> tmpvotes;
        {
            LOCK(mu);
            tmpvotes.reserve(votes.size());
            std::copy(votes.begin(), votes.end(), std::back_inserter(tmpvotes));
        }

        auto p2 = [&tmpvotes,&spentPrevouts,&failed,&mut,this](const int start, const int end,
                const Consensus::Params & consensus) -> bool
        {
            for (int i = start; i < end; ++i) {
                if (ShutdownRequested()) { // don't hold up shutdown requests
                    LOCK(mut);
                    failed = true;
                    return false;
                }
                Vote vote;
                {
                    LOCK(mut);
                    vote = tmpvotes[i].second;
                }
                // Record vote if it has an associated proposal
                if (hasProposal(vote.getProposal(), vote.getBlockNumber())) {
                    // Mark vote as spent if its utxo is spent before or on the
                    // associated proposal's superblock.
                    {
                        LOCK(mut);
                        if (spentPrevouts.count(vote.getUtxo()) && spentPrevouts[vote.getUtxo()].second <= getProposal(vote.getProposal()).getSuperblock())
                            vote.spend(spentPrevouts[vote.getUtxo()].second, spentPrevouts[vote.getUtxo()].first);
                    }
                    {
                        LOCK(mu);
                        addVote(vote);
                    }
                }
            }
            return true;
        };

        useThreadGroup = false; // initial state

        slice = static_cast<int>(tmpvotes.size()) / cores;
        for (int k = 0; k < cores; ++k) {
            const int start = k*slice;
            const int end = k == cores-1 ? static_cast<int>(tmpvotes.size())
                                         : start+slice;
            // try single threaded on failure
            try {
                if (cores > 1) {
                    tg.create_thread([start,end,consensus,&p2] {
                        RenameThread("blocknet-governance");
                        p2(start, end, consensus);
                    });
                    useThreadGroup = true;
                } else
                    p2(start, end, consensus);
            } catch (...) {
                try {
                    p2(start, end, consensus);
                } catch (std::exception & e) {
                    failed = true;
                    failReasonRet += strprintf("Failed to create thread to load governance votes: %s\n", e.what());
                    return false; // fatal error
                }
            }
        }
        // Wait for all threads to complete
        if (useThreadGroup)
            tg.join_all();

        return !failed;
    }

    /**
     * Fetch the specified proposal.
     * @param hash Proposal hash
     * @return
     */
    Proposal getProposal(const uint256 & hash) {
        LOCK(mu);
        if (proposals.count(hash) > 0)
            return proposals[hash];
        return Proposal{};
    }

    /**
     * Fetch the specified vote by its hash.
     * @param hash Vote hash
     * @return
     */
    Vote getVote(const uint256 & hash) {
        LOCK(mu);
        if (votes.count(hash) > 0)
            return votes[hash];
        return Vote{};
    }

    /**
     * Fetch the list of all known proposals.
     * @return
     */
    std::vector<Proposal> getProposals() {
        LOCK(mu);
        std::vector<Proposal> props;
        props.reserve(proposals.size());
        for (const auto & item : proposals)
            props.push_back(item.second);
        return props;
    }

    /**
     * Fetch the list of all known proposals in the specified superblock.
     * @param superblock Block number of the superblock
     * @return
     */
    std::vector<Proposal> getProposals(const int & superblock) {
        LOCK(mu);
        std::vector<Proposal> props;
        for (const auto & item : proposals) {
            if (item.second.getSuperblock() == superblock)
                props.push_back(item.second);
        }
        return props;
    }

    /**
     * Fetch the list of all known proposals who's superblocks are ahead of the specified block.
     * @param since Block number to start search from
     * @return
     */
    std::vector<Proposal> getProposalsSince(const int & since) {
        LOCK(mu);
        std::vector<Proposal> props;
        for (const auto & item : proposals) {
            if (item.second.getSuperblock() >= since)
                props.push_back(item.second);
        }
        return props;
    }

    /**
     * Return copy of all votes.
     * @return
     */
    std::unordered_map<uint256, Vote, Hasher> copyVotes() {
        LOCK(mu);
        return votes;
    }

    /**
     * Return copy of all proposals.
     * @return
     */
    std::unordered_map<uint256, Proposal, Hasher> copyProposals() {
        LOCK(mu);
        return proposals;
    }

    /**
     * Fetch the list of all known votes that haven't been spent.
     * @return
     */
    std::vector<Vote> getVotes() {
        LOCK(mu);
        std::vector<Vote> vos;
        for (const auto & item : votes) {
            if (!item.second.spent())
                vos.push_back(item.second);
        }
        return vos;
    }

    /**
     * Fetch all votes for the specified proposal that haven't been spent.
     * @param proposalHash Proposal hash
     * @return
     */
    std::vector<Vote> getVotes(const uint256 & proposalHash) {
        LOCK(mu);
        std::vector<Vote> vos;
        if (!proposals.count(proposalHash))
            return vos;

        const auto & proposal = proposals[proposalHash];
        if (!sbvotes.count(proposal.getSuperblock()))
            return vos;

        const auto & vs = sbvotes[proposal.getSuperblock()];
        for (const auto & item : vs) {
            if (item.second.getProposal() == proposalHash && !item.second.spent())
                vos.push_back(item.second);
        }
        return vos;
    }

    /**
     * Fetch all votes for the specified proposal that haven't been spent.
     * @param proposalHash Proposal hash
     * @return
     */
    std::vector<Vote*> getMutableSBVotes(const uint256 & proposalHash) {
        LOCK(mu);
        std::vector<Vote*> vos;
        if (!proposals.count(proposalHash))
            return vos;

        const auto & proposal = proposals[proposalHash];
        if (!sbvotes.count(proposal.getSuperblock()))
            return vos;

        auto & vs = sbvotes[proposal.getSuperblock()];
        for (auto & item : vs) {
            if (item.second.getProposal() == proposalHash && !item.second.spent())
                vos.push_back(&item.second);
        }
        return vos;
    }

    /**
     * Fetch all votes in the specified superblock that haven't been spent.
     * @param superblock Block number of superblock
     * @return
     */
    std::vector<Vote> getVotes(const int & superblock) {
        LOCK(mu);
        std::vector<Vote> vos;
        if (!sbvotes.count(superblock))
            return vos;

        const auto & vs = sbvotes[superblock];
        for (const auto & item : vs) {
            if (!item.second.spent())
                vos.push_back(item.second);
        }
        return vos;
    }

    /**
     * Spends the vote and ensures other data providers are synced. If the specified vote
     * is associated with a superblock that's prior to the block number, the vote is not
     * marked spent.
     * @param vote
     * @param block Block number
     * @param txhash prevout of spent vote
     */
    void spendVote(Vote *vote, const int & block, const uint256 & txhash) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (!proposals.count(vote->getProposal()))
            return;

        if (block > proposals[vote->getProposal()].getSuperblock())
            return; // do not spend a vote on a block that's after the vote's superblock

        // Spend votes across data providers
        vote->spend(block, txhash);
        auto & vv = votes[vote->getHash()];
        vv.spend(block, txhash);
    }

    /**
     * Spends the vote and ensures other data providers are synced. If the specified vote
     * is associated with a superblock that's prior to the block number, the vote is not
     * marked spent.
     * @param vote
     * @param block Block number
     * @param txhash prevout of spent vote
     */
    void spendVote(Vote & vote, const int & block, const uint256 & txhash) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (!proposals.count(vote.getProposal()))
            return;

        if (block > proposals[vote.getProposal()].getSuperblock())
            return; // do not spend a vote on a block that's after the vote's superblock

        // Update current vote
        vote.spend(block, txhash);
        // Update sbvotes data provider
        if (sbvotes.count(proposals[vote.getProposal()].getSuperblock())) {
            auto & mv = sbvotes[proposals[vote.getProposal()].getSuperblock()];
            const auto & voteHash = vote.getHash();
            if (mv.count(voteHash)) {
                auto & v = mv[voteHash];
                v.spend(block, txhash);
            }
        }
    }

    /**
     * Unspend the vote and ensures other data providers are updated. Only unspends the vote
     * if the block number is prior to the vote's associated superblock.
     * @param vote
     * @param block Block number
     * @param txhash prevout of spent vote
     */
    void unspendVote(Vote *vote, const int & block, const uint256 & txhash) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (!proposals.count(vote->getProposal()))
            return;
        if (block > proposals[vote->getProposal()].getSuperblock())
            return; // do not unspend votes who's superblocks are after the specified block

        // Update current vote
        vote->unspend(block, txhash);
        // Update sbvotes data provider
        const auto & voteHash = vote->getHash();
        if (votes.count(voteHash)) {
            auto & v = votes[voteHash];
            v.unspend(block, txhash);
        }
    }

    /**
     * Unspend the vote and ensures other data providers are updated. Only unspends the vote
     * if the block number is prior to the vote's associated superblock.
     * @param vote
     * @param block Block number
     * @param txhash prevout of spent vote
     */
    void unspendVote(Vote & vote, const int & block, const uint256 & txhash) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (!proposals.count(vote.getProposal()))
            return;
        if (block > proposals[vote.getProposal()].getSuperblock())
            return; // do not unspend votes who's superblocks are after the specified block

        // Update current vote
        vote.unspend(block, txhash);
        // Update sbvotes data provider
        if (sbvotes.count(proposals[vote.getProposal()].getSuperblock())) {
            auto & mv = sbvotes[proposals[vote.getProposal()].getSuperblock()];
            const auto & voteHash = vote.getHash();
            if (mv.count(voteHash)) {
                auto & v = mv[voteHash];
                v.unspend(block, txhash);
            }
        }
    }

    /**
     * Obtains all votes and proposals from the specified block.
     * @param block
     * @param proposalsRet
     * @param votesRet
     * @param blockIndex
     * @param checkProposal If false, disables the proposal check
     * @return
     */
    void dataFromBlock(const CBlock *block, std::set<Proposal> & proposalsRet, std::set<Vote> & votesRet,
            const Consensus::Params & params, const CBlockIndex *blockIndex=nullptr, const bool checkProposal = true)
    {
        for (const auto & tx : block->vtx) {
            if (tx->IsCoinBase())
                continue;
            std::set<VinHash> vinHashes;
            for (int n = 0; n < static_cast<int>(tx->vout.size()); ++n) {
                const auto & out = tx->vout[n];
                if (out.scriptPubKey[0] != OP_RETURN)
                    continue; // no proposal data
                CScript::const_iterator pc = out.scriptPubKey.begin();
                std::vector<unsigned char> data;
                opcodetype opcode{OP_FALSE};
                bool checkdata{false};
                while (pc < out.scriptPubKey.end()) {
                    opcode = OP_FALSE;
                    if (!out.scriptPubKey.GetOp(pc, opcode, data))
                        break;
                    checkdata = (opcode == OP_PUSHDATA1 || opcode == OP_PUSHDATA2 || opcode == OP_PUSHDATA4)
                            || (opcode < OP_PUSHDATA1 && opcode == data.size());
                    if (checkdata && !data.empty())
                        break;
                }
                if (!checkdata || data.empty())
                    continue; // skip if no data

                CDataStream ss(data, SER_NETWORK, GOV_PROTOCOL_VERSION);
                NetworkObject obj; ss >> obj;
                if (!obj.isValid())
                    continue; // must match expected version

                if (obj.getType() == PROPOSAL) {
                    CDataStream ss2(data, SER_NETWORK, GOV_PROTOCOL_VERSION);
                    Proposal proposal(blockIndex ? blockIndex->nHeight : 0); ss2 >> proposal;
                    // Skip the cutoff check if block index is not specified
                    if (proposal.isValid(params) && (!blockIndex || outsideProposalCutoff(proposal, blockIndex->nHeight, params)))
                        proposalsRet.insert(proposal);
                } else if (obj.getType() == VOTE) {
                    if (vinHashes.empty()) { // initialize vin hashes
                        for (const auto & vin : tx->vin) {
                            const auto & vhash = makeVinHash(vin.prevout);
                            vinHashes.insert(vhash);
                        }
                    }
                    CDataStream ss2(data, SER_NETWORK, GOV_PROTOCOL_VERSION);
                    Vote vote({tx->GetHash(), static_cast<uint32_t>(n)}, block->GetBlockTime(), blockIndex ? blockIndex->nHeight : 0);
                    ss2 >> vote;
                    // Check that the vote is associated with a valid proposal and
                    // the vote is valid and that it also meets the cutoff requirements.
                    // A valid proposal for this vote must exist in a previous block
                    // otherwise the vote is discarded.
                    if ((blockIndex && checkProposal && !hasProposal(vote.getProposal(), blockIndex->nHeight))
                        || !vote.isValid(vinHashes, params)
                        || (blockIndex && !outsideVotingCutoff(getProposal(vote.getProposal()), blockIndex->nHeight, params)))
                        continue;
                    // Handle vote changes, if a vote already exists and the user
                    // is submitting a change, only count the vote with the most
                    // recent timestamp. If a vote on the same utxo occurs in the
                    // same block, the vote with the larger hash is chosen as the
                    // tie breaker. This could have unintended consequences if the
                    // user intends the smaller hash to be the most recent vote.
                    // The best way to handle this is to build the voting client
                    // to require waiting at least 1 block between vote changes.
                    // Changes to this logic below must also be applied to "BlockConnected()"
                    if (votesRet.count(vote)) {
                        // Assumed that all votes in the same block have the same "time"
                        auto it = votesRet.find(vote);
                        if (UintToArith256(vote.sigHash()) > UintToArith256(it->sigHash()))
                            votesRet.insert(std::move(vote));
                    } else // if no vote exists then add
                        votesRet.insert(std::move(vote));
                }
            }
        }
    }

    /**
     * Return the superblock results for all the proposals scheduled for the specified superblock.
     * @param superblock
     * @param params
     * @return
     */
    std::map<Proposal, Tally> getSuperblockResults(const int & superblock, const Consensus::Params & params) {
        std::map<Proposal, Tally> r;
        if (!isSuperblock(superblock, params))
            return r;

        std::set<COutPoint> unique;
        std::vector<Proposal> ps;
        std::vector<Vote> vs;
        getProposalsForSuperblock(superblock, ps, vs);

        CAmount uniqueAmount{0};
        for (const auto & vote : vs) { // count all the unique voting utxos
            if (unique.count(vote.getUtxo()))
                continue;
            unique.insert(vote.getUtxo());
            uniqueAmount += vote.getAmount();
        }
        const auto uniqueVotes = static_cast<int>(uniqueAmount / params.voteBalance);

        for (const auto & proposal : ps) // get results for each proposal
            r[proposal] = getTally(proposal.getHash(), vs, params);

        // a) Exclude proposals that don't have the required yes votes.
        //    60% of votes must be "yes" on a passing proposal.
        // b) Exclude proposals that don't have at least 25% of all participating
        //    votes. i.e. at least 25% of all votes cast this superblock must have
        //    voted on this proposal.
        // c) Exclude proposals with 0 yes votes in all circumstances
        for (auto it = r.cbegin(); it != r.cend(); ) {
            const auto & tally = it->second;
            const int total = tally.yes+tally.no+tally.abstain;
            const int yaynay = tally.yes + tally.no;
            if (yaynay == 0 || static_cast<double>(tally.yes) / static_cast<double>(yaynay) < 0.6
              || static_cast<double>(total) < static_cast<double>(uniqueVotes) * 0.25
              || tally.yes <= 0)
                r.erase(it++);
            else
                ++it;
        }

        return r;
    }

    /**
     * Fetch the list of proposals scheduled for the specified superblock. Requires loadGovernanceData to have been run
     * on chain load.
     * @param superblock
     * @param allProposals
     * @param allVotes Votes specific to the selected proposals.
     */
    void getProposalsForSuperblock(const int & superblock, std::vector<Proposal> & allProposals, std::vector<Vote> & allVotes) {
        auto ps = getProposals(superblock);
        auto vs = getVotes(superblock);
        std::set<uint256> proposalHashes;
        for (const auto & p : ps) {
            if (p.getSuperblock() == superblock) {
                allProposals.push_back(p);
                proposalHashes.insert(p.getHash());
            }
        }
        // Find all votes associated with the selected proposals
        for (const auto & v : vs) {
            if (proposalHashes.count(v.getProposal()))
                allVotes.push_back(v);
        }
    }

    /**
     * Returns true if the specified block is a valid superblock, including matching the expected proposal
     * payouts to the superblock payees.
     * @param block
     * @param blockHeight
     * @param params
     * @param paymentsRet Total superblock payment
     * @return
     */
    bool isValidSuperblock(const CBlock *block, const int & blockHeight, const Consensus::Params & params, CAmount & paymentsRet)
    {
        if (!isSuperblock(blockHeight, params))
            return false;

        // Superblock payout must be in the coinstake
        if (!block->IsProofOfStake())
            return false;

        // Get the results and sort descending by passing percent.
        // We want to sort descending because the most valuable
        // proposals are those with the highest passing percentage,
        // in this case we want them at the beginning of the list.
        const auto & results = getSuperblockResults(blockHeight, params);
        if (results.empty())
            return true;

        auto payees = getSuperblockPayees(blockHeight, results, params);
        if (payees.empty())
            return false;

        // Add up the total expected superblock payment
        paymentsRet = 0;
        for (const auto & payee : payees)
            paymentsRet += payee.nValue;

        auto vouts = block->vtx[1]->vout;
        if (static_cast<int>(vouts.size()) - static_cast<int>(payees.size()) > 2) // allow 1 vout for coinbase and 1 vout for the staker's payment
            return false;

        vouts.erase(std::remove_if(vouts.begin(), vouts.end(),
            [&payees](const CTxOut & vout) {
                bool found{false};
                for (int i = (int)payees.size()-1; i >= 0; --i) {
                    if (vout == payees[i]) {
                        found = true;
                        payees.erase(payees.begin()+i);
                        break;
                    }
                }
                return found;
            }), vouts.end());

        // The superblock payment is valid if all payees
        // are accounted for.
        return vouts.size() <= 2 && payees.empty();
    }

    /**
     * Returns true if the specified utxo exists in an active and validd proposal who's voting period has ended.
     * @param utxo
     * @param tipHeight
     * @param params
     * @return
     */
    bool utxoInVoteCutoff(const COutPoint & utxo, const int & tipHeight, const Consensus::Params & params) {
        const auto superblock = NextSuperblock(params, tipHeight);
        if (!insideVoteCutoff(superblock, tipHeight, params))
            return false; // if tip isn't in the non-voting period then return

        // Check if the utxo is in a valid proposal who's voting period has ended
        std::vector<Proposal> sproposals;
        std::vector<Vote> svotes;
        getProposalsForSuperblock(superblock, sproposals, svotes);

        for (const auto & vote : svotes) {
            if (utxo == vote.getUtxo())
                return true;
        }

        return false;
    }

public: // static

    /**
     * Singleton instance.
     * @return
     */
    static Governance & instance() {
        static Governance gov;
        return gov;
    }

    /**
     * Returns the upcoming superblock.
     * @param params
     * @return
     */
    static int nextSuperblock(const Consensus::Params & params, const int fromBlock = 0) {
        return NextSuperblock(params, fromBlock);
    }

    /**
     * If the vote's pubkey matches the specified vin's pubkey returns true, otherwise
     * returns false.
     * @param vote
     * @param vin
     * @return
     */
    static bool matchesVinPubKey(const Vote & vote, const CTxIn & vin) {
        CScript::const_iterator pc = vin.scriptSig.begin();
        std::vector<unsigned char> data;
        bool isPubkey{false};
        while (pc < vin.scriptSig.end()) {
            opcodetype opcode;
            if (!vin.scriptSig.GetOp(pc, opcode, data))
                break;
            if (data.size() == CPubKey::PUBLIC_KEY_SIZE || data.size() == CPubKey::COMPRESSED_PUBLIC_KEY_SIZE) {
                isPubkey = true;
                break;
            }
        }

        if (!isPubkey)
            return false; // skip, no match

        CPubKey pubkey(data);
        return pubkey.GetID() == vote.getPubKey().GetID();
    }

    /**
     * Returns true if the specified block height is the superblock.
     * @param blockHeight
     * @param params
     * @return
     */
    static bool isSuperblock(const int & blockHeight, const Consensus::Params & params) {
        return blockHeight >= params.governanceBlock && blockHeight % params.superblock == 0;
    }

    /**
     * Returns true if a vote is found in the specified transaction output.
     * @param out
     * @param vote
     * @return
     */
    static bool isVoteInTxOut(const CTxOut & out, Vote & vote) {
        if (out.scriptPubKey[0] != OP_RETURN)
            return false;
        CScript::const_iterator pc = out.scriptPubKey.begin();
        std::vector<unsigned char> data;
        while (pc < out.scriptPubKey.end()) {
            opcodetype opcode;
            if (!out.scriptPubKey.GetOp(pc, opcode, data))
                break;
            if (!data.empty())
                break;
        }
        CDataStream ss(data, SER_NETWORK, GOV_PROTOCOL_VERSION);
        NetworkObject obj; ss >> obj;
        if (obj.getType() == VOTE) {
            CDataStream ss2(data, SER_NETWORK, GOV_PROTOCOL_VERSION);
            ss2 >> vote;
            return true;
        }
        return false;
    }

    /**
     * Returns true if the proposal is not yet in the cutoff period. Make sure mutex (mu) is not held.
     * @param proposal
     * @param blockNumber
     * @param params
     * @return
     */
    static bool outsideProposalCutoff(const Proposal & proposal, const int & blockNumber, const Consensus::Params & params) {
        if (proposal.isNull()) // check if valid
            return false;
        // Proposals can happen multiple superblocks in advance if a proposal
        // is created for a future superblock. As a result, a proposal meets
        // the cutoff if it's included in a block that's prior to its scheduled
        // superblock.
        return blockNumber < proposal.getSuperblock() - params.proposalCutoff;
    }

    /**
     * Returns true if the vote is not yet in the cutoff period. Make sure mutex (mu) is not held.
     * @param proposal
     * @param blockNumber
     * @param params
     * @return
     */
    static bool outsideVotingCutoff(const Proposal & proposal, const int & blockNumber, const Consensus::Params & params) {
        if (proposal.isNull()) // check if valid
            return false;
        // Votes can happen multiple superblocks in advance if a proposal is
        // created for a future superblock. As a result, a vote meets the
        // cutoff for a block number that's prior to the superblock of its
        // associated proposal.
        return blockNumber < proposal.getSuperblock() - params.votingCutoff;
    }

    /**
     * Returns true if the block number is in the vote cutoff. The vote cutoff is considered 1 block
     * prior to the protocol's cutoff since at least 1 block is required to confirm.
     * @param superblock
     * @param blockNumber
     * @param params
     * @return
     */
    static bool insideVoteCutoff(const int & superblock, const int & blockNumber, const Consensus::Params & params) {
        return blockNumber >= superblock - params.votingCutoff && blockNumber <= superblock;
    }

    /**
     * Returns the vote tally for the specified proposal.
     * @param proposal
     * @param votes Vote array to search
     * @param params
     * @return
     */
    static Tally getTally(const uint256 & proposal, const std::vector<Vote> & votes, const Consensus::Params & params) {
        // Organize votes by tx hash to designate common votes (from same user)
        // We can assume all the votes in the same tx are associated with the
        // same user (i.e. all privkeys in the votes are known by the tx signer)
        std::map<uint256, std::set<Vote>> userVotes;
        // Cross reference all votes associated with a destination. If a vote
        // is associated with a common destination we can assume the same user
        // casted the vote. All votes in the tx imply the same user and all
        // votes associated with the same destination imply the same user.
        std::map<CTxDestination, std::set<Vote>> userVotesDest;

        std::vector<Vote> proposalVotes = votes;
        // remove all votes that don't match the proposal
        proposalVotes.erase(std::remove_if(proposalVotes.begin(), proposalVotes.end(), [&proposal](const Vote & vote) -> bool {
            return proposal != vote.getProposal();
        }), proposalVotes.end());

        // Prep our search containers
        for (const auto & vote : proposalVotes) {
            std::set<Vote> & v = userVotes[vote.getOutpoint().hash];
            v.insert(vote);
            userVotes[vote.getOutpoint().hash] = v;

            std::set<Vote> & v2 = userVotesDest[CTxDestination{vote.getPubKey().GetID()}];
            v2.insert(vote);
            userVotesDest[CTxDestination{vote.getPubKey().GetID()}] = v2;
        }

        // Iterate over all transactions and associated votes. In order to
        // prevent counting too many votes we need to tally up votes
        // across users separately and only count up their respective
        // votes in lieu of the maximum vote balance requirements.
        std::set<Vote> counted; // track counted votes
        std::vector<Tally> tallies;
        for (const auto & item : userVotes) {
            // First count all unique votes associated with the same tx.
            // This indicates they're all likely from the same user or
            // group of users pooling votes.
            std::set<Vote> allUnique;
            allUnique.insert(item.second.begin(), item.second.end());
            for (const auto & vote : item.second) {
                // Add all unique votes associated with the same destination.
                // Since we're first iterating over all the votes in the
                // same tx, and then over the votes based on common destination
                // we're able to get all the votes associated with a user.
                // The only exception is if a user votes from different wallets
                // and doesn't reveal the connection by combining into the same
                // tx. As a result, there's an optimal way to cast votes and that
                // should be taken into consideration on the voting client.
                const auto & destVotes = userVotesDest[CTxDestination{vote.getPubKey().GetID()}];
                allUnique.insert(destVotes.begin(), destVotes.end());
            }

            // Prevent counting votes more than once
            for (auto it = allUnique.begin(); it != allUnique.end(); ) {
                if (counted.count(*it) > 0)
                    allUnique.erase(it++);
                else ++it;
            }

            if (allUnique.empty())
                continue; // nothing to count
            counted.insert(allUnique.begin(), allUnique.end());

            Tally tally;
            for (const auto & vote : allUnique)  {
                if (vote.getVote() == gov::YES)
                    tally.cyes += vote.getAmount();
                else if (vote.getVote() == gov::NO)
                    tally.cno += vote.getAmount();
                else if (vote.getVote() == gov::ABSTAIN)
                    tally.cabstain += vote.getAmount();
            }
            tally.yes = static_cast<int>(tally.cyes / params.voteBalance);
            tally.no = static_cast<int>(tally.cno / params.voteBalance);
            tally.abstain = static_cast<int>(tally.cabstain / params.voteBalance);
            // Sanity checks
            if (tally.yes < 0)
                tally.yes = 0;
            if (tally.no < 0)
                tally.no = 0;
            if (tally.abstain < 0)
                tally.abstain = 0;
            tallies.push_back(tally);
        }

        // Tally all votes across all users that voted on this proposal
        Tally finalTally;
        for (const auto & tally : tallies) {
            finalTally.yes += tally.yes;
            finalTally.no += tally.no;
            finalTally.abstain += tally.abstain;
            finalTally.cyes += tally.cyes;
            finalTally.cno += tally.cno;
            finalTally.cabstain += tally.cabstain;
        }
        return finalTally;
    }

    /**
     * List the expected superblock payees for the specified result set.
     * @param superblock
     * @param results
     * @param params
     * @return
     */
    static std::vector<CTxOut> getSuperblockPayees(const int & superblock, const std::map<Proposal, Tally> & results, const Consensus::Params & params) {
        std::vector<CTxOut> r;
        if (results.empty())
            return r;

        // Superblock payees are sorted in the following manner:
        // 1) Net "yes" votes
        // 2) if tied then by most "yes" votes
        // 3) if still tied then by block height proposal was created
        // 4) if still tied the code will probably self destruct
        std::vector<std::pair<Proposal, Tally>> props(results.begin(), results.end());
        std::sort(props.begin(), props.end(),
            [](const std::pair<Proposal, Tally> & a, const std::pair<Proposal, Tally> & b) {
                if (a.second.netyes() == b.second.netyes() && a.second.yes == b.second.yes)
                    return a.first.getBlockNumber() < b.first.getBlockNumber(); // proposal submission block number as tie breaker
                else if (a.second.netyes() == b.second.netyes())
                    return a.second.yes > b.second.yes; // use "yes" percent as tie breaker
                else
                    return a.second.netyes() > b.second.netyes(); // sort net yes votes descending
            });

        // Fill as many proposals into the payee list as possible.
        // Proposals that do not fit are skipped and the other
        // remaining proposals are filled in its place.
        std::map<CTxDestination, CAmount> payees;
        CAmount superblockTotal = std::min(params.proposalMaxAmount, params.GetBlockSubsidy(superblock, params));
        do {
            // Add the payee if the requested amount fits
            // in the superblock.
            const auto & result = props[0];
            if (superblockTotal - result.first.getAmount() >= 0) {
                superblockTotal -= result.first.getAmount();
                r.emplace_back(result.first.getAmount(), GetScriptForDestination(DecodeDestination(result.first.getAddress())));
            }
            // done with this result
            props.erase(props.begin());
        } while (!props.empty());

        return r;
    }

protected:
    void BlockConnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex,
                        const std::vector<CTransactionRef>& txn_conflicted) override
    {
        processBlock(block.get(), pindex, Params().GetConsensus());
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block) override {
        std::set<Proposal> ps;
        std::set<Vote> vs;
        dataFromBlock(block.get(), ps, vs, Params().GetConsensus()); // cutoff check disabled here b/c we're disconnecting
                                                                     // already validated votes/proposals
        // By default use max int for block height in case no
        // index is found we don't want to mark votes as unspent
        // if an accurate spent height can't be verified.
        const auto maxInt = std::numeric_limits<int>::max();
        int blockHeight{maxInt};
        {
            LOCK(cs_main);
            const auto pindex = LookupBlockIndex(block->GetHash());
            blockHeight = pindex->nHeight;
        }

        {
            LOCK(mu);
            for (auto & vote : vs) {
                const auto & voteHash = vote.getHash();
                if (!votes.count(voteHash))
                    continue;
                const auto & stvote = votes[voteHash];
                if (stvote.getBlockNumber() == blockHeight)
                    removeVote(vote);
            }
            // Remove proposals after votes because vote removal depends on an existing proposal
            for (auto & proposal : ps) {
                if (!proposals.count(proposal.getHash()))
                    continue;
                const auto & stprop = proposals[proposal.getHash()];
                if (stprop.getBlockNumber() == blockHeight)
                    removeProposal(proposal);
            }

        }

        if (blockHeight == maxInt)
            return; // do not unspend votes if block height is undefined

        // Unspend any vote utxos that were spent by this
        // block. Only unspend those votes where the block
        // index that tried to spend them was prior to
        // the proposal's superblock.
        std::map<COutPoint, uint256> prevouts; // map<outpoint, txhash>
        for (const auto & tx : block->vtx) {
            for (const auto & vin : tx->vin)
                prevouts[vin.prevout] = tx->GetHash();
        }

        // Get a list of all proposals with a superblock that is on or
        // after the current block index.
        auto sprops = getProposalsSince(blockHeight);
        // Obtain all votes for these proposals
        std::vector<Vote*> svotes;
        for (const auto & p : sprops) {
            auto s = getMutableSBVotes(p.getHash());
            svotes.insert(svotes.end(), s.begin(), s.end());
        }

        {
            LOCK(mu);
            for (auto & v : svotes) {
                if (!prevouts.count(v->getUtxo()))
                    continue;
                // Unspend this vote if it was spent in this block
                unspendVote(v, blockHeight, prevouts[v->getUtxo()]);
            }
        }
    }

    /**
     * Processes governance data from the specified block and index. If skipProposalCheckOnVotes
     * is specified, the vote data will be process regardless of whether a proposal exists
     * for that vote. Likewise, the vote spent check will be disabled.
     * @param block
     * @param pindex
     * @param processingChainTip
     */
    void processBlock(const CBlock *block, const CBlockIndex *pindex, const Consensus::Params & params, const bool processingChainTip = true) {
        std::set<Proposal> ps;
        std::set<Vote> vs;
        dataFromBlock(block, ps, vs, params, pindex, processingChainTip);
        {
            LOCK(mu);
            // Insert proposals first because vote insert requires an existing proposal
            for (auto & proposal : ps) {
                // Do not allow proposals with the same parameters to replace
                // existing proposals.
                addProposal(proposal);
            }
            for (auto & vote : vs) {
                if (processingChainTip && !proposals.count(vote.getProposal()))
                    continue; // skip votes without valid proposals
                // Handle vote changes, if a vote already exists and the user
                // is submitting a change, only count the vote with the most
                // recent timestamp. If a vote on the same utxo occurs in the
                // same block, the vote with the larger hash is chosen as the
                // tie breaker. This could have unintended consequences if the
                // user intends the smaller hash to be the most recent vote.
                // The best way to handle this is to build the voting client
                // to require waiting at least 1 block between vote changes.
                // Changes to this code below must also be applied to "dataFromBlock()"
                const auto voteHash = vote.getHash();
                if (votes.count(voteHash)) {
                    if (vote.getTime() > votes[voteHash].getTime())
                        addVote(vote); // overwrite existing vote
                    else if (UintToArith256(vote.sigHash()) > UintToArith256(votes[voteHash].sigHash()))
                        addVote(vote); // overwrite existing vote
                } else {
                    // Only check the mempool and coincache for spent utxos if
                    // we're currently processing the chain tip.
                    LEAVE_CRITICAL_SECTION(mu);
                    bool spent = processingChainTip && IsVoteSpent(vote, false); // check that utxo is unspent
                    ENTER_CRITICAL_SECTION(mu);
                    if (spent)
                        continue;
                    addVote(vote); // insert new vote
                }
            }

            if (!processingChainTip || votes.empty()) // if proposal check is disabled, return
                return;
        }

        // Mark votes as spent, i.e. any votes that have had their
        // utxos spent in this block. We'll store all the vin prevouts
        // and then check any votes that share those utxos to determine
        // if they've been spent. Only mark votes as spent if the vote's
        // utxo is spent before the proposal expires (on its superblock).
        std::map<COutPoint, uint256> prevouts; // map<outpoint, txhash>
        for (const auto & tx : block->vtx) {
            for (const auto & vin : tx->vin)
                prevouts[vin.prevout] = tx->GetHash();
        }
        // Get a list of all proposals with a superblock that is on or
        // after the current block index.
        auto sprops = getProposalsSince(pindex->nHeight);
        // Obtain all votes for these proposals
        std::vector<Vote*> svotes;
        for (const auto & p : sprops) {
            auto s = getMutableSBVotes(p.getHash());
            svotes.insert(svotes.end(), s.begin(), s.end());
        }

        {
            LOCK(mu);
            for (auto & v : svotes) {
                if (!prevouts.count(v->getUtxo()))
                    continue;
                // Only mark the vote as spent if it happens before or on its
                // proposal's superblock.
                spendVote(v, pindex->nHeight, prevouts[v->getUtxo()]);
            }
        }
    }

    /**
     * Records a vote, requires the proposal to be known.
     * @param vote
     */
    void addVote(const Vote & vote) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (!proposals.count(vote.getProposal()))
            return;

        const auto & voteHash = vote.getHash();
        votes[voteHash] = vote; // add to votes data provider

        const auto & proposal = proposals[vote.getProposal()];
        auto & vs = sbvotes[proposal.getSuperblock()];
        vs[voteHash] = vote;
    }

    /**
     * Removes and erases the specified vote from data providers.
     * @param vote
     */
    void removeVote(const Vote & vote) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        const auto & voteHash = vote.getHash();
        if (!votes.count(voteHash))
            return;

        // Remove from votes data provider
        votes.erase(voteHash);

        if (!proposals.count(vote.getProposal()))
            return;

        const auto & proposal = proposals[vote.getProposal()];
        if (!sbvotes.count(proposal.getSuperblock()))
            return; // no votes found for superblock, skip

        auto & vs = sbvotes[proposal.getSuperblock()];
        if (!vs.count(voteHash))
            return;
        // Remove from superblock votes data provider
        vs.erase(voteHash);
    }

    void addProposal(const Proposal & proposal) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        if (proposals.count(proposal.getHash()))
            return; // do not overwrite existing proposals
        proposals[proposal.getHash()] = proposal;
    }

    void removeProposal(const Proposal & proposal) EXCLUSIVE_LOCKS_REQUIRED(mu) {
        proposals.erase(proposal.getHash());
    }

protected:
    Mutex mu;
    std::unordered_map<uint256, Proposal, Hasher> proposals GUARDED_BY(mu);
    std::unordered_map<uint256, Vote, Hasher> votes GUARDED_BY(mu);
    std::unordered_map<int, std::unordered_map<uint256, Vote, Hasher>> sbvotes GUARDED_BY(mu);
};

}

#endif //BLOCKNET_GOVERNANCE_GOVERNANCE_H
