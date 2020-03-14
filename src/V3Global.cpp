// -*- mode: C++; c-file-style: "cc-mode" -*-
//*************************************************************************
// DESCRIPTION: Verilator: Common implemenetations
//
// Code available from: https://verilator.org
//
//*************************************************************************
//
// Copyright 2004-2020 by Wilson Snyder. This program is free software; you
// can redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
// SPDX-License-Identifier: LGPL-3.0-only OR Artistic-2.0
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"

#include "V3Global.h"
#include "V3Ast.h"
#include "V3File.h"
#include "V3LinkCells.h"
#include "V3Os.h"
#include "V3Parse.h"
#include "V3ParseSym.h"
#include "V3Stats.h"

#include <algorithm>  // sort

class DeleteModules : public AstNVisitor {
    std::map<std::string, bool> m_modules;  // Module_name, delete_if_true
    const bool m_delete;
    virtual void visit(AstNode* nodep) VL_OVERRIDE { iterateChildren(nodep); }
    virtual void visit(AstNodeModule* nodep) VL_OVERRIDE {
        std::map<std::string, bool>::iterator it = m_modules.find(nodep->name());
        if (it == m_modules.end() ? m_delete : it->second) { pushDeletep(nodep->unlinkFrBack()); }
    }

public:
    explicit DeleteModules(bool del)
        : m_delete(del) {}
    void add(const std::string& name, const bool del) {
        m_modules.insert(std::make_pair(name, del));
    }
    void run(AstNode* nodep) {
        iterate(nodep);
        doDeletes();
    }
};

static void readFilesAndLink(AstNetlist* rootp, const V3Options* optp,
                             const V3StringList* extraFiles, DeleteModules* deletep,
                             V3HierBlockPlan* planp) {
    // NODE STATE
    //   AstNode::user4p()      // VSymEnt*    Package and typedef symbol names
    AstUser4InUse inuser4;

    VInFilter filter(optp->pipeFilter());
    V3ParseSym parseSyms(rootp);  // Symbol table must be common across all parsing

    V3Parse parser(rootp, &filter, &parseSyms);
    // Read top module
    const V3StringList& vFiles = optp->vFiles();
    for (V3StringList::const_iterator it = vFiles.begin(); it != vFiles.end(); ++it) {
        string filename = *it;
        parser.parseFile(new FileLine(FileLine::commandLineFilename()), filename, false,
                         "Cannot find file containing module: ");
    }

    // Read libraries
    // To be compatible with other simulators,
    // this needs to be done after the top file is read
    const V3StringSet& libraryFiles = optp->libraryFiles();
    for (V3StringSet::const_iterator it = libraryFiles.begin(); it != libraryFiles.end(); ++it) {
        string filename = *it;
        parser.parseFile(new FileLine(FileLine::commandLineFilename()), filename, true,
                         "Cannot find file containing library module: ");
    }
    // v3Global.rootp()->dumpTreeFile(v3Global.debugFilename("parse.tree"));
    V3Error::abortIfErrors();

    // Delete specified modules
    if (deletep) deletep->run(rootp);

    // Load extra files
    if (extraFiles) {
        for (V3StringList::const_iterator it = extraFiles->begin(); it != extraFiles->end();
             ++it) {
            string filename = *it;
            parser.parseFile(new FileLine(FileLine::commandLineFilename()), filename, false,
                             "Cannot find file containing module: ");
        }
    }

    if (!optp->preprocOnly()) {
        // Resolve all modules cells refer to
        V3LinkCells::link(rootp, &filter, &parseSyms, planp);
    }
}

//######################################################################

class V3HierBlockPlan::HierBlock {
    const AstNodeModule* m_modp;
    std::set<HierBlock*> m_users;
    std::set<const HierBlock*> m_usees;
    std::set<const AstNodeModule*> m_useesNonHier;

public:
    explicit HierBlock(const AstNodeModule* modp)
        : m_modp(modp) {}
    ~HierBlock() {
        UASSERT(m_usees.empty(), "at least one module must be a leaf");
        for (std::set<HierBlock*>::const_iterator hit = m_users.begin(); hit != m_users.end();
             ++hit) {
            const bool deleted = (*hit)->m_usees.erase(this);
            UASSERT_OBJ(deleted, m_modp, " is not registered");
        }
    }
    void addUser(HierBlock* user) { m_users.insert(user); }
    void addUsee(const HierBlock* usee) { m_usees.insert(usee); }
    void addUseeNonHier(const AstNodeModule* usee) { m_useesNonHier.insert(usee); }
    bool hasUsees() const { return !m_usees.empty(); }
    const std::set<HierBlock*>& users() const { return m_users; }
    const std::set<const AstNodeModule*>& useesNonHier() const { return m_useesNonHier; }
    const AstNodeModule* modp() const { return m_modp; }
    struct SortByUssesCount {
        bool operator()(const HierBlock* a, const HierBlock* b) const {
            return b->m_usees.size() < a->m_usees.size();
        }
    };
};

//######################################################################

V3HierBlockPlan::V3HierBlockPlan()
    : m_state(STATE_INIT) {}

bool V3HierBlockPlan::isHierBlock(const AstNodeModule* modp) const {
    return m_blocks.find(modp) != m_blocks.end();
}

void V3HierBlockPlan::add(const AstNodeModule* modp) {
    std::map<const AstNodeModule*, HierBlock*>::iterator it = m_blocks.find(modp);
    if (it == m_blocks.end()) m_blocks.insert(std::make_pair(modp, new HierBlock(modp)));
}

void V3HierBlockPlan::registerUsage(const AstNodeModule* userp, const AstNodeModule* useep) {
    std::map<const AstNodeModule*, HierBlock*>::iterator user = m_blocks.find(userp);
    UASSERT_OBJ(user != m_blocks.end(), userp, "must be added");
    std::map<const AstNodeModule*, HierBlock*>::iterator usee = m_blocks.find(useep);
    if (usee == m_blocks.end()) {
        user->second->addUseeNonHier(useep);  // useep is not hierarchical block
    } else {
        user->second->addUsee(usee->second);
        usee->second->addUser(user->second);
    }
}

void V3HierBlockPlan::copyOptions(const V3Options* srcp, V3Options* dstp) {
    dstp->m_topModule = srcp->m_topModule;
    dstp->m_prefix = srcp->m_prefix;
    dstp->m_modPrefix = srcp->m_prefix;
    dstp->m_protectLib = srcp->m_protectLib;
    dstp->m_makeDir = srcp->m_makeDir;
    dstp->m_cppFiles = srcp->m_cppFiles;
    dstp->m_exe = srcp->m_exe;
    dstp->m_systemC = srcp->m_systemC;
}

bool V3HierBlockPlan::updateParameter(V3Global* globalp) {
    if (m_state == STATE_DONE) {
        UASSERT(m_sortedBlocks.empty(), "must be empty");
        return false;
    } else if (m_state == STATE_INIT) {
        UASSERT(m_generatedWrappers.empty(), "must be empty");
        copyOptions(&globalp->opt, &m_origOpt);
        for (std::map<const AstNodeModule*, HierBlock*>::iterator it = m_blocks.begin();
             it != m_blocks.end(); ++it) {
            m_sortedBlocks.push_back(it->second);
        }
        if (m_sortedBlocks.empty()) {  // no hierarchical block exists
            m_state = STATE_DONE;
            return true;
        } else {
            m_state = STATE_HIER_BLOCK;  // fall through
        }
    } else if (m_state == STATE_TOP) {
        UASSERT(m_sortedBlocks.empty(), "must be empty");  // the last
        UINFO(3, "Start top module");
        copyOptions(&m_origOpt, &globalp->opt);
        globalp->opt.m_hierBlocks = m_origOpt.m_hierBlocks;

        VL_DO_DANGLING(delete globalp->m_rootp, globalp->m_rootp);
        globalp->init();
        globalp->boot();
        {
            DeleteModules deleter(false);  // Keep modules that are not found in the list
            for (V3StringList::const_iterator it = m_origOpt.m_hierBlocks.begin();
                 it != m_origOpt.m_hierBlocks.end(); ++it) {
                deleter.add(
                    *it,
                    true);  // Delete all hierachy blocks because wrappers will be loaded later.
            }
            readFilesAndLink(globalp->rootp(), &globalp->opt, &m_generatedWrappers, &deleter,
                             NULL);
        }
        m_state = STATE_DONE;
        return true;  // done
    }

    UASSERT(m_state == STATE_HIER_BLOCK, "processing hierachy block");

    if (m_sortedBlocks.back()->hasUsees()) {
        std::sort(m_sortedBlocks.begin(), m_sortedBlocks.end(), HierBlock::SortByUssesCount());
    }

    HierBlock* hblockp = m_sortedBlocks.back();
    UASSERT_OBJ(!hblockp->hasUsees(), hblockp->modp(),
                " must not depend on other hierarchical block");
    m_sortedBlocks.pop_back();
    const bool erased = m_blocks.erase(hblockp->modp());
    UASSERT_OBJ(erased, hblockp->modp(), " failed to erase");
    UINFO(3, "Verilate " << hblockp->modp()->prettyNameQ() << std::endl);
    // doit for hblockp->modp();
    {
        const std::string modname = hblockp->modp()->name();
        globalp->opt.m_topModule = modname;
        globalp->opt.m_prefix = "V" + modname;
        globalp->opt.m_modPrefix = "V" + modname;
        globalp->opt.m_protectLib = modname;
        globalp->opt.m_makeDir = m_origOpt.m_makeDir + "/" + globalp->opt.m_prefix;
        globalp->opt.m_cppFiles.clear();
        globalp->opt.m_exe = false;
        globalp->opt.m_systemC = false;

        // reset to orig
        VL_DO_DANGLING(delete globalp->m_rootp, globalp->m_rootp);
        globalp->init();
        globalp->boot();
        V3Os::createDir(globalp->opt.m_makeDir);

        {
            DeleteModules deleter(true);  // Delete modules that are not found in the list.
            const std::set<const AstNodeModule*>& useesNonHier = hblockp->useesNonHier();
            for (std::set<const AstNodeModule*>::const_iterator it = useesNonHier.begin();
                 it != useesNonHier.end(); ++it) {
                deleter.add((*it)->name(),
                            isHierBlock((*it)));  // Delete if (*it) is a hierarchy block.
            }
            deleter.add(modname, false);
            readFilesAndLink(globalp->rootp(), &globalp->opt, &m_generatedWrappers, &deleter,
                             NULL);
        }

        m_generatedWrappers.push_back(globalp->opt.m_makeDir + "/" + modname + ".sv");
        m_origOpt.m_hierBlocks.push_back(modname);
    }
    delete hblockp;
    if (m_sortedBlocks.empty()) m_state = STATE_TOP;
    return true;
}

//######################################################################
// V3 Class -- top level

AstNetlist* V3Global::makeNetlist() {
    AstNetlist* newp = new AstNetlist();
    newp->addTypeTablep(new AstTypeTable(newp->fileline()));
    return newp;
}

void V3Global::checkTree() { rootp()->checkTree(); }

void V3Global::clear() {
    if (m_rootp) VL_DO_CLEAR(m_rootp->deleteTree(), m_rootp = NULL);
    if (m_planp) VL_DO_DANGLING(delete m_planp, m_planp);
}

void V3Global::readFiles() {
    if (!v3Global.opt.preprocOnly()) { m_planp = new V3HierBlockPlan(); }
    readFilesAndLink(rootp(), &opt, NULL, NULL, m_planp);
}

void V3Global::dumpCheckGlobalTree(const string& stagename, int newNumber, bool doDump) {
    v3Global.rootp()->dumpTreeFile(v3Global.debugFilename(stagename + ".tree", newNumber), false,
                                   doDump);
    if (v3Global.opt.stats()) V3Stats::statsStage(stagename);
}

bool V3Global::nextHierBlock() {
    UASSERT(!opt.preprocOnly(), "This function can not be called for preprocess only mode");
    const bool need_to_do = m_planp->updateParameter(this);
    return need_to_do;
}
