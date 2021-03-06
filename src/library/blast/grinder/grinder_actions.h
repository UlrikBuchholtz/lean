/*
Copyright (c) 2015 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#pragma once
#include "library/blast/action_result.h"
#include "library/blast/hypothesis.h"

namespace lean {
namespace blast {
action_result grinder_elim_action(hypothesis_idx hidx);
action_result grinder_intro_action();
void initialize_grinder_actions();
void finalize_grinder_actions();
}}
