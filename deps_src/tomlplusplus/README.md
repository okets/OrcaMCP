# toml++

The single-header release of [toml++](https://github.com/marzer/tomlplusplus) 3.4.0 (MIT, see `LICENSE`),
`toml.hpp` from tag v3.4.0 unchanged (SHA-256 `6b5172ad4dd6519aec67b919181fa7a38a2234131e5b2afa232dfe444819783e`).

OrcaMCP's Connect uses it to read Codex's `config.toml` and to check that an edit changed only the
`[mcp_servers.orca-slicer]` entry (`src/slic3r/GUI/OrcaMCP/OrcaMCPCodexToml.cpp`, the only file that includes it).
