// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Block code ordering
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2003-2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3ORDER_H_
#define VERILATOR_V3ORDER_H_

#include "config_build.h"
#include "verilatedos.h"

#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

class AstCFunc;
class AstNetlist;
class AstScope;
class AstSenItem;
class AstSenTree;
class AstVarScope;

namespace V3Sched {
struct LogicByScope;
class CovergroupRefBindings;
};  // namespace V3Sched

//============================================================================

namespace V3Order {

// Callable to add extra external Triggers to a variable
using ExternalDomainsProvider = std::function<void(const AstVarScope*, std::vector<AstSenTree*>&)>;
// Map from Trigger Sensitivity tree to original Sensitivity tree
using TrigToSenMap = std::unordered_map<const AstSenTree*, const AstSenTree*>;
// Inputs captured on the current edge, in deterministic insertion order per boundary.
using FreshReads = std::map<const AstScope*, std::vector<AstVarScope*>>;
// Effects of a shared child function, before binding its state to a receiver instance.
struct BoundaryUse final {
    AstVarScope* m_vscp = nullptr;
    bool m_read = false;
    bool m_write = false;
    bool m_delayedState = false;
};
struct BoundaryContract final {
    std::vector<BoundaryUse> m_uses;
    AstVarScope* m_phasePortp = nullptr;
    // An eligible FF boundary exposes acquired inputs and published outputs.
    // Its pre and post calls form a local transaction whose order must survive
    // hiding internal NBA temporaries.
    bool m_portOnly = false;
    bool m_post = false;
};
using BoundaryUses = std::unordered_map<const AstCFunc*, BoundaryContract>;

AstCFunc* order(AstNetlist* netlistp,  //
                const std::vector<V3Sched::LogicByScope*>& logic,  //
                const TrigToSenMap& trigToSen,  //
                const V3Sched::CovergroupRefBindings& cgRefBindings,  //
                const string& tag,  //
                bool parallel,  //
                bool slow,  //
                const ExternalDomainsProvider& externalDomains,  //
                AstScope* resultScopep = nullptr, const FreshReads* freshReadsp = nullptr,
                const BoundaryUses* boundaryUsesp = nullptr) VL_MT_DISABLED;

};  // namespace V3Order

#endif  // Guard
