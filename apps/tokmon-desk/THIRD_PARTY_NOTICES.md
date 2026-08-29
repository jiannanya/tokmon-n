# tokmon-desk third-party notices

`tokmon-desk` is distributed with open-source components. The release process
must copy this file together with the complete license texts installed by vcpkg.

| Component | License family | Upstream |
|---|---|---|
| SDL3 | Zlib | https://github.com/libsdl-org/SDL |
| RmlUi | MIT | https://github.com/mikke89/RmlUi |
| Skia | BSD-3-Clause and bundled third-party notices | https://skia.org/ |
| FreeType | FTL or GPL-2.0-or-later | https://freetype.org/ |
| HarfBuzz | Old MIT | https://github.com/harfbuzz/harfbuzz |
| MD4C | MIT | https://github.com/mity/md4c |
| libgit2 | GPL-2.0-only with linking exception | https://github.com/libgit2/libgit2 |
| tree-sitter | MIT | https://github.com/tree-sitter/tree-sitter |
| MiSans VF | Xiaomi MiSans font license; see `assets/fonts/MiSans-NOTICE.txt` | https://hyperos.mi.com/font |
| agent-browser runtime (optional) | Apache-2.0 | https://github.com/vercel-labs/agent-browser |
| Ghostty `libghostty-vt` | MIT | https://github.com/ghostty-org/ghostty |

HarfBuzz does not impose an advertising clause or require an in-application
“About” disclaimer. Its copyright and permission notice must remain in the
distributed license materials. The same rule applies independently to every
bundled component according to its own license.

Chrome, Chromium, Chrome for Testing, and browser codecs are not covered by
agent-browser's Apache-2.0 license. The base `tokmon-desk` package does not
redistribute a browser; it uses a user-installed Chrome/Chromium executable.
