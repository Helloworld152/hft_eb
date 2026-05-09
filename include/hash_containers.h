#pragma once

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"

template <typename... Args>
using FastHashMap = absl::flat_hash_map<Args...>;

template <typename... Args>
using FastHashSet = absl::flat_hash_set<Args...>;
