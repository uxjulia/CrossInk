# WebDAV credential regression checks

Run `python3 test/scripts/test_webdav_credentials.py` from the repository root.
The driver compiles the complete production handler, shared ProtectedPaths policy and header plus production
`FsHelpers::normalisePath` with C++17, warnings-as-errors, ASan and UBSan. It
substitutes Arduino/WebServer/HAL types and deterministic FAT-style storage.

Coverage includes request and Destination paths across GET, HEAD, PROPFIND, PUT,
DELETE, MKCOL, MOVE and COPY; raw upload start/write/end/abort; percent decoding,
case/space/dot spellings, numbered and hashed 8.3 aliases, credential directories,
JSON and legacy stores and backups, Show Hidden Files, filtered directory listings, ordinary
tilde names and direct long Unicode filenames. The local shared policy still
rejects numbered/hashed 8.3-looking names at credential depth. Successful
PUT/COPY/MOVE/DELETE, protected-book moves and MOVE rollback remain exercised.

This does not test a real SdFat volume or network server. On X3/X4 and an S3
reader, use a disposable FAT card with test credentials, enable File Transfer,
and verify 403 for direct/alias credential GET and PROPFIND (including leading
spaces/trailing dots), source/destination mutations and interrupted uploads.
Verify normal books and ordinary tilde names (such as image~photo.jpg) transfer,
protected books retain their sidecars when moved, and hidden user files/cache
paths follow Show Hidden Files. Credential-container browsing should omit stores,
and moving/copying/deleting the container must fail. No cache reset is required.
