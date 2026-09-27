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

#include "test_harness.hpp"

#include "cooling_capacity/version.hpp"

namespace {

using cooling_capacity::ErrorCode;
using cooling_capacity::Result;
using cooling_capacity::Version;
using cooling_capacity::library_version;

}  // namespace

CCAP_TEST(version_constants_are_consistent) {
  CCAP_CHECK_EQ(library_version(), (Version{1, 0, 0}));
  CCAP_CHECK_EQ(cooling_capacity::version_string(), std::string_view("1.0.0"));
  CCAP_CHECK_EQ(cooling_capacity::artifact_format_version(), 1U);
}

CCAP_TEST(version_parsing_is_strict) {
  CCAP_CHECK_OK(parsed, Version::parse("2.3.4"));
  CCAP_CHECK_EQ(parsed.major, 2);
  CCAP_CHECK_EQ(parsed.minor, 3);
  CCAP_CHECK_EQ(parsed.patch, 4);
  CCAP_CHECK_EQ(parsed.to_string(), std::string("2.3.4"));

  CCAP_CHECK_ERR(Version::parse("1.0"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Version::parse("1.0.0.0"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Version::parse("1.0.x"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Version::parse("01.0.0"), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Version::parse(""), ErrorCode::InvalidArgument);
  CCAP_CHECK_ERR(Version::parse("1.0.-1"), ErrorCode::InvalidArgument);
}

CCAP_TEST(version_ordering) {
  CCAP_CHECK((Version{1, 0, 0} < Version{1, 0, 1}));
  CCAP_CHECK((Version{1, 0, 9} < Version{1, 1, 0}));
  CCAP_CHECK((Version{1, 9, 9} < Version{2, 0, 0}));
  CCAP_CHECK((Version{1, 9, 9} < Version{2, 0, 0}));
  const Version left{1, 2, 3};
  const Version right{1, 2, 3};
  CCAP_CHECK_EQ(left, right);
}
