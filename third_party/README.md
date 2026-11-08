# Third-party code

| Library | Version | License | Used for |
|---|---|---|---|
| [toml++](https://github.com/marzer/tomlplusplus) | v3.4.0 | MIT (`tomlplusplus/LICENSE`) | Parsing record format files (`formats_toml.h`) |

`tomlplusplus/toml.hpp` is the upstream single-header build, unmodified:

- Source: https://raw.githubusercontent.com/marzer/tomlplusplus/v3.4.0/toml.hpp
- SHA-256: `6B5172AD4DD6519AEC67B919181FA7A38A2234131E5B2AFA232DFE444819783E`

To update, download the header and LICENSE from the new tag, then update the
version and hash above.

The QML edition compiles WinShun controls and Phosphor fonts from the commit pinned in `release.json`. Their MIT notices ship as `WinShun-LICENSE` and `Phosphor-LICENSE`. Qt itself is supplied by the host.
