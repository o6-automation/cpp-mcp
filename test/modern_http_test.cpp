#include "mcp_server.h"

#include <iostream>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using mcp::json;

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int free_loopback_port() {
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 0;
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return 0;
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(0);
    int port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
#ifdef _WIN32
        int length = sizeof(address);
#else
        socklen_t length = sizeof(address);
#endif
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0)
            port = ntohs(address.sin_port);
    }
#ifdef _WIN32
    ::closesocket(fd);
    WSACleanup();
#else
    ::close(fd);
#endif
    return port;
}

json metadata(const std::string& version = mcp::MCP_MODERN_VERSION) {
    return {{"io.modelcontextprotocol/protocolVersion", version},
            {"io.modelcontextprotocol/clientCapabilities", json::object()},
            {"io.modelcontextprotocol/clientInfo", {{"name", "test"}, {"version", "1"}}}};
}

json call(int id, const std::string& method, json params = json::object()) {
    params["_meta"] = metadata();
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

httplib::Headers headers(const std::string& method) {
    return {{"Accept", "application/json, text/event-stream"},
            {"MCP-Protocol-Version", mcp::MCP_MODERN_VERSION},
            {"Mcp-Method", method}};
}

void check_error(const httplib::Result& result, int status, int code) {
    require(static_cast<bool>(result), "HTTP request failed");
    require(result->status == status, "unexpected HTTP status");
    require(json::parse(result->body)["error"]["code"] == code, "unexpected JSON-RPC error");
}

void test_http() {
    const int port = free_loopback_port();
    require(port > 0, "could not reserve a loopback port");
    mcp::server::configuration config;
    config.host = "127.0.0.1";
    config.port = port;
    config.threadpool_size = 1;
    config.max_request_bytes = 4096;
    config.allowed_origins = {"http://localhost:3000"};
    mcp::server server(config);
    server.set_server_info("modern-test", "1.0");

    mcp::tool echo;
    echo.name = "echo";
    echo.description = "Echo text";
    echo.parameters_schema = {{"type", "object"}, {"properties", {
        {"text", {{"type", "string"}, {"x-mcp-header", "Text"}}}
    }}};
    server.register_tool(echo, [](const json& args, const std::string& session) {
        require(session.empty(), "modern tool received a transport session");
        json content = json::array();
        content.push_back({{"type", "text"}, {"text", args.at("text")}});
        return content;
    });
    require(server.start(false), "server did not start");
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(2, 0);
    client.set_read_timeout(3, 0);

    const auto discover_request = call(1, "server/discover");
    auto discover = client.Post("/mcp", headers("server/discover"), discover_request.dump(), "application/json");
    require(discover && discover->status == 200, "discovery failed");
    auto discovery = json::parse(discover->body)["result"];
    require(discovery["resultType"] == "complete", "missing result discriminator");
    require(discovery["supportedVersions"][0] == mcp::MCP_MODERN_VERSION, "wrong modern version");
    require(discovery["capabilities"].contains("tools"), "tool capability missing");
    require(!discovery["capabilities"]["tools"].contains("listChanged"),
            "unsupported modern list-change stream was advertised");
    require(discovery["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "modern-test",
            "server identity missing");
    require(!discover->has_header("Mcp-Session-Id"), "modern request created a session");

    auto listing = client.Post("/mcp", headers("tools/list"), call(2, "tools/list").dump(),
                               "application/json");
    require(listing && listing->status == 200, "stateless tools/list failed");
    require(json::parse(listing->body)["result"]["tools"].size() == 1, "tool list is wrong");

    auto invoke = call(3, "tools/call", {{"name", "echo"}, {"arguments", {{"text", "hello"}}}});
    auto invoke_headers = headers("tools/call");
    invoke_headers.emplace("Mcp-Name", "echo");
    invoke_headers.emplace("Mcp-Param-Text", "hello");
    auto result = client.Post("/mcp", invoke_headers, invoke.dump(), "application/json");
    require(result && result->status == 200, "stateless tool call failed");
    require(json::parse(result->body)["result"]["content"][0]["text"] == "hello",
            "tool result is wrong");

    auto missing_param_header = headers("tools/call");
    missing_param_header.emplace("Mcp-Name", "echo");
    check_error(client.Post("/mcp", missing_param_header, invoke.dump(), "application/json"),
                400, -32020);
    auto encoded = call(8, "tools/call", {{"name", "echo"},
                                           {"arguments", {{"text", " padded "}}}});
    auto encoded_headers = headers("tools/call");
    encoded_headers.emplace("Mcp-Name", "echo");
    encoded_headers.emplace("Mcp-Param-Text", "=?base64?IHBhZGRlZCA=?=");
    auto encoded_result = client.Post("/mcp", encoded_headers, encoded.dump(), "application/json");
    require(encoded_result && encoded_result->status == 200, "Base64 parameter header was rejected");
    auto empty = call(10, "tools/call", {{"name", "echo"}, {"arguments", {{"text", ""}}}});
    auto empty_headers = headers("tools/call");
    empty_headers.emplace("Mcp-Name", "echo");
    empty_headers.emplace("Mcp-Param-Text", "");
    auto empty_result = client.Post("/mcp", empty_headers, empty.dump(), "application/json");
    require(empty_result && empty_result->status == 200, "empty mirrored value was rejected");
    auto malformed_encoding = encoded_headers;
    malformed_encoding.erase("Mcp-Param-Text");
    malformed_encoding.emplace("Mcp-Param-Text", "=?base64?bad?=");
    check_error(client.Post("/mcp", malformed_encoding, encoded.dump(), "application/json"),
                400, -32020);
    auto wrong_name = invoke_headers;
    wrong_name.erase("Mcp-Name");
    wrong_name.emplace("Mcp-Name", "other");
    check_error(client.Post("/mcp", wrong_name, invoke.dump(), "application/json"), 400, -32020);

    auto wrong_method = headers("ping");
    check_error(client.Post("/mcp", wrong_method, discover_request.dump(), "application/json"),
                400, -32020);
    check_error(client.Post("/mcp", headers("ping"), call(9, "ping").dump(),
                            "application/json"), 404, -32601);
    auto wrong_version = headers("server/discover");
    wrong_version.erase("MCP-Protocol-Version");
    wrong_version.emplace("MCP-Protocol-Version", "1900-01-01");
    check_error(client.Post("/mcp", wrong_version, discover_request.dump(), "application/json"),
                400, -32020);
    auto unsupported = discover_request;
    unsupported["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
    check_error(client.Post("/mcp", wrong_version, unsupported.dump(), "application/json"),
                400, -32022);
    auto missing_meta = discover_request;
    missing_meta["params"].erase("_meta");
    check_error(client.Post("/mcp", headers("server/discover"), missing_meta.dump(),
                            "application/json"), 400, -32602);

    auto malicious_origin = headers("server/discover");
    malicious_origin.emplace("Origin", "http://evil.example");
    auto denied = client.Post("/mcp", malicious_origin, discover_request.dump(), "application/json");
    require(denied && denied->status == 403, "unapproved Origin was accepted");
    auto allowed_origin = headers("server/discover");
    allowed_origin.emplace("Origin", "http://localhost:3000");
    auto allowed = client.Post("/mcp", allowed_origin, discover_request.dump(), "application/json");
    require(allowed && allowed->status == 200, "approved Origin was denied");
    require(allowed->get_header_value("Access-Control-Allow-Origin") == "http://localhost:3000",
            "CORS origin is wrong");
    httplib::Headers preflight_headers{{"Origin", "http://localhost:3000"},
        {"Access-Control-Request-Method", "POST"},
        {"Access-Control-Request-Headers", "Mcp-Param-Text, MCP-Protocol-Version"}};
    auto preflight = client.Options("/mcp", preflight_headers);
    require(preflight && preflight->status == 204, "approved CORS preflight failed");
    require(preflight->get_header_value("Access-Control-Allow-Headers") ==
            "Mcp-Param-Text, MCP-Protocol-Version", "preflight did not allow mirrored headers");

    server.set_auth_handler([](const std::string& token, const std::string&) {
        return token == "secret";
    });
    auto unauthenticated = client.Post("/mcp", headers("server/discover"),
                                       discover_request.dump(), "application/json");
    require(unauthenticated && unauthenticated->status == 401, "bearer auth was not enforced");
    auto authenticated = headers("server/discover");
    authenticated.emplace("Authorization", "Bearer secret");
    auto authorized = client.Post("/mcp", authenticated, discover_request.dump(), "application/json");
    require(authorized && authorized->status == 200, "valid bearer token was rejected");

    auto old_initialize = json{{"jsonrpc", "2.0"}, {"id", 4}, {"method", "initialize"},
                               {"params", {{"protocolVersion", mcp::MCP_VERSION},
                                           {"capabilities", json::object()},
                                           {"clientInfo", {{"name", "legacy"}, {"version", "1"}}}}}};
    httplib::Headers legacy_headers{{"Authorization", "Bearer secret"}};
    auto legacy = client.Post("/mcp", legacy_headers, old_initialize.dump(), "application/json");
    require(legacy && legacy->status == 200, "legacy initialization regressed");
    require(legacy->has_header("Mcp-Session-Id"), "legacy session was not created");

    auto get_headers = headers("server/discover");
    get_headers.emplace("Authorization", "Bearer secret");
    auto get = client.Get("/mcp", get_headers);
    require(get && get->status == 405, "modern GET was not rejected");

    auto oversized = client.Post("/mcp", authenticated, std::string(5000, 'x'), "application/json");
    require(oversized && oversized->status == 413, "request size limit was not enforced");
    server.stop();
}

void test_stdio() {
    mcp::server::configuration config;
    config.threadpool_size = 1;
    mcp::server server(config);
    auto request = call(7, "server/discover");
    std::istringstream input(request.dump() + "\n");
    std::ostringstream output;
    auto* old_input = std::cin.rdbuf(input.rdbuf());
    auto* old_output = std::cout.rdbuf(output.rdbuf());
    server.start_stdio();
    std::cin.rdbuf(old_input);
    std::cout.rdbuf(old_output);
    const auto reply = json::parse(output.str());
    require(reply["id"] == 7, "stdio response ID is wrong");
    require(reply["result"]["resultType"] == "complete", "stdio modern lifecycle failed");
}

} // namespace

int main() {
    try {
        test_http();
        test_stdio();
        std::cout << "modern MCP wire tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "modern MCP wire test failed: " << error.what() << '\n';
        return 1;
    }
}
