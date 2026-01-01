// bt_dsl/semantic/behavior.cpp
#include "bt_dsl/semantic/behavior.hpp"

#include <string>

namespace bt_dsl
{

std::optional<DataPolicy> data_policy_from_string(std::string_view s)
{
  if (s == "All") return DataPolicy::All;
  if (s == "Any") return DataPolicy::Any;
  if (s == "None") return DataPolicy::None;
  return std::nullopt;
}

std::optional<FlowPolicy> flow_policy_from_string(std::string_view s)
{
  if (s == "Chained") return FlowPolicy::Chained;
  if (s == "Isolated") return FlowPolicy::Isolated;
  return std::nullopt;
}

std::string_view to_string(DataPolicy p)
{
  switch (p) {
    case DataPolicy::All:
      return "All";
    case DataPolicy::Any:
      return "Any";
    case DataPolicy::None:
      return "None";
  }
  return "All";
}

std::string_view to_string(FlowPolicy p)
{
  switch (p) {
    case FlowPolicy::Chained:
      return "Chained";
    case FlowPolicy::Isolated:
      return "Isolated";
  }
  return "Chained";
}

}  // namespace bt_dsl
