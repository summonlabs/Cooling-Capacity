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

// A fixed-seed randomized property test. The generator, the reference model and
// the assertions all live in this file: the point is to compare the library
// against an independently written model of the same rules, not to re-derive the
// library from itself. Nothing here reads the clock and nothing depends on an
// unseeded generator, so a failure is reproducible from the seed alone.

#include "fixtures.hpp"
#include "test_harness.hpp"

using namespace ccap_test;

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

using cooling_capacity::AnalysisBasis;
using cooling_capacity::CoolingAnalyzer;
using cooling_capacity::CoolingMedium;
using cooling_capacity::CoolingSnapshot;
using cooling_capacity::LoopCapacity;
using cooling_capacity::LoopKind;
using cooling_capacity::OperatingState;
using cooling_capacity::RedundancyClass;
using cooling_capacity::SnapshotBuilder;
using cooling_capacity::ZoneCapacity;

// Hard-coded, so every run exercises exactly the same facility shapes.
constexpr std::uint64_t kPropertySeed = 20260101ULL;
constexpr std::size_t kIterations = 250;
constexpr std::int64_t kPartsPerMillion = 1000000;

// ---------------------------------------------------------------------------
// Reference model
// ---------------------------------------------------------------------------

// One unit's capacity after its derate chain: each factor is applied in the
// canonical name order with an exact floor at every step.
std::int64_t reference_derate_chain(std::int64_t nominal_milliwatts,
                                    const std::vector<std::int64_t>& derate_parts_per_million) {
  std::int64_t value = nominal_milliwatts;
  for (const std::int64_t factor : derate_parts_per_million) {
    value = (value * factor) / kPartsPerMillion;
  }
  return value;
}

// A unit is countable for the redundancy formula while it is in service or
// standby: a standby unit carries no load now, but the formula has to see its
// own capacity so that losing an in-service unit leaves the standby capacity
// behind. A unit in maintenance, faulted or decommissioned is excluded.
bool reference_countable(OperatingState state) {
  return state == OperatingState::InService || state == OperatingState::Standby;
}

// The published group formula over the countable amounts, largest first. Class
// N is the one case that sums the in-service units alone.
std::int64_t reference_group_capacity(std::vector<std::int64_t> countable,
                                      const std::vector<std::int64_t>& in_service,
                                      RedundancyClass declared) {
  if (declared == RedundancyClass::None) {
    std::int64_t total = 0;
    for (const std::int64_t amount : in_service) {
      total += amount;
    }
    return total;
  }
  std::sort(countable.begin(), countable.end(), std::greater<std::int64_t>());
  std::int64_t total = 0;
  for (const std::int64_t amount : countable) {
    total += amount;
  }
  const std::int64_t largest = countable.empty() ? 0 : countable.front();
  const std::int64_t second = countable.size() > 1U ? countable[1] : 0;
  switch (declared) {
    case RedundancyClass::None:
      return total;
    case RedundancyClass::NPlusOne:
      return total > largest ? total - largest : 0;
    case RedundancyClass::NPlusTwo:
      return total > largest + second ? total - largest - second : 0;
    case RedundancyClass::TwoN:
      return total / 2;
    case RedundancyClass::TwoNPlusOne:
      return (total > largest ? total - largest : 0) / 2;
    case RedundancyClass::TwoNPlusTwo:
      return (total > largest + second ? total - largest - second : 0) / 2;
  }
  return 0;
}

// What the reference expects of one generated loop.
struct ReferenceLoop {
  std::vector<std::int64_t> contributing;
  std::vector<std::int64_t> in_service;
  std::int64_t nominal_total = 0;
  RedundancyClass redundancy = RedundancyClass::None;
  std::int64_t expected_usable = 0;
  std::size_t counted = 0;
  std::size_t excluded = 0;
};

struct PassResult {
  std::vector<std::int64_t> loop_usable;
  std::vector<std::int64_t> loop_nominal;
  std::vector<std::int64_t> zone_local_headroom;
  std::size_t loops_checked = 0;
};

std::string indexed(const char* prefix, std::size_t index) {
  return std::string(prefix) + std::to_string(index);
}

// Builds one random facility, checks every loop and the zone against the
// reference model, and returns the observed figures.
PassResult run_property_pass(std::uint64_t seed, std::size_t iterations) {
  std::mt19937_64 generator(seed);
  const OperatingState states[] = {OperatingState::InService, OperatingState::Standby,
                                   OperatingState::Maintenance, OperatingState::Faulted,
                                   OperatingState::Decommissioned};
  const std::size_t state_count = sizeof(states) / sizeof(states[0]);

  PassResult pass;
  for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
    const std::size_t loop_count = 1U + static_cast<std::size_t>(generator() % 4U);
    SnapshotBuilder builder;
    builder.set_generation(CapacityGeneration::from_value(iteration + 1U));
    builder.set_constructed_at(fixture_now());

    const SimpleFacility shell = simple_facility();
    CCAP_CHECK_VOID(builder.add(shell.site));
    CCAP_CHECK_VOID(builder.add(shell.facility));

    std::vector<ReferenceLoop> reference;
    std::vector<LoopId> zone_loops;
    std::vector<LoopId> loop_ids;
    std::size_t unit_serial = 0;

    for (std::size_t loop_index = 0; loop_index < loop_count; ++loop_index) {
      const std::size_t unit_count = 1U + static_cast<std::size_t>(generator() % 6U);
      const std::size_t in_service = static_cast<std::size_t>(generator() %
                                                              static_cast<std::uint64_t>(unit_count));
      ReferenceLoop expected;
      expected.redundancy = static_cast<RedundancyClass>(generator() % 6U);

      std::vector<EquipmentId> units;
      for (std::size_t unit_index = 0; unit_index < unit_count; ++unit_index) {
        const std::int64_t nominal_watts_value =
            1 + static_cast<std::int64_t>(generator() % 1000000U);
        const std::string name = indexed("u", unit_serial++);
        EquipmentRecord unit =
            make_unit(name.c_str(), EquipmentKind::Crah, nominal_watts_value);
        unit.state = unit_index == in_service ? OperatingState::InService
                                             : states[generator() % state_count];

        std::vector<std::int64_t> factors;
        const std::size_t derate_count = static_cast<std::size_t>(generator() % 4U);
        for (std::size_t derate_index = 0; derate_index < derate_count; ++derate_index) {
          const std::int64_t parts = static_cast<std::int64_t>(generator() % 1000001U);
          factors.push_back(parts);
          // Names sort into generation order, so the chain is unambiguous.
          const std::string derate_name = indexed("derate-", derate_index + 1U);
          unit.derates.push_back(DerateFactor{identifier(derate_name.c_str()), ppm(parts),
                                              cooling_capacity::DerateReason::Custom,
                                              std::nullopt});
        }

        expected.nominal_total += unit.nominal.milliwatts();
        if (reference_countable(unit.state)) {
          const std::int64_t amount =
              reference_derate_chain(unit.nominal.milliwatts(), factors);
          expected.contributing.push_back(amount);
          if (unit.state == OperatingState::InService) {
            expected.in_service.push_back(amount);
          }
          ++expected.counted;
        } else {
          ++expected.excluded;
        }
        CCAP_CHECK_VOID(builder.add(unit));
        units.push_back(unit.id);
      }

      // The transport path is declared to be as large as the equipment total, so
      // the loop's usable capacity is decided by the equipment and the
      // redundancy formula alone.
      LoopRecord loop = make_loop(indexed("air-loop-", loop_index + 1U).c_str(),
                                  loop_index % 2U == 0U ? LoopKind::AirSupply : LoopKind::AirReturn,
                                  units, expected.nominal_total / 1000, expected.redundancy);
      CCAP_CHECK_EQ(loop.transport_limit.milliwatts(), expected.nominal_total);
      expected.expected_usable = reference_group_capacity(
          expected.contributing, expected.in_service, expected.redundancy);
      CCAP_CHECK(expected.expected_usable <= expected.nominal_total);
      CCAP_CHECK_VOID(builder.add(loop));
      zone_loops.push_back(loop.id);
      loop_ids.push_back(loop.id);
      reference.push_back(expected);
    }

    const ZoneRecord zone =
        make_zone("hall-1", zone_loops, {CompatibilityClass::AirConvection});
    CCAP_CHECK_VOID(builder.add(zone));
    const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
    const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);

    std::int64_t expected_zone_total = 0;
    for (std::size_t loop_index = 0; loop_index < loop_ids.size(); ++loop_index) {
      const Result<LoopCapacity> loop_result = analyzer.loop_capacity(loop_ids[loop_index]);
      CCAP_CHECK(loop_result.ok());
      if (!loop_result.ok()) {
        continue;
      }
      const LoopCapacity& view = loop_result.value();
      const std::int64_t usable = view.breakdown.usable.amount_or_zero().milliwatts();
      const std::int64_t nominal = view.breakdown.nominal.amount_or_zero().milliwatts();

      CCAP_CHECK(view.breakdown.usable.is_known());
      CCAP_CHECK_EQ(usable, reference[loop_index].expected_usable);
      CCAP_CHECK_EQ(nominal, reference[loop_index].nominal_total);
      // A derived figure is never larger than the nameplate it came from.
      CCAP_CHECK(usable <= nominal);
      CCAP_CHECK_EQ(view.redundancy.declared, reference[loop_index].redundancy);
      CCAP_CHECK_EQ(view.redundancy.counted, reference[loop_index].counted);
      CCAP_CHECK_EQ(view.redundancy.excluded, reference[loop_index].excluded);
      // Nothing is committed, so the loop's whole usable capacity is headroom.
      CCAP_CHECK_EQ(view.breakdown.committed.amount_or_zero().milliwatts(), 0);
      expected_zone_total += usable;
      pass.loop_usable.push_back(usable);
      pass.loop_nominal.push_back(nominal);
      ++pass.loops_checked;
    }

    const Result<ZoneCapacity> zone_result =
        analyzer.zone_capacity(zone_id("hall-1"), CoolingMedium::Air,
                               CompatibilityClass::AirConvection);
    CCAP_CHECK(zone_result.ok());
    if (!zone_result.ok()) {
      continue;
    }
    const ZoneCapacity& zone_view = zone_result.value();
    CCAP_CHECK_EQ(zone_view.loops.size(), loop_count);
    // The zone rollup is the sum of the serving loops' usable capacities less
    // their committed loads, and nothing is committed here.
    CCAP_CHECK(zone_view.local_headroom.is_known());
    CCAP_CHECK_EQ(zone_view.local_headroom.amount_or_zero().milliwatts(), expected_zone_total);
    CCAP_CHECK_EQ(zone_view.breakdown.usable.amount_or_zero().milliwatts(), expected_zone_total);
    CCAP_CHECK_EQ(zone_view.breakdown.committed.amount_or_zero().milliwatts(), 0);
    CCAP_CHECK_EQ(zone_view.offered.amount_or_zero().milliwatts(), expected_zone_total);
    pass.zone_local_headroom.push_back(expected_zone_total);
  }
  return pass;
}

}  // namespace

CCAP_TEST(random_facilities_match_an_independent_reference_model) {
  const PassResult pass = run_property_pass(kPropertySeed, kIterations);

  CCAP_CHECK_EQ(pass.zone_local_headroom.size(), kIterations);
  CCAP_CHECK(pass.loops_checked >= 200U);
  CCAP_CHECK_EQ(pass.loop_usable.size(), pass.loop_nominal.size());
  CCAP_CHECK(pass.loop_usable.size() >= 200U);

  // Every loop in the pass satisfied usable <= nominal, and no usable figure is
  // negative.
  for (std::size_t index = 0; index < pass.loop_usable.size(); ++index) {
    CCAP_CHECK(pass.loop_usable[index] <= pass.loop_nominal[index]);
    CCAP_CHECK(pass.loop_usable[index] >= 0);
  }

  // A facility with no equipment at all would have no capacity; the generator
  // never produces one, and at least one loop carries a positive figure, so the
  // assertions above are not vacuous.
  bool any_positive = false;
  for (const std::int64_t usable : pass.loop_usable) {
    if (usable > 0) {
      any_positive = true;
    }
  }
  CCAP_CHECK(any_positive);
}

CCAP_TEST(the_same_seed_always_produces_the_same_answers) {
  const PassResult first = run_property_pass(kPropertySeed, kIterations);
  const PassResult second = run_property_pass(kPropertySeed, kIterations);

  CCAP_CHECK_EQ(first.loops_checked, second.loops_checked);
  CCAP_CHECK(first.loop_usable == second.loop_usable);
  CCAP_CHECK(first.loop_nominal == second.loop_nominal);
  CCAP_CHECK(first.zone_local_headroom == second.zone_local_headroom);

  // A different seed produces a different sequence of facilities, so the
  // comparison above is a property of the seed and not of a degenerate
  // generator that always emits the same shape.
  const PassResult other = run_property_pass(kPropertySeed + 1ULL, kIterations);
  CCAP_CHECK(other.loop_usable != first.loop_usable);
  CCAP_CHECK_EQ(other.zone_local_headroom.size(), first.zone_local_headroom.size());
}

CCAP_TEST(the_reference_model_agrees_with_the_published_formulas) {
  // Spot checks of the reference itself, so a mistake in the model cannot hide
  // by being consistently wrong on every random shape.
  // In these spot checks every unit is in service, so the countable set and the
  // in-service set are the same.
  const std::vector<std::int64_t> two_units{400000000, 400000000};
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::None), 800000000);
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::NPlusOne),
                400000000);
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::NPlusTwo), 0);
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::TwoN), 400000000);
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::TwoNPlusOne),
                200000000);
  CCAP_CHECK_EQ(reference_group_capacity(two_units, two_units, RedundancyClass::TwoNPlusTwo), 0);

  const std::vector<std::int64_t> three_units{400000000, 300000000, 100000000};
  CCAP_CHECK_EQ(reference_group_capacity(three_units, three_units, RedundancyClass::NPlusOne),
                400000000);
  CCAP_CHECK_EQ(reference_group_capacity(three_units, three_units, RedundancyClass::NPlusTwo),
                100000000);
  CCAP_CHECK_EQ(reference_group_capacity(three_units, three_units, RedundancyClass::TwoN),
                400000000);

  const std::vector<std::int64_t> one_unit{250000000};
  CCAP_CHECK_EQ(reference_group_capacity(one_unit, one_unit, RedundancyClass::NPlusOne), 0);
  CCAP_CHECK_EQ(reference_group_capacity(one_unit, one_unit, RedundancyClass::None), 250000000);

  // A standby unit leaves the N+1 figure unchanged but raises N+2: the formula
  // sees its capacity even though it carries no load now.
  const std::vector<std::int64_t> countable{400000000, 300000000};
  const std::vector<std::int64_t> in_service_only{400000000};
  CCAP_CHECK_EQ(reference_group_capacity(countable, in_service_only, RedundancyClass::None),
                400000000);
  CCAP_CHECK_EQ(reference_group_capacity(countable, in_service_only, RedundancyClass::NPlusOne),
                300000000);
  CCAP_CHECK_EQ(reference_group_capacity(countable, in_service_only, RedundancyClass::NPlusTwo),
                0);

  // Two ninety-percent derates: 400 kW becomes 324 kW, exactly as the chain
  // floors at each step.
  CCAP_CHECK_EQ(reference_derate_chain(400000000, {900000, 900000}), 324000000);
  // A floor at every step is observable: 1 mW * 0.999999 is 0, not 1.
  CCAP_CHECK_EQ(reference_derate_chain(1, {999999}), 0);
  CCAP_CHECK_EQ(reference_derate_chain(400000000, {}), 400000000);

  // And the same shape through the library, so the reference and the library are
  // known to agree on a case that can be checked by hand.
  SimpleFacility fixture = simple_facility();
  fixture.crah_a.derates = {DerateFactor{identifier("derate-1"), ppm(900000),
                                         cooling_capacity::DerateReason::AltitudeAirDensity,
                                         std::nullopt},
                            DerateFactor{identifier("derate-2"), ppm(900000),
                                         cooling_capacity::DerateReason::AmbientDesign,
                                         std::nullopt}};
  fixture.crah_b.derates = fixture.crah_a.derates;
  SnapshotBuilder builder;
  builder.set_generation(CapacityGeneration::first());
  builder.set_constructed_at(fixture_now());
  CCAP_CHECK_VOID(add_simple_facility(builder, fixture));
  const std::shared_ptr<const CoolingSnapshot> snapshot = build_snapshot(builder);
  const CoolingAnalyzer analyzer(*snapshot, fixture_now(), AnalysisBasis::Fresh);
  CCAP_CHECK_OK(view, analyzer.loop_capacity(loop_id("air-loop-1")));
  const std::vector<std::int64_t> derated{
      reference_derate_chain(400000000, {900000, 900000}),
      reference_derate_chain(400000000, {900000, 900000})};
  // Both units are in service, so the countable and in-service sets coincide.
  CCAP_CHECK_EQ(view.breakdown.usable.amount_or_zero().milliwatts(),
                reference_group_capacity(derated, derated, RedundancyClass::NPlusOne));
}