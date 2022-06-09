// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include <memory>

#include "core/providers/providers.h"

#include "core/providers/rocm/rocm_execution_provider_info.h"

namespace onnxruntime {
struct OrtROCMProviderOptions;

// defined in provider_bridge_ort.cc
std::shared_ptr<IExecutionProviderFactory> CreateExecutionProviderFactory_Rocm(
    const OrtROCMProviderOptions* provider_options);

}  // namespace onnxruntime
