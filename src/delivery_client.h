#pragma once
#include <string>
#include <memory>

struct DeliveryResult {
    bool success;
    int status_code;  // HTTP code or MQTT reason code
    std::string error_message;
};

// Abstract delivery interface for protocol abstraction
class DeliveryClient {
public:
    virtual ~DeliveryClient() = default;
    
    // Send report to backend with content hash
    // Returns DeliveryResult indicating success/failure
    virtual DeliveryResult send(const std::string& report_json,
                                const std::string& report_hash) = 0;
};

// Mock delivery client for testing
class MockDeliveryClient : public DeliveryClient {
public:
    explicit MockDeliveryClient(bool always_succeed);
    
    DeliveryResult send(const std::string& report_json,
                        const std::string& report_hash) override;
    
    // Test inspection
    int get_call_count() const { return call_count_; }
    std::string get_last_report() const { return last_report_; }
    std::string get_last_hash() const { return last_hash_; }
    
private:
    bool always_succeed_;
    int call_count_;
    std::string last_report_;
    std::string last_hash_;
};
