#include "delivery_client.h"

MockDeliveryClient::MockDeliveryClient(bool always_succeed) 
    : always_succeed_(always_succeed), call_count_(0) {}

DeliveryResult MockDeliveryClient::send(const std::string& report_json,
                                        const std::string& report_hash) {
    ++call_count_;
    last_report_ = report_json;
    last_hash_ = report_hash;
    
    DeliveryResult result;
    result.success = always_succeed_;
    result.status_code = always_succeed_ ? 200 : 500;
    result.error_message = always_succeed_ ? "" : "Mock delivery failure";
    
    return result;
}
