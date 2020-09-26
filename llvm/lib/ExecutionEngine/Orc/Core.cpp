//===--- Core.cpp - Core ORC APIs (MaterializationUnit, JITDylib, etc.) ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/Core.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/Orc/DebugUtils.h"
#include "llvm/ExecutionEngine/Orc/OrcError.h"
#include "llvm/Support/FormatVariadic.h"

#include <condition_variable>
#if LLVM_ENABLE_THREADS
#include <future>
#endif

#define DEBUG_TYPE "orc"

namespace llvm {
namespace orc {

char ResourceTrackerDefunct::ID = 0;
char FailedToMaterialize::ID = 0;
char SymbolsNotFound::ID = 0;
char SymbolsCouldNotBeRemoved::ID = 0;
char MissingSymbolDefinitions::ID = 0;
char UnexpectedSymbolDefinitions::ID = 0;

RegisterDependenciesFunction NoDependenciesToRegister =
    RegisterDependenciesFunction();

void MaterializationUnit::anchor() {}

ResourceTracker::ResourceTracker(JITDylibSP JD) {
  assert((reinterpret_cast<uintptr_t>(JD.get()) & 0x1) == 0 &&
         "JITDylib must be two byte aligned");
  JD->Retain();
  JDAndFlag.store(reinterpret_cast<uintptr_t>(JD.get()));
}

ResourceTracker::~ResourceTracker() {
  getJITDylib().getExecutionSession().destroyResourceTracker(*this);
  getJITDylib().Release();
}

Error ResourceTracker::remove() {
  return getJITDylib().getExecutionSession().removeResourceTracker(*this);
}

void ResourceTracker::transferTo(ResourceTracker &DstRT) {
  getJITDylib().getExecutionSession().transferResourceTracker(DstRT, *this);
}

void ResourceTracker::makeDefunct() {
  uintptr_t Val = JDAndFlag.load();
  Val |= 0x1U;
  JDAndFlag.store(Val);
}

ResourceManager::~ResourceManager() {}

ResourceTrackerDefunct::ResourceTrackerDefunct(ResourceTrackerSP RT)
    : RT(std::move(RT)) {}

std::error_code ResourceTrackerDefunct::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void ResourceTrackerDefunct::log(raw_ostream &OS) const {
  OS << "Resource tracker " << (void *)RT.get() << " became defunct";
}

FailedToMaterialize::FailedToMaterialize(
    std::shared_ptr<SymbolDependenceMap> Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols->empty() && "Can not fail to resolve an empty set");
}

std::error_code FailedToMaterialize::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void FailedToMaterialize::log(raw_ostream &OS) const {
  OS << "Failed to materialize symbols: " << *Symbols;
}

SymbolsNotFound::SymbolsNotFound(SymbolNameSet Symbols) {
  for (auto &Sym : Symbols)
    this->Symbols.push_back(Sym);
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

SymbolsNotFound::SymbolsNotFound(SymbolNameVector Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

std::error_code SymbolsNotFound::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void SymbolsNotFound::log(raw_ostream &OS) const {
  OS << "Symbols not found: " << Symbols;
}

SymbolsCouldNotBeRemoved::SymbolsCouldNotBeRemoved(SymbolNameSet Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

std::error_code SymbolsCouldNotBeRemoved::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void SymbolsCouldNotBeRemoved::log(raw_ostream &OS) const {
  OS << "Symbols could not be removed: " << Symbols;
}

std::error_code MissingSymbolDefinitions::convertToErrorCode() const {
  return orcError(OrcErrorCode::MissingSymbolDefinitions);
}

void MissingSymbolDefinitions::log(raw_ostream &OS) const {
  OS << "Missing definitions in module " << ModuleName
     << ": " << Symbols;
}

std::error_code UnexpectedSymbolDefinitions::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnexpectedSymbolDefinitions);
}

void UnexpectedSymbolDefinitions::log(raw_ostream &OS) const {
  OS << "Unexpected definitions in module " << ModuleName
     << ": " << Symbols;
}

AsynchronousSymbolQuery::AsynchronousSymbolQuery(
    const SymbolLookupSet &Symbols, SymbolState RequiredState,
    SymbolsResolvedCallback NotifyComplete)
    : NotifyComplete(std::move(NotifyComplete)), RequiredState(RequiredState) {
  assert(RequiredState >= SymbolState::Resolved &&
         "Cannot query for a symbols that have not reached the resolve state "
         "yet");

  OutstandingSymbolsCount = Symbols.size();

  for (auto &KV : Symbols)
    ResolvedSymbols[KV.first] = nullptr;
}

void AsynchronousSymbolQuery::notifySymbolMetRequiredState(
    const SymbolStringPtr &Name, JITEvaluatedSymbol Sym) {
  auto I = ResolvedSymbols.find(Name);
  assert(I != ResolvedSymbols.end() &&
         "Resolving symbol outside the requested set");
  assert(I->second.getAddress() == 0 && "Redundantly resolving symbol Name");

  // If this is a materialization-side-effects-only symbol then drop it,
  // otherwise update its map entry with its resolved address.
  if (Sym.getFlags().hasMaterializationSideEffectsOnly())
    ResolvedSymbols.erase(I);
  else
    I->second = std::move(Sym);
  --OutstandingSymbolsCount;
}

void AsynchronousSymbolQuery::handleComplete() {
  assert(OutstandingSymbolsCount == 0 &&
         "Symbols remain, handleComplete called prematurely");

  auto TmpNotifyComplete = std::move(NotifyComplete);
  NotifyComplete = SymbolsResolvedCallback();
  TmpNotifyComplete(std::move(ResolvedSymbols));
}

void AsynchronousSymbolQuery::handleFailed(Error Err) {
  assert(QueryRegistrations.empty() && ResolvedSymbols.empty() &&
         OutstandingSymbolsCount == 0 &&
         "Query should already have been abandoned");
  NotifyComplete(std::move(Err));
  NotifyComplete = SymbolsResolvedCallback();
}

void AsynchronousSymbolQuery::addQueryDependence(JITDylib &JD,
                                                 SymbolStringPtr Name) {
  bool Added = QueryRegistrations[&JD].insert(std::move(Name)).second;
  (void)Added;
  assert(Added && "Duplicate dependence notification?");
}

void AsynchronousSymbolQuery::removeQueryDependence(
    JITDylib &JD, const SymbolStringPtr &Name) {
  auto QRI = QueryRegistrations.find(&JD);
  assert(QRI != QueryRegistrations.end() &&
         "No dependencies registered for JD");
  assert(QRI->second.count(Name) && "No dependency on Name in JD");
  QRI->second.erase(Name);
  if (QRI->second.empty())
    QueryRegistrations.erase(QRI);
}

void AsynchronousSymbolQuery::dropSymbol(const SymbolStringPtr &Name) {
  auto I = ResolvedSymbols.find(Name);
  assert(I != ResolvedSymbols.end() &&
         "Redundant removal of weakly-referenced symbol");
  ResolvedSymbols.erase(I);
  --OutstandingSymbolsCount;
}

void AsynchronousSymbolQuery::detach() {
  ResolvedSymbols.clear();
  OutstandingSymbolsCount = 0;
  for (auto &KV : QueryRegistrations)
    KV.first->detachQueryHelper(*this, KV.second);
  QueryRegistrations.clear();
}

MaterializationResponsibility::~MaterializationResponsibility() {
  assert(SymbolFlags.empty() &&
         "All symbols should have been explicitly materialized or failed");
  JD->unlinkMaterializationResponsibility(*this);
}

SymbolNameSet MaterializationResponsibility::getRequestedSymbols() const {
  return JD->getRequestedSymbols(SymbolFlags);
}

Error MaterializationResponsibility::notifyResolved(const SymbolMap &Symbols) {
  LLVM_DEBUG({
    dbgs() << "In " << JD->getName() << " resolving " << Symbols << "\n";
  });
#ifndef NDEBUG
  for (auto &KV : Symbols) {
    auto WeakFlags = JITSymbolFlags::Weak | JITSymbolFlags::Common;
    auto I = SymbolFlags.find(KV.first);
    assert(I != SymbolFlags.end() &&
           "Resolving symbol outside this responsibility set");
    assert(!I->second.hasMaterializationSideEffectsOnly() &&
           "Can't resolve materialization-side-effects-only symbol");
    assert((KV.second.getFlags() & ~WeakFlags) == (I->second & ~WeakFlags) &&
           "Resolving symbol with incorrect flags");
  }
#endif

  return JD->resolve(*this, Symbols);
}

Error MaterializationResponsibility::notifyEmitted() {

  LLVM_DEBUG({
    dbgs() << "In " << JD->getName() << " emitting " << SymbolFlags << "\n";
  });

  if (auto Err = JD->emit(*this, SymbolFlags))
    return Err;

  SymbolFlags.clear();
  return Error::success();
}

Error MaterializationResponsibility::defineMaterializing(
    SymbolFlagsMap NewSymbolFlags) {

  LLVM_DEBUG({
    dbgs() << "In " << JD->getName() << " defining materializing symbols "
           << NewSymbolFlags << "\n";
  });
  if (auto AcceptedDefs = JD->defineMaterializing(std::move(NewSymbolFlags))) {
    // Add all newly accepted symbols to this responsibility object.
    for (auto &KV : *AcceptedDefs)
      SymbolFlags.insert(KV);
    return Error::success();
  } else
    return AcceptedDefs.takeError();
}

void MaterializationResponsibility::failMaterialization() {

  LLVM_DEBUG({
    dbgs() << "In " << JD->getName() << " failing materialization for "
           << SymbolFlags << "\n";
  });

  JITDylib::FailedSymbolsWorklist Worklist;

  for (auto &KV : SymbolFlags)
    Worklist.push_back(std::make_pair(JD.get(), KV.first));
  SymbolFlags.clear();

  if (Worklist.empty())
    return;

  auto &ES = JD->getExecutionSession();
  JITDylib::AsynchronousSymbolQuerySet FailedQueries;
  std::shared_ptr<SymbolDependenceMap> FailedSymbols;

  ES.runSessionLocked([&]() {
    auto RTI = JD->MRTrackers.find(this);
    assert(RTI != JD->MRTrackers.end() && "No tracker for this");
    if (RTI->second->isDefunct())
      return;

    std::tie(FailedQueries, FailedSymbols) =
        JITDylib::failSymbols(std::move(Worklist));
  });

  for (auto &Q : FailedQueries)
    Q->handleFailed(make_error<FailedToMaterialize>(FailedSymbols));
}

Error MaterializationResponsibility::replace(
    std::unique_ptr<MaterializationUnit> MU) {

  for (auto &KV : MU->getSymbols()) {
    assert(SymbolFlags.count(KV.first) &&
           "Replacing definition outside this responsibility set");
    SymbolFlags.erase(KV.first);
  }

  if (MU->getInitializerSymbol() == InitSymbol)
    InitSymbol = nullptr;

  LLVM_DEBUG(JD->getExecutionSession().runSessionLocked([&]() {
    dbgs() << "In " << JD->getName() << " replacing symbols with " << *MU
           << "\n";
  }););

  return JD->replace(*this, std::move(MU));
}

Expected<std::unique_ptr<MaterializationResponsibility>>
MaterializationResponsibility::delegate(const SymbolNameSet &Symbols) {

  SymbolStringPtr DelegatedInitSymbol;
  SymbolFlagsMap DelegatedFlags;

  for (auto &Name : Symbols) {
    auto I = SymbolFlags.find(Name);
    assert(I != SymbolFlags.end() &&
           "Symbol is not tracked by this MaterializationResponsibility "
           "instance");

    DelegatedFlags[Name] = std::move(I->second);
    if (Name == InitSymbol)
      std::swap(InitSymbol, DelegatedInitSymbol);

    SymbolFlags.erase(I);
  }

  return JD->delegate(*this, std::move(DelegatedFlags),
                      std::move(DelegatedInitSymbol));
}

void MaterializationResponsibility::addDependencies(
    const SymbolStringPtr &Name, const SymbolDependenceMap &Dependencies) {
  LLVM_DEBUG({
    dbgs() << "Adding dependencies for " << Name << ": " << Dependencies
           << "\n";
  });
  assert(SymbolFlags.count(Name) &&
         "Symbol not covered by this MaterializationResponsibility instance");
  JD->addDependencies(Name, Dependencies);
}

void MaterializationResponsibility::addDependenciesForAll(
    const SymbolDependenceMap &Dependencies) {
  LLVM_DEBUG({
    dbgs() << "Adding dependencies for all symbols in " << SymbolFlags << ": "
           << Dependencies << "\n";
  });
  for (auto &KV : SymbolFlags)
    JD->addDependencies(KV.first, Dependencies);
}

AbsoluteSymbolsMaterializationUnit::AbsoluteSymbolsMaterializationUnit(
    SymbolMap Symbols)
    : MaterializationUnit(extractFlags(Symbols), nullptr),
      Symbols(std::move(Symbols)) {}

StringRef AbsoluteSymbolsMaterializationUnit::getName() const {
  return "<Absolute Symbols>";
}

void AbsoluteSymbolsMaterializationUnit::materialize(
    std::unique_ptr<MaterializationResponsibility> R) {
  // No dependencies, so these calls can't fail.
  cantFail(R->notifyResolved(Symbols));
  cantFail(R->notifyEmitted());
}

void AbsoluteSymbolsMaterializationUnit::discard(const JITDylib &JD,
                                                 const SymbolStringPtr &Name) {
  assert(Symbols.count(Name) && "Symbol is not part of this MU");
  Symbols.erase(Name);
}

SymbolFlagsMap
AbsoluteSymbolsMaterializationUnit::extractFlags(const SymbolMap &Symbols) {
  SymbolFlagsMap Flags;
  for (const auto &KV : Symbols)
    Flags[KV.first] = KV.second.getFlags();
  return Flags;
}

ReExportsMaterializationUnit::ReExportsMaterializationUnit(
    JITDylib *SourceJD, JITDylibLookupFlags SourceJDLookupFlags,
    SymbolAliasMap Aliases)
    : MaterializationUnit(extractFlags(Aliases), nullptr), SourceJD(SourceJD),
      SourceJDLookupFlags(SourceJDLookupFlags), Aliases(std::move(Aliases)) {}

StringRef ReExportsMaterializationUnit::getName() const {
  return "<Reexports>";
}

void ReExportsMaterializationUnit::materialize(
    std::unique_ptr<MaterializationResponsibility> R) {

  auto &ES = R->getTargetJITDylib().getExecutionSession();
  JITDylib &TgtJD = R->getTargetJITDylib();
  JITDylib &SrcJD = SourceJD ? *SourceJD : TgtJD;

  // Find the set of requested aliases and aliasees. Return any unrequested
  // aliases back to the JITDylib so as to not prematurely materialize any
  // aliasees.
  auto RequestedSymbols = R->getRequestedSymbols();
  SymbolAliasMap RequestedAliases;

  for (auto &Name : RequestedSymbols) {
    auto I = Aliases.find(Name);
    assert(I != Aliases.end() && "Symbol not found in aliases map?");
    RequestedAliases[Name] = std::move(I->second);
    Aliases.erase(I);
  }

  LLVM_DEBUG({
    ES.runSessionLocked([&]() {
      dbgs() << "materializing reexports: target = " << TgtJD.getName()
             << ", source = " << SrcJD.getName() << " " << RequestedAliases
             << "\n";
    });
  });

  if (!Aliases.empty()) {
    auto Err = SourceJD ? R->replace(reexports(*SourceJD, std::move(Aliases),
                                               SourceJDLookupFlags))
                        : R->replace(symbolAliases(std::move(Aliases)));

    if (Err) {
      // FIXME: Should this be reported / treated as failure to materialize?
      // Or should this be treated as a sanctioned bailing-out?
      ES.reportError(std::move(Err));
      R->failMaterialization();
      return;
    }
  }

  // The OnResolveInfo struct will hold the aliases and responsibilty for each
  // query in the list.
  struct OnResolveInfo {
    OnResolveInfo(std::unique_ptr<MaterializationResponsibility> R,
                  SymbolAliasMap Aliases)
        : R(std::move(R)), Aliases(std::move(Aliases)) {}

    std::unique_ptr<MaterializationResponsibility> R;
    SymbolAliasMap Aliases;
  };

  // Build a list of queries to issue. In each round we build a query for the
  // largest set of aliases that we can resolve without encountering a chain of
  // aliases (e.g. Foo -> Bar, Bar -> Baz). Such a chain would deadlock as the
  // query would be waiting on a symbol that it itself had to resolve. Creating
  // a new query for each link in such a chain eliminates the possibility of
  // deadlock. In practice chains are likely to be rare, and this algorithm will
  // usually result in a single query to issue.

  std::vector<std::pair<SymbolLookupSet, std::shared_ptr<OnResolveInfo>>>
      QueryInfos;
  while (!RequestedAliases.empty()) {
    SymbolNameSet ResponsibilitySymbols;
    SymbolLookupSet QuerySymbols;
    SymbolAliasMap QueryAliases;

    // Collect as many aliases as we can without including a chain.
    for (auto &KV : RequestedAliases) {
      // Chain detected. Skip this symbol for this round.
      if (&SrcJD == &TgtJD && (QueryAliases.count(KV.second.Aliasee) ||
                               RequestedAliases.count(KV.second.Aliasee)))
        continue;

      ResponsibilitySymbols.insert(KV.first);
      QuerySymbols.add(KV.second.Aliasee,
                       KV.second.AliasFlags.hasMaterializationSideEffectsOnly()
                           ? SymbolLookupFlags::WeaklyReferencedSymbol
                           : SymbolLookupFlags::RequiredSymbol);
      QueryAliases[KV.first] = std::move(KV.second);
    }

    // Remove the aliases collected this round from the RequestedAliases map.
    for (auto &KV : QueryAliases)
      RequestedAliases.erase(KV.first);

    assert(!QuerySymbols.empty() && "Alias cycle detected!");

    auto NewR = R->delegate(ResponsibilitySymbols);
    if (!NewR) {
      ES.reportError(NewR.takeError());
      R->failMaterialization();
      return;
    }

    auto QueryInfo = std::make_shared<OnResolveInfo>(std::move(*NewR),
                                                     std::move(QueryAliases));
    QueryInfos.push_back(
        make_pair(std::move(QuerySymbols), std::move(QueryInfo)));
  }

  // Issue the queries.
  while (!QueryInfos.empty()) {
    auto QuerySymbols = std::move(QueryInfos.back().first);
    auto QueryInfo = std::move(QueryInfos.back().second);

    QueryInfos.pop_back();

    auto RegisterDependencies = [QueryInfo,
                                 &SrcJD](const SymbolDependenceMap &Deps) {
      // If there were no materializing symbols, just bail out.
      if (Deps.empty())
        return;

      // Otherwise the only deps should be on SrcJD.
      assert(Deps.size() == 1 && Deps.count(&SrcJD) &&
             "Unexpected dependencies for reexports");

      auto &SrcJDDeps = Deps.find(&SrcJD)->second;
      SymbolDependenceMap PerAliasDepsMap;
      auto &PerAliasDeps = PerAliasDepsMap[&SrcJD];

      for (auto &KV : QueryInfo->Aliases)
        if (SrcJDDeps.count(KV.second.Aliasee)) {
          PerAliasDeps = {KV.second.Aliasee};
          QueryInfo->R->addDependencies(KV.first, PerAliasDepsMap);
        }
    };

    auto OnComplete = [QueryInfo](Expected<SymbolMap> Result) {
      auto &ES = QueryInfo->R->getTargetJITDylib().getExecutionSession();
      if (Result) {
        SymbolMap ResolutionMap;
        for (auto &KV : QueryInfo->Aliases) {
          assert((KV.second.AliasFlags.hasMaterializationSideEffectsOnly() ||
                  Result->count(KV.second.Aliasee)) &&
                 "Result map missing entry?");
          // Don't try to resolve materialization-side-effects-only symbols.
          if (KV.second.AliasFlags.hasMaterializationSideEffectsOnly())
            continue;

          ResolutionMap[KV.first] = JITEvaluatedSymbol(
              (*Result)[KV.second.Aliasee].getAddress(), KV.second.AliasFlags);
        }
        if (auto Err = QueryInfo->R->notifyResolved(ResolutionMap)) {
          ES.reportError(std::move(Err));
          QueryInfo->R->failMaterialization();
          return;
        }
        if (auto Err = QueryInfo->R->notifyEmitted()) {
          ES.reportError(std::move(Err));
          QueryInfo->R->failMaterialization();
          return;
        }
      } else {
        ES.reportError(Result.takeError());
        QueryInfo->R->failMaterialization();
      }
    };

    ES.lookup(LookupKind::Static,
              JITDylibSearchOrder({{&SrcJD, SourceJDLookupFlags}}),
              QuerySymbols, SymbolState::Resolved, std::move(OnComplete),
              std::move(RegisterDependencies));
  }
}

void ReExportsMaterializationUnit::discard(const JITDylib &JD,
                                           const SymbolStringPtr &Name) {
  assert(Aliases.count(Name) &&
         "Symbol not covered by this MaterializationUnit");
  Aliases.erase(Name);
}

SymbolFlagsMap
ReExportsMaterializationUnit::extractFlags(const SymbolAliasMap &Aliases) {
  SymbolFlagsMap SymbolFlags;
  for (auto &KV : Aliases)
    SymbolFlags[KV.first] = KV.second.AliasFlags;

  return SymbolFlags;
}

Expected<SymbolAliasMap>
buildSimpleReexportsAliasMap(JITDylib &SourceJD, const SymbolNameSet &Symbols) {
  SymbolLookupSet LookupSet(Symbols);
  auto Flags = SourceJD.lookupFlags(
      LookupKind::Static, JITDylibLookupFlags::MatchAllSymbols, LookupSet);

  if (!Flags)
    return Flags.takeError();

  if (!LookupSet.empty()) {
    LookupSet.sortByName();
    return make_error<SymbolsNotFound>(LookupSet.getSymbolNames());
  }

  SymbolAliasMap Result;
  for (auto &Name : Symbols) {
    assert(Flags->count(Name) && "Missing entry in flags map");
    Result[Name] = SymbolAliasMapEntry(Name, (*Flags)[Name]);
  }

  return Result;
}

ReexportsGenerator::ReexportsGenerator(JITDylib &SourceJD,
                                       JITDylibLookupFlags SourceJDLookupFlags,
                                       SymbolPredicate Allow)
    : SourceJD(SourceJD), SourceJDLookupFlags(SourceJDLookupFlags),
      Allow(std::move(Allow)) {}

Error ReexportsGenerator::tryToGenerate(LookupKind K, JITDylib &JD,
                                        JITDylibLookupFlags JDLookupFlags,
                                        const SymbolLookupSet &LookupSet) {
  assert(&JD != &SourceJD && "Cannot re-export from the same dylib");

  // Use lookupFlags to find the subset of symbols that match our lookup.
  auto Flags = SourceJD.lookupFlags(K, JDLookupFlags, LookupSet);
  if (!Flags)
    return Flags.takeError();

  // Create an alias map.
  orc::SymbolAliasMap AliasMap;
  for (auto &KV : *Flags)
    if (!Allow || Allow(KV.first))
      AliasMap[KV.first] = SymbolAliasMapEntry(KV.first, KV.second);

  if (AliasMap.empty())
    return Error::success();

  // Define the re-exports.
  return JD.define(reexports(SourceJD, AliasMap, SourceJDLookupFlags));
}

JITDylib::DefinitionGenerator::~DefinitionGenerator() {}

Error JITDylib::clear() {
  std::vector<ResourceTrackerSP> TrackersToRemove;
  ES.runSessionLocked([&]() {
    for (auto &KV : TrackerSymbols)
      TrackersToRemove.push_back(KV.first);
    TrackersToRemove.push_back(getDefaultResourceTracker());
  });

  Error Err = Error::success();
  for (auto &RT : TrackersToRemove)
    Err = joinErrors(std::move(Err), RT->remove());
  return Err;
}

ResourceTrackerSP JITDylib::getDefaultResourceTracker() {
  return ES.runSessionLocked([this] {
    if (!DefaultTracker)
      DefaultTracker = new ResourceTracker(this);
    return DefaultTracker;
  });
}

ResourceTrackerSP JITDylib::createResourceTracker() {
  return ES.runSessionLocked([this] {
    ResourceTrackerSP RT = new ResourceTracker(this);
    return RT;
  });
}

void JITDylib::removeGenerator(DefinitionGenerator &G) {
  ES.runSessionLocked([&]() {
    auto I = std::find_if(DefGenerators.begin(), DefGenerators.end(),
                          [&](const std::unique_ptr<DefinitionGenerator> &H) {
                            return H.get() == &G;
                          });
    assert(I != DefGenerators.end() && "Generator not found");
    DefGenerators.erase(I);
  });
}

Expected<SymbolFlagsMap>
JITDylib::defineMaterializing(SymbolFlagsMap SymbolFlags) {

  return ES.runSessionLocked([&]() -> Expected<SymbolFlagsMap> {
    std::vector<SymbolTable::iterator> AddedSyms;
    std::vector<SymbolFlagsMap::iterator> RejectedWeakDefs;

    for (auto SFItr = SymbolFlags.begin(), SFEnd = SymbolFlags.end();
         SFItr != SFEnd; ++SFItr) {

      auto &Name = SFItr->first;
      auto &Flags = SFItr->second;

      auto EntryItr = Symbols.find(Name);

      // If the entry already exists...
      if (EntryItr != Symbols.end()) {

        // If this is a strong definition then error out.
        if (!Flags.isWeak()) {
          // Remove any symbols already added.
          for (auto &SI : AddedSyms)
            Symbols.erase(SI);

          // FIXME: Return all duplicates.
          return make_error<DuplicateDefinition>(std::string(*Name));
        }

        // Otherwise just make a note to discard this symbol after the loop.
        RejectedWeakDefs.push_back(SFItr);
        continue;
      } else
        EntryItr =
          Symbols.insert(std::make_pair(Name, SymbolTableEntry(Flags))).first;

      AddedSyms.push_back(EntryItr);
      EntryItr->second.setState(SymbolState::Materializing);
    }

    // Remove any rejected weak definitions from the SymbolFlags map.
    while (!RejectedWeakDefs.empty()) {
      SymbolFlags.erase(RejectedWeakDefs.back());
      RejectedWeakDefs.pop_back();
    }

    return SymbolFlags;
  });
}

Error JITDylib::replace(MaterializationResponsibility &FromMR,
                        std::unique_ptr<MaterializationUnit> MU) {
  assert(MU != nullptr && "Can not replace with a null MaterializationUnit");
  std::unique_ptr<MaterializationUnit> MustRunMU;
  std::unique_ptr<MaterializationResponsibility> MustRunMR;

  auto Err =
      ES.runSessionLocked([&, this]() -> Error {
        auto RT = getTracker(FromMR);

        if (RT->isDefunct())
          return make_error<ResourceTrackerDefunct>(std::move(RT));

#ifndef NDEBUG
        for (auto &KV : MU->getSymbols()) {
          auto SymI = Symbols.find(KV.first);
          assert(SymI != Symbols.end() && "Replacing unknown symbol");
          assert(SymI->second.getState() == SymbolState::Materializing &&
                 "Can not replace a symbol that ha is not materializing");
          assert(!SymI->second.hasMaterializerAttached() &&
                 "Symbol should not have materializer attached already");
          assert(UnmaterializedInfos.count(KV.first) == 0 &&
                 "Symbol being replaced should have no UnmaterializedInfo");
        }
#endif // NDEBUG

        // If the tracker is defunct we need to bail out immediately.

        // If any symbol has pending queries against it then we need to
        // materialize MU immediately.
        for (auto &KV : MU->getSymbols()) {
          auto MII = MaterializingInfos.find(KV.first);
          if (MII != MaterializingInfos.end()) {
            if (MII->second.hasQueriesPending()) {
              MustRunMR = ES.createMaterializationResponsibility(
                  *RT, std::move(MU->SymbolFlags), std::move(MU->InitSymbol));
              MustRunMU = std::move(MU);
              return Error::success();
            }
          }
        }

        // Otherwise, make MU responsible for all the symbols.
        auto RTI = MRTrackers.find(&FromMR);
        assert(RTI != MRTrackers.end() && "No tracker for FromMR");
        auto UMI =
            std::make_shared<UnmaterializedInfo>(std::move(MU), RTI->second);
        for (auto &KV : UMI->MU->getSymbols()) {
          auto SymI = Symbols.find(KV.first);
          assert(SymI->second.getState() == SymbolState::Materializing &&
                 "Can not replace a symbol that is not materializing");
          assert(!SymI->second.hasMaterializerAttached() &&
                 "Can not replace a symbol that has a materializer attached");
          assert(UnmaterializedInfos.count(KV.first) == 0 &&
                 "Unexpected materializer entry in map");
          SymI->second.setAddress(SymI->second.getAddress());
          SymI->second.setMaterializerAttached(true);

          auto &UMIEntry = UnmaterializedInfos[KV.first];
          assert((!UMIEntry || !UMIEntry->MU) &&
                 "Replacing symbol with materializer still attached");
          UMIEntry = UMI;
        }

        return Error::success();
      });

  if (Err)
    return Err;

  if (MustRunMU) {
    assert(MustRunMR && "MustRunMU set implies MustRunMR set");
    ES.dispatchMaterialization(std::move(MustRunMU), std::move(MustRunMR));
  } else {
    assert(!MustRunMR && "MustRunMU unset implies MustRunMR unset");
  }

  return Error::success();
}

Expected<std::unique_ptr<MaterializationResponsibility>>
JITDylib::delegate(MaterializationResponsibility &FromMR,
                   SymbolFlagsMap SymbolFlags, SymbolStringPtr InitSymbol) {

  return ES.runSessionLocked(
      [&]() -> Expected<std::unique_ptr<MaterializationResponsibility>> {
        auto RT = getTracker(FromMR);

        if (RT->isDefunct())
          return make_error<ResourceTrackerDefunct>(std::move(RT));

        return ES.createMaterializationResponsibility(
            *RT, std::move(SymbolFlags), std::move(InitSymbol));
      });
}

SymbolNameSet
JITDylib::getRequestedSymbols(const SymbolFlagsMap &SymbolFlags) const {
  return ES.runSessionLocked([&]() {
    SymbolNameSet RequestedSymbols;

    for (auto &KV : SymbolFlags) {
      assert(Symbols.count(KV.first) && "JITDylib does not cover this symbol?");
      assert(Symbols.find(KV.first)->second.getState() !=
                 SymbolState::NeverSearched &&
             Symbols.find(KV.first)->second.getState() != SymbolState::Ready &&
             "getRequestedSymbols can only be called for symbols that have "
             "started materializing");
      auto I = MaterializingInfos.find(KV.first);
      if (I == MaterializingInfos.end())
        continue;

      if (I->second.hasQueriesPending())
        RequestedSymbols.insert(KV.first);
    }

    return RequestedSymbols;
  });
}

void JITDylib::addDependencies(const SymbolStringPtr &Name,
                               const SymbolDependenceMap &Dependencies) {
  assert(Symbols.count(Name) && "Name not in symbol table");
  assert(Symbols[Name].getState() < SymbolState::Emitted &&
         "Can not add dependencies for a symbol that is not materializing");

  LLVM_DEBUG({
      dbgs() << "In " << getName() << " adding dependencies for "
             << *Name << ": " << Dependencies << "\n";
    });

  // If Name is already in an error state then just bail out.
  if (Symbols[Name].getFlags().hasError())
    return;

  auto &MI = MaterializingInfos[Name];
  assert(Symbols[Name].getState() != SymbolState::Emitted &&
         "Can not add dependencies to an emitted symbol");

  bool DependsOnSymbolInErrorState = false;

  // Register dependencies, record whether any depenendency is in the error
  // state.
  for (auto &KV : Dependencies) {
    assert(KV.first && "Null JITDylib in dependency?");
    auto &OtherJITDylib = *KV.first;
    auto &DepsOnOtherJITDylib = MI.UnemittedDependencies[&OtherJITDylib];

    for (auto &OtherSymbol : KV.second) {

      // Check the sym entry for the dependency.
      auto OtherSymI = OtherJITDylib.Symbols.find(OtherSymbol);

      // Assert that this symbol exists and has not reached the ready state
      // already.
      assert(OtherSymI != OtherJITDylib.Symbols.end() &&
             "Dependency on unknown symbol");

      auto &OtherSymEntry = OtherSymI->second;

      // If the other symbol is already in the Ready state then there's no
      // dependency to add.
      if (OtherSymEntry.getState() == SymbolState::Ready)
        continue;

      // If the dependency is in an error state then note this and continue,
      // we will move this symbol to the error state below.
      if (OtherSymEntry.getFlags().hasError()) {
        DependsOnSymbolInErrorState = true;
        continue;
      }

      // If the dependency was not in the error state then add it to
      // our list of dependencies.
      auto &OtherMI = OtherJITDylib.MaterializingInfos[OtherSymbol];

      if (OtherSymEntry.getState() == SymbolState::Emitted)
        transferEmittedNodeDependencies(MI, Name, OtherMI);
      else if (&OtherJITDylib != this || OtherSymbol != Name) {
        OtherMI.Dependants[this].insert(Name);
        DepsOnOtherJITDylib.insert(OtherSymbol);
      }
    }

    if (DepsOnOtherJITDylib.empty())
      MI.UnemittedDependencies.erase(&OtherJITDylib);
  }

  // If this symbol dependended on any symbols in the error state then move
  // this symbol to the error state too.
  if (DependsOnSymbolInErrorState)
    Symbols[Name].setFlags(Symbols[Name].getFlags() | JITSymbolFlags::HasError);
}

Error JITDylib::resolve(MaterializationResponsibility &MR,
                        const SymbolMap &Resolved) {
  AsynchronousSymbolQuerySet CompletedQueries;

  if (auto Err = ES.runSessionLocked([&, this]() -> Error {
        auto RTI = MRTrackers.find(&MR);
        assert(RTI != MRTrackers.end() && "No resource tracker for MR?");
        if (RTI->second->isDefunct())
          return make_error<ResourceTrackerDefunct>(RTI->second);

        struct WorklistEntry {
          SymbolTable::iterator SymI;
          JITEvaluatedSymbol ResolvedSym;
        };

        SymbolNameSet SymbolsInErrorState;
        std::vector<WorklistEntry> Worklist;
        Worklist.reserve(Resolved.size());

        // Build worklist and check for any symbols in the error state.
        for (const auto &KV : Resolved) {

          assert(!KV.second.getFlags().hasError() &&
                 "Resolution result can not have error flag set");

          auto SymI = Symbols.find(KV.first);

          assert(SymI != Symbols.end() && "Symbol not found");
          assert(!SymI->second.hasMaterializerAttached() &&
                 "Resolving symbol with materializer attached?");
          assert(SymI->second.getState() == SymbolState::Materializing &&
                 "Symbol should be materializing");
          assert(SymI->second.getAddress() == 0 &&
                 "Symbol has already been resolved");

          if (SymI->second.getFlags().hasError())
            SymbolsInErrorState.insert(KV.first);
          else {
            auto Flags = KV.second.getFlags();
            Flags &= ~(JITSymbolFlags::Weak | JITSymbolFlags::Common);
            assert(Flags ==
                       (SymI->second.getFlags() &
                        ~(JITSymbolFlags::Weak | JITSymbolFlags::Common)) &&
                   "Resolved flags should match the declared flags");

            Worklist.push_back(
                {SymI, JITEvaluatedSymbol(KV.second.getAddress(), Flags)});
          }
        }

        // If any symbols were in the error state then bail out.
        if (!SymbolsInErrorState.empty()) {
          auto FailedSymbolsDepMap = std::make_shared<SymbolDependenceMap>();
          (*FailedSymbolsDepMap)[this] = std::move(SymbolsInErrorState);
          return make_error<FailedToMaterialize>(
              std::move(FailedSymbolsDepMap));
        }

        while (!Worklist.empty()) {
          auto SymI = Worklist.back().SymI;
          auto ResolvedSym = Worklist.back().ResolvedSym;
          Worklist.pop_back();

          auto &Name = SymI->first;

          // Resolved symbols can not be weak: discard the weak flag.
          JITSymbolFlags ResolvedFlags = ResolvedSym.getFlags();
          SymI->second.setAddress(ResolvedSym.getAddress());
          SymI->second.setFlags(ResolvedFlags);
          SymI->second.setState(SymbolState::Resolved);

          auto MII = MaterializingInfos.find(Name);
          if (MII == MaterializingInfos.end())
            continue;

          auto &MI = MII->second;
          for (auto &Q : MI.takeQueriesMeeting(SymbolState::Resolved)) {
            Q->notifySymbolMetRequiredState(Name, ResolvedSym);
            Q->removeQueryDependence(*this, Name);
            if (Q->isComplete())
              CompletedQueries.insert(std::move(Q));
          }
        }

        return Error::success();
      }))
    return Err;

  // Otherwise notify all the completed queries.
  for (auto &Q : CompletedQueries) {
    assert(Q->isComplete() && "Q not completed");
    Q->handleComplete();
  }

  return Error::success();
}

Error JITDylib::emit(MaterializationResponsibility &MR,
                     const SymbolFlagsMap &Emitted) {
  AsynchronousSymbolQuerySet CompletedQueries;
  DenseMap<JITDylib *, SymbolNameVector> ReadySymbols;

  if (auto Err = ES.runSessionLocked([&, this]() -> Error {
        auto RTI = MRTrackers.find(&MR);
        assert(RTI != MRTrackers.end() && "No resource tracker for MR?");
        if (RTI->second->isDefunct())
          return make_error<ResourceTrackerDefunct>(RTI->second);

        SymbolNameSet SymbolsInErrorState;
        std::vector<SymbolTable::iterator> Worklist;

        // Scan to build worklist, record any symbols in the erorr state.
        for (const auto &KV : Emitted) {
          auto &Name = KV.first;

          auto SymI = Symbols.find(Name);
          assert(SymI != Symbols.end() && "No symbol table entry for Name");

          if (SymI->second.getFlags().hasError())
            SymbolsInErrorState.insert(Name);
          else
            Worklist.push_back(SymI);
        }

        // If any symbols were in the error state then bail out.
        if (!SymbolsInErrorState.empty()) {
          auto FailedSymbolsDepMap = std::make_shared<SymbolDependenceMap>();
          (*FailedSymbolsDepMap)[this] = std::move(SymbolsInErrorState);
          return make_error<FailedToMaterialize>(
              std::move(FailedSymbolsDepMap));
        }

        // Otherwise update dependencies and move to the emitted state.
        while (!Worklist.empty()) {
          auto SymI = Worklist.back();
          Worklist.pop_back();

          auto &Name = SymI->first;
          auto &SymEntry = SymI->second;

          // Move symbol to the emitted state.
          assert(((SymEntry.getFlags().hasMaterializationSideEffectsOnly() &&
                   SymEntry.getState() == SymbolState::Materializing) ||
                  SymEntry.getState() == SymbolState::Resolved) &&
                 "Emitting from state other than Resolved");
          SymEntry.setState(SymbolState::Emitted);

          auto MII = MaterializingInfos.find(Name);

          // If this symbol has no MaterializingInfo then it's trivially ready.
          // Update its state and continue.
          if (MII == MaterializingInfos.end()) {
            SymEntry.setState(SymbolState::Ready);
            continue;
          }

          auto &MI = MII->second;

          // For each dependant, transfer this node's emitted dependencies to
          // it. If the dependant node is ready (i.e. has no unemitted
          // dependencies) then notify any pending queries.
          for (auto &KV : MI.Dependants) {
            auto &DependantJD = *KV.first;
            auto &DependantJDReadySymbols = ReadySymbols[&DependantJD];
            for (auto &DependantName : KV.second) {
              auto DependantMII =
                  DependantJD.MaterializingInfos.find(DependantName);
              assert(DependantMII != DependantJD.MaterializingInfos.end() &&
                     "Dependant should have MaterializingInfo");

              auto &DependantMI = DependantMII->second;

              // Remove the dependant's dependency on this node.
              assert(DependantMI.UnemittedDependencies.count(this) &&
                     "Dependant does not have an unemitted dependencies record "
                     "for "
                     "this JITDylib");
              assert(DependantMI.UnemittedDependencies[this].count(Name) &&
                     "Dependant does not count this symbol as a dependency?");

              DependantMI.UnemittedDependencies[this].erase(Name);
              if (DependantMI.UnemittedDependencies[this].empty())
                DependantMI.UnemittedDependencies.erase(this);

              // Transfer unemitted dependencies from this node to the
              // dependant.
              DependantJD.transferEmittedNodeDependencies(DependantMI,
                                                          DependantName, MI);

              auto DependantSymI = DependantJD.Symbols.find(DependantName);
              assert(DependantSymI != DependantJD.Symbols.end() &&
                     "Dependant has no entry in the Symbols table");
              auto &DependantSymEntry = DependantSymI->second;

              // If the dependant is emitted and this node was the last of its
              // unemitted dependencies then the dependant node is now ready, so
              // notify any pending queries on the dependant node.
              if (DependantSymEntry.getState() == SymbolState::Emitted &&
                  DependantMI.UnemittedDependencies.empty()) {
                assert(DependantMI.Dependants.empty() &&
                       "Dependants should be empty by now");

                // Since this dependant is now ready, we erase its
                // MaterializingInfo and update its materializing state.
                DependantSymEntry.setState(SymbolState::Ready);
                DependantJDReadySymbols.push_back(DependantName);

                for (auto &Q :
                     DependantMI.takeQueriesMeeting(SymbolState::Ready)) {
                  Q->notifySymbolMetRequiredState(
                      DependantName, DependantSymI->second.getSymbol());
                  if (Q->isComplete())
                    CompletedQueries.insert(Q);
                  Q->removeQueryDependence(DependantJD, DependantName);
                }
              }
            }
          }

          auto &ThisJDReadySymbols = ReadySymbols[this];
          MI.Dependants.clear();
          if (MI.UnemittedDependencies.empty()) {
            SymI->second.setState(SymbolState::Ready);
            ThisJDReadySymbols.push_back(Name);
            for (auto &Q : MI.takeQueriesMeeting(SymbolState::Ready)) {
              Q->notifySymbolMetRequiredState(Name, SymI->second.getSymbol());
              if (Q->isComplete())
                CompletedQueries.insert(Q);
              Q->removeQueryDependence(*this, Name);
            }
          }
        }

        return Error::success();
      }))
    return Err;

  // Otherwise notify all the completed queries.
  for (auto &Q : CompletedQueries) {
    assert(Q->isComplete() && "Q is not complete");
    Q->handleComplete();
  }

  return Error::success();
}

void JITDylib::unlinkMaterializationResponsibility(
    MaterializationResponsibility &MR) {
  ES.runSessionLocked([&]() {
    auto I = MRTrackers.find(&MR);
    assert(I != MRTrackers.end() && "MaterializationResponsibility not linked");
    MRTrackers.erase(I);
  });
}

std::pair<JITDylib::AsynchronousSymbolQuerySet,
          std::shared_ptr<SymbolDependenceMap>>
JITDylib::failSymbols(FailedSymbolsWorklist Worklist) {
  AsynchronousSymbolQuerySet FailedQueries;
  auto FailedSymbolsMap = std::make_shared<SymbolDependenceMap>();

  while (!Worklist.empty()) {
    assert(Worklist.back().first && "Failed JITDylib can not be null");
    auto &JD = *Worklist.back().first;
    auto Name = std::move(Worklist.back().second);
    Worklist.pop_back();

    (*FailedSymbolsMap)[&JD].insert(Name);

    assert(JD.Symbols.count(Name) && "No symbol table entry for Name");
    auto &Sym = JD.Symbols[Name];

    // Move the symbol into the error state.
    // Note that this may be redundant: The symbol might already have been
    // moved to this state in response to the failure of a dependence.
    Sym.setFlags(Sym.getFlags() | JITSymbolFlags::HasError);

    // FIXME: Come up with a sane mapping of state to
    // presence-of-MaterializingInfo so that we can assert presence / absence
    // here, rather than testing it.
    auto MII = JD.MaterializingInfos.find(Name);

    if (MII == JD.MaterializingInfos.end())
      continue;

    auto &MI = MII->second;

    // Move all dependants to the error state and disconnect from them.
    for (auto &KV : MI.Dependants) {
      auto &DependantJD = *KV.first;
      for (auto &DependantName : KV.second) {
        assert(DependantJD.Symbols.count(DependantName) &&
               "No symbol table entry for DependantName");
        auto &DependantSym = DependantJD.Symbols[DependantName];
        DependantSym.setFlags(DependantSym.getFlags() |
                              JITSymbolFlags::HasError);

        assert(DependantJD.MaterializingInfos.count(DependantName) &&
               "No MaterializingInfo for dependant");
        auto &DependantMI = DependantJD.MaterializingInfos[DependantName];

        auto UnemittedDepI = DependantMI.UnemittedDependencies.find(&JD);
        assert(UnemittedDepI != DependantMI.UnemittedDependencies.end() &&
               "No UnemittedDependencies entry for this JITDylib");
        assert(UnemittedDepI->second.count(Name) &&
               "No UnemittedDependencies entry for this symbol");
        UnemittedDepI->second.erase(Name);
        if (UnemittedDepI->second.empty())
          DependantMI.UnemittedDependencies.erase(UnemittedDepI);

        // If this symbol is already in the emitted state then we need to
        // take responsibility for failing its queries, so add it to the
        // worklist.
        if (DependantSym.getState() == SymbolState::Emitted) {
          assert(DependantMI.Dependants.empty() &&
                 "Emitted symbol should not have dependants");
          Worklist.push_back(std::make_pair(&DependantJD, DependantName));
        }
      }
    }
    MI.Dependants.clear();

    // Disconnect from all unemitted depenencies.
    for (auto &KV : MI.UnemittedDependencies) {
      auto &UnemittedDepJD = *KV.first;
      for (auto &UnemittedDepName : KV.second) {
        auto UnemittedDepMII =
            UnemittedDepJD.MaterializingInfos.find(UnemittedDepName);
        assert(UnemittedDepMII != UnemittedDepJD.MaterializingInfos.end() &&
               "Missing MII for unemitted dependency");
        assert(UnemittedDepMII->second.Dependants.count(&JD) &&
               "JD not listed as a dependant of unemitted dependency");
        assert(UnemittedDepMII->second.Dependants[&JD].count(Name) &&
               "Name is not listed as a dependant of unemitted dependency");
        UnemittedDepMII->second.Dependants[&JD].erase(Name);
        if (UnemittedDepMII->second.Dependants[&JD].empty())
          UnemittedDepMII->second.Dependants.erase(&JD);
      }
    }
    MI.UnemittedDependencies.clear();

    // Collect queries to be failed for this MII.
    AsynchronousSymbolQueryList ToDetach;
    for (auto &Q : MII->second.pendingQueries()) {
      // Add the query to the list to be failed and detach it.
      FailedQueries.insert(Q);
      ToDetach.push_back(Q);
    }
    for (auto &Q : ToDetach)
      Q->detach();

    assert(MI.Dependants.empty() &&
           "Can not delete MaterializingInfo with dependants still attached");
    assert(MI.UnemittedDependencies.empty() &&
           "Can not delete MaterializingInfo with unemitted dependencies "
           "still attached");
    assert(!MI.hasQueriesPending() &&
           "Can not delete MaterializingInfo with queries pending");
    JD.MaterializingInfos.erase(MII);
  }

  return std::make_pair(std::move(FailedQueries), std::move(FailedSymbolsMap));
}

void JITDylib::setLinkOrder(JITDylibSearchOrder NewLinkOrder,
                            bool LinkAgainstThisJITDylibFirst) {
  ES.runSessionLocked([&]() {
    if (LinkAgainstThisJITDylibFirst) {
      LinkOrder.clear();
      if (NewLinkOrder.empty() || NewLinkOrder.front().first != this)
        LinkOrder.push_back(
            std::make_pair(this, JITDylibLookupFlags::MatchAllSymbols));
      LinkOrder.insert(LinkOrder.end(), NewLinkOrder.begin(),
                       NewLinkOrder.end());
    } else
      LinkOrder = std::move(NewLinkOrder);
  });
}

void JITDylib::addToLinkOrder(JITDylib &JD, JITDylibLookupFlags JDLookupFlags) {
  ES.runSessionLocked([&]() { LinkOrder.push_back({&JD, JDLookupFlags}); });
}

void JITDylib::replaceInLinkOrder(JITDylib &OldJD, JITDylib &NewJD,
                                  JITDylibLookupFlags JDLookupFlags) {
  ES.runSessionLocked([&]() {
    for (auto &KV : LinkOrder)
      if (KV.first == &OldJD) {
        KV = {&NewJD, JDLookupFlags};
        break;
      }
  });
}

void JITDylib::removeFromLinkOrder(JITDylib &JD) {
  ES.runSessionLocked([&]() {
    auto I = std::find_if(LinkOrder.begin(), LinkOrder.end(),
                          [&](const JITDylibSearchOrder::value_type &KV) {
                            return KV.first == &JD;
                          });
    if (I != LinkOrder.end())
      LinkOrder.erase(I);
  });
}

Error JITDylib::remove(const SymbolNameSet &Names) {
  return ES.runSessionLocked([&]() -> Error {
    using SymbolMaterializerItrPair =
        std::pair<SymbolTable::iterator, UnmaterializedInfosMap::iterator>;
    std::vector<SymbolMaterializerItrPair> SymbolsToRemove;
    SymbolNameSet Missing;
    SymbolNameSet Materializing;

    for (auto &Name : Names) {
      auto I = Symbols.find(Name);

      // Note symbol missing.
      if (I == Symbols.end()) {
        Missing.insert(Name);
        continue;
      }

      // Note symbol materializing.
      if (I->second.getState() != SymbolState::NeverSearched &&
          I->second.getState() != SymbolState::Ready) {
        Materializing.insert(Name);
        continue;
      }

      auto UMII = I->second.hasMaterializerAttached()
                      ? UnmaterializedInfos.find(Name)
                      : UnmaterializedInfos.end();
      SymbolsToRemove.push_back(std::make_pair(I, UMII));
    }

    // If any of the symbols are not defined, return an error.
    if (!Missing.empty())
      return make_error<SymbolsNotFound>(std::move(Missing));

    // If any of the symbols are currently materializing, return an error.
    if (!Materializing.empty())
      return make_error<SymbolsCouldNotBeRemoved>(std::move(Materializing));

    // Remove the symbols.
    for (auto &SymbolMaterializerItrPair : SymbolsToRemove) {
      auto UMII = SymbolMaterializerItrPair.second;

      // If there is a materializer attached, call discard.
      if (UMII != UnmaterializedInfos.end()) {
        UMII->second->MU->doDiscard(*this, UMII->first);
        UnmaterializedInfos.erase(UMII);
      }

      auto SymI = SymbolMaterializerItrPair.first;
      Symbols.erase(SymI);
    }

    return Error::success();
  });
}

Expected<SymbolFlagsMap>
JITDylib::lookupFlags(LookupKind K, JITDylibLookupFlags JDLookupFlags,
                      SymbolLookupSet LookupSet) {
  return ES.runSessionLocked([&, this]() -> Expected<SymbolFlagsMap> {
    SymbolFlagsMap Result;
    lookupFlagsImpl(Result, K, JDLookupFlags, LookupSet);

    // Run any definition generators.
    for (auto &DG : DefGenerators) {

      // Bail out early if we found everything.
      if (LookupSet.empty())
        break;

      // Run this generator.
      if (auto Err = DG->tryToGenerate(K, *this, JDLookupFlags, LookupSet))
        return std::move(Err);

      // Re-try the search.
      lookupFlagsImpl(Result, K, JDLookupFlags, LookupSet);
    }

    return Result;
  });
}

void JITDylib::lookupFlagsImpl(SymbolFlagsMap &Result, LookupKind K,
                               JITDylibLookupFlags JDLookupFlags,
                               SymbolLookupSet &LookupSet) {

  LookupSet.forEachWithRemoval(
      [&](const SymbolStringPtr &Name, SymbolLookupFlags Flags) -> bool {
        auto I = Symbols.find(Name);
        if (I == Symbols.end())
          return false;
        assert(!Result.count(Name) && "Symbol already present in Flags map");
        Result[Name] = I->second.getFlags();
        return true;
      });
}

Error JITDylib::lodgeQuery(UnmaterializedInfosList &UMIs,
                           std::shared_ptr<AsynchronousSymbolQuery> &Q,
                           LookupKind K, JITDylibLookupFlags JDLookupFlags,
                           SymbolLookupSet &Unresolved) {
  assert(Q && "Query can not be null");

  if (auto Err = lodgeQueryImpl(UMIs, Q, K, JDLookupFlags, Unresolved))
    return Err;

  // Run any definition generators.
  for (auto &DG : DefGenerators) {

    // Bail out early if we have resolved everything.
    if (Unresolved.empty())
      break;

    // Run the generator.
    if (auto Err = DG->tryToGenerate(K, *this, JDLookupFlags, Unresolved))
      return Err;

    // Lodge query. This can not fail as any new definitions were added
    // by the generator under the session locked. Since they can't have
    // started materializing yet they can not have failed.
    cantFail(lodgeQueryImpl(UMIs, Q, K, JDLookupFlags, Unresolved));
  }

  return Error::success();
}

Error JITDylib::lodgeQueryImpl(UnmaterializedInfosList &UMIs,
                               std::shared_ptr<AsynchronousSymbolQuery> &Q,
                               LookupKind K, JITDylibLookupFlags JDLookupFlags,
                               SymbolLookupSet &Unresolved) {

  return Unresolved.forEachWithRemoval(
      [&](const SymbolStringPtr &Name,
          SymbolLookupFlags SymLookupFlags) -> Expected<bool> {
        // Search for name in symbols. If not found then continue without
        // removal.
        auto SymI = Symbols.find(Name);
        if (SymI == Symbols.end())
          return false;

        // If we match against a materialization-side-effects only symbol then
        // make sure it is weakly-referenced. Otherwise bail out with an error.
        // FIXME: Use a "materialization-side-effecs-only symbols must be weakly
        // referenced" specific error here to reduce confusion.
        if (SymI->second.getFlags().hasMaterializationSideEffectsOnly() &&
            SymLookupFlags != SymbolLookupFlags::WeaklyReferencedSymbol)
          return make_error<SymbolsNotFound>(SymbolNameVector({Name}));

        // If this is a non exported symbol and we're matching exported symbols
        // only then skip this symbol without removal.
        if (!SymI->second.getFlags().isExported() &&
            JDLookupFlags == JITDylibLookupFlags::MatchExportedSymbolsOnly)
          return false;

        // If we matched against this symbol but it is in the error state then
        // bail out and treat it as a failure to materialize.
        if (SymI->second.getFlags().hasError()) {
          auto FailedSymbolsMap = std::make_shared<SymbolDependenceMap>();
          (*FailedSymbolsMap)[this] = {Name};
          return make_error<FailedToMaterialize>(std::move(FailedSymbolsMap));
        }

        // If this symbol already meets the required state for then notify the
        // query, then remove the symbol and continue.
        if (SymI->second.getState() >= Q->getRequiredState()) {
          Q->notifySymbolMetRequiredState(Name, SymI->second.getSymbol());
          return true;
        }

        // Otherwise this symbol does not yet meet the required state. Check
        // whether it has a materializer attached, and if so prepare to run it.
        if (SymI->second.hasMaterializerAttached()) {
          assert(SymI->second.getAddress() == 0 &&
                 "Symbol not resolved but already has address?");
          auto UMII = UnmaterializedInfos.find(Name);
          assert(UMII != UnmaterializedInfos.end() &&
                 "Lazy symbol should have UnmaterializedInfo");

          auto UMI = UMII->second;
          assert(UMI->MU && "Materializer should not be null");
          assert(UMI->RT && "Tracker should not be null");

          // Move all symbols associated with this MaterializationUnit into
          // materializing state.
          for (auto &KV : UMI->MU->getSymbols()) {
            auto SymK = Symbols.find(KV.first);
            SymK->second.setMaterializerAttached(false);
            SymK->second.setState(SymbolState::Materializing);
            UnmaterializedInfos.erase(KV.first);
          }

          // Add MU to the list of MaterializationUnits to be materialized.
          UMIs.push_back(std::move(UMI));
        }

        // Add the query to the PendingQueries list and continue, deleting the
        // element.
        assert(SymI->second.getState() != SymbolState::NeverSearched &&
               SymI->second.getState() != SymbolState::Ready &&
               "By this line the symbol should be materializing");
        auto &MI = MaterializingInfos[Name];
        MI.addQuery(Q);
        Q->addQueryDependence(*this, Name);
        return true;
      });
}

void JITDylib::dump(raw_ostream &OS) {
  ES.runSessionLocked([&, this]() {
    OS << "JITDylib \"" << JITDylibName << "\" (ES: "
       << format("0x%016" PRIx64, reinterpret_cast<uintptr_t>(&ES)) << "):\n"
       << "Link order: " << LinkOrder << "\n"
       << "Symbol table:\n";

    for (auto &KV : Symbols) {
      OS << "    \"" << *KV.first << "\": ";
      if (auto Addr = KV.second.getAddress())
        OS << format("0x%016" PRIx64, Addr) << ", " << KV.second.getFlags()
           << " ";
      else
        OS << "<not resolved> ";

      OS << KV.second.getFlags() << " " << KV.second.getState();

      if (KV.second.hasMaterializerAttached()) {
        OS << " (Materializer ";
        auto I = UnmaterializedInfos.find(KV.first);
        assert(I != UnmaterializedInfos.end() &&
               "Lazy symbol should have UnmaterializedInfo");
        OS << I->second->MU.get() << ", " << I->second->MU->getName() << ")\n";
      } else
        OS << "\n";
    }

    if (!MaterializingInfos.empty())
      OS << "  MaterializingInfos entries:\n";
    for (auto &KV : MaterializingInfos) {
      OS << "    \"" << *KV.first << "\":\n"
         << "      " << KV.second.pendingQueries().size()
         << " pending queries: { ";
      for (const auto &Q : KV.second.pendingQueries())
        OS << Q.get() << " (" << Q->getRequiredState() << ") ";
      OS << "}\n      Dependants:\n";
      for (auto &KV2 : KV.second.Dependants)
        OS << "        " << KV2.first->getName() << ": " << KV2.second << "\n";
      OS << "      Unemitted Dependencies:\n";
      for (auto &KV2 : KV.second.UnemittedDependencies)
        OS << "        " << KV2.first->getName() << ": " << KV2.second << "\n";
    }
  });
}

void JITDylib::MaterializingInfo::addQuery(
    std::shared_ptr<AsynchronousSymbolQuery> Q) {

  auto I = std::lower_bound(
      PendingQueries.rbegin(), PendingQueries.rend(), Q->getRequiredState(),
      [](const std::shared_ptr<AsynchronousSymbolQuery> &V, SymbolState S) {
        return V->getRequiredState() <= S;
      });
  PendingQueries.insert(I.base(), std::move(Q));
}

void JITDylib::MaterializingInfo::removeQuery(
    const AsynchronousSymbolQuery &Q) {
  // FIXME: Implement 'find_as' for shared_ptr<T>/T*.
  auto I =
      std::find_if(PendingQueries.begin(), PendingQueries.end(),
                   [&Q](const std::shared_ptr<AsynchronousSymbolQuery> &V) {
                     return V.get() == &Q;
                   });
  assert(I != PendingQueries.end() &&
         "Query is not attached to this MaterializingInfo");
  PendingQueries.erase(I);
}

JITDylib::AsynchronousSymbolQueryList
JITDylib::MaterializingInfo::takeQueriesMeeting(SymbolState RequiredState) {
  AsynchronousSymbolQueryList Result;
  while (!PendingQueries.empty()) {
    if (PendingQueries.back()->getRequiredState() > RequiredState)
      break;

    Result.push_back(std::move(PendingQueries.back()));
    PendingQueries.pop_back();
  }

  return Result;
}

JITDylib::JITDylib(ExecutionSession &ES, std::string Name)
    : ES(ES), JITDylibName(std::move(Name)) {
  LinkOrder.push_back({this, JITDylibLookupFlags::MatchAllSymbols});
}

ResourceTrackerSP JITDylib::getTracker(MaterializationResponsibility &MR) {
  auto I = MRTrackers.find(&MR);
  assert(I != MRTrackers.end() && "MR is not linked");
  assert(I->second && "Linked tracker is null");
  return I->second;
}

std::pair<JITDylib::AsynchronousSymbolQuerySet,
          std::shared_ptr<SymbolDependenceMap>>
JITDylib::removeTracker(ResourceTracker &RT) {
  // Note: Should be called under the session lock.

  SymbolNameVector SymbolsToRemove;
  std::vector<std::pair<JITDylib *, SymbolStringPtr>> SymbolsToFail;

  if (&RT == DefaultTracker.get()) {
    SymbolNameSet TrackedSymbols;
    for (auto &KV : TrackerSymbols)
      for (auto &Sym : KV.second)
        TrackedSymbols.insert(Sym);

    for (auto &KV : Symbols) {
      auto &Sym = KV.first;
      if (!TrackedSymbols.count(Sym))
        SymbolsToRemove.push_back(Sym);
    }

    DefaultTracker.reset();
  } else {
    /// Check for a non-default tracker.
    auto I = TrackerSymbols.find(&RT);
    if (I != TrackerSymbols.end()) {
      SymbolsToRemove = std::move(I->second);
      TrackerSymbols.erase(I);
    }
    // ... if not found this tracker was already defunct. Nothing to do.
  }

  for (auto &Sym : SymbolsToRemove) {
    auto I = Symbols.find(Sym);
    assert(I != Symbols.end() && "Symbol not in symbol table");

    // If there is a MaterializingInfo then collect any queries to fail.
    auto MII = MaterializingInfos.find(Sym);
    if (MII != MaterializingInfos.end())
      SymbolsToFail.push_back({this, Sym});
  }

  AsynchronousSymbolQuerySet QueriesToFail;
  auto Result = failSymbols(std::move(SymbolsToFail));

  // Removed symbols should be taken out of the table altogether.
  for (auto &Sym : SymbolsToRemove) {
    auto I = Symbols.find(Sym);
    assert(I != Symbols.end() && "Symbol not present in table");

    // Remove Materializer if present.
    if (I->second.hasMaterializerAttached()) {
      // FIXME: Should this discard the symbols?
      UnmaterializedInfos.erase(Sym);
    } else {
      assert(!UnmaterializedInfos.count(Sym) &&
             "Symbol has materializer attached");
    }

    Symbols.erase(I);
  }

  return Result;
}

void JITDylib::transferTracker(ResourceTracker &DstRT, ResourceTracker &SrcRT) {
  assert(&DstRT != &SrcRT && "No-op transfers shouldn't call transferTracker");
  assert(&DstRT.getJITDylib() == this && "DstRT is not for this JITDylib");
  assert(&SrcRT.getJITDylib() == this && "SrcRT is not for this JITDylib");

  // Update trackers for any not-yet materialized units.
  for (auto &KV : UnmaterializedInfos) {
    if (KV.second->RT == &SrcRT)
      KV.second->RT = &DstRT;
  }

  // Update trackers for any active materialization responsibilities.
  for (auto &KV : MRTrackers) {
    if (KV.second == &SrcRT)
      KV.second = &DstRT;
  }

  // If we're transfering to the default tracker we just need to delete the
  // tracked symbols for the source tracker.
  if (&DstRT == DefaultTracker.get()) {
    TrackerSymbols.erase(&SrcRT);
    return;
  }

  // If we're transferring from the default tracker we need to find all
  // currently untracked symbols.
  if (&SrcRT == DefaultTracker.get()) {
    assert(!TrackerSymbols.count(&SrcRT) &&
           "Default tracker should not appear in TrackerSymbols");

    SymbolNameVector SymbolsToTrack;

    SymbolNameSet CurrentlyTrackedSymbols;
    for (auto &KV : TrackerSymbols)
      for (auto &Sym : KV.second)
        CurrentlyTrackedSymbols.insert(Sym);

    for (auto &KV : Symbols) {
      auto &Sym = KV.first;
      if (!CurrentlyTrackedSymbols.count(Sym))
        SymbolsToTrack.push_back(Sym);
    }

    TrackerSymbols[&DstRT] = std::move(SymbolsToTrack);
    return;
  }

  // Finally if neither SrtRT or DstRT are the default tracker then
  // just append DstRT's tracked symbols to SrtRT's.
  auto SI = TrackerSymbols.find(&SrcRT);
  if (SI == TrackerSymbols.end())
    return;

  auto &DstTrackedSymbols = TrackerSymbols[&DstRT];
  DstTrackedSymbols.reserve(DstTrackedSymbols.size() + SI->second.size());
  for (auto &Sym : SI->second)
    DstTrackedSymbols.push_back(std::move(Sym));
  TrackerSymbols.erase(SI);
}

Error JITDylib::defineImpl(MaterializationUnit &MU) {

  LLVM_DEBUG({ dbgs() << "  " << MU.getSymbols() << "\n"; });

  SymbolNameSet Duplicates;
  std::vector<SymbolStringPtr> ExistingDefsOverridden;
  std::vector<SymbolStringPtr> MUDefsOverridden;

  for (const auto &KV : MU.getSymbols()) {
    auto I = Symbols.find(KV.first);

    if (I != Symbols.end()) {
      if (KV.second.isStrong()) {
        if (I->second.getFlags().isStrong() ||
            I->second.getState() > SymbolState::NeverSearched)
          Duplicates.insert(KV.first);
        else {
          assert(I->second.getState() == SymbolState::NeverSearched &&
                 "Overridden existing def should be in the never-searched "
                 "state");
          ExistingDefsOverridden.push_back(KV.first);
        }
      } else
        MUDefsOverridden.push_back(KV.first);
    }
  }

  // If there were any duplicate definitions then bail out.
  if (!Duplicates.empty()) {
    LLVM_DEBUG(
        { dbgs() << "  Error: Duplicate symbols " << Duplicates << "\n"; });
    return make_error<DuplicateDefinition>(std::string(**Duplicates.begin()));
  }

  // Discard any overridden defs in this MU.
  LLVM_DEBUG({
    if (!MUDefsOverridden.empty())
      dbgs() << "  Defs in this MU overridden: " << MUDefsOverridden << "\n";
  });
  for (auto &S : MUDefsOverridden)
    MU.doDiscard(*this, S);

  // Discard existing overridden defs.
  LLVM_DEBUG({
    if (!ExistingDefsOverridden.empty())
      dbgs() << "  Existing defs overridden by this MU: " << MUDefsOverridden
             << "\n";
  });
  for (auto &S : ExistingDefsOverridden) {

    auto UMII = UnmaterializedInfos.find(S);
    assert(UMII != UnmaterializedInfos.end() &&
           "Overridden existing def should have an UnmaterializedInfo");
    UMII->second->MU->doDiscard(*this, S);
  }

  // Finally, add the defs from this MU.
  for (auto &KV : MU.getSymbols()) {
    auto &SymEntry = Symbols[KV.first];
    SymEntry.setFlags(KV.second);
    SymEntry.setState(SymbolState::NeverSearched);
    SymEntry.setMaterializerAttached(true);
  }

  return Error::success();
}

void JITDylib::installMaterializationUnit(
    std::unique_ptr<MaterializationUnit> MU, ResourceTracker &RT) {

  /// defineImpl succeeded.
  if (&RT != DefaultTracker.get()) {
    auto &TS = TrackerSymbols[&RT];
    TS.reserve(TS.size() + MU->getSymbols().size());
    for (auto &KV : MU->getSymbols())
      TS.push_back(KV.first);
  }

  auto UMI = std::make_shared<UnmaterializedInfo>(std::move(MU), &RT);
  for (auto &KV : UMI->MU->getSymbols())
    UnmaterializedInfos[KV.first] = UMI;
}

void JITDylib::detachQueryHelper(AsynchronousSymbolQuery &Q,
                                 const SymbolNameSet &QuerySymbols) {
  for (auto &QuerySymbol : QuerySymbols) {
    assert(MaterializingInfos.count(QuerySymbol) &&
           "QuerySymbol does not have MaterializingInfo");
    auto &MI = MaterializingInfos[QuerySymbol];
    MI.removeQuery(Q);
  }
}

void JITDylib::transferEmittedNodeDependencies(
    MaterializingInfo &DependantMI, const SymbolStringPtr &DependantName,
    MaterializingInfo &EmittedMI) {
  for (auto &KV : EmittedMI.UnemittedDependencies) {
    auto &DependencyJD = *KV.first;
    SymbolNameSet *UnemittedDependenciesOnDependencyJD = nullptr;

    for (auto &DependencyName : KV.second) {
      auto &DependencyMI = DependencyJD.MaterializingInfos[DependencyName];

      // Do not add self dependencies.
      if (&DependencyMI == &DependantMI)
        continue;

      // If we haven't looked up the dependencies for DependencyJD yet, do it
      // now and cache the result.
      if (!UnemittedDependenciesOnDependencyJD)
        UnemittedDependenciesOnDependencyJD =
            &DependantMI.UnemittedDependencies[&DependencyJD];

      DependencyMI.Dependants[this].insert(DependantName);
      UnemittedDependenciesOnDependencyJD->insert(DependencyName);
    }
  }
}

Platform::~Platform() {}

Expected<DenseMap<JITDylib *, SymbolMap>> Platform::lookupInitSymbols(
    ExecutionSession &ES,
    const DenseMap<JITDylib *, SymbolLookupSet> &InitSyms) {

  DenseMap<JITDylib *, SymbolMap> CompoundResult;
  Error CompoundErr = Error::success();
  std::mutex LookupMutex;
  std::condition_variable CV;
  uint64_t Count = InitSyms.size();

  LLVM_DEBUG({
    dbgs() << "Issuing init-symbol lookup:\n";
    for (auto &KV : InitSyms)
      dbgs() << "  " << KV.first->getName() << ": " << KV.second << "\n";
  });

  for (auto &KV : InitSyms) {
    auto *JD = KV.first;
    auto Names = std::move(KV.second);
    ES.lookup(
        LookupKind::Static,
        JITDylibSearchOrder({{JD, JITDylibLookupFlags::MatchAllSymbols}}),
        std::move(Names), SymbolState::Ready,
        [&, JD](Expected<SymbolMap> Result) {
          {
            std::lock_guard<std::mutex> Lock(LookupMutex);
            --Count;
            if (Result) {
              assert(!CompoundResult.count(JD) &&
                     "Duplicate JITDylib in lookup?");
              CompoundResult[JD] = std::move(*Result);
            } else
              CompoundErr =
                  joinErrors(std::move(CompoundErr), Result.takeError());
          }
          CV.notify_one();
        },
        NoDependenciesToRegister);
  }

  std::unique_lock<std::mutex> Lock(LookupMutex);
  CV.wait(Lock, [&] { return Count == 0 || CompoundErr; });

  if (CompoundErr)
    return std::move(CompoundErr);

  return std::move(CompoundResult);
}

ExecutionSession::ExecutionSession(std::shared_ptr<SymbolStringPool> SSP)
    : SSP(SSP ? std::move(SSP) : std::make_shared<SymbolStringPool>()) {}

Error ExecutionSession::endSession() {
  LLVM_DEBUG(dbgs() << "Ending ExecutionSession " << this << "\n");

  std::vector<JITDylibSP> JITDylibsToClose = runSessionLocked([&] {
    SessionOpen = false;
    return std::move(JDs);
  });

  // TODO: notifiy platform? run static deinits?

  Error Err = Error::success();
  for (auto &JD : JITDylibsToClose)
    Err = joinErrors(std::move(Err), JD->clear());
  return Err;
}

void ExecutionSession::registerResourceManager(ResourceManager &RM) {
  runSessionLocked([&] { ResourceManagers.push_back(&RM); });
}

void ExecutionSession::deregisterResourceManager(ResourceManager &RM) {
  runSessionLocked([&] {
    assert(!ResourceManagers.empty() && "No managers registered");
    if (ResourceManagers.back() == &RM)
      ResourceManagers.pop_back();
    else {
      auto I = llvm::find(ResourceManagers, &RM);
      assert(I != ResourceManagers.end() && "RM not registered");
      ResourceManagers.erase(I);
    }
  });
}

JITDylib *ExecutionSession::getJITDylibByName(StringRef Name) {
  return runSessionLocked([&, this]() -> JITDylib * {
    for (auto &JD : JDs)
      if (JD->getName() == Name)
        return JD.get();
    return nullptr;
  });
}

JITDylib &ExecutionSession::createBareJITDylib(std::string Name) {
  assert(!getJITDylibByName(Name) && "JITDylib with that name already exists");
  return runSessionLocked([&, this]() -> JITDylib & {
    JDs.push_back(new JITDylib(*this, std::move(Name)));
    return *JDs.back();
  });
}

Expected<JITDylib &> ExecutionSession::createJITDylib(std::string Name) {
  auto &JD = createBareJITDylib(Name);
  if (P)
    if (auto Err = P->setupJITDylib(JD))
      return std::move(Err);
  return JD;
}

std::vector<JITDylibSP> JITDylib::getDFSLinkOrder(ArrayRef<JITDylibSP> JDs) {
  if (JDs.empty())
    return {};

  auto &ES = JDs.front()->getExecutionSession();
  return ES.runSessionLocked([&]() {
    DenseSet<JITDylib *> Visited;
    std::vector<JITDylibSP> Result;

    for (auto &JD : JDs) {

      if (Visited.count(JD.get()))
        continue;

      SmallVector<JITDylibSP, 64> WorkStack;
      WorkStack.push_back(JD);
      Visited.insert(JD.get());

      while (!WorkStack.empty()) {
        Result.push_back(std::move(WorkStack.back()));
        WorkStack.pop_back();

        for (auto &KV : llvm::reverse(Result.back()->LinkOrder)) {
          auto &JD = *KV.first;
          if (Visited.count(&JD))
            continue;
          Visited.insert(&JD);
          WorkStack.push_back(&JD);
        }
      }
    }
    return Result;
  });
}

std::vector<JITDylibSP>
JITDylib::getReverseDFSLinkOrder(ArrayRef<JITDylibSP> JDs) {
  auto Tmp = getDFSLinkOrder(JDs);
  std::reverse(Tmp.begin(), Tmp.end());
  return Tmp;
}

std::vector<JITDylibSP> JITDylib::getDFSLinkOrder() {
  return getDFSLinkOrder({this});
}

std::vector<JITDylibSP> JITDylib::getReverseDFSLinkOrder() {
  return getReverseDFSLinkOrder({this});
}

void ExecutionSession::lookup(
    LookupKind K, const JITDylibSearchOrder &SearchOrder,
    SymbolLookupSet Symbols, SymbolState RequiredState,
    SymbolsResolvedCallback NotifyComplete,
    RegisterDependenciesFunction RegisterDependencies) {

  LLVM_DEBUG({
    runSessionLocked([&]() {
      dbgs() << "Looking up " << Symbols << " in " << SearchOrder
             << " (required state: " << RequiredState << ")\n";
    });
  });

  // lookup can be re-entered recursively if running on a single thread. Run any
  // outstanding MUs in case this query depends on them, otherwise this lookup
  // will starve waiting for a result from an MU that is stuck in the queue.
  runOutstandingMUs();

  auto Unresolved = std::move(Symbols);
  auto Q = std::make_shared<AsynchronousSymbolQuery>(Unresolved, RequiredState,
                                                     std::move(NotifyComplete));
  bool QueryComplete = false;

  auto LodgingErr = runSessionLocked([&]() -> Error {
    DenseMap<JITDylib *, JITDylib::UnmaterializedInfosList> CollectedMUsMap;

    auto LodgeQuery = [&]() -> Error {
      for (auto &KV : SearchOrder) {
        assert(KV.first && "JITDylibList entries must not be null");
        assert(!CollectedMUsMap.count(KV.first) &&
               "JITDylibList should not contain duplicate entries");

        auto &JD = *KV.first;
        auto JDLookupFlags = KV.second;
        if (auto Err = JD.lodgeQuery(CollectedMUsMap[&JD], Q, K, JDLookupFlags,
                                     Unresolved))
          return Err;
      }

      // Strip any weakly referenced symbols that were not found.
      Unresolved.forEachWithRemoval(
          [&](const SymbolStringPtr &Name, SymbolLookupFlags Flags) {
            if (Flags == SymbolLookupFlags::WeaklyReferencedSymbol) {
              Q->dropSymbol(Name);
              return true;
            }
            return false;
          });

      if (!Unresolved.empty())
        return make_error<SymbolsNotFound>(Unresolved.getSymbolNames());

      return Error::success();
    };

    if (auto Err = LodgeQuery()) {
      // Query failed.

      // Disconnect the query from its dependencies.
      Q->detach();

      // Replace the MUs.
      for (auto &KV : CollectedMUsMap) {
        auto &JD = *KV.first;
        for (auto &UMI : KV.second)
          for (auto &KV2 : UMI->MU->getSymbols()) {
            assert(!JD.UnmaterializedInfos.count(KV2.first) &&
                   "Unexpected materializer in map");
            auto SymI = JD.Symbols.find(KV2.first);
            assert(SymI != JD.Symbols.end() && "Missing symbol entry");
            assert(SymI->second.getState() == SymbolState::Materializing &&
                   "Can not replace symbol that is not materializing");
            assert(!SymI->second.hasMaterializerAttached() &&
                   "MaterializerAttached flag should not be set");
            SymI->second.setMaterializerAttached(true);
            JD.UnmaterializedInfos[KV2.first] = UMI;
          }
      }

      return Err;
    }
    // Query lodged successfully. Add MRs to the OutstandingMUs list.

    // Move the MUs to the OutstandingMUs list.
    {
      std::lock_guard<std::recursive_mutex> Lock(OutstandingMUsMutex);

      for (auto &KV : CollectedMUsMap) {
        auto &JD = *KV.first;
        for (auto &UMI : KV.second) {
          std::unique_ptr<MaterializationResponsibility> MR(
              new MaterializationResponsibility(
                  &JD, std::move(UMI->MU->SymbolFlags),
                  std::move(UMI->MU->InitSymbol)));
          JD.MRTrackers[MR.get()] = UMI->RT;
          OutstandingMUs.push_back(
              std::make_pair(std::move(UMI->MU), std::move(MR)));
        }
      }
    }

    // Record whether this query is fully ready / resolved. We will use
    // this to call handleFullyResolved/handleFullyReady outside the session
    // lock.
    QueryComplete = Q->isComplete();

    // Call the register dependencies function.
    if (RegisterDependencies && !Q->QueryRegistrations.empty())
      RegisterDependencies(Q->QueryRegistrations);

    return Error::success();
  });

  if (LodgingErr) {
    Q->handleFailed(std::move(LodgingErr));
    return;
  }

  if (QueryComplete)
    Q->handleComplete();

  runOutstandingMUs();
}

Expected<SymbolMap>
ExecutionSession::lookup(const JITDylibSearchOrder &SearchOrder,
                         const SymbolLookupSet &Symbols, LookupKind K,
                         SymbolState RequiredState,
                         RegisterDependenciesFunction RegisterDependencies) {
#if LLVM_ENABLE_THREADS
  // In the threaded case we use promises to return the results.
  std::promise<SymbolMap> PromisedResult;
  Error ResolutionError = Error::success();

  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    if (R)
      PromisedResult.set_value(std::move(*R));
    else {
      ErrorAsOutParameter _(&ResolutionError);
      ResolutionError = R.takeError();
      PromisedResult.set_value(SymbolMap());
    }
  };

#else
  SymbolMap Result;
  Error ResolutionError = Error::success();

  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    ErrorAsOutParameter _(&ResolutionError);
    if (R)
      Result = std::move(*R);
    else
      ResolutionError = R.takeError();
  };
#endif

  // Perform the asynchronous lookup.
  lookup(K, SearchOrder, Symbols, RequiredState, NotifyComplete,
         RegisterDependencies);

#if LLVM_ENABLE_THREADS
  auto ResultFuture = PromisedResult.get_future();
  auto Result = ResultFuture.get();

  if (ResolutionError)
    return std::move(ResolutionError);

  return std::move(Result);

#else
  if (ResolutionError)
    return std::move(ResolutionError);

  return Result;
#endif
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(const JITDylibSearchOrder &SearchOrder,
                         SymbolStringPtr Name, SymbolState RequiredState) {
  SymbolLookupSet Names({Name});

  if (auto ResultMap = lookup(SearchOrder, std::move(Names), LookupKind::Static,
                              RequiredState, NoDependenciesToRegister)) {
    assert(ResultMap->size() == 1 && "Unexpected number of results");
    assert(ResultMap->count(Name) && "Missing result for symbol");
    return std::move(ResultMap->begin()->second);
  } else
    return ResultMap.takeError();
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(ArrayRef<JITDylib *> SearchOrder, SymbolStringPtr Name,
                         SymbolState RequiredState) {
  return lookup(makeJITDylibSearchOrder(SearchOrder), Name, RequiredState);
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(ArrayRef<JITDylib *> SearchOrder, StringRef Name,
                         SymbolState RequiredState) {
  return lookup(SearchOrder, intern(Name), RequiredState);
}

void ExecutionSession::dump(raw_ostream &OS) {
  runSessionLocked([this, &OS]() {
    for (auto &JD : JDs)
      JD->dump(OS);
  });
}

void ExecutionSession::runOutstandingMUs() {
  while (1) {
    Optional<std::pair<std::unique_ptr<MaterializationUnit>,
                       std::unique_ptr<MaterializationResponsibility>>>
        JMU;

    {
      std::lock_guard<std::recursive_mutex> Lock(OutstandingMUsMutex);
      if (!OutstandingMUs.empty()) {
        JMU.emplace(std::move(OutstandingMUs.back()));
        OutstandingMUs.pop_back();
      }
    }

    if (!JMU)
      break;

    assert(JMU->first && "No MU?");
    dispatchMaterialization(std::move(JMU->first), std::move(JMU->second));
  }
}

Error ExecutionSession::removeResourceTracker(ResourceTracker &RT) {
  LLVM_DEBUG({
    dbgs() << "In " << RT.getJITDylib().getName() << " removing tracker "
           << formatv("{0:x}", RT.getKeyUnsafe()) << "\n";
  });
  std::vector<ResourceManager *> CurrentResourceManagers;

  JITDylib::AsynchronousSymbolQuerySet QueriesToFail;
  std::shared_ptr<SymbolDependenceMap> FailedSymbols;

  runSessionLocked([&] {
    CurrentResourceManagers = ResourceManagers;
    RT.makeDefunct();
    std::tie(QueriesToFail, FailedSymbols) = RT.getJITDylib().removeTracker(RT);
  });

  Error Err = Error::success();

  for (auto *L : reverse(CurrentResourceManagers))
    Err =
        joinErrors(std::move(Err), L->handleRemoveResources(RT.getKeyUnsafe()));

  for (auto &Q : QueriesToFail)
    Q->handleFailed(make_error<FailedToMaterialize>(FailedSymbols));

  return Err;
}

void ExecutionSession::transferResourceTracker(ResourceTracker &DstRT,
                                               ResourceTracker &SrcRT) {
  LLVM_DEBUG({
    dbgs() << "In " << SrcRT.getJITDylib().getName()
           << " transfering resources from tracker "
           << formatv("{0:x}", SrcRT.getKeyUnsafe()) << " to tracker "
           << formatv("{0:x}", DstRT.getKeyUnsafe()) << "\n";
  });

  // No-op transfers are allowed and do not invalidate the source.
  if (&DstRT == &SrcRT)
    return;

  assert(&DstRT.getJITDylib() == &SrcRT.getJITDylib() &&
         "Can't transfer resources between JITDylibs");
  runSessionLocked([&]() {
    SrcRT.makeDefunct();
    auto &JD = DstRT.getJITDylib();
    JD.transferTracker(DstRT, SrcRT);
    for (auto *L : reverse(ResourceManagers))
      L->handleTransferResources(DstRT.getKeyUnsafe(), SrcRT.getKeyUnsafe());
  });
}

void ExecutionSession::destroyResourceTracker(ResourceTracker &RT) {
  runSessionLocked([&]() {
    LLVM_DEBUG({
      dbgs() << "In " << RT.getJITDylib().getName() << " destroying tracker "
             << formatv("{0:x}", RT.getKeyUnsafe()) << "\n";
    });
    if (!RT.isDefunct())
      transferResourceTracker(*RT.getJITDylib().getDefaultResourceTracker(),
                              RT);
  });
}

#ifndef NDEBUG
void ExecutionSession::dumpDispatchInfo(JITDylib &JD, MaterializationUnit &MU) {
  runSessionLocked([&]() {
    dbgs() << "Dispatching " << MU << " for " << JD.getName() << "\n";
  });
}
#endif // NDEBUG

} // End namespace orc.
} // End namespace llvm.
