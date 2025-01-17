// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <pthread.h>
#include <string.h>

#include "grpc/fork.h"
#include "kmsp11/main/fork_support.h"
#include "kmsp11/util/global_provider.h"
#include "kmsp11/util/logging.h"

namespace cloud_kms::kmsp11 {

absl::Status RegisterForkHandlers() {
  int result = pthread_atfork(grpc_prefork, grpc_postfork_parent, [] {
    grpc_postfork_child();
    // This deadlocks unless it comes after the gRPC postfork routine.
    // Presumably there is some mutex/counter of created gRPC objects.
    ReleaseGlobalProvider().IgnoreError();
    ShutdownLogging();
  });
  if (result != 0) {
    return absl::InternalError(
        absl::StrCat("pthread_atfork failed with error ", strerror(result)));
  }
  return absl::OkStatus();
}

}  // namespace cloud_kms::kmsp11