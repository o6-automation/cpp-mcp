#include "mcp_server.h"
#include "base64.hpp"

#include <algorithm>
#include <cctype>
#include <optional>

namespace {

using mcp::json;

json protocol_error(const json& id, mcp::error_code code,
                    const std::string& message, const json& data = json::object()) {
    return mcp::response::create_error(id, code, message, data).to_json();
}

bool plain_header_value(const std::string& value) {
    if (value.empty() || value.front() == ' ' || value.back() == ' ' ||
        value.front() == '\t' || value.back() == '\t') {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return c == '\t' || (c >= 0x20 && c <= 0x7e);
    });
}

std::optional<std::string> decode_mirrored_value(const std::string& value) {
    constexpr const char* prefix = "=?base64?";
    constexpr const char* suffix = "?=";
    if (value.compare(0, 9, prefix) == 0) {
        if (value.size() < 11 || value.compare(value.size() - 2, 2, suffix) != 0) {
            return std::nullopt;
        }
        const std::string encoded = value.substr(9, value.size() - 11);
        try {
            const std::string decoded = base64::decode(encoded, base64::alphabet::standard);
            if (base64::encode(decoded) != encoded) {
                return std::nullopt;
            }
            return decoded;
        } catch (const base64_error&) {
            return std::nullopt;
        }
    }
    if (!plain_header_value(value)) {
        return std::nullopt;
    }
    return value;
}

bool valid_header_token(const std::string& token) {
    if (token.empty()) return false;
    static const std::string punctuation = "!#$%&'*+-.^_`|~";
    return std::all_of(token.begin(), token.end(), [&](unsigned char c) {
        return std::isalnum(c) || punctuation.find(static_cast<char>(c)) != std::string::npos;
    });
}

bool validate_parameter_headers(const json& schema, const json& value,
                                const httplib::Request& request, std::string& error) {
    if (!schema.is_object()) return true;
    if (schema.contains("x-mcp-header")) {
        if (!schema["x-mcp-header"].is_string()) {
            error = "Invalid x-mcp-header annotation";
            return false;
        }
        const std::string suffix = schema["x-mcp-header"].get<std::string>();
        if (!valid_header_token(suffix)) {
            error = "Invalid x-mcp-header name";
            return false;
        }
        const std::string header_name = "Mcp-Param-" + suffix;
        if (value.is_null()) {
            if (request.has_header(header_name)) {
                error = header_name + " must be omitted for a missing value";
                return false;
            }
        } else {
            std::string expected;
            if (value.is_string()) expected = value.get<std::string>();
            else if (value.is_boolean()) expected = value.get<bool>() ? "true" : "false";
            else if (value.is_number_unsigned()) {
                const auto number = value.get<uint64_t>();
                if (number > 9007199254740991ULL) {
                    error = header_name + " integer exceeds the safe range";
                    return false;
                }
                expected = std::to_string(number);
            } else if (value.is_number_integer()) {
                const auto number = value.get<int64_t>();
                if (number < -9007199254740991LL || number > 9007199254740991LL) {
                    error = header_name + " integer exceeds the safe range";
                    return false;
                }
                expected = std::to_string(number);
            } else {
                error = header_name + " must mirror a string, integer, or boolean";
                return false;
            }
            if (request.get_header_value_count(header_name) != 1) {
                error = "Missing " + header_name;
                return false;
            }
            auto mirrored = decode_mirrored_value(request.get_header_value(header_name));
            if (!mirrored || *mirrored != expected) {
                error = header_name + " does not match the body";
                return false;
            }
        }
    }
    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto it = schema["properties"].begin(); it != schema["properties"].end(); ++it) {
            const json child = value.is_object() && value.contains(it.key())
                ? value[it.key()] : json(nullptr);
            if (!validate_parameter_headers(it.value(), child, request, error)) return false;
        }
    }
    return true;
}

} // namespace

namespace mcp {

void server::handle_modern_post(const httplib::Request& http_req,
                                httplib::Response& http_res, const json& body) {
    http_res.set_header("MCP-Protocol-Version", MCP_MODERN_VERSION);
    auto fail = [&](const json& id, error_code code, const std::string& message,
                    const json& data = json::object()) {
        http_res.status = 400;
        http_res.set_content(protocol_error(id, code, message, data).dump(), "application/json");
    };

    const json id = body.is_object() && body.contains("id") ? body["id"] : json(nullptr);
    if (!body.is_object() || !body.contains("jsonrpc") || body["jsonrpc"] != "2.0" ||
        !body.contains("method") || !body["method"].is_string() ||
        (body.contains("id") && !body["id"].is_string() && !body["id"].is_number_integer())) {
        fail(id, error_code::invalid_request, "Expected a single JSON-RPC request or notification");
        return;
    }

    if (!body.contains("params") || !body["params"].is_object() ||
        !body["params"].contains("_meta") || !body["params"]["_meta"].is_object()) {
        fail(id, error_code::invalid_params, "Missing per-request _meta");
        return;
    }
    const json& meta = body["params"]["_meta"];
    if (!meta.contains("io.modelcontextprotocol/protocolVersion") ||
        !meta["io.modelcontextprotocol/protocolVersion"].is_string() ||
        !meta.contains("io.modelcontextprotocol/clientCapabilities") ||
        !meta["io.modelcontextprotocol/clientCapabilities"].is_object()) {
        fail(id, error_code::invalid_params, "Missing protocol version or client capabilities in _meta");
        return;
    }

    const std::string version = meta["io.modelcontextprotocol/protocolVersion"].get<std::string>();
    if (http_req.get_header_value_count("MCP-Protocol-Version") != 1 ||
        http_req.get_header_value("MCP-Protocol-Version") != version) {
        fail(id, error_code::header_mismatch, "MCP-Protocol-Version header does not match _meta");
        return;
    }
    if (version != MCP_MODERN_VERSION) {
        fail(id, error_code::unsupported_protocol_version, "Unsupported protocol version",
             {{"supported", json::array({MCP_MODERN_VERSION, MCP_VERSION})}, {"requested", version}});
        return;
    }

    const std::string method = body["method"].get<std::string>();
    if (http_req.get_header_value_count("Mcp-Method") != 1 ||
        !plain_header_value(http_req.get_header_value("Mcp-Method")) ||
        http_req.get_header_value("Mcp-Method") != method) {
        fail(id, error_code::header_mismatch, "Mcp-Method header does not match method");
        return;
    }

    const char* name_field = method == "resources/read" ? "uri" :
        (method == "tools/call" || method == "prompts/get" ? "name" : nullptr);
    if (name_field) {
        const json& params = body["params"];
        if (!params.contains(name_field) || !params[name_field].is_string() ||
            http_req.get_header_value_count("Mcp-Name") != 1) {
            fail(id, error_code::header_mismatch, "Missing Mcp-Name header or body value");
            return;
        }
        auto mirrored_name = decode_mirrored_value(http_req.get_header_value("Mcp-Name"));
        if (!mirrored_name || *mirrored_name != params[name_field].get<std::string>()) {
            fail(id, error_code::header_mismatch, "Mcp-Name header does not match body value");
            return;
        }
    }

    if (method == "tools/call") {
        json schema;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const std::string name = body["params"]["name"].get<std::string>();
            auto it = tools_.find(name);
            if (it != tools_.end()) schema = it->second.first.parameters_schema;
        }
        const json arguments = body["params"].contains("arguments")
            ? body["params"]["arguments"] : json::object();
        std::string header_error;
        if (!validate_parameter_headers(schema, arguments, http_req, header_error)) {
            fail(id, error_code::header_mismatch, header_error);
            return;
        }
    }

    if (!body.contains("id")) {
        // HTTP notifications have no response body. The current core revision
        // defines no client-to-server notifications on this transport.
        http_res.status = 202;
        return;
    }

    request mcp_req = request::from_json(body);
    int status = 200;
    json result = process_modern_request(mcp_req, status);
    http_res.status = status;
    http_res.set_content(result.dump(), "application/json");
}

json server::process_modern_request(const request& req, int& http_status) {
    json result;
    try {
        if (!req.params.is_object() || !req.params.contains("_meta") ||
            !req.params["_meta"].is_object()) {
            http_status = 400;
            return protocol_error(req.id, error_code::invalid_params, "Missing per-request _meta");
        }
        const json& meta = req.params["_meta"];
        if (!meta.contains("io.modelcontextprotocol/protocolVersion") ||
            !meta["io.modelcontextprotocol/protocolVersion"].is_string() ||
            !meta.contains("io.modelcontextprotocol/clientCapabilities") ||
            !meta["io.modelcontextprotocol/clientCapabilities"].is_object()) {
            http_status = 400;
            return protocol_error(req.id, error_code::invalid_params,
                                  "Missing protocol version or client capabilities in _meta");
        }
        const std::string version = meta["io.modelcontextprotocol/protocolVersion"].get<std::string>();
        if (version != MCP_MODERN_VERSION) {
            http_status = 400;
            return protocol_error(req.id, error_code::unsupported_protocol_version,
                                  "Unsupported protocol version",
                                  {{"supported", json::array({MCP_MODERN_VERSION, MCP_VERSION})},
                                   {"requested", version}});
        }
        if (req.method == "server/discover") {
            std::lock_guard<std::mutex> lock(mutex_);
            json caps = capabilities_.is_object() ? capabilities_ : json::object();
            if (!tools_.empty() && !caps.contains("tools")) caps["tools"] = json::object();
            if ((!resources_.empty() || !resource_templates_.empty()) && !caps.contains("resources"))
                caps["resources"] = json::object();
            if (!prompts_.empty() && !caps.contains("prompts")) caps["prompts"] = json::object();
            result = {
                {"supportedVersions", json::array({MCP_MODERN_VERSION, MCP_VERSION})},
                {"capabilities", caps},
                {"ttlMs", 0},
                {"cacheScope", "private"}
            };
            if (!instructions_.empty()) result["instructions"] = instructions_;
        } else if (req.method == "ping") {
            result = json::object();
        } else {
            method_handler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = method_handlers_.find(req.method);
                if (it != method_handlers_.end()) handler = it->second;
            }
            if (!handler) {
                http_status = 404;
                return protocol_error(req.id, error_code::method_not_found,
                                      "Method not found: " + req.method);
            }
            // Empty session ID is intentional: modern requests are stateless.
            result = handler(req.params, "");
        }
        if (!result.is_object()) {
            return protocol_error(req.id, error_code::internal_error,
                                  "Method handler must return a result object");
        }
        if (!result.contains("resultType")) result["resultType"] = "complete";
        if (!result.contains("_meta") || !result["_meta"].is_object())
            result["_meta"] = json::object();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            result["_meta"]["io.modelcontextprotocol/serverInfo"] =
                {{"name", name_}, {"version", version_}};
        }
        return response::create_success(req.id, result).to_json();
    } catch (const mcp_exception& e) {
        return protocol_error(req.id, e.code(), e.what());
    } catch (const std::exception& e) {
        LOG_ERROR("Modern method failed: ", e.what());
        return protocol_error(req.id, error_code::internal_error, "Internal error");
    } catch (...) {
        LOG_ERROR("Modern method failed with unknown exception");
        return protocol_error(req.id, error_code::internal_error, "Internal error");
    }
}

} // namespace mcp
