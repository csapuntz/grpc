//
// Copyright 2015 gRPC authors.
//
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
//

#include <grpc/support/port_platform.h>

#include <stdlib.h>
#include <string.h>

#include <grpc/support/alloc.h>

#include "src/core/ext/filters/client_channel/lb_policy/subchannel_list.h"
#include "src/core/ext/filters/client_channel/lb_policy_registry.h"
#include "src/core/ext/filters/client_channel/subchannel.h"
#include "src/core/lib/address_utils/sockaddr_utils.h"
#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/debug/trace.h"
#include "src/core/lib/gprpp/ref_counted_ptr.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"

namespace grpc_core {

TraceFlag grpc_lb_round_robin_trace(false, "round_robin");

namespace {

//
// round_robin LB policy
//

constexpr char kRoundRobin[] = "round_robin";

class RoundRobin : public LoadBalancingPolicy {
 public:
  explicit RoundRobin(Args args);

  const char* name() const override { return kRoundRobin; }

  void UpdateLocked(UpdateArgs args) override;
  void ResetBackoffLocked() override;

 private:
  ~RoundRobin() override;

  // Forward declaration.
  class RoundRobinSubchannelList;

  // Data for a particular subchannel in a subchannel list.
  // This subclass adds the following functionality:
  // - Tracks the previous connectivity state of the subchannel, so that
  //   we know how many subchannels are in each state.
  class RoundRobinSubchannelData
      : public SubchannelData<RoundRobinSubchannelList,
                              RoundRobinSubchannelData> {
   public:
    RoundRobinSubchannelData(
        SubchannelList<RoundRobinSubchannelList, RoundRobinSubchannelData>*
            subchannel_list,
        const ServerAddress& address,
        RefCountedPtr<SubchannelInterface> subchannel)
        : SubchannelData(subchannel_list, address, std::move(subchannel)) {}

    grpc_connectivity_state connectivity_state() const {
      return logical_connectivity_state_;
    }

    // Computes and updates the logical connectivity state of the subchannel.
    // Note that the logical connectivity state may differ from the
    // actual reported state in some cases (e.g., after we see
    // TRANSIENT_FAILURE, we ignore any subsequent state changes until
    // we see READY).  Returns true if the state changed.
    bool UpdateLogicalConnectivityStateLocked(
        grpc_connectivity_state connectivity_state);

   private:
    // Performs connectivity state updates that need to be done only
    // after we have started watching.
    void ProcessConnectivityChangeLocked(
        grpc_connectivity_state connectivity_state) override;

    grpc_connectivity_state logical_connectivity_state_ = GRPC_CHANNEL_IDLE;
  };

  // A list of subchannels.
  class RoundRobinSubchannelList
      : public SubchannelList<RoundRobinSubchannelList,
                              RoundRobinSubchannelData> {
   public:
    RoundRobinSubchannelList(RoundRobin* policy, TraceFlag* tracer,
                             ServerAddressList addresses,
                             const grpc_channel_args& args)
        : SubchannelList(policy, tracer, std::move(addresses),
                         policy->channel_control_helper(), args) {
      // Need to maintain a ref to the LB policy as long as we maintain
      // any references to subchannels, since the subchannels'
      // pollset_sets will include the LB policy's pollset_set.
      policy->Ref(DEBUG_LOCATION, "subchannel_list").release();
    }

    ~RoundRobinSubchannelList() override {
      RoundRobin* p = static_cast<RoundRobin*>(policy());
      p->Unref(DEBUG_LOCATION, "subchannel_list");
    }

    // Starts watching the subchannels in this list.
    void StartWatchingLocked(absl::Status status_for_tf);

    // Updates the counters of subchannels in each state when a
    // subchannel transitions from old_state to new_state.
    void UpdateStateCountersLocked(grpc_connectivity_state old_state,
                                   grpc_connectivity_state new_state);

    // Ensures that the right subchannel list is used and then updates
    // the RR policy's connectivity state based on the subchannel list's
    // state counters.
    void MaybeUpdateRoundRobinConnectivityStateLocked(
        absl::Status status_for_tf);

   private:
    std::string CountersString() const {
      return absl::StrCat("num_subchannels=", num_subchannels(),
                          " num_ready=", num_ready_,
                          " num_connecting=", num_connecting_,
                          " num_transient_failure=", num_transient_failure_);
    }

    size_t num_ready_ = 0;
    size_t num_connecting_ = 0;
    size_t num_transient_failure_ = 0;
  };

  class Picker : public SubchannelPicker {
   public:
    Picker(RoundRobin* parent, RoundRobinSubchannelList* subchannel_list);

    PickResult Pick(PickArgs args) override;

   private:
    // Using pointer value only, no ref held -- do not dereference!
    RoundRobin* parent_;

    size_t last_picked_index_;
    absl::InlinedVector<RefCountedPtr<SubchannelInterface>, 10> subchannels_;
  };

  void ShutdownLocked() override;

  // List of subchannels.
  OrphanablePtr<RoundRobinSubchannelList> subchannel_list_;
  // Latest pending subchannel list.
  // When we get an updated address list, we create a new subchannel list
  // for it here, and we wait to swap it into subchannel_list_ until the new
  // list becomes READY.
  OrphanablePtr<RoundRobinSubchannelList> latest_pending_subchannel_list_;

  bool shutdown_ = false;
};

//
// RoundRobin::Picker
//

RoundRobin::Picker::Picker(RoundRobin* parent,
                           RoundRobinSubchannelList* subchannel_list)
    : parent_(parent) {
  for (size_t i = 0; i < subchannel_list->num_subchannels(); ++i) {
    RoundRobinSubchannelData* sd = subchannel_list->subchannel(i);
    if (sd->connectivity_state() == GRPC_CHANNEL_READY) {
      subchannels_.push_back(sd->subchannel()->Ref());
    }
  }
  // For discussion on why we generate a random starting index for
  // the picker, see https://github.com/grpc/grpc-go/issues/2580.
  // TODO(roth): rand(3) is not thread-safe.  This should be replaced with
  // something better as part of https://github.com/grpc/grpc/issues/17891.
  last_picked_index_ = rand() % subchannels_.size();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(GPR_INFO,
            "[RR %p picker %p] created picker from subchannel_list=%p "
            "with %" PRIuPTR " READY subchannels; last_picked_index_=%" PRIuPTR,
            parent_, this, subchannel_list, subchannels_.size(),
            last_picked_index_);
  }
}

RoundRobin::PickResult RoundRobin::Picker::Pick(PickArgs /*args*/) {
  last_picked_index_ = (last_picked_index_ + 1) % subchannels_.size();
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(GPR_INFO,
            "[RR %p picker %p] returning index %" PRIuPTR ", subchannel=%p",
            parent_, this, last_picked_index_,
            subchannels_[last_picked_index_].get());
  }
  return PickResult::Complete(subchannels_[last_picked_index_]);
}

//
// RoundRobin
//

RoundRobin::RoundRobin(Args args) : LoadBalancingPolicy(std::move(args)) {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(GPR_INFO, "[RR %p] Created", this);
  }
}

RoundRobin::~RoundRobin() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(GPR_INFO, "[RR %p] Destroying Round Robin policy", this);
  }
  GPR_ASSERT(subchannel_list_ == nullptr);
  GPR_ASSERT(latest_pending_subchannel_list_ == nullptr);
}

void RoundRobin::ShutdownLocked() {
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(GPR_INFO, "[RR %p] Shutting down", this);
  }
  shutdown_ = true;
  subchannel_list_.reset();
  latest_pending_subchannel_list_.reset();
}

void RoundRobin::ResetBackoffLocked() {
  subchannel_list_->ResetBackoffLocked();
  if (latest_pending_subchannel_list_ != nullptr) {
    latest_pending_subchannel_list_->ResetBackoffLocked();
  }
}

void RoundRobin::UpdateLocked(UpdateArgs args) {
  ServerAddressList addresses;
  if (args.addresses.ok()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO, "[RR %p] received update with %" PRIuPTR " addresses",
              this, args.addresses->size());
    }
    addresses = std::move(*args.addresses);
  } else {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO, "[RR %p] received update with address error: %s", this,
              args.addresses.status().ToString().c_str());
    }
    // If we already have a subchannel list, then ignore the resolver
    // failure and keep using the existing list.
    if (subchannel_list_ != nullptr) return;
  }
  // Create new subchannel list, replacing the previous pending list, if any.
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace) &&
      latest_pending_subchannel_list_ != nullptr) {
    gpr_log(GPR_INFO, "[RR %p] replacing previous pending subchannel list %p",
            this, latest_pending_subchannel_list_.get());
  }
  latest_pending_subchannel_list_ = MakeOrphanable<RoundRobinSubchannelList>(
      this, &grpc_lb_round_robin_trace, std::move(addresses), *args.args);
  // Start watching the new list.  If appropriate, this will cause it to be
  // immediately promoted to subchannel_list_ and to generate a new picker.
  latest_pending_subchannel_list_->StartWatchingLocked(
      args.addresses.ok() ? absl::UnavailableError(absl::StrCat(
                                "empty address list: ", args.resolution_note))
                          : args.addresses.status());
}

//
// RoundRobinSubchannelList
//

void RoundRobin::RoundRobinSubchannelList::StartWatchingLocked(
    absl::Status status_for_tf) {
  // Check current state of each subchannel synchronously, since any
  // subchannel already used by some other channel may have a non-IDLE
  // state.
  for (size_t i = 0; i < num_subchannels(); ++i) {
    grpc_connectivity_state state =
        subchannel(i)->CheckConnectivityStateLocked();
    if (state != GRPC_CHANNEL_IDLE) {
      subchannel(i)->UpdateLogicalConnectivityStateLocked(state);
    }
  }
  // Start connectivity watch for each subchannel.
  for (size_t i = 0; i < num_subchannels(); i++) {
    if (subchannel(i)->subchannel() != nullptr) {
      subchannel(i)->StartConnectivityWatchLocked();
      subchannel(i)->subchannel()->RequestConnection();
    }
  }
  // Update RR connectivity state if needed.
  MaybeUpdateRoundRobinConnectivityStateLocked(status_for_tf);
}

void RoundRobin::RoundRobinSubchannelList::UpdateStateCountersLocked(
    grpc_connectivity_state old_state, grpc_connectivity_state new_state) {
  GPR_ASSERT(old_state != GRPC_CHANNEL_SHUTDOWN);
  GPR_ASSERT(new_state != GRPC_CHANNEL_SHUTDOWN);
  if (old_state == GRPC_CHANNEL_READY) {
    GPR_ASSERT(num_ready_ > 0);
    --num_ready_;
  } else if (old_state == GRPC_CHANNEL_CONNECTING) {
    GPR_ASSERT(num_connecting_ > 0);
    --num_connecting_;
  } else if (old_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    GPR_ASSERT(num_transient_failure_ > 0);
    --num_transient_failure_;
  }
  if (new_state == GRPC_CHANNEL_READY) {
    ++num_ready_;
  } else if (new_state == GRPC_CHANNEL_CONNECTING) {
    ++num_connecting_;
  } else if (new_state == GRPC_CHANNEL_TRANSIENT_FAILURE) {
    ++num_transient_failure_;
  }
}

void RoundRobin::RoundRobinSubchannelList::
    MaybeUpdateRoundRobinConnectivityStateLocked(absl::Status status_for_tf) {
  RoundRobin* p = static_cast<RoundRobin*>(policy());
  // If this is latest_pending_subchannel_list_, then swap it into
  // subchannel_list_ in the following cases:
  // - subchannel_list_ is null (i.e., this is the first update).
  // - subchannel_list_ has no READY subchannels.
  // - This list has at least one READY subchannel.
  // - All of the subchannels in this list are in TRANSIENT_FAILURE, or
  //   the list is empty.  (This may cause the channel to go from READY
  //   to TRANSIENT_FAILURE, but we're doing what the control plane told
  //   us to do.
  if (p->latest_pending_subchannel_list_.get() == this &&
      (p->subchannel_list_ == nullptr || p->subchannel_list_->num_ready_ == 0 ||
       num_ready_ > 0 ||
       // Note: num_transient_failure_ and num_subchannels() may both be 0.
       num_transient_failure_ == num_subchannels())) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      const std::string old_counters_string =
          p->subchannel_list_ != nullptr ? p->subchannel_list_->CountersString()
                                         : "";
      gpr_log(
          GPR_INFO,
          "[RR %p] swapping out subchannel list %p (%s) in favor of %p (%s)", p,
          p->subchannel_list_.get(), old_counters_string.c_str(), this,
          CountersString().c_str());
    }
    p->subchannel_list_ = std::move(p->latest_pending_subchannel_list_);
  }
  // Only set connectivity state if this is the current subchannel list.
  if (p->subchannel_list_.get() != this) return;
  // First matching rule wins:
  // 1) ANY subchannel is READY => policy is READY.
  // 2) ANY subchannel is CONNECTING => policy is CONNECTING.
  // 3) ALL subchannels are TRANSIENT_FAILURE => policy is TRANSIENT_FAILURE.
  if (num_ready_ > 0) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO, "[RR %p] reporting READY with subchannel list %p", p,
              this);
    }
    p->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_READY, absl::Status(), absl::make_unique<Picker>(p, this));
  } else if (num_connecting_ > 0) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO, "[RR %p] reporting CONNECTING with subchannel list %p",
              p, this);
    }
    p->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_CONNECTING, absl::Status(),
        absl::make_unique<QueuePicker>(p->Ref(DEBUG_LOCATION, "QueuePicker")));
  } else if (num_transient_failure_ == num_subchannels()) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO,
              "[RR %p] reporting TRANSIENT_FAILURE with subchannel list %p: %s",
              p, this, status_for_tf.ToString().c_str());
    }
    p->channel_control_helper()->UpdateState(
        GRPC_CHANNEL_TRANSIENT_FAILURE, status_for_tf,
        absl::make_unique<TransientFailurePicker>(status_for_tf));
  }
}

//
// RoundRobinSubchannelData
//

bool RoundRobin::RoundRobinSubchannelData::UpdateLogicalConnectivityStateLocked(
    grpc_connectivity_state connectivity_state) {
  RoundRobin* p = static_cast<RoundRobin*>(subchannel_list()->policy());
  if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
    gpr_log(
        GPR_INFO,
        "[RR %p] connectivity changed for subchannel %p, subchannel_list %p "
        "(index %" PRIuPTR " of %" PRIuPTR "): prev_state=%s new_state=%s",
        p, subchannel(), subchannel_list(), Index(),
        subchannel_list()->num_subchannels(),
        ConnectivityStateName(logical_connectivity_state_),
        ConnectivityStateName(connectivity_state));
  }
  // Decide what state to report for aggregation purposes.
  // If the last logical state was TRANSIENT_FAILURE, then ignore the
  // state change unless the new state is READY.
  if (logical_connectivity_state_ == GRPC_CHANNEL_TRANSIENT_FAILURE &&
      connectivity_state != GRPC_CHANNEL_READY) {
    return false;
  }
  // If the new state is IDLE, treat it as CONNECTING, since it will
  // immediately transition into CONNECTING anyway.
  if (connectivity_state == GRPC_CHANNEL_IDLE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO,
              "[RR %p] subchannel %p, subchannel_list %p (index %" PRIuPTR
              " of %" PRIuPTR "): treating IDLE as CONNECTING",
              p, subchannel(), subchannel_list(), Index(),
              subchannel_list()->num_subchannels());
    }
    connectivity_state = GRPC_CHANNEL_CONNECTING;
  }
  // If no change, return false.
  if (logical_connectivity_state_ == connectivity_state) return false;
  // Otherwise, update counters and logical state.
  subchannel_list()->UpdateStateCountersLocked(logical_connectivity_state_,
                                               connectivity_state);
  logical_connectivity_state_ = connectivity_state;
  return true;
}

void RoundRobin::RoundRobinSubchannelData::ProcessConnectivityChangeLocked(
    grpc_connectivity_state connectivity_state) {
  RoundRobin* p = static_cast<RoundRobin*>(subchannel_list()->policy());
  GPR_ASSERT(subchannel() != nullptr);
  // If the new state is TRANSIENT_FAILURE or IDLE, re-resolve.
  // Only do this if we've started watching, not at startup time.
  // Otherwise, if the subchannel was already in state TRANSIENT_FAILURE
  // when the subchannel list was created, we'd wind up in a constant
  // loop of re-resolution.
  // Also attempt to reconnect.
  if (connectivity_state == GRPC_CHANNEL_TRANSIENT_FAILURE ||
      connectivity_state == GRPC_CHANNEL_IDLE) {
    if (GRPC_TRACE_FLAG_ENABLED(grpc_lb_round_robin_trace)) {
      gpr_log(GPR_INFO,
              "[RR %p] Subchannel %p reported %s; requesting re-resolution", p,
              subchannel(), ConnectivityStateName(connectivity_state));
    }
    p->channel_control_helper()->RequestReresolution();
    subchannel()->RequestConnection();
  }
  // Update logical connectivity state.
  // If it changed, update the policy state.
  if (UpdateLogicalConnectivityStateLocked(connectivity_state)) {
    subchannel_list()->MaybeUpdateRoundRobinConnectivityStateLocked(
        absl::UnavailableError("connections to all backends failing"));
  }
}

//
// factory
//

class RoundRobinConfig : public LoadBalancingPolicy::Config {
 public:
  const char* name() const override { return kRoundRobin; }
};

class RoundRobinFactory : public LoadBalancingPolicyFactory {
 public:
  OrphanablePtr<LoadBalancingPolicy> CreateLoadBalancingPolicy(
      LoadBalancingPolicy::Args args) const override {
    return MakeOrphanable<RoundRobin>(std::move(args));
  }

  const char* name() const override { return kRoundRobin; }

  RefCountedPtr<LoadBalancingPolicy::Config> ParseLoadBalancingConfig(
      const Json& /*json*/, grpc_error_handle* /*error*/) const override {
    return MakeRefCounted<RoundRobinConfig>();
  }
};

}  // namespace

}  // namespace grpc_core

void grpc_lb_policy_round_robin_init() {
  grpc_core::LoadBalancingPolicyRegistry::Builder::
      RegisterLoadBalancingPolicyFactory(
          absl::make_unique<grpc_core::RoundRobinFactory>());
}

void grpc_lb_policy_round_robin_shutdown() {}
