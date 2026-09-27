// Copyright 2026 Summon Software Labs
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace ccap_test;

#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using cooling_capacity::ExplanationBuilder;
using cooling_capacity::ExplanationCode;
using cooling_capacity::ExplanationParam;
using cooling_capacity::ExplanationStep;
using cooling_capacity::kExplanationCodeCount;

// One value for every placeholder a code's template declares, in the order the
// template first mentions them.
std::vector<ExplanationParam> parameters_for(ExplanationCode code) {
  std::vector<ExplanationParam> params;
  const std::vector<std::string> required =
      cooling_capacity::explanation_required_parameters(code);
  for (std::size_t index = 0; index < required.size(); ++index) {
    params.push_back(cooling_capacity::param(required[index], "v" + std::to_string(index)));
  }
  return params;
}

void expect_required_parameters(ExplanationCode code, const std::vector<std::string>& expected) {
  const std::vector<std::string> actual = cooling_capacity::explanation_required_parameters(code);
  CCAP_CHECK(actual == expected);
}

}  // namespace

CCAP_TEST(every_explanation_code_has_a_template_and_a_stable_name) {
  std::set<std::string> names;
  for (std::size_t index = 0; index < kExplanationCodeCount; ++index) {
    const ExplanationCode code = cooling_capacity::explanation_code_at(index);
    CCAP_CHECK_EQ(static_cast<std::size_t>(code), index);

    const std::string_view name = cooling_capacity::to_string(code);
    CCAP_CHECK_FALSE(name.empty());
    CCAP_CHECK_NE(std::string(name), std::string("unrecognised"));

    const std::string_view text = cooling_capacity::explanation_template(code);
    CCAP_CHECK_FALSE(text.empty());
    CCAP_CHECK_NE(std::string(text), std::string("unrecognised explanation code"));

    // The name is the machine-readable contract and must be unique.
    CCAP_CHECK(names.insert(std::string(name)).second);
  }
  CCAP_CHECK_EQ(names.size(), kExplanationCodeCount);

  // A name is never reused for another code, and an index outside the table
  // degrades to the first code rather than reading past the end.
  CCAP_CHECK_EQ(cooling_capacity::to_string(cooling_capacity::explanation_code_at(0)),
                std::string_view("snapshot-basis"));
  CCAP_CHECK_EQ(cooling_capacity::explanation_code_at(kExplanationCodeCount),
                ExplanationCode::SnapshotBasis);
  CCAP_CHECK_EQ(cooling_capacity::to_string(ExplanationCode::DiffSummary),
                std::string_view("diff-summary"));
}

CCAP_TEST(required_parameters_are_the_placeholders_in_order) {
  expect_required_parameters(ExplanationCode::SnapshotBasis,
                             {"generation", "basis", "evaluated_at"});
  expect_required_parameters(ExplanationCode::Bottleneck,
                             {"kind", "subject", "limit", "consumed", "remaining"});
  expect_required_parameters(ExplanationCode::EquipmentDerated,
                             {"equipment", "factor", "reason", "cumulative"});
  expect_required_parameters(ExplanationCode::CandidateDisposition,
                             {"disposition", "reason"});
  expect_required_parameters(ExplanationCode::DiffSummary,
                             {"from", "to", "changes", "added", "removed", "modified"});
  expect_required_parameters(ExplanationCode::PlantShared,
                             {"plant", "loops", "usable", "committed", "headroom"});
  expect_required_parameters(ExplanationCode::ObservationCharged,
                             {"scope", "observed", "evidence"});

  for (std::size_t index = 0; index < kExplanationCodeCount; ++index) {
    const ExplanationCode code = cooling_capacity::explanation_code_at(index);
    const std::string_view text = cooling_capacity::explanation_template(code);
    const std::vector<std::string> required =
        cooling_capacity::explanation_required_parameters(code);
    CCAP_CHECK_FALSE(required.empty());

    std::size_t previous = 0;
    std::set<std::string> seen;
    for (const std::string& key : required) {
      // No placeholder is reported twice, and every reported key really is a
      // placeholder of this template.
      CCAP_CHECK(seen.insert(key).second);
      CCAP_CHECK_FALSE(key.empty());
      const std::string placeholder = "{" + key + "}";
      const std::size_t position = text.find(placeholder);
      CCAP_CHECK_NE(position, std::string_view::npos);
      // Order of first appearance, so a rendered sentence cannot silently
      // reorder its own parameters.
      CCAP_CHECK_LT(previous, position + 1U);
      previous = position + 1U;
    }
  }
  CCAP_CHECK(cooling_capacity::explanation_required_parameters(ExplanationCode::ZoneNotFound) ==
             std::vector<std::string>{"zone"});
}

CCAP_TEST(a_missing_parameter_renders_visibly_rather_than_silently) {
  const ExplanationStep partial(
      ExplanationCode::CandidateDisposition,
      std::vector<ExplanationParam>{cooling_capacity::param("disposition", std::string("admitted"))});
  CCAP_CHECK(partial.has("disposition"));
  CCAP_CHECK_FALSE(partial.has("reason"));
  CCAP_CHECK_EQ(partial.render(),
                std::string("candidate is admitted: <missing:reason>"));
  CCAP_CHECK_EQ(partial.to_string(),
                std::string("candidate-disposition: candidate is admitted: <missing:reason>"));

  const ExplanationStep empty(ExplanationCode::ZoneNotFound, std::vector<ExplanationParam>{});
  CCAP_CHECK_EQ(empty.render(), std::string("zone <missing:zone> is not present in the snapshot"));

  const ExplanationStep zone_only(
      ExplanationCode::LoopClassMismatch,
      std::vector<ExplanationParam>{cooling_capacity::param("loop", std::string("air-loop-1"))});
  const std::string rendered = zone_only.render();
  CCAP_CHECK(rendered.find("<missing:class>") != std::string::npos);
  CCAP_CHECK_EQ(rendered, std::string("loop air-loop-1 cannot serve compatibility class "
                                      "<missing:class>"));
}

CCAP_TEST(a_fully_parameterised_step_renders_without_placeholders) {
  for (std::size_t index = 0; index < kExplanationCodeCount; ++index) {
    const ExplanationCode code = cooling_capacity::explanation_code_at(index);
    const ExplanationStep step(code, parameters_for(code));
    const std::string rendered = step.render();
    CCAP_CHECK(rendered.find("<missing:") == std::string::npos);
    CCAP_CHECK(rendered.find('{') == std::string::npos);
    CCAP_CHECK(rendered.find('}') == std::string::npos);
    CCAP_CHECK_FALSE(rendered.empty());
    // Every declared parameter really is substituted: the sentence differs from
    // the raw template.
    CCAP_CHECK_NE(rendered, std::string(cooling_capacity::explanation_template(code)));
    CCAP_CHECK_EQ(step.params().size(),
                  cooling_capacity::explanation_required_parameters(code).size());
  }

  const ExplanationStep step(
      ExplanationCode::CandidateDisposition,
      std::vector<ExplanationParam>{cooling_capacity::param("disposition", std::string("rejected")),
                                    cooling_capacity::param("reason", std::string("over-committed"))});
  CCAP_CHECK_EQ(step.render(), std::string("candidate is rejected: over-committed"));
  CCAP_CHECK_EQ(step.to_string(),
                std::string("candidate-disposition: candidate is rejected: over-committed"));
}

CCAP_TEST(rendering_is_deterministic) {
  ExplanationBuilder builder;
  builder.add(ExplanationCode::SnapshotBasis,
              std::vector<ExplanationParam>{cooling_capacity::param("generation", std::uint64_t{7}),
                                            cooling_capacity::param("basis", std::string("fresh")),
                                            cooling_capacity::param("evaluated_at",
                                                                    std::string("2026-01-01"))});
  builder.add(ExplanationCode::ZoneLocalHeadroom,
              std::vector<ExplanationParam>{cooling_capacity::param("zone", std::string("hall-1")),
                                            cooling_capacity::param("local", std::string("400 kW")),
                                            cooling_capacity::param("loops", std::uint64_t{1})});

  const std::string first = builder.render();
  const std::string second = builder.render();
  CCAP_CHECK_EQ(first, second);
  CCAP_CHECK(builder.steps().size() == 2U);

  const std::vector<ExplanationStep> taken = builder.take();
  CCAP_CHECK_EQ(cooling_capacity::render_steps(taken), first);
  CCAP_CHECK_EQ(taken.size(), 2U);
  CCAP_CHECK_EQ(taken.front().code(), ExplanationCode::SnapshotBasis);
  CCAP_CHECK_EQ(taken.back().code(), ExplanationCode::ZoneLocalHeadroom);

  const std::vector<ExplanationStep> empty;
  CCAP_CHECK_EQ(cooling_capacity::render_steps(empty), std::string());

  const std::vector<ExplanationStep> repeated{taken.front(), taken.front()};
  CCAP_CHECK_EQ(cooling_capacity::render_steps(repeated),
                taken.front().to_string() + "\n" + taken.front().to_string() + "\n");
}

CCAP_TEST(param_overloads_produce_the_expected_text) {
  const ExplanationParam from_string =
      cooling_capacity::param("k", std::string("text value"));
  CCAP_CHECK_EQ(from_string.key, std::string("k"));
  CCAP_CHECK_EQ(from_string.value, std::string("text value"));

  const std::string_view view = "view value";
  const ExplanationParam from_view = cooling_capacity::param("k", view);
  CCAP_CHECK_EQ(from_view.value, std::string("view value"));

  const ExplanationParam from_literal = cooling_capacity::param("k", "literal value");
  CCAP_CHECK_EQ(from_literal.value, std::string("literal value"));

  const ExplanationParam from_int = cooling_capacity::param("k", static_cast<std::int64_t>(-42));
  CCAP_CHECK_EQ(from_int.value, std::string("-42"));
  const ExplanationParam from_int_min =
      cooling_capacity::param("k", std::numeric_limits<std::int64_t>::min());
  CCAP_CHECK_EQ(from_int_min.value, std::string("-9223372036854775808"));

  const ExplanationParam from_uint = cooling_capacity::param("k", static_cast<std::uint64_t>(42));
  CCAP_CHECK_EQ(from_uint.value, std::string("42"));
  const ExplanationParam from_uint_max =
      cooling_capacity::param("k", std::numeric_limits<std::uint64_t>::max());
  CCAP_CHECK_EQ(from_uint_max.value, std::string("18446744073709551615"));

  const ExplanationParam from_true = cooling_capacity::param("k", true);
  CCAP_CHECK_EQ(from_true.value, std::string("true"));
  const ExplanationParam from_false = cooling_capacity::param("k", false);
  CCAP_CHECK_EQ(from_false.value, std::string("false"));

  // Order is preserved and equality is by value, so an explanation is
  // comparable across two runs.
  const ExplanationStep step(ExplanationCode::CandidateDisposition,
                             std::vector<ExplanationParam>{from_true, from_uint});
  CCAP_CHECK_EQ(step.params().size(), 2U);
  CCAP_CHECK_EQ(step.params()[0].value, std::string("true"));
  CCAP_CHECK_EQ(step.params()[1].value, std::string("42"));
  const ExplanationStep same(ExplanationCode::CandidateDisposition,
                             std::vector<ExplanationParam>{from_true, from_uint});
  CCAP_CHECK(step == same);
  CCAP_CHECK_FALSE(step == ExplanationStep(ExplanationCode::CandidateDisposition,
                                           std::vector<ExplanationParam>{from_uint}));
}