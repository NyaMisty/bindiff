// Copyright 2011-2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Command-line version of BinDiff.

#include <atomic>
#include <cassert>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>  // NOLINT
#include <memory>
#include <sstream>  // NOLINT
#include <stdexcept>
#include <string>
#include <thread>  // NOLINT
#include <utility>
#include <vector>

#ifdef GOOGLE
#include "base/commandlineflags.h"
#include "base/init_google.h"
#include "base/logging_extensions.h"
#else
#include "third_party/absl/flags/flag.h"
#include "third_party/absl/flags/internal/usage.h"
#include "third_party/absl/flags/parse.h"
#include "third_party/absl/flags/usage.h"
#include "third_party/absl/flags/usage_config.h"
#include "third_party/absl/strings/match.h"
#endif  // GOOGLE
#include "base/logging.h"
#include "third_party/absl/base/const_init.h"
#include "third_party/absl/container/flat_hash_map.h"
#include "third_party/absl/memory/memory.h"
#include "third_party/absl/status/status.h"
#include "third_party/absl/strings/ascii.h"
#include "third_party/absl/strings/str_cat.h"
#include "third_party/absl/strings/str_format.h"
#include "third_party/absl/synchronization/mutex.h"
#include "third_party/absl/time/time.h"
#include "third_party/zynamics/bindiff/call_graph.h"
#include "third_party/zynamics/bindiff/call_graph_match.h"
#include "third_party/zynamics/bindiff/config.h"
#include "third_party/zynamics/bindiff/database_writer.h"
#include "third_party/zynamics/bindiff/differ.h"
#include "third_party/zynamics/bindiff/flow_graph.h"
#include "third_party/zynamics/bindiff/flow_graph_match.h"
#include "third_party/zynamics/bindiff/log_writer.h"
#include "third_party/zynamics/bindiff/match_context.h"
#include "third_party/zynamics/bindiff/start_ui.h"
#include "third_party/zynamics/bindiff/version.h"
#include "third_party/zynamics/binexport/binexport2.pb.h"
#include "third_party/zynamics/binexport/types.h"
#include "third_party/zynamics/binexport/util/filesystem.h"
#include "third_party/zynamics/binexport/util/format.h"
#include "third_party/zynamics/binexport/util/idb_export.h"
#include "third_party/zynamics/binexport/util/status_macros.h"
#include "third_party/zynamics/binexport/util/timer.h"

ABSL_FLAG(bool, nologo, false, "do not display version/copyright information");
ABSL_FLAG(bool, ui, false, "launch the BinDiff UI");
ABSL_FLAG(std::string, primary, "", "primary input file or path in batch mode");
ABSL_FLAG(std::string, secondary, "", "secondary input file (optional)");
ABSL_FLAG(std::string, output_dir, "",
          "output path, defaults to current directory");
ABSL_FLAG(std::vector<std::string>, output_format, {"bin"},
          "comma-separated list of output formats: log (text file), bin[ary] "
          "(BinDiff database loadable by the disassembler plugins)");
ABSL_FLAG(bool, md_index, false, "dump MD indices (will not diff anything)");
ABSL_FLAG(bool, export, false,
          "batch export .idb files from input directory to BinExport format");
ABSL_FLAG(bool, ls, false,
          "list hash/filenames for all .BinExport files in input directory");
ABSL_FLAG(std::string, config, "", "specify config file name");

namespace security::bindiff {

using ::security::binexport::CollectIdbsToExport;
using ::security::binexport::FormatAddress;
using ::security::binexport::HumanReadableDuration;
using ::security::binexport::IdbExporter;
using ::security::binexport::kBinExportExtension;

// BinDiff default configuration.
ABSL_CONST_INIT const absl::string_view kDefaultConfig =
    R"raw(<?xml version="1.0"?>
<bindiff config-version="6">
  <ui server="127.0.0.1" port="2000" retries="20" />
  <function-matching>
    <step confidence="1.0" algorithm="function: name hash matching" />
    <step confidence="1.0" algorithm="function: hash matching" />
    <step confidence="1.0" algorithm="function: edges flowgraph MD index" />
    <step confidence="0.9" algorithm="function: edges callgraph MD index" />
    <step confidence="0.9" algorithm="function: MD index matching (flowgraph MD index, top down)" />
    <step confidence="0.9" algorithm="function: MD index matching (flowgraph MD index, bottom up)" />
    <step confidence="0.9" algorithm="function: prime signature matching" />
    <step confidence="0.8" algorithm="function: MD index matching (callGraph MD index, top down)" />
    <step confidence="0.8" algorithm="function: MD index matching (callGraph MD index, bottom up)" />
    <!-- <step confidence="0.7" algorithm="function: edges proximity MD index" /> -->
    <step confidence="0.7" algorithm="function: relaxed MD index matching" />
    <step confidence="0.4" algorithm="function: instruction count" />
    <step confidence="0.4" algorithm="function: address sequence" />
    <step confidence="0.7" algorithm="function: string references" />
    <step confidence="0.6" algorithm="function: loop count matching" />
    <step confidence="0.1" algorithm="function: call sequence matching(exact)" />
    <step confidence="0.0" algorithm="function: call sequence matching(topology)" />
    <step confidence="0.0" algorithm="function: call sequence matching(sequence)" />
  </function-matching>
  <basic-block-matching>
    <step confidence="1.0" algorithm="basicBlock: edges prime product" />
    <step confidence="1.0" algorithm="basicBlock: hash matching (4 instructions minimum)" />
    <step confidence="0.9" algorithm="basicBlock: prime matching (4 instructions minimum)" />
    <step confidence="0.8" algorithm="basicBlock: call reference matching" />
    <step confidence="0.8" algorithm="basicBlock: string references matching" />
    <step confidence="0.7" algorithm="basicBlock: edges MD index (top down)" />
    <step confidence="0.7" algorithm="basicBlock: MD index matching (top down)" />
    <step confidence="0.7" algorithm="basicBlock: edges MD index (bottom up)" />
    <step confidence="0.7" algorithm="basicBlock: MD index matching (bottom up)" />
    <step confidence="0.6" algorithm="basicBlock: relaxed MD index matching" />
    <step confidence="0.5" algorithm="basicBlock: prime matching (0 instructions minimum)" />
    <step confidence="0.4" algorithm="basicBlock: edges Lengauer Tarjan dominated" />
    <step confidence="0.4" algorithm="basicBlock: loop entry matching" />
    <step confidence="0.3" algorithm="basicBlock: self loop matching" />
    <step confidence="0.2" algorithm="basicBlock: entry point matching" />
    <step confidence="0.1" algorithm="basicBlock: exit point matching" />
    <step confidence="0.0" algorithm="basicBlock: instruction count matching" />
    <step confidence="0.0" algorithm="basicBlock: jump sequence matching" />
  </basic-block-matching>
</bindiff>)raw";

ABSL_CONST_INIT absl::Mutex g_queue_mutex(absl::kConstInit);
std::atomic<bool> g_wants_to_quit = ATOMIC_VAR_INIT(false);

bool g_output_binary = false;
bool g_output_log = false;

using DiffPairList = std::list<std::pair<std::string, std::string>>;

void PrintMessage(absl::string_view message) {
  auto size = message.size();
  fwrite(message.data(), 1 /* Size */, size, stdout);
  fwrite("\n", 1 /* Size */, 1 /* Count */, stdout);
#ifdef GOOGLE
  // If writing to logfiles is enabled, log the message.
  LOG_IF(INFO, absl::GetFlag(FLAGS_logtostderr)) << message;
#endif
}

void PrintErrorMessage(absl::string_view message) {
  auto size = message.size();
  fwrite(message.data(), 1 /* Size */, size, stderr);
  fwrite("\n", 1 /* Size */, 1 /* Count */, stderr);
#ifdef GOOGLE
  // If writing to logfiles is enabled, log the message.
  LOG_IF(ERROR, absl::GetFlag(FLAGS_logtostderr)) << message;
#endif
}

#ifndef GOOGLE
void UnprefixedLogHandler(google::protobuf::LogLevel level,
                          const char* filename, int line,
                          const std::string& message) {
  fwrite(message.data(), 1 /* Size */, message.size(), stdout);
  fwrite("\n", 1 /* Size */, 1 /* Count */, stdout);
}
#endif

// This function will try and create a fully specified filename no longer than
// 250 characters. It'll truncate part1 and part2, leaving all other fragments
// as is. If it is not possible to get a short enough name it'll throw an
// exception.
std::string GetTruncatedFilename(
    const std::string& path /* Must include trailing slash */,
    const std::string& part1 /* Potentially truncated */,
    const std::string& middle,
    const std::string& part2 /* Potentially truncated */,
    const std::string& extension) {
  enum { kMaxFilename = 250 };

  const std::string::size_type length = path.size() + part1.size() +
                                        middle.size() + part2.size() +
                                        extension.size();
  if (length <= kMaxFilename) {
    return absl::StrCat(path, part1, middle, part2, extension);
  }

  std::string::size_type overflow = length - kMaxFilename;

  // First, shorten the longer of the two strings.
  std::string one = part1;
  std::string two = part2;
  if (part1.size() > part2.size()) {
    one = part1.substr(
        0, std::max(part2.size(),
                    part1.size() > overflow ? part1.size() - overflow : 0));
    overflow -= part1.size() - one.size();
  } else if (part2.size() > part1.size()) {
    two = part2.substr(
        0, std::max(part1.size(),
                    part2.size() > overflow ? part2.size() - overflow : 0));
    overflow -= part2.size() - two.size();
  }
  if (!overflow) {
    return path + one + middle + two + extension;
  }

  // Second, if that still wasn't enough, shorten both strings equally.
  assert(one.size() == two.size());
  if (overflow / 2 >= one.size()) {
    throw std::runtime_error(
        absl::StrCat("Cannot create a valid filename, choose shorter input "
                     "names/directories: '",
                     path, part1, middle, part2, extension, "'"));
  }
  return absl::StrCat(path, part1.substr(0, one.size() - overflow / 2), middle,
                      part2.substr(0, two.size() - overflow / 2), extension);
}

class DifferThread {
 public:
  explicit DifferThread(const std::string& path, const std::string& out_path,
                        DiffPairList* files);  // Not owned.

  void operator()();

 private:
  DiffPairList* file_queue_;
  std::string path_;
  std::string out_path_;
};

DifferThread::DifferThread(const std::string& path, const std::string& out_path,
                           DiffPairList* files)
    : file_queue_(files), path_(path), out_path_(out_path) {}

void DifferThread::operator()() {
  const MatchingSteps default_callgraph_steps(GetDefaultMatchingSteps());
  const MatchingStepsFlowGraph default_basicblock_steps(
      GetDefaultMatchingStepsBasicBlock());

  Instruction::Cache instruction_cache;
  FlowGraphs flow_graphs1;
  FlowGraphs flow_graphs2;
  CallGraph call_graph1;
  CallGraph call_graph2;
  std::string last_file1;
  std::string last_file2;
  ScopedCleanup cleanup(&flow_graphs1, &flow_graphs2, &instruction_cache);
  do {
    std::string file1;
    std::string file2;
    try {
      Timer<> timer;
      {
        // Pop pair from todo queue.
        absl::MutexLock lock{&g_queue_mutex};
        if (file_queue_->empty()) {
          break;
        }
        file1 = file_queue_->front().first;
        file2 = file_queue_->front().second;
        file_queue_->pop_front();
      }

      // We need to keep the cache around if one file stays the same
      if (last_file1 != file1 && last_file2 != file2) {
        instruction_cache.clear();
      }

      // Perform setup and diff.
      // TODO(cblichmann): Consider inverted pairs as well, i.e. file1 ==
      //                   last_file2.
      if (last_file1 != file1) {
        PrintMessage(absl::StrCat("Reading ", file1));
        DeleteFlowGraphs(&flow_graphs1);
        FlowGraphInfos infos;
        Read(JoinPath(path_, file1), &call_graph1, &flow_graphs1, &infos,
             &instruction_cache);
      } else {
        ResetMatches(&flow_graphs1);
      }

      if (last_file2 != file2) {
        PrintMessage(absl::StrCat("Reading ", file2));
        DeleteFlowGraphs(&flow_graphs2);
        FlowGraphInfos infos;
        Read(JoinPath(path_, file2), &call_graph2, &flow_graphs2, &infos,
             &instruction_cache);
      } else {
        ResetMatches(&flow_graphs2);
      }

      PrintMessage(absl::StrCat("Diffing ", file1, " vs ", file2));

      FixedPoints fixed_points;
      MatchingContext context(call_graph1, call_graph2, flow_graphs1,
                              flow_graphs2, fixed_points);
      Diff(&context, default_callgraph_steps, default_basicblock_steps);

      Histogram histogram;
      Counts counts;
      GetCountsAndHistogram(flow_graphs1, flow_graphs2, fixed_points,
                            &histogram, &counts);
      const double similarity =
          GetSimilarityScore(call_graph1, call_graph2, histogram, counts);
      Confidences confidences;
      const double confidence = GetConfidence(histogram, &confidences);

      PrintMessage("Writing results");
      {
        ChainWriter writer;
        if (g_output_log) {
          writer.Add(std::make_shared<ResultsLogWriter>(GetTruncatedFilename(
              out_path_ + kPathSeparator, call_graph1.GetFilename(), "_vs_",
              call_graph2.GetFilename(), ".results")));
        }
        if (g_output_binary || writer.IsEmpty()) {
          writer.Add(std::make_shared<DatabaseWriter>(GetTruncatedFilename(
              out_path_ + kPathSeparator, call_graph1.GetFilename(), "_vs_",
              call_graph2.GetFilename(), ".BinDiff")));
        }

        writer.Write(call_graph1, call_graph2, flow_graphs1, flow_graphs2,
                     fixed_points);

        std::string result_message = absl::StrCat(
            file1, " vs ", file2, " (", HumanReadableDuration(timer.elapsed()),
            "):\tsimilarity:\t", similarity, "\tconfidence:\t", confidence);
        for (int i = 0; i < counts.ui_entry_size(); ++i) {
          const auto& [name, value] = counts.GetEntry(i);
          absl::StrAppend(&result_message, "\n\t", name, ":\t", value);
        }
        PrintMessage(result_message);
      }

      last_file1 = file1;
      last_file2 = file2;
    } catch (const std::bad_alloc&) {
      PrintErrorMessage(
          absl::StrCat("out of memory diffing ", file1, " vs ", file2));
      last_file1.clear();
      last_file2.clear();
    } catch (const std::exception& error) {
      PrintErrorMessage(absl::StrCat("while diffing ", file1, " vs ", file2,
                                     ": ", error.what()));

      last_file1.clear();
      last_file2.clear();
    }
  } while (!g_wants_to_quit);
}

void ListFiles(const std::string& path) {
  std::vector<std::string> entries;
  if (absl::Status status = GetDirectoryEntries(path, &entries); !status.ok()) {
    PrintErrorMessage(absl::StrCat("error listing files: ", status.message()));
    return;
  }

  for (const auto& entry : entries) {
    const auto file_path(JoinPath(path, entry));
    if (IsDirectory(file_path)) {
      continue;
    }
    const auto extension = absl::AsciiStrToUpper(GetFileExtension(file_path));
    if (extension != ".BINEXPORT") {
      continue;
    }
    std::ifstream file(file_path, std::ios_base::binary);
    BinExport2 proto;
    if (proto.ParseFromIstream(&file)) {
      const auto& meta_information = proto.meta_information();
      PrintErrorMessage(absl::StrCat(file_path, ": ",
                                     meta_information.executable_id(), " (",
                                     meta_information.executable_name(), ")"));
    }
  }
}

void BatchDiff(const std::string& path, const std::string& reference_file,
               const std::string& out_path) {
  const std::string full_path = GetFullPathName(path);
  const std::string full_reference_file =
      !reference_file.empty() ? GetFullPathName(reference_file) : "";
  const std::string full_out_path = GetFullPathName(out_path);

  std::vector<std::string> idbs;
  std::vector<std::string> binexports;
  if (auto idbs_or = CollectIdbsToExport(full_path, &binexports);
      !idbs_or.ok()) {
    throw std::runtime_error(std::string(idbs_or.status().message()));
  } else {
    idbs = std::move(idbs_or).value();
  }

  const auto* config = GetConfig();
  const int num_threads = config->ReadInt("/bindiff/threads/@use",
                                          std::thread::hardware_concurrency());
  IdbExporter exporter(
      IdbExporter::Options()
          .set_export_dir(full_out_path)
          .set_num_threads(num_threads)
          .set_ida_dir(config->ReadString("/bindiff/ida/@directory", ""))
          .set_ida_exe(config->ReadString("/bindiff/ida/@executable", ""))
          .set_ida_exe64(config->ReadString("/bindiff/ida/@executable64", "")));
  for (const std::string& idb : idbs) {
    const std::string full_idb_path = JoinPath(full_path, idb);
    if (GetFileSize(full_idb_path).value_or(0) > 0) {
      exporter.AddDatabase(full_idb_path);
      binexports.push_back(ReplaceFileExtension(idb, kBinExportExtension));
    } else {
      PrintMessage(
          absl::StrCat("Warning: skipping empty file ", full_idb_path));
    }
  }

  // Create todo list of file pairs.
  DiffPairList files;
  for (auto it = binexports.begin(), end = binexports.end(); it != end; ++it) {
    for (auto jt = binexports.begin(); jt != end; ++jt) {
      if (it == jt) {
        continue;
      }
      if (full_reference_file.empty() ||
          full_reference_file == JoinPath(full_path, *it)) {
        files.emplace_back(*it, *jt);
      }
    }
  }

  Timer<> timer;
  int num_exported = 0;
  exporter
      .Export([&num_exported](const absl::Status& status,
                              const std::string& idb_path, double elapsed) {
        if (!status.ok()) {
          PrintErrorMessage(status.message());
        } else {
          PrintMessage(absl::StrCat(HumanReadableDuration(elapsed), "\t",
                                    GetFileSize(idb_path).value_or(0), "\t",
                                    idb_path));
          ++num_exported;
        }
        return !g_wants_to_quit;
      })
      .IgnoreError();
  const auto export_time = timer.elapsed();
  PrintMessage(absl::StrCat(num_exported, " files exported in ",
                            HumanReadableDuration(export_time)));

  timer.restart();
  if (!absl::GetFlag(FLAGS_export)) {  // Perform diff
    std::vector<std::thread> threads;
    threads.reserve(num_threads);
    int num_diffed = files.size();
    for (int i = 0; i < num_threads; ++i) {
      threads.emplace_back(DifferThread(full_path, full_out_path, &files));
    }
    for (auto& thread : threads) {
      thread.join();
    }
    const auto diff_time = timer.elapsed();
    PrintMessage(absl::StrCat(num_diffed, " pairs diffed in ",
                              HumanReadableDuration(diff_time)));
  }
}

void DumpMdIndices(const CallGraph& call_graph, const FlowGraphs& flow_graphs) {
  std::cout << "\n"
            << call_graph.GetFilename() << "\n"
            << call_graph.GetMdIndex();
  for (auto i = flow_graphs.cbegin(), end = flow_graphs.cend(); i != end; ++i) {
    std::cout << "\n"
              << FormatAddress((*i)->GetEntryPointAddress()) << "\t"
              << std::fixed << std::setprecision(12) << (*i)->GetMdIndex()
              << "\t" << ((*i)->IsLibrary() ? "Library" : "Non-library");
  }
  std::cout << std::endl;
}

void BatchDumpMdIndices(const std::string& path) {
  std::vector<std::string> entries;
  if (absl::Status status = GetDirectoryEntries(path, &entries); !status.ok()) {
    PrintErrorMessage(absl::StrCat("error listing files in `", path,
                                   "`: ", status.message()));
    return;
  }
  for (const auto& entry : entries) {
    auto file_path(JoinPath(path, entry));
    if (IsDirectory(file_path)) {
      continue;
    }
    auto extension = absl::AsciiStrToUpper(GetFileExtension(file_path));
    if (extension != ".CALL_GRAPH") {
      continue;
    }

    CallGraph call_graph;
    FlowGraphs flow_graphs;
    Instruction::Cache instruction_cache;
    ScopedCleanup cleanup(&flow_graphs, 0, &instruction_cache);
    FlowGraphInfos infos;
    Read(file_path, &call_graph, &flow_graphs, &infos, &instruction_cache);
    DumpMdIndices(call_graph, flow_graphs);
  }
}

void SignalHandler(int code) {
  static int signal_count = 0;
  switch (code) {
#ifdef WIN32
    case SIGBREAK:  // Ctrl-Break, not available on Unix
#endif
    case SIGINT:  // Ctrl-C
      if (++signal_count < 3) {
        PrintErrorMessage("shutting down after current operations finish");
        g_wants_to_quit = true;
      } else {
        PrintErrorMessage("forcefully terminating process");
        exit(1);
      }
      break;
  }
}

#ifndef GOOGLE
// Install Abseil Flags' library usage callbacks. This needs to be done before
// any operation that may call one of the callbacks.
void InstallFlagsUsageConfig() {
  absl::FlagsUsageConfig usage_config;
  usage_config.contains_help_flags = [](absl::string_view filename) {
    return !absl::StartsWith(filename, "core library");
  };
  usage_config.contains_helpshort_flags = usage_config.contains_help_flags;
  usage_config.version_string = []() {
    return absl::StrCat(kBinDiffName, " ", kBinDiffDetailedVersion, "\n");
  };
  usage_config.normalize_filename =
      [](absl::string_view filename) -> std::string {
    return absl::StartsWith(filename, "absl") ? "core library" : "this binary";
  };
  absl::SetFlagsUsageConfig(usage_config);
}
#endif

absl::Status BinDiffMain(int argc, char* argv[]) {
#ifdef WIN32
  signal(SIGBREAK, SignalHandler);
#endif
  signal(SIGINT, SignalHandler);

  const std::string binary_name = Basename(argv[0]);
  const std::string current_path = GetCurrentDirectory();
  if (absl::GetFlag(FLAGS_output_dir).empty()) {
    absl::SetFlag(&FLAGS_output_dir, current_path);
  }

  std::string usage = absl::StrFormat(
      "Find similarities and differences in disassembled code.\n"
      "Usage: %1$s [OPTION] DIRECTORY\n"
      "  or:  %1$s [OPTION] PRIMARY SECONDARY\n"
      "  or:  %1$s [OPTION] --primary=PRIMARY [--secondary=SECONDARY]\n"
      "  or:  %1$s --ui [UIOPTION...]\n"
      "In the 1st form, diff all files in a directory against each other. If\n"
      "the directory contains IDA Pro databases these will be exported first.\n"
      "In the 2nd and 3rd form, diff two previously exported binaries.\n"
      "In the 4th form, launch the BinDiff UI.",
      binary_name);
  std::vector<std::string> positional;
  positional.reserve(argc - 1);
#ifdef GOOGLE
  InitGoogle(usage.c_str(), &argc, &argv, /*remove_flags=*/true);
  for (int i = 1; i < argc; ++i) {
    positional.push_back(argv[i]);
  }
#else
  absl::SetProgramUsageMessage(usage);
  InstallFlagsUsageConfig();
  {
    const std::vector<char*> parsed_argv = absl::ParseCommandLine(argc, argv);
    for (int i = 1; i < parsed_argv.size(); ++i) {
      positional.push_back(parsed_argv[i]);
    }
  }
  SetLogHandler(&UnprefixedLogHandler);
#endif

  if (!absl::GetFlag(FLAGS_nologo)) {
    PrintMessage(absl::StrCat(kBinDiffName, " ", kBinDiffDetailedVersion, ", ",
                              kBinDiffCopyright));
  }

  auto config = GetConfig();
  NA_RETURN_IF_ERROR(
      !absl::GetFlag(FLAGS_config).empty()
          ? config->LoadFromFileWithDefaults(absl::GetFlag(FLAGS_config),
                                             std::string(kDefaultConfig))
          : InitConfig());

  // Launch Java UI if requested
  if (binary_name == "bindiff_ui" || absl::GetFlag(FLAGS_ui)) {
    NA_RETURN_IF_ERROR(StartUiWithOptions(
        positional,
        StartUiOptions{}
            .set_java_binary(config->ReadString("/bindiff/ui/@java-binary", ""))
            .set_java_vm_options(
                config->ReadString("/bindiff/ui/@java-vm-options", ""))
            .set_max_heap_size_mb(
                config->ReadInt("/bindiff/ui/@max-heap-size-mb", -1))
            .set_gui_dir(config->ReadString("/bindiff/ui/@directory", ""))));
    return absl::OkStatus();
  }

  // This initializes static variables before the threads get to them
  if (GetDefaultMatchingSteps().empty() ||
      GetDefaultMatchingStepsBasicBlock().empty()) {
    return absl::FailedPreconditionError("Config file invalid");
  }

  for (const auto& entry : absl::GetFlag(FLAGS_output_format)) {
    const std::string format = absl::AsciiStrToUpper(entry);
    if (format == "BIN" || format == "BINARY") {
      g_output_binary = true;
    } else if (format == "LOG") {
      g_output_log = true;
    } else {
      return absl::InvalidArgumentError(
          absl::StrCat("Invalid output format: ", entry));
    }
  }

  // Prefer named arguments over positional ones
  std::string primary = absl::GetFlag(FLAGS_primary);
  std::string secondary = absl::GetFlag(FLAGS_secondary);
  {
    auto pos_it = positional.begin();
    auto pos_end = positional.end();
    if (primary.empty() && pos_it != pos_end) {
      primary = *pos_it++;
    }
    if (secondary.empty() && pos_it != pos_end) {
      secondary = *pos_it++;
    }
    if (pos_it != pos_end) {
      return absl::InvalidArgumentError("Extra arguments on command line");
    }
  }

  if (primary.empty()) {
    return absl::InvalidArgumentError("Need primary input (--primary)");
  }

  try {
    Timer<> timer;
    bool done_something = false;

    std::unique_ptr<CallGraph> call_graph1;
    std::unique_ptr<CallGraph> call_graph2;
    Instruction::Cache instruction_cache;
    FlowGraphs flow_graphs1;
    FlowGraphs flow_graphs2;
    ScopedCleanup cleanup(&flow_graphs1, &flow_graphs2, &instruction_cache);

    if (absl::GetFlag(FLAGS_output_dir) == current_path /* Defaulted */ &&
        IsDirectory(primary)) {
      absl::SetFlag(&FLAGS_output_dir, primary);
    }

    if (!IsDirectory(absl::GetFlag(FLAGS_output_dir))) {
      return absl::FailedPreconditionError(absl::StrCat(
          "Output parameter (--output_dir) must be a writable directory: ",
          absl::GetFlag(FLAGS_output_dir)));
    }

    if (FileExists(primary)) {
      // Primary from file system.
      FlowGraphInfos infos;
      call_graph1 = absl::make_unique<CallGraph>();
      Read(primary, call_graph1.get(), &flow_graphs1, &infos,
           &instruction_cache);
    }

    if (IsDirectory(primary)) {
      // File system batch diff.
      if (absl::GetFlag(FLAGS_ls)) {
        ListFiles(primary);
      } else if (absl::GetFlag(FLAGS_md_index)) {
        BatchDumpMdIndices(primary);
      } else {
        BatchDiff(primary, secondary, absl::GetFlag(FLAGS_output_dir));
      }
      done_something = true;
    }

    if (absl::GetFlag(FLAGS_md_index) && call_graph1 != nullptr) {
      DumpMdIndices(*call_graph1, flow_graphs1);
      done_something = true;
    }

    if (!secondary.empty() && FileExists(secondary)) {
      // secondary from filesystem
      FlowGraphInfos infos;
      call_graph2 = absl::make_unique<CallGraph>();
      Read(secondary, call_graph2.get(), &flow_graphs2, &infos,
           &instruction_cache);
    }

    if ((!done_something && !FileExists(primary) && !IsDirectory(primary)) ||
        (!secondary.empty() && !FileExists(secondary) &&
         !IsDirectory(secondary))) {
      return absl::FailedPreconditionError(
          "Invalid inputs, --primary and --secondary must point to valid "
          "files/directories.");
    }

    if (call_graph1.get() && call_graph2.get()) {
      const int edges1 = num_edges(call_graph1->GetGraph());
      const int vertices1 = num_vertices(call_graph1->GetGraph());
      const int edges2 = num_edges(call_graph2->GetGraph());
      const int vertices2 = num_vertices(call_graph2->GetGraph());
      PrintMessage(
          absl::StrCat("Setup: ", HumanReadableDuration(timer.elapsed())));
      PrintMessage(absl::StrCat("primary:   ", call_graph1->GetFilename(), ": ",
                                vertices1, " functions, ", edges1, " calls"));
      PrintMessage(absl::StrCat("secondary: ", call_graph2->GetFilename(), ": ",
                                vertices2, " functions, ", edges2, " calls"));
      timer.restart();

      const MatchingSteps default_callgraph_steps(GetDefaultMatchingSteps());
      const MatchingStepsFlowGraph default_basicblock_steps(
          GetDefaultMatchingStepsBasicBlock());
      FixedPoints fixed_points;
      MatchingContext context(*call_graph1, *call_graph2, flow_graphs1,
                              flow_graphs2, fixed_points);
      Diff(&context, default_callgraph_steps, default_basicblock_steps);

      Histogram histogram;
      Counts counts;
      GetCountsAndHistogram(flow_graphs1, flow_graphs2, fixed_points,
                            &histogram, &counts);
      Confidences confidences;
      const double confidence = GetConfidence(histogram, &confidences);
      const double similarity =
          GetSimilarityScore(*call_graph1, *call_graph2, histogram, counts);

      PrintMessage(
          absl::StrCat("Matching: ", HumanReadableDuration(timer.elapsed())));
      timer.restart();

      PrintMessage(absl::StrCat(
          "matched: ", fixed_points.size(), " of ", flow_graphs1.size(), "/",
          flow_graphs2.size(), " (primary/secondary, ",
          counts[Counts::kFunctionsPrimaryNonLibrary], "/",
          counts[Counts::kFunctionsSecondaryNonLibrary], " non-library)"));

      PrintMessage(absl::StrCat("call graph MD index: primary   ",
                                call_graph1->GetMdIndex()));
      PrintMessage(absl::StrCat("                     secondary ",
                                call_graph2->GetMdIndex()));
      PrintMessage(absl::StrCat("Similarity: ", similarity * 100,
                                "% (Confidence: ", confidence * 100, "%)"));

      ChainWriter writer;
      if (g_output_log) {
        writer.Add(std::make_shared<ResultsLogWriter>(GetTruncatedFilename(
            absl::GetFlag(FLAGS_output_dir) + kPathSeparator,
            call_graph1->GetFilename(), "_vs_", call_graph2->GetFilename(),
            ".results")));
      }
      if (g_output_binary || writer.IsEmpty()) {
        writer.Add(std::make_shared<DatabaseWriter>(GetTruncatedFilename(
            absl::GetFlag(FLAGS_output_dir) + kPathSeparator,
            call_graph1->GetFilename(), "_vs_", call_graph2->GetFilename(),
            ".BinDiff")));
      }

      if (!writer.IsEmpty()) {
        writer.Write(*call_graph1, *call_graph2, flow_graphs1, flow_graphs2,
                     fixed_points);
        PrintMessage(absl::StrCat("Writing results: ",
                                  HumanReadableDuration(timer.elapsed())));
      }
      timer.restart();
      done_something = true;
    }

    if (!done_something) {
      absl::flags_internal::FlagsHelp(
          std::cout, "", absl::flags_internal::HelpFormat::kHumanReadable,
          usage);
    }
  } catch (const std::exception& error) {
    return absl::UnknownError(error.what());
  } catch (...) {
    return absl::UnknownError("An unknown error occurred");
  }
  return absl::OkStatus();
}

}  // namespace security::bindiff

int main(int argc, char** argv) {
  if (auto status = security::bindiff::BinDiffMain(argc, argv); !status.ok()) {
    security::bindiff::PrintErrorMessage(
        absl::StrCat("Error: ", status.message()));
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
