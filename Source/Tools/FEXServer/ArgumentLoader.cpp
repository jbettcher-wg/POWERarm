// SPDX-License-Identifier: MIT
#include "ArgumentLoader.h"
#include "Common/cpp-optparse/OptionParser.h"
#include "PipeScanner.h"
#include "ProcessPipe.h"

#include "git_version.h"

#include <fmt/format.h>

namespace FEXServer::Config {
static fextl::string Version = "POWERarm (" GIT_DESCRIBE_STRING ") ";

FEXServerOptions Load(int argc, char** argv) {
  FEXServerOptions FEXOptions {};
  optparse::OptionParser Parser = optparse::OptionParser().version(Version);

  Parser.add_option("-k", "--kill").action("store_true").set_default(false).help("Shutdown an already active POWERarmServer");

  Parser.add_option("-f", "--foreground").action("store_true").set_default(false).help("Run this POWERarmServer in the foreground");

  Parser.add_option("-p", "--persistent").action("store").type("int").set_default(0).set_optional_value(true).metavar("n").help("Make POWERarmServer persistent. Optional number of seconds");

  Parser.add_option("-w", "--wait").action("store_true").set_default(false).help("Wait for the POWERarmServer to shutdown");
  Parser.add_option("--wait_pipe").action("store").type("int").set_default(-1).set_optional_value(true);
  Parser.add_option("--watch_fd").action("store").type("int").set_default(-1).set_optional_value(true).help("Adds FD to watch list of active processes");

  Parser.add_option("-v").action("version").help("Version string");

  optparse::Values Options = Parser.parse_args(argc, argv);

  FEXOptions.Kill = Options.get("kill");
  FEXOptions.Foreground = Options.get("foreground");
  FEXOptions.Wait = Options.get("wait");
  if (FEXOptions.Wait) {
    FEXOptions.Foreground = true;
  }
  FEXOptions.PersistentTimeout = Options.get("persistent");

  int WaitPipe = Options.get("wait_pipe");
  if (WaitPipe != -1) {
    PipeScanner::SetWaitPipe(WaitPipe);
  }
  ProcessPipe::SetWatchFD(Options.get("watch_fd"));
  return FEXOptions;
}
} // namespace FEXServer::Config
