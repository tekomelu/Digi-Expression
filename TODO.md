# To do: Modwerk integration

Status on 7 November 2026: Digi Expression 0.13.0-experimental is in the Modwerk pull request from tekomelu/modwerk, branch `digitakt-midi-expression`. Every item of the owner's first review is done, `npm run pr:ready` says Ready, and the 0.13.0 unit check passed (11 of 11, TESTING.md).

1. **Post the reply on the pull request** (the owner's review): what changed, "what the guides lacked" (capture.json needs `media/` paths), the 0.13.0 unit check 11 of 11, and the offer to build a shared list of pages behind TRIG in the core.
2. **Wait** for the owner to approve the workflow run and review again.
3. **If CI's `elemod-source` check fails:** the package was built natively, not in Modwerk's Docker image. Rebuild it the CI way (`scripts/build-elemod-isolated.sh`) or ask the owner for the CI-built one, then update `sdk/imports/packages/midi-expression/`, its record and the pin's sha256.
4. **If main moves before the merge:** merge Modwerk's main into the branch, run `npm run pr:ready` again, push.
5. **Optional:** the shared "pages behind TRIG" core hook, as its own pull request, if the owner wants it. Digi Expression then moves its page there and keeps the SETTINGS row.

Working notes: the session that can push needs the fork attached alone (two repos named `modwerk` can't share a session). In a cloud workspace npm needs the proxy for the registry: `npm ci --noproxy=localhost --proxy=$HTTPS_PROXY --https-proxy=$HTTPS_PROXY`.
