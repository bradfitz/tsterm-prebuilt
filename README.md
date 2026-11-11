# tsterm-prebuilt

Prebuilt assets for [tsterm](https://github.com/bradfitz/tsterm), each a Go
module that embeds its asset, so that tsterm depends on them like any Go
module and its own repository doesn't carry the history of binaries that
change with each rebuild. Each directory has the source and script that build
its asset, and its own licenses.

| Module | Asset |
|---|---|
| [`ghosttyvt`](ghosttyvt) | [libghostty-vt](https://ghostty.org) compiled to WebAssembly, with glue for tsterm. Rebuild with `ghosttyvt/build.sh`, which fetches pinned versions of Zig and libghostty-vt; builds are reproducible. Licenses: `ghosttyvt/LICENSE` (Ghostty's) and `ghosttyvt/THIRD_PARTY_LICENSES`. |
