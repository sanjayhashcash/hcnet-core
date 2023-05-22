// Copyright 2017 Hcnet Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "catchup/ApplyBucketsWork.h"
#include "ledger/LedgerHashUtils.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTxnEntry.h"
#include "ledger/LedgerTxnHeader.h"
#include "ledger/test/LedgerTestUtils.h"
#include "lib/catch.hpp"
#include "lib/util/stdrandom.h"
#include "main/Application.h"
#include "test/TestUtils.h"
#include "test/test.h"
#include "transactions/TransactionUtils.h"
#include "util/Decoder.h"
#include "util/Math.h"
#include "util/UnorderedSet.h"
#include "util/XDROperators.h"
#include "work/WorkScheduler.h"
#include "xdr/Hcnet-ledger-entries.h"
#include <random>
#include <vector>

using namespace hcnet;

namespace BucketListIsConsistentWithDatabaseTests
{

struct BucketListGenerator
{
    VirtualClock mClock;
    Application::pointer mAppGenerate;
    uint32_t mLedgerSeq;
    UnorderedSet<LedgerKey> mLiveKeys;

  public:
    BucketListGenerator()
        : mAppGenerate(createTestApplication(mClock, getTestConfig(0)))
        , mLedgerSeq(1)
    {
        auto skey = SecretKey::fromSeed(mAppGenerate->getNetworkID());
        LedgerKey key(ACCOUNT);
        key.account().accountID = skey.getPublicKey();
        mLiveKeys.insert(key);

        LedgerTxn ltx(mAppGenerate->getLedgerTxnRoot(), false);
        REQUIRE(mLedgerSeq == ltx.loadHeader().current().ledgerSeq);
    }

    template <typename T = ApplyBucketsWork, typename... Args>
    void
    applyBuckets(Application::pointer app, Args&&... args)
    {
        std::map<std::string, std::shared_ptr<Bucket>> buckets;
        auto has = getHistoryArchiveState(app);
        auto& wm = app->getWorkScheduler();
        wm.executeWork<T>(buckets, has,
                          app->getConfig().LEDGER_PROTOCOL_VERSION,
                          std::forward<Args>(args)...);
    }

    template <typename T = ApplyBucketsWork, typename... Args>
    void
    applyBuckets(Args&&... args)
    {
        VirtualClock clock;
        Application::pointer app =
            createTestApplication(clock, getTestConfig(1));
        applyBuckets<T, Args...>(app, std::forward<Args>(args)...);
    }

    void
    generateLedger()
    {
        auto& app = mAppGenerate;
        LedgerTxn ltx(app->getLedgerTxnRoot(), false);
        REQUIRE(mLedgerSeq == ltx.loadHeader().current().ledgerSeq);
        mLedgerSeq = ++ltx.loadHeader().current().ledgerSeq;
        auto vers = ltx.loadHeader().current().ledgerVersion;

        auto dead = generateDeadEntries(ltx);
        assert(dead.size() <= mLiveKeys.size());
        for (auto const& key : dead)
        {
            auto iter = mLiveKeys.find(key);
            assert(iter != mLiveKeys.end());
            ltx.erase(key);
            mLiveKeys.erase(iter);
        }

        auto live = generateLiveEntries(ltx);
        for (auto& le : live)
        {
            auto key = LedgerEntryKey(le);
            auto iter = mLiveKeys.find(key);
            if (iter == mLiveKeys.end())
            {
                ltx.create(le);
            }
            else
            {
                ltx.load(key).current() = le;
            }
            mLiveKeys.insert(LedgerEntryKey(le));
        }

        std::vector<LedgerEntry> initEntries, liveEntries;
        std::vector<LedgerKey> deadEntries;
        ltx.getAllEntries(initEntries, liveEntries, deadEntries);
        app->getBucketManager().addBatch(*app, mLedgerSeq, vers, initEntries,
                                         liveEntries, deadEntries);
        ltx.commit();
    }

    void
    generateLedgers(uint32_t n)
    {
        uint32_t stopLedger = mLedgerSeq + n - 1;
        while (mLedgerSeq <= stopLedger)
        {
            generateLedger();
        }
    }

    virtual std::vector<LedgerEntry>
    generateLiveEntries(AbstractLedgerTxn& ltx)
    {
        auto entries =
            LedgerTestUtils::generateValidLedgerEntriesWithExclusions(
                {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                    CONFIG_SETTING
#endif
                },
                5);
        for (auto& le : entries)
        {
            le.lastModifiedLedgerSeq = mLedgerSeq;
        }
        return entries;
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries(AbstractLedgerTxn& ltx)
    {
        UnorderedSet<LedgerKey> live(mLiveKeys);
        std::vector<LedgerKey> dead;
        while (dead.size() < 2 && !live.empty())
        {
            auto dist =
                hcnet::uniform_int_distribution<size_t>(0, live.size() - 1);
            auto index = dist(gRandomEngine);
            auto iter = live.begin();
            std::advance(iter, index);
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            if (iter->type() == CONFIG_SETTING)
            {
                // Configuration can not be deleted.
                continue;
            }
#endif
            dead.push_back(*iter);
            live.erase(iter);
        }
        return dead;
    }

    HistoryArchiveState
    getHistoryArchiveState(Application::pointer app)
    {
        auto& blGenerate = mAppGenerate->getBucketManager().getBucketList();
        auto& bmApply = app->getBucketManager();
        MergeCounters mergeCounters;
        LedgerTxn ltx(mAppGenerate->getLedgerTxnRoot(), false);
        auto vers = ltx.loadHeader().current().ledgerVersion;
        for (uint32_t i = 0; i <= BucketList::kNumLevels - 1; i++)
        {
            auto& level = blGenerate.getLevel(i);
            auto meta = testutil::testBucketMetadata(vers);
            auto keepDead = BucketList::keepDeadEntries(i);

            auto writeBucketFile = [&](auto b) {
                BucketOutputIterator out(bmApply.getTmpDir(), keepDead, meta,
                                         mergeCounters, mClock.getIOContext(),
                                         /*doFsync=*/true);
                for (BucketInputIterator in(b); in; ++in)
                {
                    out.put(*in);
                }

                auto bucket =
                    out.getBucket(bmApply, /*shouldSynchronouslyIndex=*/false);
            };
            writeBucketFile(level.getCurr());
            writeBucketFile(level.getSnap());

            auto& next = level.getNext();
            if (next.hasOutputHash())
            {
                auto nextBucket = next.resolve();
                REQUIRE(nextBucket);
                writeBucketFile(nextBucket);
            }
        }
        return HistoryArchiveState(
            mLedgerSeq, blGenerate,
            mAppGenerate->getConfig().NETWORK_PASSPHRASE);
    }
};

bool
doesBucketContain(std::shared_ptr<Bucket const> bucket, const BucketEntry& be)
{
    for (BucketInputIterator iter(bucket); iter; ++iter)
    {
        if (*iter == be)
        {
            return true;
        }
    }
    return false;
}

bool
doesBucketListContain(BucketList& bl, const BucketEntry& be)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        for (auto const& bucket : {level.getCurr(), level.getSnap()})
        {
            if (doesBucketContain(bucket, be))
            {
                return true;
            }
        }
    }
    return false;
}

struct SelectBucketListGenerator : public BucketListGenerator
{
    uint32_t const mSelectLedger;
    LedgerEntryType const mType;
    std::shared_ptr<LedgerEntry> mSelected;

    SelectBucketListGenerator(uint32_t selectLedger, LedgerEntryType type)
        : mSelectLedger(selectLedger), mType(type)
    {
    }

    virtual std::vector<LedgerEntry>
    generateLiveEntries(AbstractLedgerTxn& ltx)
    {
        if (mLedgerSeq == mSelectLedger)
        {
            UnorderedSet<LedgerKey> filteredKeys(mLiveKeys.size());
            std::copy_if(
                mLiveKeys.begin(), mLiveKeys.end(),
                std::inserter(filteredKeys, filteredKeys.end()),
                [this](LedgerKey const& key) { return key.type() == mType; });

            if (!filteredKeys.empty())
            {
                hcnet::uniform_int_distribution<size_t> dist(
                    0, filteredKeys.size() - 1);
                auto iter = filteredKeys.begin();
                std::advance(iter, dist(gRandomEngine));

                mSelected = std::make_shared<LedgerEntry>(
                    ltx.loadWithoutRecord(*iter).current());
            }
        }
        return BucketListGenerator::generateLiveEntries(ltx);
    }

    virtual std::vector<LedgerKey>
    generateDeadEntries(AbstractLedgerTxn& ltx)
    {
        auto dead = BucketListGenerator::generateDeadEntries(ltx);
        if (mSelected)
        {
            auto key = LedgerEntryKey(*mSelected);
            auto iter = std::find(dead.begin(), dead.end(), key);
            if (iter != dead.end())
            {
                dead.erase(iter);
            }
        }
        return dead;
    }
};

class ApplyBucketsWorkAddEntry : public ApplyBucketsWork
{
  private:
    LedgerEntry mEntry;
    bool mAdded;

  public:
    ApplyBucketsWorkAddEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        std::function<bool(LedgerEntryType)> filter, LedgerEntry const& entry)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion, filter)
        , mEntry(entry)
        , mAdded{false}
    {
        REQUIRE(entry.lastModifiedLedgerSeq >= 2);
    }

    BasicWork::State
    doWork() override
    {
        if (!mAdded)
        {
            uint32_t minLedger = mEntry.lastModifiedLedgerSeq;
            uint32_t maxLedger = std::numeric_limits<int32_t>::max() - 1;
            auto& ltxRoot = mApp.getLedgerTxnRoot();

            size_t count = 0;
            for (auto let : xdr::xdr_traits<LedgerEntryType>::enum_values())
            {
                count += ltxRoot.countObjects(
                    static_cast<LedgerEntryType>(let),
                    LedgerRange::inclusive(minLedger, maxLedger));
            }

            if (count > 0)
            {
                LedgerTxn ltx(ltxRoot, false);
                ltx.create(mEntry);
                ltx.commit();
                mAdded = true;
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mAdded);
        }
        return r;
    }
};

class ApplyBucketsWorkDeleteEntry : public ApplyBucketsWork
{
  private:
    LedgerKey mKey;
    LedgerEntry mEntry;
    bool mDeleted;

  public:
    ApplyBucketsWorkDeleteEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        LedgerEntry const& target)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mDeleted{false}
    {
    }

    BasicWork::State
    doWork() override
    {
        if (!mDeleted)
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
            auto entry = ltx.load(mKey);
            if (entry && entry.current() == mEntry)
            {
                entry.erase();
                ltx.commit();
                mDeleted = true;
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mDeleted);
        }
        return r;
    }
};

class ApplyBucketsWorkModifyEntry : public ApplyBucketsWork
{
  private:
    LedgerKey mKey;
    LedgerEntry mEntry;
    bool mModified;

    void
    modifyAccountEntry(LedgerEntry& entry)
    {
        AccountEntry const& account = mEntry.data.account();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.account() = LedgerTestUtils::generateValidAccountEntry(5);
        entry.data.account().accountID = account.accountID;
    }

    void
    modifyTrustLineEntry(LedgerEntry& entry)
    {
        TrustLineEntry const& trustLine = mEntry.data.trustLine();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.trustLine() =
            LedgerTestUtils::generateValidTrustLineEntry(5);
        entry.data.trustLine().accountID = trustLine.accountID;
        entry.data.trustLine().asset = trustLine.asset;
    }

    void
    modifyOfferEntry(LedgerEntry& entry)
    {
        OfferEntry const& offer = mEntry.data.offer();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.offer() = LedgerTestUtils::generateValidOfferEntry(5);
        entry.data.offer().sellerID = offer.sellerID;
        entry.data.offer().offerID = offer.offerID;
    }

    void
    modifyDataEntry(LedgerEntry& entry)
    {
        DataEntry const& data = mEntry.data.data();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        do
        {
            entry.data.data() = LedgerTestUtils::generateValidDataEntry(5);
        } while (entry.data.data().dataValue == data.dataValue);
        entry.data.data().accountID = data.accountID;
        entry.data.data().dataName = data.dataName;
    }

    void
    modifyClaimableBalanceEntry(LedgerEntry& entry)
    {
        ClaimableBalanceEntry const& cb = mEntry.data.claimableBalance();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.claimableBalance() =
            LedgerTestUtils::generateValidClaimableBalanceEntry(5);

        entry.data.claimableBalance().balanceID = cb.balanceID;
    }

    void
    modifyLiquidityPoolEntry(LedgerEntry& entry)
    {
        LiquidityPoolEntry const& lp = mEntry.data.liquidityPool();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.liquidityPool() =
            LedgerTestUtils::generateValidLiquidityPoolEntry(5);

        entry.data.liquidityPool().liquidityPoolID = lp.liquidityPoolID;
    }

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    void
    modifyConfigSettingEntry(LedgerEntry& entry)
    {
        ConfigSettingEntry const& cfg = mEntry.data.configSetting();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.configSetting() =
            LedgerTestUtils::generateValidConfigSettingEntry(5);

        entry.data.configSetting().configSettingID(cfg.configSettingID());
    }

    void
    modifyContractDataEntry(LedgerEntry& entry)
    {
        ContractDataEntry const& cd = mEntry.data.contractData();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.contractData() =
            LedgerTestUtils::generateValidContractDataEntry(5);

        entry.data.contractData().contractID = cd.contractID;
        entry.data.contractData().key = cd.key;
    }

    void
    modifyContractCodeEntry(LedgerEntry& entry)
    {
        ContractCodeEntry const& cc = mEntry.data.contractCode();
        entry.lastModifiedLedgerSeq = mEntry.lastModifiedLedgerSeq;
        entry.data.contractCode() =
            LedgerTestUtils::generateValidContractCodeEntry(5);

        entry.data.contractCode().hash = cc.hash;
    }

#endif

  public:
    ApplyBucketsWorkModifyEntry(
        Application& app,
        std::map<std::string, std::shared_ptr<Bucket>> const& buckets,
        HistoryArchiveState const& applyState, uint32_t maxProtocolVersion,
        LedgerEntry const& target)
        : ApplyBucketsWork(app, buckets, applyState, maxProtocolVersion)
        , mKey(LedgerEntryKey(target))
        , mEntry(target)
        , mModified{false}
    {
    }

    BasicWork::State
    doWork() override
    {
        if (!mModified)
        {
            LedgerTxn ltx(mApp.getLedgerTxnRoot(), false);
            auto entry = ltx.load(mKey);
            if (entry && entry.current() == mEntry)
            {
                switch (mEntry.data.type())
                {
                case ACCOUNT:
                    modifyAccountEntry(entry.current());
                    break;
                case TRUSTLINE:
                    modifyTrustLineEntry(entry.current());
                    break;
                case OFFER:
                    modifyOfferEntry(entry.current());
                    break;
                case DATA:
                    modifyDataEntry(entry.current());
                    break;
                case CLAIMABLE_BALANCE:
                    modifyClaimableBalanceEntry(entry.current());
                    break;
                case LIQUIDITY_POOL:
                    modifyLiquidityPoolEntry(entry.current());
                    break;
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                case CONFIG_SETTING:
                    modifyConfigSettingEntry(entry.current());
                    break;
                case CONTRACT_DATA:
                    modifyContractDataEntry(entry.current());
                    break;
                case CONTRACT_CODE:
                    modifyContractCodeEntry(entry.current());
                    break;
#endif
                default:
                    REQUIRE(false);
                }
                ltx.commit();
                mModified = true;
            }
        }
        auto r = ApplyBucketsWork::doWork();
        if (r == State::WORK_SUCCESS)
        {
            REQUIRE(mModified);
        }
        return r;
    }
};
}

using namespace BucketListIsConsistentWithDatabaseTests;

TEST_CASE("BucketListIsConsistentWithDatabase succeed",
          "[invariant][bucketlistconsistent]")
{
    BucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase empty ledgers",
          "[invariant][bucketlistconsistent]")
{
    class EmptyBucketListGenerator : public BucketListGenerator
    {
        virtual std::vector<LedgerEntry>
        generateLiveEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }
    };

    EmptyBucketListGenerator blg;
    blg.generateLedgers(100);
    REQUIRE_NOTHROW(blg.applyBuckets());
}

TEST_CASE("BucketListIsConsistentWithDatabase test root account",
          "[invariant][bucketlistconsistent]")
{
    struct TestRootBucketListGenerator : public BucketListGenerator
    {
        uint32_t const mTargetLedger;
        bool mModifiedRoot;

        TestRootBucketListGenerator()
            : mTargetLedger(hcnet::uniform_int_distribution<uint32_t>(2, 100)(
                  gRandomEngine))
            , mModifiedRoot(false)
        {
        }

        virtual std::vector<LedgerEntry>
        generateLiveEntries(AbstractLedgerTxn& ltx)
        {
            if (mLedgerSeq == mTargetLedger)
            {
                mModifiedRoot = true;
                auto& app = mAppGenerate;
                auto skey = SecretKey::fromSeed(app->getNetworkID());
                auto root = skey.getPublicKey();
                auto le =
                    hcnet::loadAccountWithoutRecord(ltx, root).current();
                le.lastModifiedLedgerSeq = mLedgerSeq;
                return {le};
            }
            else
            {
                return BucketListGenerator::generateLiveEntries(ltx);
            }
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }
    };

    for (size_t j = 0; j < 5; ++j)
    {
        TestRootBucketListGenerator blg;
        blg.generateLedgers(100);
        REQUIRE(blg.mModifiedRoot);
        REQUIRE_NOTHROW(blg.applyBuckets());
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase added entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    auto runTest = [](bool withFilter) {
        for (size_t nTests = 0; nTests < 40; ++nTests)
        {
            BucketListGenerator blg;
            blg.generateLedgers(100);

            hcnet::uniform_int_distribution<uint32_t> addAtLedgerDist(
                2, blg.mLedgerSeq);
            auto le = LedgerTestUtils::generateValidLedgerEntry(5);
            le.lastModifiedLedgerSeq = addAtLedgerDist(gRandomEngine);

            if (!withFilter)
            {
                auto filter = [](auto) { return true; };
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
                if (le.data.type() == CONFIG_SETTING)
                {
                    // Config settings would have a duplicate key due to low key
                    // space.
                    REQUIRE_THROWS_AS(
                        blg.applyBuckets<ApplyBucketsWorkAddEntry>(filter, le),
                        std::runtime_error);
                }
                else
#endif
                    REQUIRE_THROWS_AS(
                        blg.applyBuckets<ApplyBucketsWorkAddEntry>(filter, le),
                        InvariantDoesNotHold);
            }
            else
            {
                auto filter = [&](auto let) { return let != le.data.type(); };
                REQUIRE_NOTHROW(
                    blg.applyBuckets<ApplyBucketsWorkAddEntry>(filter, le));
            }
        }
    };

    runTest(true);

    // This tests the filtering behavior of BucketListIsConsistentWithDatabase
    // because the bucket apply will not add anything of the specified
    // LedgerEntryType, but we will inject an additional LedgerEntry of that
    // type anyway. But it shouldn't throw because the invariant isn't looking
    // for those changes.
    runTest(false);
}

TEST_CASE("BucketListIsConsistentWithDatabase deleted entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        size_t nTests = 0;
        while (nTests < 10)
        {
            SelectBucketListGenerator blg(100, static_cast<LedgerEntryType>(t));
            blg.generateLedgers(100);
            if (!blg.mSelected)
            {
                continue;
            }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            if (t == CONFIG_SETTING)
            {
                // Configuration can not be deleted.
                REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkDeleteEntry>(
                                      *blg.mSelected),
                                  std::runtime_error);
            }
            else
#endif
                REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkDeleteEntry>(
                                      *blg.mSelected),
                                  InvariantDoesNotHold);
            ++nTests;
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase modified entries",
          "[invariant][bucketlistconsistent][acceptance]")
{
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        size_t nTests = 0;
        while (nTests < 10)
        {
            SelectBucketListGenerator blg(100, static_cast<LedgerEntryType>(t));
            blg.generateLedgers(100);
            if (!blg.mSelected)
            {
                continue;
            }
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
            if (t == CONFIG_SETTING)
            {
                // Configuration can not be deleted.
                REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkDeleteEntry>(
                                      *blg.mSelected),
                                  std::runtime_error);
            }
            else
#endif
                REQUIRE_THROWS_AS(blg.applyBuckets<ApplyBucketsWorkDeleteEntry>(
                                      *blg.mSelected),
                                  InvariantDoesNotHold);
            ++nTests;
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase bucket bounds",
          "[invariant][bucketlistconsistent][acceptance]")
{
    struct LastModifiedBucketListGenerator : public BucketListGenerator
    {
        uint32_t const mTargetLedger;
        uint32_t const mChangeLedgerTo;
        uint32_t mModifiedLedger;

        LastModifiedBucketListGenerator(uint32_t targetLedger,
                                        uint32_t changeLedgerTo)
            : mTargetLedger(targetLedger)
            , mChangeLedgerTo(changeLedgerTo)
            , mModifiedLedger(false)
        {
        }

        virtual std::vector<LedgerEntry>
        generateLiveEntries(AbstractLedgerTxn& ltx)
        {
            auto entries = BucketListGenerator::generateLiveEntries(ltx);
            if (mLedgerSeq == mTargetLedger)
            {
                mModifiedLedger = true;
                for (auto& le : entries)
                {
                    le.lastModifiedLedgerSeq = mChangeLedgerTo;
                }
            }
            return entries;
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            return {};
        }
    };

    for (uint32_t level = 0; level < BucketList::kNumLevels; ++level)
    {
        uint32_t oldestLedger = BucketList::oldestLedgerInSnap(101, level);
        if (oldestLedger == std::numeric_limits<uint32_t>::max())
        {
            break;
        }
        uint32_t newestLedger = BucketList::oldestLedgerInCurr(101, level) +
                                BucketList::sizeOfCurr(101, level) - 1;
        hcnet::uniform_int_distribution<uint32_t> ledgerToModifyDist(
            std::max(2u, oldestLedger), newestLedger);

        for (uint32_t i = 0; i < 10; ++i)
        {
            uint32_t ledgerToModify = ledgerToModifyDist(gRandomEngine);
            uint32_t maxLowTargetLedger = 0;
            uint32_t minHighTargetLedger = 0;
            if (ledgerToModify >= BucketList::oldestLedgerInCurr(101, level))
            {
                maxLowTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level) - 1;
                minHighTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level) +
                    BucketList::sizeOfCurr(101, level);
            }
            else
            {
                maxLowTargetLedger =
                    BucketList::oldestLedgerInSnap(101, level) - 1;
                minHighTargetLedger =
                    BucketList::oldestLedgerInCurr(101, level);
            }
            hcnet::uniform_int_distribution<uint32_t> lowTargetLedgerDist(
                1, maxLowTargetLedger);
            hcnet::uniform_int_distribution<uint32_t> highTargetLedgerDist(
                minHighTargetLedger, std::numeric_limits<int32_t>::max());

            uint32_t lowTarget = lowTargetLedgerDist(gRandomEngine);
            uint32_t highTarget = highTargetLedgerDist(gRandomEngine);
            for (auto target : {lowTarget, highTarget})
            {
                LastModifiedBucketListGenerator blg(ledgerToModify, target);
                blg.generateLedgers(100);
                REQUIRE_THROWS_AS(blg.applyBuckets(), InvariantDoesNotHold);
            }
        }
    }
}

TEST_CASE("BucketListIsConsistentWithDatabase merged LIVEENTRY and DEADENTRY",
          "[invariant][bucketlistconsistent][acceptance]")
{
    struct MergeBucketListGenerator : public SelectBucketListGenerator
    {
        uint32_t const mTargetLedger;

        MergeBucketListGenerator(LedgerEntryType let)
            : SelectBucketListGenerator(25, let), mTargetLedger(110)
        {
        }

        virtual std::vector<LedgerKey>
        generateDeadEntries(AbstractLedgerTxn& ltx)
        {
            if (mLedgerSeq == mTargetLedger)
            {
                return {LedgerEntryKey(*mSelected)};
            }
            else
            {
                return SelectBucketListGenerator::generateDeadEntries(ltx);
            }
        }
    };

    auto exists = [](Application& app, LedgerEntry const& le) {
        LedgerTxn ltx(app.getLedgerTxnRoot());
        return (bool)ltx.load(LedgerEntryKey(le));
    };

    testutil::BucketListDepthModifier bldm(3);
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
        if (t == CONFIG_SETTING)
        {
            // Merge logic is not applicable to configuration.
            continue;
        }
#endif
        uint32_t nTests = 0;
        while (nTests < 5)
        {
            MergeBucketListGenerator blg(static_cast<LedgerEntryType>(t));
            auto& blGenerate =
                blg.mAppGenerate->getBucketManager().getBucketList();

            blg.generateLedgers(100);
            if (!blg.mSelected)
            {
                continue;
            }

            BucketEntry dead(DEADENTRY);
            dead.deadEntry() = LedgerEntryKey(*blg.mSelected);
            BucketEntry live(LIVEENTRY);
            live.liveEntry() = *blg.mSelected;
            BucketEntry init(INITENTRY);
            init.liveEntry() = *blg.mSelected;

            {
                VirtualClock clock;
                Application::pointer appApply =
                    createTestApplication(clock, getTestConfig(1));
                REQUIRE_NOTHROW(blg.applyBuckets(appApply));
                REQUIRE(exists(*blg.mAppGenerate, *blg.mSelected));
                REQUIRE(exists(*appApply, *blg.mSelected));
            }

            blg.generateLedgers(10);
            REQUIRE(doesBucketListContain(blGenerate, dead));
            REQUIRE((doesBucketListContain(blGenerate, live) ||
                     doesBucketListContain(blGenerate, init)));

            blg.generateLedgers(100);
            REQUIRE(!doesBucketListContain(blGenerate, dead));
            REQUIRE(!(doesBucketListContain(blGenerate, live) ||
                      doesBucketListContain(blGenerate, init)));
            REQUIRE(!exists(*blg.mAppGenerate, *blg.mSelected));

            {
                VirtualClock clock;
                Application::pointer appApply =
                    createTestApplication(clock, getTestConfig(1));
                REQUIRE_NOTHROW(blg.applyBuckets(appApply));
                auto& blApply = appApply->getBucketManager().getBucketList();
                REQUIRE(!doesBucketListContain(blApply, dead));
                REQUIRE(!(doesBucketListContain(blApply, live) ||
                          doesBucketListContain(blApply, init)));
                REQUIRE(!exists(*appApply, *blg.mSelected));
            }

            ++nTests;
        }
    }
}
