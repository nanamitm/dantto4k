# PC/SC cleanup at Windows process exit

During TBS6812 dual-receiver testing, the October 21 deployed BonDriver binary
sometimes remained alive after CloseTuner, Release and FreeLibrary returned.
A non-invasive debugger snapshot found the main thread in:

```
ntdll!LdrShutdownProcess
  CRT static destruction
    WinSCard!SCardDisconnect
      WinSCard!CSCardSubcontext::WaitForAvailable
```

DLL_PROCESS_DETACH now records whether its reserved argument is non-null
(process termination). LocalSmartCard skips Disconnect, ReleaseContext and module
release only in that case. Windows reclaims the process resources. Normal object
destruction and explicit DLL unloading retain card/context cleanup; the existing
loader-lock guard still applies to FreeLibrary.

Validation on Windows x64: Release CLI/DLL build; all 11 regression suites;
two receiver processes using one shared reader, with 4K/8K selection, closing
and reopening the other tuner in both orders. All six rebuilt-DLL probe runs
exited with code 0. This is a focused process-termination fix, not a redesign of
the DLL's general unload or reader lifetime.
