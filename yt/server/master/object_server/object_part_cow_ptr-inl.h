#ifndef OBJECT_PART_COW_PTR_INL_H_
#error "Direct inclusion of this file is not allowed, include object_part_cow_ptr.h"
// For the sake of sane code completion.
#include "object_part_cow_ptr.h"
#endif

namespace NYT::NObjectServer {

///////////////////////////////////////////////////////////////////////////////

template <class TObjectPart>
TObjectPart TObjectPartCoWPtr<TObjectPart>::DefaultObjectPart;

template <class TObjectPart>
TObjectPartCoWPtr<TObjectPart>::~TObjectPartCoWPtr()
{
    YCHECK(!ObjectPart_);
}

template <class TObjectPart>
TObjectPartCoWPtr<TObjectPart>::operator bool() const
{
    return static_cast<bool>(ObjectPart_);
}

template <class TObjectPart>
inline const TObjectPart& TObjectPartCoWPtr<TObjectPart>::Get() const
{
    return ObjectPart_ ? *ObjectPart_ : DefaultObjectPart;
}

template <class TObjectPart>
TObjectPart& TObjectPartCoWPtr<TObjectPart>::MutableGet(
    const NObjectServer::TObjectManagerPtr& objectManager)
{
    MaybeCopyOnWrite(objectManager);
    return *ObjectPart_;
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::Assign(const TObjectPartCoWPtr& rhs, const NObjectServer::TObjectManagerPtr& objectManager)
{
    if (ObjectPart_ == rhs.ObjectPart_) {
        return;
    }

    Reset(objectManager);
    ObjectPart_ = rhs.ObjectPart_;

    if (ObjectPart_) {
        ObjectPart_->Ref();
    }
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::Reset(const NObjectServer::TObjectManagerPtr& objectManager)
{
    if (ObjectPart_) {
        ObjectPart_->Unref();
        if (ObjectPart_->GetRefCount() == 0) {
            TObjectPart::Destroy(ObjectPart_, objectManager);
        }

        ObjectPart_ = nullptr;
    }
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;

    if (ObjectPart_) {
        auto it = context.SavedInternedObjects().find(ObjectPart_);
        if (it == context.SavedInternedObjects().end()) {
            Save(context, NHydra::TEntitySerializationKey::Inline);
            Save(context, *ObjectPart_);
            YCHECK(context.SavedInternedObjects().emplace(ObjectPart_, context.GenerateSerializationKey()).second);
        } else {
            Save(context, it->second);
        }
    } else {
        Save(context, NHydra::TEntitySerializationKey::Null);
    }
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;

    YCHECK(!ObjectPart_);

    auto key = Load<NHydra::TEntitySerializationKey>(context);
    if (key == NHydra::TEntitySerializationKey::Null) {
        SERIALIZATION_DUMP_WRITE(context, "objref <null>");
    } else if (key == NHydra::TEntitySerializationKey::Inline) {
        ResetToDefaultConstructed();
        SERIALIZATION_DUMP_INDENT(context) {
            Load(context, *ObjectPart_);
        }
        context.RegisterEntity(ObjectPart_);
        SERIALIZATION_DUMP_WRITE(context, "objref %v", key.Index);
    } else {
        ObjectPart_ = context.template GetEntity<TObjectPart>(key);
        // NB: this only works iff the ref counter is embedded into the object.
        // This is essentially the same as re-wrapping raw ptrs into intrusive ones.
        ObjectPart_->Ref();
        SERIALIZATION_DUMP_WRITE(context, "objref %v", key.Index);
    }
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::MaybeCopyOnWrite(const NObjectServer::TObjectManagerPtr& objectManager)
{
    if (!ObjectPart_) {
        ResetToDefaultConstructed();
    } else if (ObjectPart_->GetRefCount() > 1) {
        auto* objectPartCopy = TObjectPart::Copy(ObjectPart_, objectManager);
        objectPartCopy->Ref();

        ObjectPart_->Unref();
        ObjectPart_ = objectPartCopy;

        YCHECK(ObjectPart_->GetRefCount() == 1);
    }

    YCHECK(ObjectPart_ && ObjectPart_->GetRefCount() == 1);
}

template <class TObjectPart>
void TObjectPartCoWPtr<TObjectPart>::ResetToDefaultConstructed()
{
    YCHECK(!ObjectPart_);
    ObjectPart_ = new TObjectPart();
    ObjectPart_->Ref();
    Y_ASSERT(ObjectPart_->GetRefCount() == 1);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
