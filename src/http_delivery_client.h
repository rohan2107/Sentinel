// src/http_delivery_client.h
#pragma once
#include "delivery_client.h"
#include <string>

// HTTP delivery client using cpp-httplib (header-only)
// POST JSON reports to backend /reports endpoint
class HttpDeliveryClient : public DeliveryClient {
public:
    // backend_url: e.g., "http://localhost:8000"
    explicit HttpDeliveryClient(const std::string& backend_url, int timeout_seconds = 30);
    
    DeliveryResult send(const std::string& report_json,
                       const std::string& report_hash) override;

private:
    std::string backend_url_;
    int timeout_seconds_;
};
