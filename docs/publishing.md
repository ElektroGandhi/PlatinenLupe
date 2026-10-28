# Publishing (maintainers)

The repository lives on a self-hosted forge first. Going public is one script — from a cleaned copy, the
original history is never rewritten.

## Once

1. Create an **empty** repository on GitHub (no README, no licence — they would conflict).
2. A token with *Contents: read & write* and **Workflows: read & write** (without it GitHub refuses
   `.github/workflows/`). Pages and releases need *Pages* and *Administration* on first setup.
3. Put your own identifiers (home network, domains, real names) into
   `~/.config/platinenlupe/privacy-patterns.txt`, one regular expression per line. That file never goes
   into the repository.

## Every time

```sh
PL_DRY_RUN=1 sh scripts/publish.sh https://github.com/<you>/Platinenlupe.git   # check only
sh scripts/publish.sh https://github.com/<you>/Platinenlupe.git                # check, then push
```

The script clones main, rewrites every author/committer without a *noreply* address to your public
identity (merge commits created by the forge carry the account's real e-mail), runs
`scripts/privacy_check.py` over the **whole** cleaned history and only pushes when it finds nothing.

Merge pull requests on the forge **fast-forward only** where possible — then no merge commits with the
account's address are created in the first place.

## After the first push

- Settings → Pages → Source: **GitHub Actions**. The `pages` workflow publishes the demo at `/` and the
  browser flasher with freshly built firmware at `/flasher/`. (Pages on a free account needs a public repo.)
- Tag a version (`git tag v1.0.0 && git push --tags` via the script) — the `release` workflow attaches
  firmware images and the printable case.
- Then replace "open `web/index.html` from disk" in the README with the Pages link.
