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
#include "test_process.hpp"

using namespace ccap_test;

#include <memory>
#include <string>

namespace {

constexpr const char* kRoot = "ccap-test-paths";

using cooling_capacity::CoolingCapacityStore;
using cooling_capacity::ErrorCode;
using cooling_capacity::StoreAccess;
using cooling_capacity::StoreOpenOptions;

std::string join_path(const std::string& base, const std::string& name) {
#if defined(_WIN32)
  return base + "\\" + name;
#else
  return base + "/" + name;
#endif
}

StoreOpenOptions writable() {
  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = true;
  return options;
}

StoreOpenOptions strict() {
  StoreOpenOptions options;
  options.access = StoreAccess::ReadWrite;
  options.create_if_missing = false;
  return options;
}

}  // namespace

CCAP_TEST(a_missing_store_directory_is_not_found_without_create_if_missing) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));
  const std::string absent = join_path(root, "absent");

  CCAP_CHECK_ERR(CoolingCapacityStore::open(absent, strict()), ErrorCode::NotFound);
  CCAP_CHECK_FALSE(file_exists(absent));

  // A read-only handle may not create the store either, and says why.
  StoreOpenOptions read_only = strict();
  read_only.access = StoreAccess::ReadOnly;
  read_only.create_if_missing = true;
  CCAP_CHECK_ERR(CoolingCapacityStore::open(absent, read_only), ErrorCode::ReadOnly);
  CCAP_CHECK_FALSE(file_exists(absent));

  CCAP_CHECK_OK(store, CoolingCapacityStore::open(absent, writable()));
  CCAP_CHECK_EQ(store->state(), cooling_capacity::StoreState::OpenWritable);
  CCAP_CHECK(store->directory().find("absent") != std::string::npos);
  CCAP_CHECK(file_exists(join_path(store->directory(), "MANIFEST")));
  CCAP_CHECK_VOID(store->close());
  store.reset();

  // Once it exists, the strict handle opens it.
  CCAP_CHECK_OK(again, CoolingCapacityStore::open(absent, strict()));
  CCAP_CHECK_EQ(again->state(), cooling_capacity::StoreState::OpenWritable);
  CCAP_CHECK_VOID(again->close());
  again.reset();

}

CCAP_TEST(a_store_path_that_is_a_file_is_invalid) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));
  const std::string file = join_path(root, "not-a-directory");
  CCAP_CHECK_VOID(write_text(file, "this is not a store\n"));
  CCAP_CHECK(file_exists(file));

  CCAP_CHECK_ERR(CoolingCapacityStore::open(file, writable()), ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(file, strict()), ErrorCode::PathInvalid);
  // The file is left exactly as it was found.
  CCAP_CHECK_OK(content, read_text(file));
  CCAP_CHECK_EQ(content, std::string("this is not a store\n"));

}

CCAP_TEST(an_embedded_nul_or_invalid_utf8_path_is_refused) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));

  std::string with_nul = join_path(root, "bad");
  with_nul.push_back('\0');
  with_nul.append("name");
  CCAP_CHECK_ERR(CoolingCapacityStore::open(with_nul, writable()), ErrorCode::PathInvalid);

  std::string invalid_utf8 = join_path(root, "bad-");
  invalid_utf8.push_back(static_cast<char>(0xC3));
  invalid_utf8.push_back(static_cast<char>(0x28));
  CCAP_CHECK_ERR(CoolingCapacityStore::open(invalid_utf8, writable()),
                 ErrorCode::InvalidUtf8);

  std::string lone_continuation = join_path(root, "bad-");
  lone_continuation.push_back(static_cast<char>(0xBF));
  CCAP_CHECK_ERR(CoolingCapacityStore::open(lone_continuation, writable()),
                 ErrorCode::InvalidUtf8);

  // An empty path names nothing at all.
  CCAP_CHECK_ERR(CoolingCapacityStore::open(std::string(), writable()),
                 ErrorCode::PathInvalid);
  // A control character inside a component is refused too.
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "bad\nname"), writable()),
                 ErrorCode::PathInvalid);

}

CCAP_TEST(a_component_windows_would_rewrite_is_refused) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));

#if defined(_WIN32)
  // Windows strips a trailing dot or space from a component, which maps two
  // different requests onto one directory. A requested path that the platform
  // would rewrite must be refused rather than silently redirected.
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "trailing."), writable()),
                 ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "trailing "), writable()),
                 ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "trailing..."), writable()),
                 ErrorCode::PathInvalid);
  // Refused means no directory was created for the request anywhere.
  CCAP_CHECK_FALSE(file_exists(join_path(root, "trailing")));
#endif

}

CCAP_TEST(a_reserved_device_name_is_refused) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));

#if defined(_WIN32)
  // Reserved device names are refused, with or without an extension.
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "NUL"), writable()),
                 ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "CON"), writable()),
                 ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "com1"), writable()),
                 ErrorCode::PathInvalid);
  CCAP_CHECK_ERR(CoolingCapacityStore::open(join_path(root, "NUL.txt"), writable()),
                 ErrorCode::PathInvalid);
#endif

  // A name that merely looks like a device name is still a name.
  CCAP_CHECK_OK(store, CoolingCapacityStore::open(join_path(root, "console"), writable()));
  CCAP_CHECK(store->directory().find("console") != std::string::npos);
  CCAP_CHECK_VOID(store->close());
  store.reset();
  CCAP_CHECK_OK(with_dot_inside, CoolingCapacityStore::open(join_path(root, "v1.0"), writable()));
  CCAP_CHECK_VOID(with_dot_inside->close());
  with_dot_inside.reset();

}

CCAP_TEST(a_relative_path_with_dot_segments_resolves_to_one_directory) {
  CCAP_CHECK_OK(root, fresh_directory(kRoot));
  CCAP_CHECK_VOID(make_directory(join_path(root, "inner")));
  CCAP_CHECK_OK(direct, CoolingCapacityStore::open(root, writable()));
  const std::string resolved = direct->directory();
  CCAP_CHECK(resolved.find("..") == std::string::npos);
  CCAP_CHECK(resolved.find(root) != std::string::npos);
  CCAP_CHECK(file_exists(join_path(resolved, "MANIFEST")));
  CCAP_CHECK_VOID(direct->close());
  direct.reset();

  // The same store reached through "." and ".." segments is the same directory,
  // and it still opens and reads.
  CCAP_CHECK_OK(dotted,
                CoolingCapacityStore::open(join_path(join_path(root, "inner"), ".."), writable()));
  CCAP_CHECK_EQ(dotted->directory(), resolved);
  CCAP_CHECK_ERR(dotted->head_generation(), ErrorCode::NotFound);
  CCAP_CHECK_OK(generations, dotted->generations());
  CCAP_CHECK_EQ(generations.size(), 0U);
  CCAP_CHECK_VOID(dotted->close());
  dotted.reset();

  CCAP_CHECK_OK(plain, CoolingCapacityStore::open(join_path(".", kRoot), strict()));
  CCAP_CHECK_EQ(plain->directory(), resolved);
  CCAP_CHECK_VOID(plain->close());
  plain.reset();

}