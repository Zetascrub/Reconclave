# Public-release readiness review

Reviewed 8 September 2026. Repository remains private. No visibility, branch,
key, licence or GitHub settings were changed. This is a publication-readiness
review, not a full application vulnerability audit or legal clearance.

## Verdict

The project is a credible candidate for a public development preview after a
release-preparation pass. Do not describe it as a production-ready release yet.
There is no confirmed committed secret finding from this review, and no evidence
here requiring a history rewrite. Resolve the items below before switching
visibility, and review the exact final commit rather than relying on this snapshot.

## Evidence checked

- GitHub reports `Zetascrub/Reconclave` as PRIVATE, default branch `main`, no
  description and no detected licence. No releases or Actions artifacts were
  listed, and the issues endpoint returned no entries.
- Latest listed CI run passed for commit `47bfa776ff4ad0bd563da34da3517635d4ea27f7`.
  Workflow covers native tests, desktop Python tests, web build and Cardputer
  compilation with throwaway generated trust. It does not build the P4 firmware.
- Gitleaks 8.30.1, downloaded from its official release and verified against
  the release checksum, reported zero findings across all local refs (27
  commits) and zero across a separate 117-file export of tracked and unignored
  source files. Reports were fully redacted and retained outside the repo.
  GitHub listed only `main`; local refs include additional worktree branches.
- Generated deployment headers and the provisioning/data directories are ignored.
  Neither generated header is tracked; history path inspection did not show them.
- Existing native tests and firmware builds passed during the preceding changes;
  this review did not repeat a full clean-room installation or hardware validation.

## Address before publication

### 1. Decide the intended licence and asset terms

There is no root LICENSE and GitHub detects no licence. Making source visible
is different from giving others permission to reuse it. Choose the intended
terms deliberately; do not let a licence template silently decide this.
Document the mascot/artwork terms separately if they differ from the code.
Confirm provenance for artwork and any copied code, and assemble dependency
notices. A spot check found MIT declarations for RadioLib and M5Unit-NFC; that
is not a complete transitive dependency/licence audit. Consulting Evil-M5project
is not itself proof that its code was copied or that its licence applies here.

### 2. Fix and verify newcomer setup

The root README tells users to flash devices and then provision trust. The
Cardputer source unconditionally includes generated_trust.h, but the Cardputer
Build section starts directly with pio run and omits header generation. CI
already handles this prerequisite. Put provisioning before building/flashing,
explain device IDs and stable key storage, and test the documented steps from
an isolated clean clone with new development identities.

Some device documentation still describes older navigation and pairing flows.
Reconcile these with the current firmware before publishing screenshots or a
quick-start guide. The desktop documentation also describes inline environment
assignments as avoiding shell history: typed assignments can still enter shell
history, so that guidance needs correction.

### 3. Establish a public security-reporting route

No SECURITY.md or private reporting instructions were found. Define supported
versions and a private contact/reporting route before inviting reports. Do not
promise response deadlines that cannot be maintained. The repository API did
not return security_and_analysis settings, so this review does not claim that
GitHub secret scanning or push protection is enabled or disabled.

### 4. Finish source hygiene and the release snapshot

The recent Cardputer features include both modified tracked files and untracked
source/tests. They are not in the currently passing GitHub commit. Review and
commit the intended source, then run CI on that exact commit.

The ignore file protects known deployment paths, but does not ignore .env or
arbitrary evidence output directories, including the README's ./evidence
example. Add deliberate exclusions for local secret/evidence outputs, retaining
explicitly safe examples. Consider a CI secret scan as a continuing check.

Review personal details in docs and commit metadata. A local absolute path is
present in docs/platform-roadmap.md; the current firmware review also contains
physical device identifiers. These are not authentication secrets, but their
publication should be intentional. Review screenshots and sample evidence too.

### 5. Calibrate public claims

The README says most of the platform is complete and running on hardware and
makes broad authentication/scope guarantees. Separate implemented features,
automated validation, device-tested behaviour and experimental work. Existing
firmware review notes already identify physical NFC validation and foreground
network blocking as outstanding. A clear development-preview label and a
supported hardware/test matrix would set more accurate expectations.

## Firmware downloads need a separate release process

`tools/provision_fleet.py` writes pairwise authentication keys and per-device
storage keys into generated C headers. Firmware compiled with those headers
contains that material even though Git ignores the headers. Do not upload the
locally deployed .bin/.elf images or provisioning directory as public releases.
Publish source first, or implement a release build/provisioning process that
never distributes usable private fleet keys. Throwaway CI identities are for
compilation checks, not a shared production fleet identity.

## Useful polish after the essentials

- Add a repository description, concise hardware overview and current screenshots.
- Add contribution guidance and focused bug-report templates.
- Give the preview a version/changelog tied to a tested commit.
- Add P4 build coverage when its CI toolchain is reproducible.
- Declare least-privilege Actions permissions and consider immutable action pins.
- Keep the npm lockfile (already present); prefer npm ci in reproducible setup.
  Review broad Python version ranges and npm latest specifications as an update
  policy, rather than assuming they are inherently current or vulnerable.

## Final visibility gate

Review retained Actions logs before publication: GitHub states that Actions
history and logs become public when repository visibility changes. This review
checked run metadata, not every log. Also review any content added after this
snapshot, including releases, attachments and issue discussions. Secret scans
are pattern-based and do not establish absence of all sensitive data. A full
application security review, dependency vulnerability analysis and artwork
provenance review remain separate work.

## References

- [GitHub: setting repository visibility](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/managing-repository-settings/setting-repository-visibility)
- [GitHub: licensing a repository](https://docs.github.com/en/repositories/managing-your-repositorys-settings-and-features/customizing-your-repository/licensing-a-repository)
- [Gitleaks scanner and usage](https://github.com/gitleaks/gitleaks)


## Preparation follow-up

The subsequent preparation added SECURITY.md, CONTRIBUTING.md, a bug template,
third-party inventory, changelog and release guide. Setup now generates private
trust before firmware compilation. Known deployment outputs and .env files are
ignored; provisioning validates IDs/keys and writes headers with owner-only
permissions. Public-facing example device IDs and a local absolute path were
sanitised without rewriting history. The GitHub description was updated; the
repository remains private.

Release tooling now creates Ed25519 signatures with a separate key stored
outside the checkout. Only the public key is included in source. Verification
checks artifact bytes and signed metadata, and the source packager rejects
known deployment files, private PEM material and an uncommitted tree. Current
firmware still uses its existing OTA authentication; detached publisher
signatures are not enforced by the device, and no eFuses were changed.

All 9 retained Actions run logs were downloaded and scanned with Gitleaks:
zero matches. This is a secret-pattern check, not manual clearance of every
log line. Local tests/builds passed during preparation; exact public release CI
still requires committing and pushing the final selected source.

Remaining owner/release gates: code licence choice, mascot rights/provenance,
offline signing-key backup, final source commit and CI, and private vulnerability
reporting enablement at the explicitly authorised public-visibility change.
No code licence was selected on the owner's behalf.


Final preparation validation: 20 provisioning/release tests passed; the desktop
suite ran 200 tests with 2 skips; all 5 native suites passed; web and Cardputer
builds passed. A fresh isolated source snapshot successfully generated synthetic
trust inputs. The final 129-file prospective source export passed Gitleaks with
zero findings after eliminating a scanner false positive in the PEM-detection
implementation. This does not establish absence of every possible secret.
The actual Cardputer image was signed and verified privately with a signed
uncommitted-source flag; it was not published or reflashed during preparation.


## Licence decision

The owner approved MIT for code, with mascot artwork rights reserved. Added a
standard root MIT LICENSE, explicit ARTWORK_LICENSE.md and notices beside both
the original images and generated pixel data. README and dependency notices
make this exception visible. The earlier pending-licence gate is resolved;
rights/provenance of third-party material still require the usual care.
