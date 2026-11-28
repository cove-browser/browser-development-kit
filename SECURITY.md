# Security policy

## Reporting a vulnerability

Report a vulnerability in BDK privately, through GitHub: on this repository's **Security** tab, choose
**Report a vulnerability**. Please do not open a public issue, pull request or discussion for it.

Include what is affected (the runtime, the Swift SDK, the C ABI, the `bdk` CLI, a Chromium patch of BDK's, or
the JavaScript packages), the BDK version or commit, and the steps or code that show the problem. You will get an
acknowledgement, and the fix is made on a private fork, released, and then disclosed in a GitHub security
advisory that credits you unless you ask otherwise.

**Chromium's own bugs** go to Chromium, not here: <https://www.chromium.org/Home/chromium-security/reporting-security-bugs/>.
A bug that is in Chromium and also reachable in a BDK runtime is fixed in BDK by adopting Chromium's fix.

## Supported versions

BDK follows Chromium's stable channel on macOS. Security fixes are released for the newest BDK release only, as
a patch release on its Chromium milestone; there is no fixed response time, and a release that Google rates
Critical or reports as exploited in the wild comes first. See
[docs/release/security-patches.md](docs/release/security-patches.md) for the process and
[docs/release/versioning.md](docs/release/versioning.md) for version numbers.

BDK never updates itself: a fix reaches people only when each application built with BDK ships an update with
the new runtime.
