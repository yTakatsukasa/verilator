// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Experimental subgraph scheduling helpers
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

#ifndef VERILATOR_V3SCHEDSUBGRAPH_H_
#define VERILATOR_V3SCHEDSUBGRAPH_H_

#include "config_build.h"
#include "verilatedos.h"

#include "V3Order.h"
#include "V3Sched.h"

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace V3Sched {

class SubgraphPlan final {
    struct Impl;
    const std::unique_ptr<Impl> m_impl;

public:
    struct Use final {
        AstVarScope* m_vscp = nullptr;
        bool m_read = false;
        bool m_write = false;
    };

    explicit SubgraphPlan(AstNetlist* netlistp);
    ~SubgraphPlan();
    VL_UNCOPYABLE(SubgraphPlan);

    bool extract(AstScope* scopep, AstActive* activep);
    void breakCycles(AstNetlist* netlistp);
    void partitionAndReplicate();
    void materializeNba(const std::unordered_map<const AstSenTree*, AstSenTree*>& senTreeMap,
                        const std::vector<LogicByScope*>& parentLogic);
    void foreachUse(const std::function<void(const Use&)>& callback) const;
    void foreachBoundary(const std::function<void(AstScope*, AstSenTree*,
                                                  const std::vector<Use>&)>& callback) const;
};

V3Order::FreshReads lowerSubgraphNbaLogic(AstNetlist* netlistp,
                                          const std::vector<LogicByScope*>& logic,
                                          const V3Order::TrigToSenMap& trigToSen,
                                          const CovergroupRefBindings& cgRefBindings, bool slow,
                                          const V3Order::ExternalDomainsProvider& externalDomains,
                                          V3Order::BoundaryUses& boundaryUses);

}  // namespace V3Sched

#endif  // Guard
