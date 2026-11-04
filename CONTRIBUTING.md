# Contributing

## Build

```bash
./mk                                  # release build + run
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

Unit tests are enabled by default (`CROSSFADE_BUILD_TESTS=ON`):

```bash
ctest --test-dir build --output-on-failure
```

The Python backend has its own protocol test:

```bash
python3 tests/test_backend_protocol.py
```

## Ground rules

- **C++17**, vendored ImGui/stb/SQLite/httplib/qrcodegen. Discuss new
  dependencies first.
- **No mass reformatting.** Match the surrounding file; a clang-format sweep
  destroys blame and reviewability.
- **Persistence** belongs in the config directory, never next to the binary.
- **Backend protocol** is one command per line and one JSON reply per command.
  Both auth entry points call `py/cf_backend/core.py`; put shared commands
  there, not in the entry points.
- **Threads:** UI-owned vectors/maps stay on the UI thread. Workers publish
  `shared_ptr<const vector<...>>` snapshots or call `tasks::spawn`. See
  [`docs/THREADING.md`](docs/THREADING.md).
- **Commits:** `area: lowercase summary`, with a body explaining the *why*.
  Keep refactors and behavior changes separate.

## Checks before opening a PR

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
python3 tests/test_backend_protocol.py
./scripts/party-smoke-test.sh
```

`scripts/bench.sh` prints binary/source/test sizes and idle runtime numbers.
