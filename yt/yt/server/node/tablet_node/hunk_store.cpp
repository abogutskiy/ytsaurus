#include "hunk_store.h"

#include "hunk_tablet.h"
#include "serialize.h"

#include <yt/yt/core/misc/serialize.h>

#include <yt/yt/core/ytree/fluent.h>

namespace NYT::NTabletNode {

using namespace NJournalClient;
using namespace NHydra;
using namespace NYson;
using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

THunkStore::THunkStore(TStoreId storeId, THunkTablet* tablet)
    : TObjectBase(storeId)
    , Tablet_(tablet)
    , Logger(Tablet_->GetLogger().WithTag("StoreId: %v", storeId))
    , LockingState_(/*objectId*/ storeId)
{ }

TFuture<std::vector<TJournalHunkDescriptor>> THunkStore::WriteHunks(std::vector<TSharedRef> payloads)
{
    if (State_ != EHunkStoreState::Active) {
        auto error = TError("Store %v is not active", Id_);
        return MakeFuture<std::vector<TJournalHunkDescriptor>>(error);
    }

    if (WriterOpenedFuture_.IsSet()) {
        // Fast path.
        return Writer_->WriteHunks(std::move(payloads));
    }

    // Slow path.
    return WriterOpenedFuture_.Apply(BIND([=, this, this_ = MakeStrong(this)] {
        return Writer_->WriteHunks(std::move(payloads));
    }));
}

void THunkStore::Save(TSaveContext& context) const
{
    using NYT::Save;

    Save(context, MarkedSealable_);
    Save(context, TabletIdToLockCount_);
    Save(context, LockingState_);
}

void THunkStore::Load(TLoadContext& context)
{
    using NYT::Load;

    Load(context, MarkedSealable_);
    Load(context, TabletIdToLockCount_);

    // COMPAT(gritukan)
    if (context.GetVersion() < ETabletReign::LockingState) {
        Load(context, LockingState_.ExclusiveLockTransactionId_);
        Load(context, LockingState_.SharedLockTransactionIds_);
    } else {
        Load(context, LockingState_);
    }
}

void THunkStore::Lock(TTabletId tabletId)
{
    YT_VERIFY(HasHydraContext());

    auto newLockCount = ++TabletIdToLockCount_[tabletId];

    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Hunk store is locked by tablet (LockerTabletId: %v, LockCount: %v)",
        tabletId,
        newLockCount);
}

void THunkStore::Unlock(TTabletId tabletId)
{
    YT_VERIFY(HasHydraContext());

    auto newLockCount = --TabletIdToLockCount_[tabletId];
    YT_VERIFY(newLockCount >= 0);
    if (newLockCount == 0) {
        EraseOrCrash(TabletIdToLockCount_, tabletId);
    }

    YT_LOG_DEBUG_IF(IsMutationLoggingEnabled(),
        "Hunk store is unlocked by tablet (LockerTabletId: %v, LockCount: %v)",
        tabletId,
        newLockCount);
}

bool THunkStore::IsLockedByTablet(TTabletId tabletId) const
{
    return TabletIdToLockCount_.contains(tabletId);
}

void THunkStore::Lock(TTransactionId transactionId, EObjectLockMode lockMode)
{
    LockingState_.Lock(transactionId, lockMode);
}

void THunkStore::Unlock(TTransactionId transactionId, EObjectLockMode lockMode)
{
    LockingState_.Unlock(transactionId, lockMode);
}

bool THunkStore::IsLocked() const
{
    return
        !TabletIdToLockCount_.empty() ||
        LockingState_.IsLocked();
}

void THunkStore::SetWriter(IJournalHunkChunkWriterPtr writer)
{
    YT_VERIFY(!Writer_);
    Writer_ = std::move(writer);
    WriterOpenedFuture_ = Writer_->Open();
}

const IJournalHunkChunkWriterPtr THunkStore::GetWriter() const
{
    return Writer_;
}

bool THunkStore::IsReadyToWrite() const
{
    return Writer_ && WriterOpenedFuture_.IsSet();
}

void THunkStore::BuildOrchidYson(IYsonConsumer* consumer) const
{
    BuildYsonFluently(consumer).BeginMap()
        .Item("id").Value(Id_)
        .Item("state").Value(State_)
        .Item("marked_sealable").Value(MarkedSealable_)
        .Item("creation_time").Value(CreationTime_)
        .Item("last_write_time").Value(LastWriteTime_)
        .Item("tablet_locks").DoMapFor(TabletIdToLockCount_, [&] (auto fluent, auto lock) {
            fluent.Item(ToString(lock.first)).Value(lock.second);
        })
        .Item("locking_state").Do([&] (auto fluent) {
            LockingState_.BuildOrchidYson(fluent.GetConsumer());
        })
    .EndMap();
}

bool THunkStore::IsMutationLoggingEnabled() const
{
    return Tablet_->IsMutationLoggingEnabled();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTabletNode
