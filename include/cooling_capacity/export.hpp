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

#ifndef COOLING_CAPACITY_EXPORT_HPP
#define COOLING_CAPACITY_EXPORT_HPP

// Visibility decoration for the shared-library build. The default build is a
// static library, where no decoration is needed.

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(CCAP_SHARED_BUILD)
#define CCAP_EXPORT __declspec(dllexport)
#elif defined(CCAP_SHARED_USE)
#define CCAP_EXPORT __declspec(dllimport)
#else
#define CCAP_EXPORT
#endif
#else
#if defined(CCAP_SHARED_BUILD)
#define CCAP_EXPORT __attribute__((visibility("default")))
#else
#define CCAP_EXPORT
#endif
#endif

#endif  // COOLING_CAPACITY_EXPORT_HPP
