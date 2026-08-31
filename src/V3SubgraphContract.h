// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Immutable subgraph scheduling contract
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of either the GNU Lesser General Public License Version 3
// or the Perl Artistic License Version 2.0.
// SPDX-FileCopyrightText: 2026 Wilson Snyder
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#ifndef VERILATOR_V3SUBGRAPHCONTRACT_H_
#define VERILATOR_V3SUBGRAPHCONTRACT_H_

#include "config_build.h"
#include "verilatedos.h"

#include <unordered_map>
#include <vector>

class AstCFunc;
class AstScope;
class AstSenTree;
class AstVarScope;

class V3SubgraphContract final {
public:
    struct Use final {
        AstVarScope* m_varScopep;
        bool m_read;
        bool m_write;
        bool m_cuttable;
    };
    struct LogicalUse final {
        std::string m_name;
        bool m_read;
        bool m_write;
    };

private:
    AstScope* const m_boundaryScopep;
    AstSenTree* const m_domainp;
    const bool m_post;
    const std::vector<Use> m_boundaryUses;
    const std::vector<Use> m_externalUses;
    const std::vector<Use> m_internalUses;

    V3SubgraphContract(AstScope* boundaryScopep, AstSenTree* domainp, bool post,
                       std::vector<Use>&& boundaryUses, std::vector<Use>&& externalUses,
                       std::vector<Use>&& internalUses);

public:
    static V3SubgraphContract make(AstCFunc* funcp, AstScope* boundaryScopep, AstSenTree* domainp,
                                   bool post, bool refresh);
    static V3SubgraphContract
    remap(const V3SubgraphContract& source, AstScope* boundaryScopep, AstSenTree* domainp,
          const std::unordered_map<AstVarScope*, AstVarScope*>& sourceToTarget);
    static std::vector<LogicalUse> makeLogicalBoundaryUses(AstScope* boundaryScopep);

    AstScope* boundaryScopep() const { return m_boundaryScopep; }
    AstSenTree* domainp() const { return m_domainp; }
    bool post() const { return m_post; }
    const std::vector<Use>& boundaryUses() const { return m_boundaryUses; }
    const std::vector<Use>& externalUses() const { return m_externalUses; }
    const std::vector<Use>& internalUses() const { return m_internalUses; }
};

#endif
