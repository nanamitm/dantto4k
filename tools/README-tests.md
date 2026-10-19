# Windows regression tests

Run from PowerShell 7 with Visual Studio C++ x64 tools and initialized
submodules installed:

```powershell
.\tools\test-regressions.ps1
```

The script builds the Release x64 solution, compiles each test against the
production objects, and fails on the first unsuccessful build or test. Compiler
response files and executables use `work/regression-tests`.
No tuner, smart card, CasProxy service, or broadcast recording is required.

After an existing Release build, individual suites can be selected:

```powershell
.\tools\test-regressions.ps1 -SkipBuild -Tests acas,casproxy
```

| Suite | Regression covered |
| --- | --- |
| `recorder` | Active media/map paths, case aliases and hard links cannot be reused, even with overwrite enabled; independent sessions work and stopped paths can be reused. |
| `output` | CLI output preserves all data and a closed stdout pipe returns a failure; buffered writes expose short-write and final flush errors. |
| `paths` | Relative aliases, case differences, hard links and map/media collisions are rejected without modifying input or existing output; separate outputs still work. |
| `acas` | Successful ECM publishes keys; failed ECM preserves ciphertext; repeats of a failed ECM stay off the card for 1 s while a new ECM is sent at once; the current parity keeps decrypting while a new ECM is on the card and only a parity switch waits; cancellation discards old generations and never waits for blocked card I/O. |
| `casproxy` | Refused/fast connections, eight concurrent callers, duplicate replies, EOF, explicit close, and a bounded request timeout all recover without replacing the client. |
| `mmtp` | Multi-type MMTP extensions in either order, malformed lengths, parser reuse, and decoded TLV flag placement. |
| `demux` | A partially parsed MPT cannot remove existing streams; truncated ECM never reaches handlers; the next complete MPT remains usable. |
| `bontuner` | Failed pointer calls return empty outputs; failed copy calls preserve the destination; successful calls return TS; purge and selection reset buffered input and PCR suppression. |
| `ttml` | Existing parser, style inheritance, line break and DRCS regression tests. |
| `appdata` | The application service descriptor gives the MH-AIT, data transmission message and EMT packet_ids, with and without a data transmission message; an EMT whose descriptors have 16-bit lengths parses; app data sections are routed to their service, held back while unchanged and resent after the interval; item sections split, number and CRC correctly and oversized items are refused. |

The CasProxy suite deliberately waits for a request timeout and repeated refused
TCP connections, so it can take a few minutes on Windows. The loopback server
listens only on `127.0.0.1`. The path suite removes only its unique temporary
directory after checking that the cleanup target remains under the temp root.
