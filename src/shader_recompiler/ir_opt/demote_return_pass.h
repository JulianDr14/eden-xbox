// SPDX-FileCopyrightText: Copyright 2026 JulianDr14
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Optimization {
// Emission-time canonicalization for hosts with native helper-preserving demote.
// Only terminal discard arms containing identities/output writes are eligible.
// Returns the number of early returns removed; zero leaves the program untouched.
std::size_t DemoteReturnPass(IR::Program& program);
}
