// src/http_delivery_client.cpp
#include "http_delivery_client.h"
#include <nlohmann/json.hpp>
#include <httplib.h>

using json = nlohmann::json;

HttpDeliveryClient::HttpDeliveryClient(const std::string& backend_url, int timeout_seconds)
    : backend_url_(backend_url), timeout_seconds_(timeout_seconds) {}

DeliveryResult HttpDeliveryClient::send(const std::string& report_json,
                                       const std::string& report_hash) {
    DeliveryResult result;
    result.success = false;
    result.status_code = 0;
    result.error_message = "";
    
    try {
        // Parse URL to extract host and port
        std::string host;
        int port = 80;
        std::string scheme = "http";
        
        // Simple URL parsing (http://host:port or http://host)
        size_t scheme_end = backend_url_.find("://");
        size_t host_start = 0;
        if (scheme_end != std::string::npos) {
            scheme = backend_url_.substr(0, scheme_end);
            host_start = scheme_end + 3;
        }
        
        size_t port_start = backend_url_.find(':', host_start);
        size_t path_start = backend_url_.find('/', host_start);
        
        if (port_start != std::string::npos && (path_start == std::string::npos || port_start < path_start)) {
            // Has port
            host = backend_url_.substr(host_start, port_start - host_start);
            size_t port_end = (path_start != std::string::npos) ? path_start : backend_url_.length();
            port = std::stoi(backend_url_.substr(port_start + 1, port_end - port_start - 1));
        } else if (path_start != std::string::npos) {
            // No port, has path
            host = backend_url_.substr(host_start, path_start - host_start);
        } else {
            // No port, no path
            host = backend_url_.substr(host_start);
        }
        
        if (scheme == "https") {
            // cpp-httplib requires CPPHTTPLIB_OPENSSL_SUPPORT for HTTPS.
            // Reject https:// URLs until TLS support is added.
            result.error_message = "HTTPS not supported: use http:// or terminate TLS at a reverse proxy";
            return result;
        }
        
        // Create HTTP client
        httplib::Client client(host, port);
        client.set_connection_timeout(timeout_seconds_, 0);
        client.set_read_timeout(timeout_seconds_, 0);
        client.set_write_timeout(timeout_seconds_, 0);
        
        // Build request body
        json body;
        try {
            body["report"] = json::parse(report_json);
        } catch (...) {
            // If parsing fails, send as string
            body["report"] = report_json;
        }
        body["hash"] = report_hash;
        
        std::string body_str = body.dump();
        
        // POST to /reports endpoint
        httplib::Headers headers = {
            {"Content-Type", "application/json"},
            {"X-Report-Hash", report_hash}
        };
        
        auto res = client.Post("/reports", headers, body_str, "application/json");
        
        if (!res) {
            result.error_message = "HTTP request failed: connection error";
            return result;
        }
        
        result.status_code = res->status;
        
        // Success codes: 200 OK, 201 Created, 409 Conflict (duplicate - idempotent)
        if (res->status == 200 || res->status == 201 || res->status == 409) {
            result.success = true;
            if (res->status == 409) {
                result.error_message = "Duplicate (already delivered)";
            }
        }
        // Retry codes: 5xx server errors, 429 rate limit
        else if (res->status >= 500 || res->status == 429) {
            result.success = false;
            result.error_message = "Server error (retry): HTTP " + std::to_string(res->status);
        }
        // Terminal failure: 4xx client errors (except 429)
        else if (res->status >= 400) {
            result.success = false;
            result.error_message = "Client error (terminal): HTTP " + std::to_string(res->status);
        }
        else {
            result.success = false;
            result.error_message = "Unexpected HTTP status: " + std::to_string(res->status);
        }
        
    } catch (const std::exception& e) {
        result.success = false;
        result.status_code = 0;
        result.error_message = std::string("Exception: ") + e.what();
    } catch (...) {
        result.success = false;
        result.status_code = 0;
        result.error_message = "Unknown exception during HTTP request";
    }
    
    return result;
}
