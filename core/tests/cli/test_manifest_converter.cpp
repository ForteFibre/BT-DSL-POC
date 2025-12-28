#include <gtest/gtest.h>
#include "manifest_converter.hpp"

using namespace bt_dsl;

namespace {

TEST(ManifestConverterTest, ConvertSingleAction) {
  const std::string xml = R"(
    <root>
      <TreeNodesModel>
        <Action ID="MoveTo">
          <input_port name="x" type="int"/>
          <input_port name="y" type="int"/>
        </Action>
      </TreeNodesModel>
    </root>
  )";

  auto result = ManifestConverter::convert(xml);
  EXPECT_EQ(result.nodes_count, 1);
  EXPECT_NE(result.bt_text.find("declare Action MoveTo(in x: int, in y: int)"), std::string::npos);
}

TEST(ManifestConverterTest, ConvertAllPortTypes) {
  const std::string xml = R"(
    <root>
      <TreeNodesModel>
        <Action ID="TestNode">
          <input_port name="in_p" />
          <output_port name="out_p" />
          <inout_port name="ref_p" />
        </Action>
      </TreeNodesModel>
    </root>
  )";

  auto result = ManifestConverter::convert(xml);
  EXPECT_EQ(result.nodes_count, 1);
  // Order might vary if I didn't enforce specific iteration order, but XML parser usually keeps document order.
  // Input: in, Output: out, InOut: ref
  EXPECT_NE(result.bt_text.find("in in_p: any"), std::string::npos);
  EXPECT_NE(result.bt_text.find("out out_p: any"), std::string::npos);
  EXPECT_NE(result.bt_text.find("ref ref_p: any"), std::string::npos);
}

TEST(ManifestConverterTest, SanitizeTypeNames) {
  const std::string xml = R"(
    <TreeNodesModel>
        <Condition ID="Check">
            <input_port name="val" type="std::string" />
            <input_port name="ptr" type="void*" />
        </Condition>
    </TreeNodesModel>
  )";

  auto result = ManifestConverter::convert(xml);
  EXPECT_NE(result.bt_text.find("std__string"), std::string::npos);
  EXPECT_NE(result.bt_text.find("void_"), std::string::npos);
}

TEST(ManifestConverterTest, HandlesDescriptionsAlsoMultiline) {
  const std::string xml = R"(
    <TreeNodesModel>
        <Action ID="DoWork">
            <input_port name="amount" description="The amount to work" />
        </Action>
    </TreeNodesModel>
  )";

  auto result = ManifestConverter::convert(xml);
  EXPECT_NE(result.bt_text.find("/// The amount to work"), std::string::npos);
  EXPECT_NE(result.bt_text.find("in amount: any"), std::string::npos);
}

TEST(ManifestConverterTest, IgnoresStructureWithoutNodes) {
  const std::string xml = "<root><NothingHere/></root>";
  auto result = ManifestConverter::convert(xml);
  EXPECT_EQ(result.nodes_count, 0);
}

} // namespace
