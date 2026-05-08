// mcpp.fetcher — backward-compat shim. The implementation has moved
// to `mcpp.pm.package_fetcher` as part of the package-management
// subsystem refactor (PR-R3 in
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`).
//
// All existing call sites continue to use `mcpp::fetcher::Fetcher`,
// `mcpp::fetcher::EventHandler`, etc. — the aliases below resolve those
// to the new `mcpp::pm` types. The shim disappears once `cli.cppm`
// migrates to the `mcpp::pm::` qualified names directly.
//
// (The architecture doc's "split into index_repo + index_store +
// package_fetcher" is reserved for the index-config feature work; the
// current `fetcher.cppm` body is a single xlings NDJSON client and
// stays a single module here.)

export module mcpp.fetcher;

import std;
export import mcpp.pm.package_fetcher;

export namespace mcpp::fetcher {

using EventKind     = mcpp::pm::EventKind;
using ProgressEvent = mcpp::pm::ProgressEvent;
using LogEvent      = mcpp::pm::LogEvent;
using DataEvent     = mcpp::pm::DataEvent;
using ErrorEvent    = mcpp::pm::ErrorEvent;
using ResultEvent   = mcpp::pm::ResultEvent;
using Event         = mcpp::pm::Event;
using EventHandler  = mcpp::pm::EventHandler;
using CallError     = mcpp::pm::CallError;
using CallResult    = mcpp::pm::CallResult;
using Fetcher       = mcpp::pm::Fetcher;

} // namespace mcpp::fetcher
