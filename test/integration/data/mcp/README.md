# Official MCP Protocol Schemas

These are unmodified copies of `schema/<version>/schema.json` from
[modelcontextprotocol/modelcontextprotocol](https://github.com/modelcontextprotocol/modelcontextprotocol/tree/271ecc9accafdd9b83a3c869fa67c22953b2af80/schema),
at commit `271ecc9accafdd9b83a3c869fa67c22953b2af80`. The upstream license notice
(including its MIT/Apache-2.0 transition terms) is included unchanged.
They are test fixtures, not runtime dependencies. Validation uses local references
and requires no schema downloads.

| Version | SHA-256 |
| --- | --- |
| 2025-06-18 | `af845e7e5b9d27107d1690f0936022546177a1403e63ffb11470135b296a2e01` |
| 2025-11-25 | `268a5f82ba70fd7e4b6dc4aa1e64f116f74b4d0edcb69dc046829c79dd4e97e7` |
| 2026-07-28 | `ef70b61f99b6d2e5e3b46863822eab08dff6a45bedc7a08914e0e5b133f40203` |

When adopting another protocol revision, update these fixtures and the explicit
wire-contract tests together. Do not relax upstream schemas to fit our responses.
`test-native-mcp.py` validates both JSON-RPC envelopes and method-specific results;
the envelope alone cannot detect missing `ListToolsResult` caching fields.
The independently pinned official Python SDK also exercises real discovery,
tool catalog decoding, caching, SSE results and legacy negotiation.
