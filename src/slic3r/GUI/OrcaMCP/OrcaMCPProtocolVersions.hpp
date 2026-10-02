// src/slic3r/GUI/OrcaMCP/OrcaMCPProtocolVersions.hpp
#pragma once
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

// Which MCP protocol versions OrcaMCP speaks, and the one it answers a client with.
//
// MCP's version negotiation: a client's initialize names the version it wants. A server that supports
// it MUST answer with that version, otherwise with another it supports, its newest. Over HTTP the client
// then names the agreed version in an MCP-Protocol-Version header on every later request, and a server
// MUST answer an unsupported one with 400. Through v2.5.0.9 the app answered 2024-11-05 whatever was
// asked, and the bridge echoed whatever was asked, versions it had never heard of included.
//
// Only the versions OrcaMCP meets are listed, newest first:
//  - 2025-06-18.
//  - 2024-11-05, for older clients.
// Not 2025-03-26: a server of that version MUST accept JSON-RPC batches (2025-06-18 dropped them), and
// neither the app nor the bridge does. Not 2025-11-25: it returns a tool's argument refusals as tool
// errors (isError) for the model to correct, where OrcaMCP refuses them with JSON-RPC -32602
// (OrcaMCPToolArguments.hpp).
//
// The bridge answers initialize itself, by the copy OrcaMCPServer::tools_manifest() writes into
// scripts/orcamcp_tools.json.
//
// Unit-tested in tests/slic3rutils/test_mcp_protocol_versions.cpp.

namespace Slic3r { namespace GUI { namespace OrcaMCP {

// Every MCP protocol version OrcaMCP speaks, newest first.
const std::vector<std::string>& supported_protocol_versions();

// The version initialize answers: the one `initialize_params` asks for (its protocolVersion) when OrcaMCP
// speaks it, otherwise the newest it speaks.
std::string negotiated_protocol_version(const nlohmann::json& initialize_params);

// Why a request's MCP-Protocol-Version header is refused, or nothing when it names a supported version or
// the request has none (a client of 2025-03-26 or older, and the bridge, send none).
std::optional<std::string> protocol_version_header_refusal(const std::optional<std::string>& header);

}}} // namespace Slic3r::GUI::OrcaMCP
