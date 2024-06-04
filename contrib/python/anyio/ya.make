# Generated by devtools/yamaker (pypi).

PY3_LIBRARY()

VERSION(4.4.0)

LICENSE(MIT)

PEERDIR(
    contrib/python/idna
    contrib/python/sniffio
)

NO_LINT()

NO_CHECK_IMPORTS(
    anyio._backends._trio
    anyio.pytest_plugin
)

PY_SRCS(
    TOP_LEVEL
    anyio/__init__.py
    anyio/_backends/__init__.py
    anyio/_backends/_asyncio.py
    anyio/_backends/_trio.py
    anyio/_core/__init__.py
    anyio/_core/_eventloop.py
    anyio/_core/_exceptions.py
    anyio/_core/_fileio.py
    anyio/_core/_resources.py
    anyio/_core/_signals.py
    anyio/_core/_sockets.py
    anyio/_core/_streams.py
    anyio/_core/_subprocesses.py
    anyio/_core/_synchronization.py
    anyio/_core/_tasks.py
    anyio/_core/_testing.py
    anyio/_core/_typedattr.py
    anyio/abc/__init__.py
    anyio/abc/_eventloop.py
    anyio/abc/_resources.py
    anyio/abc/_sockets.py
    anyio/abc/_streams.py
    anyio/abc/_subprocesses.py
    anyio/abc/_tasks.py
    anyio/abc/_testing.py
    anyio/from_thread.py
    anyio/lowlevel.py
    anyio/pytest_plugin.py
    anyio/streams/__init__.py
    anyio/streams/buffered.py
    anyio/streams/file.py
    anyio/streams/memory.py
    anyio/streams/stapled.py
    anyio/streams/text.py
    anyio/streams/tls.py
    anyio/to_process.py
    anyio/to_thread.py
)

RESOURCE_FILES(
    PREFIX contrib/python/anyio/
    .dist-info/METADATA
    .dist-info/entry_points.txt
    .dist-info/top_level.txt
    anyio/py.typed
)

END()
