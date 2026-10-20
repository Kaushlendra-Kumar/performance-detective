# Third-party components

All dependencies are vendored in `third_party/` so the project builds offline.

| Component | Version | File(s) | License | Used for |
|---|---|---|---|---|
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | 0.18.3 | `third_party/httplib.h` | MIT | local HTTP server (and HTTP client in tests) |
| [nlohmann/json](https://github.com/nlohmann/json) | 3.11.3 | `third_party/nlohmann/json.hpp` | MIT | JSON serialization and parsing |
| [Catch2](https://github.com/catchorg/Catch2) | 3.7.1 (amalgamated) | `third_party/catch_amalgamated.{hpp,cpp}` | BSL-1.0 | unit and integration tests (test-only) |

GoogleTest was the first choice for tests but could not be downloaded in the build environment;
Catch2 provides the same capabilities.

## System tools and libraries (optional at runtime)

| Tool | Package (Debian/Ubuntu) | Effect if missing |
|---|---|---|
| `addr2line` | `binutils` | no source file/line for functions |
| glibc debug symbols | `libc6-dbg` | glibc-internal functions appear as `libc.so.6+0x…`; thread stacks may be classified as "unknown" completeness |
| Node.js ≥ 18 | `nodejs` | frontend unit tests (`ctest -R frontend`) cannot run; the UI itself needs only a browser |

The frontend uses no external libraries, fonts or CDNs.
