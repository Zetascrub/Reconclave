# Public source release checklist

Reconclave is distributed as source for independently provisioned fleets.
A public source release does not include usable fleet keys or preconfigured
firmware. See [release tooling](releasing.md) for packaging and verification.

## Repository contents

- Code/documentation use MIT, with reserved mascot artwork rights stated in
  the root README and beside the image assets.
- Setup generates a new private trust store before compiling device firmware.
- Deployment headers, keys, firmware outputs, evidence, captures and local
  databases/logs must stay outside the committed source tree.
- CI runs a committed-file policy check and a redacted history secret scan.
  It compiles firmware with disposable test identities, without publishing or
  caching firmware binaries or generated trust headers.
- Public release packages contain committed source, licence notices and the
  public verification key. The signing private key stays outside the repo.

## Checks for each release

1. Review the exact source commit and confirm a clean working tree.
2. Run all CI jobs, including the public-tree policy and history secret scan.
3. Inspect the source archive and verify its signature against a trusted key.
4. Check screenshots, example data and metadata for private target information.
5. Review retained Actions logs and any existing GitHub artifacts or releases.
6. Confirm the hardware/test matrix and known limitations are accurate.
7. Maintain a private offline backup of the signing key and deployment stores.

## Before changing visibility

Repository visibility is a separate, deliberate action. Review historical
commit attribution and documents as well as the current tree. Changing a file
on main does not remove its previous contents from history. Any history rewrite
needs a coordinated plan, including old signatures and downstream clones.

Enable private vulnerability reporting when the repository becomes public and
verify the form linked in SECURITY.md. Confirm available secret scanning and
push protection settings. Do not assume those services are active merely
because the policy file exists.

## Security limits

A clean secret scan is useful evidence, not proof that every sensitive value
has been found. Scope, authentication and parsing still need security review;
compilation and native tests do not establish physical interoperability.

Fleet HMAC authentication does not hide discovery or encrypt HTTP. Detached
publisher signatures are verified off-device and are separate from the OTA
trust mechanism. Hardware Secure Boot is not enabled by these tools.
