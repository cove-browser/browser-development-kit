# Release documents

How BDK is released and what a release promises. Written in M9 (release hardening). The planning documents in
[../planning](../planning/README.md) remain the design; these are the procedures and the published policies,
in Markdown so the documentation site (`apps/docs`) can link or import them.

| Document | For | Task |
|---|---|---|
| [procedure.md](procedure.md) | Maintainers: cutting a release by hand, and the dry run `chromium:release-check` | T-PKG-06 |
| [security-patches.md](security-patches.md) | Everyone: the security-patch policy; maintainers: the process and the rehearsal checklist | T-REL-01 |
| [versioning.md](versioning.md) | Application developers: SemVer, one version for everything, the Chromium version, upgrades | T-REL-02 |
| [compatibility.md](compatibility.md) | Application developers: what differs from Chrome and what is not supported | T-REL-02 |
| [experimental-apis.md](experimental-apis.md) | Application developers: what may change, and how it is marked | T-REL-02 |
| [licence-review.md](licence-review.md) | Maintainers and counsel: what ships under which terms, notices, D4, D5, D7 | T-REL-02 |
| [cross-platform-review.md](cross-platform-review.md) | Maintainers: the ABI and service contracts reviewed for Windows, Linux and .NET | T-XPL-01 |

When behaviour changes, update the page that describes it in the same change. Each "not done" names a task.
