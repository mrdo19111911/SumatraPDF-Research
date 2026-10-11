# Hidden Windows tests

Run suitable tests on a separate Win32 desktop without switching the user's display:

```powershell
$env:SUMATRA_TEST_EXE = 'E:\Work\SumatraPdf\out\rel64\SumatraPDF.exe'
bun tests/run-hidden.ts tests/ad-hoc-research-assistant.ts --no-build
```

The child verifies its desktop before starting the test. Failure never falls back to the visible desktop. The existing harness still passes `-for-testing`; logs go to `out/hidden-tests/`. A ten-minute deadline stops a hung launcher.

Verified: research assistant, Reading DOM/font/math/theme, source links, original-page image loading and provider-history checks. Cursor/input-desktop tests need a normal visible run; announce these before running them. This runner is for ad-hoc checks, not registered in the normal suites.

Win32 APIs: [CreateDesktopW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-createdesktopw), [STARTUPINFOW.lpDesktop](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/ns-processthreadsapi-startupinfow).
