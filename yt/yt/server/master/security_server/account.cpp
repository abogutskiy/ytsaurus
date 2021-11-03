#include "account.h"
#include "private.h"

#include <yt/yt/server/master/cell_master/serialize.h>

#include <yt/yt/server/master/object_server/object.h>

#include <yt/yt/server/lib/misc/interned_attributes.h>

#include <yt/yt/server/lib/security_server/proto/security_manager.pb.h>

#include <yt/yt/ytlib/object_client/config.h>

#include <yt/yt/core/concurrency/throughput_throttler.h>

#include <yt/yt/core/ytree/convert.h>
#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NSecurityServer {

using namespace NYson;
using namespace NYTree;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;
using namespace NConcurrency;

////////////////////////////////////////////////////////////////////////////////

static const auto& Logger = SecurityServerLogger;

////////////////////////////////////////////////////////////////////////////////

void TAccountStatistics::Persist(const NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, ResourceUsage);
    Persist(context, CommittedResourceUsage);
}

void ToProto(NProto::TAccountStatistics* protoStatistics, const TAccountStatistics& statistics)
{
    ToProto(protoStatistics->mutable_resource_usage(), statistics.ResourceUsage);
    ToProto(protoStatistics->mutable_committed_resource_usage(), statistics.CommittedResourceUsage);
}

void FromProto(TAccountStatistics* statistics, const NProto::TAccountStatistics& protoStatistics)
{
    FromProto(&statistics->ResourceUsage, protoStatistics.resource_usage());
    FromProto(&statistics->CommittedResourceUsage, protoStatistics.committed_resource_usage());
}

void Serialize(const TAccountStatistics& statistics, IYsonConsumer* consumer, const TBootstrap* bootstrap)
{
    auto fluent = BuildYsonFluently(consumer)
        .BeginMap();

    fluent
        .Item("resource_usage");
    SerializeClusterResources(statistics.ResourceUsage, fluent.GetConsumer(), bootstrap);

    fluent
        .Item("committed_resource_usage");
    SerializeClusterResources(statistics.CommittedResourceUsage, fluent.GetConsumer(), bootstrap);

    fluent
        .EndMap();
}

TAccountStatistics& operator += (TAccountStatistics& lhs, const TAccountStatistics& rhs)
{
    lhs.ResourceUsage += rhs.ResourceUsage;
    lhs.CommittedResourceUsage += rhs.CommittedResourceUsage;
    return lhs;
}

TAccountStatistics operator + (const TAccountStatistics& lhs, const TAccountStatistics& rhs)
{
    auto result = lhs;
    result += rhs;
    return result;
}

TAccountStatistics& operator -= (TAccountStatistics& lhs, const TAccountStatistics& rhs)
{
    lhs.ResourceUsage -= rhs.ResourceUsage;
    lhs.CommittedResourceUsage -= rhs.CommittedResourceUsage;
    return lhs;
}

TAccountStatistics operator - (const TAccountStatistics& lhs, const TAccountStatistics& rhs)
{
    auto result = lhs;
    result -= rhs;
    return result;
}

////////////////////////////////////////////////////////////////////////////////

void AddToAccountMulticellStatistics(
    TAccountMulticellStatistics& lhs,
    const TAccountMulticellStatistics& rhs)
{
    for (const auto& [cellTag, accountStatistics] : rhs) {
        lhs[cellTag] += accountStatistics;
    }
}

void SubtractFromAccountMulticellStatistics(
    TAccountMulticellStatistics& lhs,
    const TAccountMulticellStatistics& rhs)
{
    for (const auto& [cellTag, accountStatistics] : rhs) {
        lhs[cellTag] -= accountStatistics;
    }
}


TAccountMulticellStatistics AddAccountMulticellStatistics(
    const TAccountMulticellStatistics& lhs,
    const TAccountMulticellStatistics& rhs)
{
    auto result = lhs;
    AddToAccountMulticellStatistics(result, rhs);
    return result;
}

TAccountMulticellStatistics SubtractAccountMulticellStatistics(
    const TAccountMulticellStatistics& lhs,
    const TAccountMulticellStatistics& rhs)
{
    auto result = lhs;
    SubtractFromAccountMulticellStatistics(result, rhs);
    return result;
}

////////////////////////////////////////////////////////////////////////////////

TAccount::TAccount(TAccountId id, bool isRoot)
    : TNonversionedMapObjectBase<TAccount>(id, isRoot)
    , MergeJobThrottler_(CreateReconfigurableThroughputThrottler(New<TThroughputThrottlerConfig>(0)))
    , ChunkMergerNodeTraversals_(id)
{ }

TString TAccount::GetLowercaseObjectName() const
{
    return Format("account %Qv", GetName());
}

TString TAccount::GetCapitalizedObjectName() const
{
    return Format("Account %Qv", GetName());
}

TString TAccount::GetRootName() const
{
    YT_VERIFY(IsRoot());
    return NSecurityClient::RootAccountName;
}

void TAccount::Save(NCellMaster::TSaveContext& context) const
{
    TNonversionedMapObjectBase<TAccount>::Save(context);

    using NYT::Save;
    Save(context, ClusterStatistics_);
    Save(context, MulticellStatistics_);
    Save(context, ClusterResourceLimits_);
    Save(context, AllowChildrenLimitOvercommit_);
    Save(context, MergeJobRateLimit_);
    Save(context, AbcConfig_.operator bool());
    if (AbcConfig_) {
        Save(context, *AbcConfig_);
    }
    Save(context, FolderId_);
    Save(context, ChunkMergerNodeTraversalConcurrency_);
}

void TAccount::Load(NCellMaster::TLoadContext& context)
{
    TNonversionedMapObjectBase<TAccount>::Load(context);

    using NYT::Load;
    Load(context, ClusterStatistics_);
    Load(context, MulticellStatistics_);
    Load(context, ClusterResourceLimits_);
    Load(context, AllowChildrenLimitOvercommit_);

    // COMPAT(aleksandra-zh)
    if (context.GetVersion() >= EMasterReign::MasterMergeJobs) {
        Load(context, MergeJobRateLimit_);
        MergeJobThrottler_->SetLimit(MergeJobRateLimit_);
    }

    // COMPAT(cookiedoth)
    if (context.GetVersion() < EMasterReign::MakeAbcFolderIdBuiltin) {
        auto moveUserToBuiltinAttribute = [&] (auto& field, TInternedAttributeKey internedAttributeKey) {
            const auto& attributeName = internedAttributeKey.Unintern();
            if (auto attribute = FindAttribute(attributeName)) {
                auto value = std::move(*attribute);
                YT_VERIFY(Attributes_->Remove(attributeName));
                try {
                    field = ConvertTo<std::decay_t<decltype(field)>>(value);
                } catch (const std::exception& ex) {
                    YT_LOG_WARNING(ex, "Cannot parse %Qv attribute (Value: %v, AccountId: %v)",
                        attributeName,
                        value,
                        GetId());
                }
            }
        };
        moveUserToBuiltinAttribute(AbcConfig_, EInternedAttributeKey::Abc);
        moveUserToBuiltinAttribute(FolderId_, EInternedAttributeKey::FolderId);
    } else {
        if (Load<bool>(context)) {
            AbcConfig_ = New<TAbcConfig>();
            Load(context, *AbcConfig_);
        }
        Load(context, FolderId_);
    }

    // COMPAT(aleksandra-zh)
    if (context.GetVersion() >= EMasterReign::MoreChunkMergerLimits) {
        Load(context, ChunkMergerNodeTraversalConcurrency_);
    }
}

TAccountStatistics& TAccount::LocalStatistics()
{
    return *LocalStatisticsPtr_;
}

bool TAccount::IsDiskSpaceLimitViolated() const
{
    const auto& usage = ClusterStatistics_.ResourceUsage.DiskSpace();

    for (const auto& [mediumIndex, diskSpace] : usage) {
        if (diskSpace > ClusterResourceLimits_.DiskSpace().lookup(mediumIndex)) {
            return true;
        }
    }

    return false;
}

bool TAccount::IsDiskSpaceLimitViolated(int mediumIndex) const
{
    const auto& usage = ClusterStatistics_.ResourceUsage.DiskSpace();
    auto limit = ClusterResourceLimits_.DiskSpace().lookup(mediumIndex);
    return usage.lookup(mediumIndex) > limit;
}

bool TAccount::IsNodeCountLimitViolated() const
{
    // See TSecurityManager::ValidateResourceUsageIncrease for the reason why committed usage is compared here.
    return ClusterStatistics_.CommittedResourceUsage.GetNodeCount() > ClusterResourceLimits_.GetNodeCount();
}

bool TAccount::IsChunkCountLimitViolated() const
{
    return ClusterStatistics_.ResourceUsage.GetChunkCount() > ClusterResourceLimits_.GetChunkCount();
}

bool TAccount::IsTabletCountLimitViolated() const
{
    return ClusterStatistics_.ResourceUsage.GetTabletCount() > ClusterResourceLimits_.GetTabletCount();
}

bool TAccount::IsTabletStaticMemoryLimitViolated() const
{
    return ClusterStatistics_.ResourceUsage.GetTabletStaticMemory() > ClusterResourceLimits_.GetTabletStaticMemory();
}

bool TAccount::IsMasterMemoryLimitViolated() const
{
    return ClusterStatistics_.ResourceUsage.GetTotalMasterMemory() > ClusterResourceLimits_.MasterMemory().Total;
}

bool TAccount::IsMasterMemoryLimitViolated(TCellTag cellTag) const
{
    const auto& perCellLimits = ClusterResourceLimits_.MasterMemory().PerCell;
    auto limitIt = perCellLimits.find(cellTag);
    if (limitIt == perCellLimits.end()) {
        return false;
    }

    auto usageIt = MulticellStatistics_.find(cellTag);
    if (usageIt == MulticellStatistics_.end()) {
        return false;
    }

    return usageIt->second.ResourceUsage.GetTotalMasterMemory() > limitIt->second;
}

bool TAccount::IsChunkHostMasterMemoryLimitViolated(const TMulticellManagerPtr& multicellManager) const
{
    auto totalRoleMasterMemory = GetChunkHostMasterMemoryUsage(multicellManager);
    return totalRoleMasterMemory > ClusterResourceLimits_.MasterMemory().ChunkHost;
}

i64 TAccount::GetChunkHostMasterMemoryUsage(const TMulticellManagerPtr& multicellManager) const
{
    auto cellTags = multicellManager->GetRoleMasterCells(EMasterCellRole::ChunkHost);
    i64 totalRoleMasterMemory = 0;
    for (auto cellTag : cellTags) {
        auto roleMulticellStatisticsIt = MulticellStatistics_.find(cellTag);
        if (roleMulticellStatisticsIt != MulticellStatistics_.end()) {
            totalRoleMasterMemory += roleMulticellStatisticsIt->second.ResourceUsage.GetTotalMasterMemory();
        }
    }

    return totalRoleMasterMemory;
}

TAccountStatistics* TAccount::GetCellStatistics(TCellTag cellTag)
{
    return &GetOrCrash(MulticellStatistics_, cellTag);
}

void TAccount::RecomputeClusterStatistics()
{
    ClusterStatistics_ = TAccountStatistics();
    for (const auto& [cellTag, statistics] : MulticellStatistics_) {
        ClusterStatistics_ += statistics;
    }
}

void TAccount::AttachChild(const TString& key, TAccount* child) noexcept
{
    TNonversionedMapObjectBase<TAccount>::AttachChild(key, child);

    const auto& childLocalResourceUsage = child->LocalStatistics().ResourceUsage;
    const auto& childLocalCommittedResourceUsage = child->LocalStatistics().CommittedResourceUsage;

    const auto& childResourceUsage = child->ClusterStatistics().ResourceUsage;
    const auto& childCommittedResourceUsage = child->ClusterStatistics().CommittedResourceUsage;

    for (auto* account = this; account; account = account->GetParent()) {
        auto& localStatistics = account->LocalStatistics();
        auto& clusterStatistics = account->ClusterStatistics();

        localStatistics.ResourceUsage += childLocalResourceUsage;
        clusterStatistics.ResourceUsage += childResourceUsage;

        localStatistics.CommittedResourceUsage += childLocalCommittedResourceUsage;
        clusterStatistics.CommittedResourceUsage += childCommittedResourceUsage;
    }
}

void TAccount::DetachChild(TAccount* child) noexcept
{
    TNonversionedMapObjectBase<TAccount>::DetachChild(child);

    const auto& childLocalResourceUsage = child->LocalStatistics().ResourceUsage;
    const auto& childLocalCommittedResourceUsage = child->LocalStatistics().CommittedResourceUsage;

    const auto& childResourceUsage = child->ClusterStatistics().ResourceUsage;
    const auto& childCommittedResourceUsage = child->ClusterStatistics().CommittedResourceUsage;

    for (auto* account = this; account; account = account->GetParent()) {
        auto& localStatistics = account->LocalStatistics();
        auto& clusterStatistics = account->ClusterStatistics();

        localStatistics.ResourceUsage -= childLocalResourceUsage;
        clusterStatistics.ResourceUsage -= childResourceUsage;

        localStatistics.CommittedResourceUsage -= childLocalCommittedResourceUsage;
        clusterStatistics.CommittedResourceUsage -= childCommittedResourceUsage;
    }
}

TClusterResourceLimits TAccount::ComputeTotalChildrenLimits() const
{
    auto result = TClusterResourceLimits();
    for (const auto& [key, child] : KeyToChild()) {
        result += child->ClusterResourceLimits();
    }
    return result;
}

TClusterResources TAccount::ComputeTotalChildrenResourceUsage(bool committed) const
{
    auto result = TClusterResources();
    for (const auto& [key, child] : KeyToChild()) {
        result += committed
            ? child->ClusterStatistics().CommittedResourceUsage
            : child->ClusterStatistics().ResourceUsage;
    }
    return result;
}

TAccountMulticellStatistics TAccount::ComputeTotalChildrenMulticellStatistics() const
{
    auto result = TAccountMulticellStatistics();
    for (const auto& [key, child] : KeyToChild()) {
        AddToAccountMulticellStatistics(result, child->MulticellStatistics());
    }
    return result;
}

TViolatedClusterResourceLimits TAccount::GetViolatedResourceLimits(
    NCellMaster::TBootstrap* bootstrap,
    bool enableTabletResourceValidation) const
{
    const auto& multicellManager = bootstrap->GetMulticellManager();

    auto primaryCellTag = multicellManager->GetPrimaryCellTag();
    const auto& cellTags = multicellManager->GetSecondaryCellTags();

    TViolatedClusterResourceLimits violatedLimits;
    for (auto* account = this; account; account = account->GetParent()) {
        if (account->IsNodeCountLimitViolated()) {
            violatedLimits.SetNodeCount(1);
        }
        if (account->IsChunkCountLimitViolated()) {
            violatedLimits.SetChunkCount(1);
        }

        if (enableTabletResourceValidation) {
            if (account->IsTabletCountLimitViolated()) {
                violatedLimits.SetTabletCount(1);
            }
            if (account->IsTabletStaticMemoryLimitViolated()) {
                violatedLimits.SetTabletStaticMemory(1);
            }
        }

        for (const auto& [mediumIndex, usage] : account->ClusterStatistics().ResourceUsage.DiskSpace()) {
            if (account->IsDiskSpaceLimitViolated(mediumIndex)) {
                violatedLimits.SetMediumDiskSpace(mediumIndex, 1);
            } else {
                violatedLimits.AddToMediumDiskSpace(mediumIndex, 0);
            }
        }
        for (const auto& [mediumIndex, usage] : account->ClusterResourceLimits().DiskSpace()) {
            violatedLimits.AddToMediumDiskSpace(mediumIndex, 0);
        }

        if (account->IsMasterMemoryLimitViolated()) {
            violatedLimits.MasterMemory().Total = 1;
        }
        if (account->IsChunkHostMasterMemoryLimitViolated(multicellManager)) {
            violatedLimits.MasterMemory().ChunkHost = 1;
        }
        if (account->IsMasterMemoryLimitViolated(primaryCellTag)) {
            violatedLimits.MasterMemory().PerCell[primaryCellTag] = 1;
        }
        for (auto cellTag : cellTags) {
            if (account->IsMasterMemoryLimitViolated(cellTag)) {
                violatedLimits.MasterMemory().PerCell[cellTag] = 1;
            }
        }
    }

    return violatedLimits;
}

TViolatedClusterResourceLimits TAccount::GetRecursiveViolatedResourceLimits(
    NCellMaster::TBootstrap* bootstrap,
    bool enableTabletResourceValidation) const
{
    const auto& multicellManager = bootstrap->GetMulticellManager();

    auto primaryCellTag = multicellManager->GetPrimaryCellTag();
    const auto& cellTags = multicellManager->GetSecondaryCellTags();

    return AccumulateOverMapObjectSubtree(
        this,
        TViolatedClusterResourceLimits(),
        [&] (const TAccount* account, TViolatedClusterResourceLimits* violatedLimits) {
            if (account->IsNodeCountLimitViolated()) {
                violatedLimits->SetNodeCount(violatedLimits->GetNodeCount() + 1);
            }
            if (account->IsChunkCountLimitViolated()) {
                violatedLimits->SetChunkCount(violatedLimits->GetChunkCount() + 1);
            }

            if (enableTabletResourceValidation) {
                if (account->IsTabletCountLimitViolated()) {
                    violatedLimits->SetTabletCount(violatedLimits->GetTabletCount() + 1);
                }
                if (account->IsTabletStaticMemoryLimitViolated()) {
                    violatedLimits->SetTabletStaticMemory(violatedLimits->GetTabletStaticMemory() + 1);
                }
            }

            for (const auto& [mediumIndex, usage] : account->ClusterStatistics().ResourceUsage.DiskSpace()) {
                if (account->IsDiskSpaceLimitViolated(mediumIndex)) {
                    violatedLimits->AddToMediumDiskSpace(mediumIndex, 1);
                } else {
                    violatedLimits->AddToMediumDiskSpace(mediumIndex, 0);
                }
            }
            for (const auto& [mediumIndex, usage] : account->ClusterResourceLimits().DiskSpace()) {
                violatedLimits->AddToMediumDiskSpace(mediumIndex, 0);
            }

            if (account->IsMasterMemoryLimitViolated()) {
                ++violatedLimits->MasterMemory().Total;
            }
            if (account->IsChunkHostMasterMemoryLimitViolated(multicellManager)) {
                ++violatedLimits->MasterMemory().ChunkHost;
            }
            if (account->IsMasterMemoryLimitViolated(primaryCellTag)) {
                ++violatedLimits->MasterMemory().PerCell[primaryCellTag];
            }
            for (auto cellTag : cellTags) {
                if (account->IsMasterMemoryLimitViolated(cellTag)) {
                    ++violatedLimits->MasterMemory().PerCell[cellTag];
                }
            }
        });
}

int TAccount::GetMergeJobRateLimit() const
{
    return MergeJobRateLimit_;
}

void TAccount::SetMergeJobRateLimit(int mergeJobRateLimit)
{
    MergeJobRateLimit_ = mergeJobRateLimit;
    MergeJobThrottler_->SetLimit(MergeJobRateLimit_);
}

int TAccount::GetChunkMergerNodeTraversals() const
{
    return ChunkMergerNodeTraversals_.GetValue();
}

void TAccount::IncrementChunkMergerNodeTraversals()
{
    ChunkMergerNodeTraversals_.UpdateValue(+1);
}

void TAccount::DecrementChunkMergerNodeTraversals()
{
    ChunkMergerNodeTraversals_.UpdateValue(-1);
}

////////////////////////////////////////////////////////////////////////////////

/*static*/ TAccountChunkMergerNodeTraversalsPtrContext TAccountChunkMergerNodeTraversalsPtrContext::Capture()
{
    TAccountChunkMergerNodeTraversalsPtrContext result;
    static_cast<TEphemeralObjectPtrContext&>(result) = TEphemeralObjectPtrContext::Capture();
    return result;
}

void TAccountChunkMergerNodeTraversalsPtrContext::Ref(TObject* object)
{
    auto* account = object->As<TAccount>();
    account->IncrementChunkMergerNodeTraversals();

    TEphemeralObjectPtrContext::Ref(object);
}

void TAccountChunkMergerNodeTraversalsPtrContext::Unref(TObject* object)
{
    SafeUnref([account = object->As<TAccount>()] {
        account->DecrementChunkMergerNodeTraversals();
    });

    TEphemeralObjectPtrContext::Unref(object);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer
