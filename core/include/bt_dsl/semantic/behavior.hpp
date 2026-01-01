// bt_dsl/semantic/behavior.hpp - behavior(DataPolicy, FlowPolicy)
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace bt_dsl
{

enum class DataPolicy : uint8_t {
  All,
  Any,
  None,
};

enum class FlowPolicy : uint8_t {
  Chained,
  Isolated,
};

struct Behavior
{
  DataPolicy data = DataPolicy::All;
  FlowPolicy flow = FlowPolicy::Chained;
};

std::optional<DataPolicy> data_policy_from_string(std::string_view s);
std::optional<FlowPolicy> flow_policy_from_string(std::string_view s);
std::string_view to_string(DataPolicy p);
std::string_view to_string(FlowPolicy p);

}  // namespace bt_dsl
