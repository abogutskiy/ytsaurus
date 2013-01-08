import sys

if sys.version_info[:2] == (2, 6):
    import zipfile
    class ZipFile(zipfile.ZipFile):
        def __init__(self, file, mode="r", compression=zipfile.ZipFile.ZIP_STORED, allowZip64=False):
            super(self, ZipFile).__init__(file, mode, compression, allowZip64)
        def __enter__(self):
            return self

        def __exit__(self, type, value, traceback):
            self.close()

else:
    from zipfile import ZipFile

