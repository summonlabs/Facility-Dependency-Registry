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

#ifndef FACILITY_DEPENDENCY_REGISTRY_FACILITY_DEPENDENCY_REGISTRY_HPP
#define FACILITY_DEPENDENCY_REGISTRY_FACILITY_DEPENDENCY_REGISTRY_HPP

/// Facility Dependency Registry: explicit cross-domain facility dependency
/// relationships and their canonical graph semantics.
///
/// Public surface, in dependency order. Every header is self-contained and may
/// be included on its own.

#include "facility_dependency_registry/cancel.hpp"
#include "facility_dependency_registry/clock.hpp"
#include "facility_dependency_registry/constraint.hpp"
#include "facility_dependency_registry/digest.hpp"
#include "facility_dependency_registry/edge.hpp"
#include "facility_dependency_registry/errors.hpp"
#include "facility_dependency_registry/export.hpp"
#include "facility_dependency_registry/ids.hpp"
#include "facility_dependency_registry/lifecycle.hpp"
#include "facility_dependency_registry/limits.hpp"
#include "facility_dependency_registry/mask.hpp"
#include "facility_dependency_registry/node_ref.hpp"
#include "facility_dependency_registry/persistence.hpp"
#include "facility_dependency_registry/provenance.hpp"
#include "facility_dependency_registry/query.hpp"
#include "facility_dependency_registry/registry.hpp"
#include "facility_dependency_registry/requests.hpp"
#include "facility_dependency_registry/snapshot.hpp"
#include "facility_dependency_registry/version.hpp"

#endif  // FACILITY_DEPENDENCY_REGISTRY_FACILITY_DEPENDENCY_REGISTRY_HPP
