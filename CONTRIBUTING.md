# Working on AMDRadeonRDNA4

## Branches

| Branch | What it is for |
|---|---|
| `main` | What is released. Every commit on it is a tagged version or a fix to one. |
| `next` | What is to be merged into `main` next. Work arrives here when it is finished. |
| `devel/*` | Development work, one branch a subject (`devel/vulkan`, `devel/multi-display`, ...). |
| `premetal/*` | Preparation for Metal support, etc. |

Work moves one way: `devel/*` or `premetal/*` into `next`, `next` into `main`.

- Start a branch from `next`.
- Bring a branch into `next` with a merge (`git merge --no-ff devel/x`), and bring `next` into a long-running branch
  the same way when it needs what is there.
- Do not copy commits between branches that both live on (cherry-pick, or a rebase of a branch somebody else has):
  the same change then exists twice under two names, and the next merge reports conflicts in files nobody
  changed. That happened in October 2026 with the display work, which was on `devel/multi-display` and, as copies,
  on `devel/vulkan`.
- A branch that has been merged and is not continued can be deleted; its commits stay reachable from `next`.

## Versions

The version is written in one place, `VERSION` in the `Makefile`. It goes into the bundle's `Info.plist` and the
kext's own version record, which have to agree for the kext to load.

- `0.MINOR.0`: `next` merged into `main`. `MINOR` goes up by one.
- `0.MINOR.PATCH`: a fix made on `main` between two such merges.
- `1.0.0` is not planned yet.

`CHANGELOG.md` has a section for the version `next` will become. Add to it when something is finished, and say what
of it has run on the card.

## Releasing

On `next`, with `VERSION` and `CHANGELOG.md` saying what is being released:

```bash
git checkout main
git merge --no-ff next
git tag -a v0.1.0 -m "AMDRadeonRDNA4 0.1.0"
git push origin main v0.1.0
```

Then, on `next`, raise `VERSION` to the following one and open its section in `CHANGELOG.md`.

## Which build is this?

Every build carries its version and the commit it was made from. `git describe` supplies the second part, so after
the first tag it reads like `v0.1.0-12-g1a2b3c4d`: twelve commits after 0.1.0, at commit `1a2b3c4d`. `-dirty` at the
end means the tree had changes that were not committed.

- Kernel log, first line of the driver: `AMDRadeonRDNA4: version 0.1.0 (a634d7d9-dirty)`
- Registry: `ioreg -r -w0 -l -k AMDRadeonRDNA4,Build`
- `tools/diagnostic-log.sh` prints it under "kext loaded?", next to the UUID from `kextstat`.

A test log should come from a committed build where possible: a `-dirty` build can only be told from another by its
UUID.
