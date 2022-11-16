package ru.yandex.yt.ytclient.proxy;

import ru.yandex.inside.yt.kosher.impl.ytree.object.serializers.YTreeObjectSerializer;
import ru.yandex.lang.NonNullApi;
import ru.yandex.lang.NonNullFields;
import ru.yandex.yt.ytclient.object.MappedRowSerializer;
import ru.yandex.yt.ytclient.object.WireRowSerializer;

@NonNullApi
@NonNullFields
public class MappedLookupRowsRequest<T> extends tech.ytsaurus.client.request.MappedLookupRowsRequest.BuilderBase<
        T, MappedLookupRowsRequest<T>> {
    public MappedLookupRowsRequest(String path, YTreeObjectSerializer<T> serializer) {
        this(path, MappedRowSerializer.forClass(serializer));
    }

    public MappedLookupRowsRequest(String path, WireRowSerializer<T> serializer) {
        setPath(path).setSerializer(serializer);
    }

    @Override
    protected MappedLookupRowsRequest<T> self() {
        return this;
    }

    @Override
    public tech.ytsaurus.client.request.MappedLookupRowsRequest<T> build() {
        return new tech.ytsaurus.client.request.MappedLookupRowsRequest<>(this);
    }
}
