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

#ifndef COOLING_CAPACITY_ENGINE_HPP
#define COOLING_CAPACITY_ENGINE_HPP

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "cooling_capacity/candidate.hpp"
#include "cooling_capacity/clock.hpp"
#include "cooling_capacity/commitment.hpp"
#include "cooling_capacity/diff.hpp"
#include "cooling_capacity/reconciliation.hpp"
#include "cooling_capacity/rollup.hpp"
#include "cooling_capacity/store.hpp"

namespace cooling_capacity {

struct EngineOptions {
  StoreOpenOptions store;
  // Used when the store is created, and as the policy of a snapshot that has no
  // policy of its own. The policy of the current snapshot always wins once one
  // exists.
  CoolingPolicy policy = CoolingPolicy::defaults();
  ActorId actor;
  // The time source used for every freshness decision. When null the engine
  // owns a system clock. Nothing else in the library reads the clock.
  const Clock* clock = nullptr;
};

// The composition of a durable store and an analysis view.
//
// Concurrency contract. Readers never take a lock: they load an immutable
// snapshot through an atomic shared pointer, so a reader cannot block a writer
// and a writer cannot block a reader. Writers are serialised by one mutex, and
// that mutex is the only engine lock. The lock order is exactly:
//
//   engine mutation mutex  ->  store writer state  ->  operating-system file lock
//
// No callback, observer or user-supplied function is ever invoked while a lock
// is held, and the engine owns no threads, so there is no shutdown ordering to
// get wrong.
class CCAP_EXPORT CoolingCapacityEngine {
 public:
  [[nodiscard]] static Result<std::unique_ptr<CoolingCapacityEngine>> open(
      const std::string& directory, const EngineOptions& options);

  ~CoolingCapacityEngine();
  CoolingCapacityEngine(const CoolingCapacityEngine&) = delete;
  CoolingCapacityEngine& operator=(const CoolingCapacityEngine&) = delete;

  // The current immutable snapshot. Holding the returned pointer keeps the
  // generation alive even if a writer publishes a newer one.
  [[nodiscard]] std::shared_ptr<const CoolingSnapshot> snapshot() const;

  // How much authority the current snapshot carries. A snapshot recovered from
  // a store reports `RecoveredPendingRevalidation` until it is revalidated.
  [[nodiscard]] AnalysisBasis basis() const;

  [[nodiscard]] const std::string& directory() const noexcept { return store_->directory(); }

  // Re-reads the head generation from the store and adopts it. The adopted
  // snapshot carries the recovered basis until revalidated.
  [[nodiscard]] Result<CapacityGeneration> reload();

  [[nodiscard]] Result<LoopCapacity> loop_capacity(const LoopId& loop) const;
  [[nodiscard]] Result<PlantCapacity> plant_capacity(const PlantId& plant) const;
  [[nodiscard]] Result<ZoneCapacity> zone_capacity(const ZoneId& zone, CoolingMedium medium,
                                                   CompatibilityClass compatibility) const;
  [[nodiscard]] Result<CapacityValue> available(const ZoneId& zone, CoolingMedium medium,
                                                CompatibilityClass compatibility) const;
  [[nodiscard]] Result<CoolingCapacityReport> report() const;
  [[nodiscard]] Result<CandidateEvaluation> evaluate(const CandidateLoad& candidate) const;
  [[nodiscard]] Result<ReconciliationReport> reconcile() const;
  [[nodiscard]] Result<GenerationDiff> diff(const CapacityGeneration& from,
                                            const CapacityGeneration& to) const;
  [[nodiscard]] Result<std::vector<GenerationInfo>> generations() const;
  [[nodiscard]] Result<std::shared_ptr<const CoolingSnapshot>> load_generation(
      const CapacityGeneration& generation) const;

  // Copies the current generation into a builder whose generation number is the
  // next one. The builder is a private candidate: nothing is durable until
  // `publish` succeeds.
  [[nodiscard]] Result<SnapshotBuilder> begin_mutation() const;

  // Validates, then commits one generation through the store protocol. The
  // store's current head is the precondition: if another writer moved it, the
  // commit is refused with `StaleGeneration` and nothing is written.
  [[nodiscard]] Result<CommitReceipt> publish(SnapshotBuilder& builder);

  // Applies one commitment operation. The request names the generation it was
  // formed against; a request formed against an older generation is refused.
  [[nodiscard]] Result<CommitmentOutcome> apply(const CommitmentRequest& request);

  // Marks the recovered state as checked against current evidence and publishes
  // a new generation. Revalidation does not make stale evidence fresh: it
  // records that the state was examined at this instant.
  [[nodiscard]] Result<CapacityGeneration> revalidate(const Clock& clock);

  [[nodiscard]] Result<WriterEpoch> acquire_writer();
  [[nodiscard]] Result<void> release_writer();
  [[nodiscard]] bool holds_writer() const noexcept;

 private:
  CoolingCapacityEngine(std::unique_ptr<CoolingCapacityStore> store, EngineOptions options);

  Result<void> adopt(std::shared_ptr<const CoolingSnapshot> snapshot, AnalysisBasis basis);
  Result<CommitReceipt> commit_candidate(std::shared_ptr<const CoolingSnapshot> candidate);
  [[nodiscard]] Result<SnapshotBuilder> builder_from(const CoolingSnapshot& snapshot) const;
  [[nodiscard]] Timestamp clock_now() const;

  std::unique_ptr<CoolingCapacityStore> store_;
  EngineOptions options_;
  std::unique_ptr<Clock> owned_clock_;
  mutable std::mutex mutation_mutex_;
  std::atomic<std::shared_ptr<const CoolingSnapshot>> snapshot_;
  std::atomic<AnalysisBasis> basis_;
};

}  // namespace cooling_capacity

#endif  // COOLING_CAPACITY_ENGINE_HPP
