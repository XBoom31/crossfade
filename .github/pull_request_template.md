## What changed

## Why

## Checks
- [ ] `cmake --build build -j"$(nproc)"`
- [ ] `ctest --test-dir build --output-on-failure`
- [ ] `python3 tests/test_backend_protocol.py`
- [ ] `./scripts/party-smoke-test.sh`
- [ ] No auth files, `imgui.ini`, `library.db` or caches committed

## Screenshots / notes
