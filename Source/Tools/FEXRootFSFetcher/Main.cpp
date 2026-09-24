// SPDX-License-Identifier: MIT
//
// POWERarmRootFSFetcher: build and select POWERarm's AArch64 guest rootfs.
//
// This tool used to be FEX's fetcher with the names swapped.  It read
// https://rootfs.fex-emu.gg/RootFS_links.json -- an index of x86_64 and i386 squashfs
// images -- and would happily install an Ubuntu x86_64 tree as the guest rootfs of an
// AArch64 emulator.  None of that is here any more.  The supported path builds a pinned
// Arch Linux ARM aarch64 tree with Scripts/powerarm/rootfs/alarm_sysroot.py, which does
// the sha256 pins, the OpenPGP signature check against the pinned Arch Linux ARM build
// key, the rootless extraction and the overlay bootstrap; this tool only drives it, checks
// the result really is AArch64, and writes the user's Config.json so the emulator finds it.
//
// It builds *two* pieces, because that is what a usable guest is here: the pinned base, and
// the per-user "<base>-overlay" with guest pacman bootstrapped into it.  A base alone is a
// sysroot -- everything a GUI program needs (libxkbcommon, dbus, the fonts, the application)
// is installed into the overlay by guest pacman afterwards.
//
// Why a separate binary rather than a `rootfs` subcommand on POWERarm: POWERarm's argv[1]
// is a *guest program path*, handed over by the shell or by binfmt_misc.  A subcommand
// there would shadow any guest program called "rootfs", in the emulator's own entry path.
// POWERarmConfig, POWERarmGetConfig and POWERarmBash are already separate tools, so this
// is the established shape, and the name is the one already installed and documented.
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include "Common/cpp-optparse/OptionParser.h"
#include "Common/Config.h"
#include "Common/RootFSCheck.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// ---------------------------------------------------------------- options

struct Options {
  fextl::string Manifest {"vk"};
  fextl::string Dest {};
  fextl::string Cache {};
  fextl::string Scripts {};
  fextl::string Emulator {};
  fextl::vector<fextl::string> Mirrors {};
  bool AssumeYes {false};
  bool Force {false};
  bool VerifySignatures {true};
  bool SetDefault {true};
  bool Align {true};
  bool DryRun {false};
  // Tri-state: an unset overlay follows the manifest (on, except for the M2 pin, whose
  // byte-compare gates are keyed to the bare base).
  std::optional<bool> Overlay {};
  bool PacmanInit {true};
};

Options Opts;

// The shipped manifests.  "vk" is the default because it is the shape every desktop app on
// this project is tested against: the toolchain roots plus Mesa/RADV and the X11/Wayland
// libraries underneath them.  A base alone cannot run a GUI program; what the apps actually
// need arrives through guest pacman in the per-user overlay, which is why `build` produces
// base *and* overlay.  "m2" is the toolchain-only pin, and its byte-compare gates are keyed
// to the bare base, so it does not get an overlay unless asked.
struct KnownManifest {
  const char* Alias;
  const char* File;
  const char* RootFSName;
  const char* Summary;
  bool OverlayByDefault;
};

constexpr std::array<KnownManifest, 2> KnownManifests {{
  {"vk", "alarm-vk.manifest", "ArchLinuxARM-vk", "the toolchain roots plus Vulkan/RADV and the desktop libraries -- the default", true},
  {"m2", "alarm-m2.manifest", "ArchLinuxARM-m2", "the M2 GCC toolchain sysroot (its gates are keyed to the bare base)", false},
}};

// ---------------------------------------------------------------- small helpers

bool Exists(const fextl::string& Path) {
  struct stat Buffer {};
  return ::stat(Path.c_str(), &Buffer) == 0;
}

bool IsDirectory(const fextl::string& Path) {
  struct stat Buffer {};
  return ::stat(Path.c_str(), &Buffer) == 0 && S_ISDIR(Buffer.st_mode);
}

bool DirectoryIsEmpty(const fextl::string& Path) {
  DIR* Dir = ::opendir(Path.c_str());
  if (!Dir) {
    return true;
  }
  bool Empty = true;
  while (const auto* Entry = ::readdir(Dir)) {
    if (std::strcmp(Entry->d_name, ".") == 0 || std::strcmp(Entry->d_name, "..") == 0) {
      continue;
    }
    Empty = false;
    break;
  }
  ::closedir(Dir);
  return Empty;
}

fextl::string ParentOf(const fextl::string& Path) {
  const auto Slash = Path.find_last_of('/');
  if (Slash == fextl::string::npos) {
    return ".";
  }
  return Slash == 0 ? fextl::string {"/"} : Path.substr(0, Slash);
}

fextl::string BaseNameOf(const fextl::string& Path) {
  auto Trimmed = Path;
  while (Trimmed.size() > 1 && Trimmed.back() == '/') {
    Trimmed.pop_back();
  }
  const auto Slash = Trimmed.find_last_of('/');
  return Slash == fextl::string::npos ? Trimmed : Trimmed.substr(Slash + 1);
}

fextl::string EnvOr(const char* Name, const fextl::string& Fallback) {
  const char* Value = ::getenv(Name);
  return (Value && Value[0]) ? fextl::string {Value} : Fallback;
}

fextl::string HomeDirectory() {
  return FEX::Config::GetHomeDirectory();
}

bool MakeDirectories(const fextl::string& Path) {
  if (Path.empty() || Path == "/") {
    return true;
  }
  if (IsDirectory(Path)) {
    return true;
  }
  if (!MakeDirectories(ParentOf(Path))) {
    return false;
  }
  return ::mkdir(Path.c_str(), 0755) == 0 || errno == EEXIST;
}

fextl::string HumanBytes(uint64_t Bytes) {
  if (Bytes >= 1024ULL * 1024 * 1024) {
    return fextl::fmt::format("{:.1f} GB", static_cast<double>(Bytes) / (1024.0 * 1024 * 1024));
  }
  if (Bytes >= 1024ULL * 1024) {
    return fextl::fmt::format("{:.0f} MB", static_cast<double>(Bytes) / (1024.0 * 1024));
  }
  if (Bytes >= 1024) {
    return fextl::fmt::format("{:.0f} KB", static_cast<double>(Bytes) / 1024.0);
  }
  return fextl::fmt::format("{} B", Bytes);
}

bool AskYesNo(const fextl::string& Question) {
  if (Opts.AssumeYes) {
    return true;
  }
  if (!::isatty(STDIN_FILENO)) {
    fextl::fmt::print(stderr, "{} -- stdin is not a terminal; pass -y to proceed.\n", Question);
    return false;
  }
  for (;;) {
    fextl::fmt::print("{} [y/N] ", Question);
    std::fflush(stdout);
    std::string Line;
    if (!std::getline(std::cin, Line)) {
      fextl::fmt::print("\n");
      return false;
    }
    if (Line.empty() || Line == "n" || Line == "N" || Line == "no") {
      return false;
    }
    if (Line == "y" || Line == "Y" || Line == "yes") {
      return true;
    }
  }
}

// ---------------------------------------------------------------- running the builder

// Runs the child with stdout/stderr inherited, so alarm_sysroot.py's progress and its
// verification failures land in front of the user unfiltered.  `Env` entries are set in the
// child only: the guest pacman steps need POWERARM_PORTABLE and POWERARM_ROOTFS, and neither
// belongs in this process.
int RunCommand(const fextl::vector<fextl::string>& Args, const fextl::vector<std::pair<fextl::string, fextl::string>>& Env = {}) {
  fextl::vector<const char*> Argv;
  Argv.reserve(Args.size() + 1);
  for (const auto& Arg : Args) {
    Argv.emplace_back(Arg.c_str());
  }
  Argv.emplace_back(nullptr);

  const pid_t Pid = ::fork();
  if (Pid == -1) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: fork failed: {}\n", std::strerror(errno));
    return -1;
  }
  if (Pid == 0) {
    for (const auto& [Name, Value] : Env) {
      ::setenv(Name.c_str(), Value.c_str(), 1);
    }
    ::execvp(Argv[0], const_cast<char* const*>(Argv.data()));
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: cannot run {}: {}\n", Argv[0], std::strerror(errno));
    ::_exit(127);
  }

  int Status {};
  while (::waitpid(Pid, &Status, 0) == -1) {
    if (errno != EINTR) {
      return -1;
    }
  }
  if (WIFEXITED(Status)) {
    return WEXITSTATUS(Status);
  }
  if (WIFSIGNALED(Status)) {
    return 128 + WTERMSIG(Status);
  }
  return -1;
}

fextl::string Quoted(const fextl::string& Arg) {
  if (Arg.find_first_of(" \t\"'$") == fextl::string::npos) {
    return Arg;
  }
  return "'" + Arg + "'";
}

void PrintCommand(const fextl::vector<fextl::string>& Args, const fextl::vector<std::pair<fextl::string, fextl::string>>& Env = {}) {
  fextl::string Line;
  auto Append = [&Line](const fextl::string& Text) {
    if (!Line.empty()) {
      Line += " ";
    }
    Line += Text;
  };
  for (const auto& [Name, Value] : Env) {
    Append(Name + "=" + Quoted(Value));
  }
  for (const auto& Arg : Args) {
    Append(Quoted(Arg));
  }
  fextl::fmt::print("+ {}\n", Line);
}

// ---------------------------------------------------------------- locating the builder

// Where alarm_sysroot.py and the manifests live.  Checked in order:
//   1. --scripts / POWERARM_ROOTFS_SCRIPTS
//   2. next to the running binary in a build tree (<bindir>/../../Scripts/powerarm/rootfs)
//   3. next to the running binary when installed (<bindir>/../share/<dir>/rootfs)
//   4. the configured install prefix, then the source tree this binary was built from
fextl::string SelfDirectory() {
  fextl::string Buffer;
  Buffer.resize(PATH_MAX);
  const auto Read = ::readlink("/proc/self/exe", Buffer.data(), Buffer.size());
  if (Read <= 0) {
    return {};
  }
  Buffer.resize(Read);
  return ParentOf(Buffer);
}

std::optional<fextl::string> FindScriptsDirectory() {
  fextl::vector<fextl::string> Candidates;

  if (!Opts.Scripts.empty()) {
    // An explicit request is never silently replaced by a fallback.
    if (Exists(Opts.Scripts + "/alarm_sysroot.py")) {
      return Opts.Scripts;
    }
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: no alarm_sysroot.py under '{}'.\n", Opts.Scripts);
    return std::nullopt;
  }

  const auto SelfDir = SelfDirectory();
  if (!SelfDir.empty()) {
    Candidates.emplace_back(SelfDir + "/../../Scripts/powerarm/rootfs");
    Candidates.emplace_back(SelfDir + "/../share/" POWERARM_DIR_NAME "/rootfs");
    Candidates.emplace_back(SelfDir + "/../Scripts/powerarm/rootfs");
  }
  Candidates.emplace_back(GLOBAL_DATA_DIRECTORY "rootfs");
  Candidates.emplace_back("/usr/share/" POWERARM_DIR_NAME "/rootfs");
  Candidates.emplace_back(POWERARM_SOURCE_DIR "/Scripts/powerarm/rootfs");

  for (const auto& Candidate : Candidates) {
    if (Exists(Candidate + "/alarm_sysroot.py")) {
      return Candidate;
    }
  }

  fextl::fmt::print(stderr, "POWERarmRootFSFetcher: could not find alarm_sysroot.py. Looked in:\n");
  for (const auto& Candidate : Candidates) {
    fextl::fmt::print(stderr, "  {}\n", Candidate);
  }
  fextl::fmt::print(stderr, "Point --scripts (or POWERARM_ROOTFS_SCRIPTS) at Scripts/powerarm/rootfs.\n");
  return std::nullopt;
}

struct PickedManifest {
  fextl::string Path;
  fextl::string DefaultRootFSName;
  fextl::string Summary;
  fextl::string Alias {};
  bool OverlayByDefault {true};
};

std::optional<PickedManifest> PickManifest(const fextl::string& ScriptsDir) {
  for (const auto& Known : KnownManifests) {
    if (Opts.Manifest == Known.Alias) {
      PickedManifest Picked;
      Picked.Path = ScriptsDir + "/" + Known.File;
      Picked.DefaultRootFSName = Known.RootFSName;
      Picked.Summary = Known.Summary;
      Picked.OverlayByDefault = Known.OverlayByDefault;
      Picked.Alias = Known.Alias;
      if (!Exists(Picked.Path)) {
        fextl::fmt::print(stderr, "POWERarmRootFSFetcher: manifest '{}' is missing from '{}'.\n", Known.File, ScriptsDir);
        return std::nullopt;
      }
      return Picked;
    }
  }

  // Anything else is taken as a path to a manifest.
  if (!Exists(Opts.Manifest)) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: unknown manifest '{}'. Known names:", Opts.Manifest);
    for (const auto& Known : KnownManifests) {
      fextl::fmt::print(stderr, " {}", Known.Alias);
    }
    fextl::fmt::print(stderr, "; or pass a path to a manifest file.\n");
    return std::nullopt;
  }

  PickedManifest Picked;
  Picked.Path = Opts.Manifest;
  // "alarm-foo.manifest" -> "ArchLinuxARM-foo"
  auto Stem = BaseNameOf(Opts.Manifest);
  if (Stem.size() > 9 && Stem.compare(Stem.size() - 9, 9, ".manifest") == 0) {
    Stem.resize(Stem.size() - 9);
  }
  if (Stem.compare(0, 6, "alarm-") == 0) {
    Stem = Stem.substr(6);
  }
  Picked.DefaultRootFSName = "ArchLinuxARM-" + Stem;
  Picked.Summary = "custom manifest";
  return Picked;
}

// What the pins add up to, for the confirmation prompt.  Parsing failures are not fatal:
// alarm_sysroot.py is the authority on the manifest, this is only a preview.
struct ManifestSummary {
  size_t Packages {0};
  uint64_t Download {0};
  uint64_t Installed {0};
  fextl::string Mirror {};
  fextl::string Arch {};
};

ManifestSummary SummariseManifest(const fextl::string& Path) {
  ManifestSummary Summary;
  std::ifstream File(Path.c_str());
  std::string Line;
  while (std::getline(File, Line)) {
    std::istringstream Stream(Line);
    std::string Key;
    if (!(Stream >> Key) || Key.empty() || Key[0] == '#') {
      continue;
    }
    if (Key == "mirror") {
      std::string Value;
      Stream >> Value;
      Summary.Mirror = Value.c_str();
    } else if (Key == "arch") {
      std::string Value;
      Stream >> Value;
      Summary.Arch = Value.c_str();
    } else if (Key == "pkg" || Key == "keyring") {
      std::string Name, Version, Repo, FileName, Sha;
      uint64_t CSize {}, ISize {};
      if (Stream >> Name >> Version >> Repo >> FileName >> Sha >> CSize >> ISize) {
        if (Key == "pkg") {
          ++Summary.Packages;
        }
        Summary.Download += CSize;
        Summary.Installed += ISize;
      }
    }
  }
  return Summary;
}

// ---------------------------------------------------------------- config writing

// The layering, as FEXCore::Config::LoadOrder has it, is
//   LAYER_GLOBAL_MAIN < LAYER_MAIN < ... < LAYER_ENVIRONMENT
// so the global file (GetConfigFileLocation(true)) is the *lowest* layer and the user's
// own file (GetConfigFileLocation(false)) overrides it.  This only ever writes the user's
// file, and says so when a global layer is also setting RootFS.
fextl::string GlobalRootFSSetting() {
  auto Global = FEX::Config::CreateGlobalMainLayer();
  Global->Load();
  auto Value = Global->Get(FEXCore::Config::ConfigOption::CONFIG_ROOTFS);
  if (Value && *Value) {
    return **Value;
  }
  return {};
}

bool SetUserDefaultRootFS(const fextl::string& Value) {
  const fextl::string Filename = FEXCore::Config::GetConfigFileLocation(false);
  const auto Directory = ParentOf(Filename);
  if (!MakeDirectories(Directory)) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: cannot create config directory '{}': {}\n", Directory, std::strerror(errno));
    return false;
  }

  auto Layer = FEX::Config::CreateMainLayer(&Filename);
  // Load() first so every other key in the user's file survives the write.
  Layer->Load();
  Layer->Set(FEXCore::Config::ConfigOption::CONFIG_ROOTFS, Value);
  FEX::Config::SaveLayerToJSON(Filename, Layer.get());

  // SaveLayerToJSON is silent when the file cannot be opened, so confirm the write.
  std::ifstream Check(Filename.c_str());
  if (!Check.is_open()) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: failed to write '{}'.\n", Filename);
    return false;
  }
  fextl::fmt::print("RootFS \"{}\" written to {}\n", Value, Filename);

  const auto Global = GlobalRootFSSetting();
  if (!Global.empty() && Global != Value) {
    fextl::fmt::print("note: the global config ({}) sets RootFS \"{}\"; the user file above is the higher layer and wins.\n",
                      FEXCore::Config::GetConfigFileLocation(true), Global);
  }
  if (::getenv(POWERARM_ENV_PREFIX "ROOTFS")) {
    fextl::fmt::print("note: " POWERARM_ENV_PREFIX "ROOTFS is set in this environment and overrides both config files.\n");
  }
  return true;
}

// A name resolves under <datadir>/RootFS/, which is nicer to read and survives the tree
// being moved with the data directory -- but POWERARM_PORTABLE redirects the data
// directory, so a name cannot be resolved there.  Prefer the name only when the tree
// really is the named one, and only when portable mode is not in play.
fextl::string ConfigValueFor(const fextl::string& Dest, const fextl::string& Name) {
  const bool Portable = ::getenv(POWERARM_ENV_PREFIX "PORTABLE") != nullptr;
  const fextl::string NamedPath = FEXCore::Config::GetDataDirectory(false) + "RootFS/" + Name;
  if (!Portable && Dest == NamedPath) {
    return Name;
  }
  return Dest;
}

// ---------------------------------------------------------------- reporting a tree

const char* VerdictWord(FEX::RootFSCheck::Verdict V) {
  switch (V) {
  case FEX::RootFSCheck::Verdict::OK: return "ok";
  case FEX::RootFSCheck::Verdict::UNVERIFIED: return "unverified";
  case FEX::RootFSCheck::Verdict::MISSING: return "missing";
  case FEX::RootFSCheck::Verdict::EMPTY: return "empty";
  case FEX::RootFSCheck::Verdict::WRONG_MACHINE: return "wrong-architecture";
  case FEX::RootFSCheck::Verdict::NOT_A_DIRECTORY: return "not-a-directory";
  }
  return "?";
}

// ---------------------------------------------------------------- the overlay

// Guest pacman runs *inside* the emulator, so the tool needs the emulator binary.  It sits
// next to this one in both a build tree and an install.
std::optional<fextl::string> FindEmulator() {
  if (!Opts.Emulator.empty()) {
    if (Exists(Opts.Emulator)) {
      return Opts.Emulator;
    }
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: no emulator at '{}'.\n", Opts.Emulator);
    return std::nullopt;
  }
  const auto SelfDir = SelfDirectory();
  if (!SelfDir.empty() && Exists(SelfDir + "/" POWERARM_EXE_PREFIX)) {
    return SelfDir + "/" POWERARM_EXE_PREFIX;
  }
  // Last resort: let execvp find it on PATH.
  return fextl::string {POWERARM_EXE_PREFIX};
}

// The per-user writable layer.  The emulator picks up "<rootfs>-overlay" on its own whenever
// that directory exists (DESIGN 6.2a.1), so nothing else has to be configured -- but a base
// with no overlay is a sysroot, not a usable desktop guest: everything a GUI app needs
// (libxkbcommon, dbus, the fonts, the app itself) arrives through guest pacman, and guest
// pacman needs the local package database and the keyring that overlay-init writes.
//
// The pacman dance is the README's recipe: pacman and pacman-key insist on uid 0, so they run
// as root of a user namespace (no privilege), and POWERARM_PORTABLE=1 is required there or
// the emulator looks for its server socket under the namespace's uid 0 and fails.
bool BuildOverlay(const fextl::string& ScriptsDir, const fextl::string& ManifestPath, const fextl::string& Base, const fextl::string& Cache) {
  const fextl::string Overlay = Base + "-overlay";

  if (Exists(Overlay) && !DirectoryIsEmpty(Overlay)) {
    fextl::fmt::print("\noverlay {} already exists and is not empty; leaving it alone.\n", Overlay);
    return true;
  }
  if (!MakeDirectories(Overlay)) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: cannot create '{}': {}\n", Overlay, std::strerror(errno));
    return false;
  }

  fextl::vector<fextl::string> Args {
    "python3", ScriptsDir + "/alarm_sysroot.py", "overlay-init", "--dest", Overlay, "--base", Base, "--manifest", ManifestPath,
    "--cache", Cache, "--with-pacman", "--package", "pacman", "--package", "archlinuxarm-keyring",
  };
  for (const auto& Mirror : Opts.Mirrors) {
    Args.emplace_back("--mirror");
    Args.emplace_back(Mirror);
  }
  if (!Opts.VerifySignatures) {
    Args.emplace_back("--no-verify-signatures");
  }

  fextl::fmt::print("\n--- per-user overlay: {}\n", Overlay);
  PrintCommand(Args);
  const int Result = RunCommand(Args);
  if (Result != 0) {
    fextl::fmt::print(stderr, "\nPOWERarmRootFSFetcher: overlay-init failed (exit {}). The base is fine; the overlay is not.\n", Result);
    return false;
  }
  return true;
}

// pacman-key --init, --populate and pacman -Sy, in the sealed user namespace.  A failure here
// is reported and does not undo the rootfs: the base and the overlay are both usable, the
// guest just cannot install packages yet, and the exact commands are printed to retry by hand.
bool InitGuestPacman(const fextl::string& Base) {
  const auto Emulator = FindEmulator();
  if (!Emulator) {
    return false;
  }

  const fextl::vector<std::pair<fextl::string, fextl::string>> Env {
    {POWERARM_ENV_PREFIX "PORTABLE", "1"},
    {POWERARM_ENV_PREFIX "ROOTFS", Base},
  };

  const std::array<fextl::vector<fextl::string>, 3> Steps {{
    {"unshare", "-r", *Emulator, "/usr/bin/pacman-key", "--init"},
    {"unshare", "-r", *Emulator, "/usr/bin/pacman-key", "--populate", "archlinuxarm"},
    {"unshare", "-r", *Emulator, "/usr/bin/pacman", "-Sy"},
  }};

  fextl::fmt::print("\n--- guest pacman\n");
  for (const auto& Step : Steps) {
    PrintCommand(Step, Env);
    const int Result = RunCommand(Step, Env);
    if (Result != 0) {
      fextl::fmt::print(stderr, "\nPOWERarmRootFSFetcher: that step exited {}. The rootfs and the overlay are in place; "
                                "guest pacman is not initialised yet. Re-run the three commands above by hand "
                                "(they need a user namespace: 'unshare -r' must be permitted).\n",
                        Result);
      return false;
    }
  }
  return true;
}

// ---------------------------------------------------------------- subcommands

int CommandBuild(const fextl::string& NameArgument) {
  const auto ScriptsDir = FindScriptsDirectory();
  if (!ScriptsDir) {
    return 1;
  }
  const auto Picked = PickManifest(*ScriptsDir);
  if (!Picked) {
    return 1;
  }

  const fextl::string Name = NameArgument.empty() ? Picked->DefaultRootFSName : NameArgument;
  const bool WantOverlay = Opts.Overlay.value_or(Picked->OverlayByDefault);
  fextl::string Dest = Opts.Dest;
  if (Dest.empty()) {
    Dest = FEXCore::Config::GetDataDirectory(false) + "RootFS/" + Name;
  }

  fextl::string Cache = Opts.Cache;
  if (Cache.empty()) {
    Cache = EnvOr("XDG_CACHE_HOME", HomeDirectory() + "/.cache") + "/powerarm/alarm-pkgs";
  }

  const auto Summary = SummariseManifest(Picked->Path);
  if (!Summary.Arch.empty() && Summary.Arch != "aarch64") {
    fextl::fmt::print(stderr,
                      "POWERarmRootFSFetcher: manifest '{}' is for arch '{}'. POWERarm runs AArch64 "
                      "guests; refusing.\n",
                      Picked->Path, Summary.Arch);
    return 1;
  }

  fextl::fmt::print("POWERarm rootfs build\n");
  fextl::fmt::print("  manifest    {} ({})\n", Picked->Path, Picked->Summary);
  fextl::fmt::print("  packages    {} pinned, {} to download, {} installed\n", Summary.Packages, HumanBytes(Summary.Download),
                    HumanBytes(Summary.Installed));
  fextl::fmt::print("  mirror      {}\n", Opts.Mirrors.empty() ? Summary.Mirror : Opts.Mirrors.front());
  for (size_t i = 1; i < Opts.Mirrors.size(); ++i) {
    fextl::fmt::print("              {} (fallback {})\n", Opts.Mirrors[i], i);
  }
  fextl::fmt::print("  signatures  {}\n", Opts.VerifySignatures ? "verified against the pinned Arch Linux ARM key" :
                                                                 "NOT VERIFIED (--no-verify-signatures): sha256 pins only");
  fextl::fmt::print("  cache       {}\n", Cache);
  fextl::fmt::print("  destination {}\n", Dest);
  if (WantOverlay) {
    fextl::fmt::print("  overlay     {}-overlay, with guest pacman{}\n", Dest,
                      Opts.PacmanInit ? " (pacman-key --init, --populate, pacman -Sy)" : " (--no-pacman-init: not initialised)");
  } else if (Picked->Alias == "m2" && !Opts.Overlay.has_value()) {
    fextl::fmt::print("  overlay     none: the M2 pin's byte-compare gates are keyed to the bare base "
                      "(pass --overlay to add one anyway)\n");
  } else {
    fextl::fmt::print("  overlay     none (--no-overlay)\n");
  }

  if (Exists(Dest) && !DirectoryIsEmpty(Dest) && !Opts.Force) {
    fextl::fmt::print(stderr,
                      "\nPOWERarmRootFSFetcher: '{}' already exists and is not empty. Pass --force to replace it, "
                      "or --dest/NAME to build elsewhere.\n",
                      Dest);
    fextl::fmt::print(stderr, "Nothing was touched.\n");
    return 1;
  }

  if (Opts.DryRun) {
    fextl::fmt::print("\n(--dry-run: stopping here)\n");
    return 0;
  }

  if (!AskYesNo("\nBuild it?")) {
    fextl::fmt::print("Nothing was touched.\n");
    return 1;
  }

  if (!MakeDirectories(ParentOf(Dest)) || !MakeDirectories(Cache)) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: cannot create '{}' or '{}': {}\n", ParentOf(Dest), Cache, std::strerror(errno));
    return 1;
  }

  const fextl::string Contents = ParentOf(Cache) + "/" + BaseNameOf(Dest) + ".contents";

  fextl::vector<fextl::string> Args {
    "python3", *ScriptsDir + "/alarm_sysroot.py", "extract", "--manifest", Picked->Path, "--cache", Cache, "--dest", Dest,
    "--contents-out", Contents,
  };
  for (const auto& Mirror : Opts.Mirrors) {
    Args.emplace_back("--mirror");
    Args.emplace_back(Mirror);
  }
  if (!Opts.VerifySignatures) {
    Args.emplace_back("--no-verify-signatures");
  }
  if (Opts.Force) {
    Args.emplace_back("--force");
  }

  fextl::fmt::print("\n");
  PrintCommand(Args);
  const int Result = RunCommand(Args);
  if (Result != 0) {
    fextl::fmt::print(stderr, "\nPOWERarmRootFSFetcher: alarm_sysroot.py extract failed (exit {}). RootFS not changed in the config.\n", Result);
    return Result == -1 ? 1 : Result;
  }

  // The builder succeeded, but "succeeded" is not the same as "usable by this emulator":
  // check what actually landed rather than assume it.
  const auto Verdict = FEX::RootFSCheck::Check(Dest);
  if (FEX::RootFSCheck::IsFatal(Verdict.Verdict)) {
    fextl::fmt::print(stderr, "\nPOWERarmRootFSFetcher: the built tree is not usable: {}.\n", Verdict.Reason);
    fextl::fmt::print(stderr, "RootFS not changed in the config.\n");
    return 1;
  }
  if (Verdict.Verdict == FEX::RootFSCheck::Verdict::UNVERIFIED) {
    fextl::fmt::print("\nwarning: {}.\n", Verdict.Reason);
  } else {
    fextl::fmt::print("\nverified AArch64 ({} is AArch64)\n", Verdict.Probe);
  }
  fextl::fmt::print("contents listing {}\n", Contents);

  if (Opts.Align) {
    fextl::vector<fextl::string> AlignArgs {"python3", *ScriptsDir + "/alarm_sysroot.py", "align", Dest};
    fextl::fmt::print("\n--- PT_LOAD p_align\n");
    RunCommand(AlignArgs);
  }

  // The base is the read-only half.  Neither of the next two stages can damage it, and a
  // failure in either still leaves a working (if uninstallable-into) guest, so they report
  // and carry on to the config write rather than throwing the build away.
  bool OverlayOK = true;
  bool PacmanOK = true;
  if (WantOverlay) {
    OverlayOK = BuildOverlay(*ScriptsDir, Picked->Path, Dest, Cache);
    if (OverlayOK && Opts.PacmanInit) {
      PacmanOK = InitGuestPacman(Dest);
    }
  }

  if (Opts.SetDefault) {
    const auto Value = ConfigValueFor(Dest, Name);
    if (!SetUserDefaultRootFS(Value)) {
      return 1;
    }
  } else {
    fextl::fmt::print("\n(--no-set-default) Select it with " POWERARM_ENV_PREFIX "ROOTFS={} or "
                      "'{}RootFSFetcher default {}'.\n",
                      Name, POWERARM_EXE_PREFIX, Name);
  }

  fextl::fmt::print("\nDone. Run a guest program with: {} /usr/bin/uname -m\n", POWERARM_EXE_PREFIX);
  if (WantOverlay && OverlayOK && PacmanOK) {
    fextl::fmt::print("Install into the guest with: " POWERARM_ENV_PREFIX "PORTABLE=1 " POWERARM_ENV_PREFIX
                      "ROOTFS={} unshare -r {} /usr/bin/pacman -S <package>\n",
                      Name, POWERARM_EXE_PREFIX);
  }
  return (OverlayOK && PacmanOK) ? 0 : 1;
}

// Everything the emulator would search for a named rootfs, in the same order
// FEXCore::Config::ReloadMetaLayer walks it.
fextl::vector<fextl::string> RootFSSearchDirectories() {
  fextl::vector<fextl::string> Dirs;
  for (bool Global : {false, true}) {
    Dirs.emplace_back(FEXCore::Config::GetDataDirectory(Global) + "RootFS/");
    Dirs.emplace_back(FEXCore::Config::GetConfigDirectory(Global) + "RootFS/");
  }
  return Dirs;
}

int CommandList() {
  const auto Configured = FEXCore::Config::Get(FEXCore::Config::CONFIG_ROOTFS);
  const fextl::string ConfiguredPath = (Configured && *Configured) ? **Configured : fextl::string {};

  fextl::vector<fextl::string> Seen;
  size_t Count = 0;
  for (const auto& Dir : RootFSSearchDirectories()) {
    DIR* Handle = ::opendir(Dir.c_str());
    if (!Handle) {
      continue;
    }
    fextl::vector<fextl::string> Entries;
    while (const auto* Entry = ::readdir(Handle)) {
      if (Entry->d_name[0] == '.') {
        continue;
      }
      Entries.emplace_back(Entry->d_name);
    }
    ::closedir(Handle);
    std::sort(Entries.begin(), Entries.end());

    for (const auto& Entry : Entries) {
      // "<name>-overlay" is the writable layer of <name>, not a rootfs of its own.
      if (Entry.size() > 8 && Entry.compare(Entry.size() - 8, 8, "-overlay") == 0) {
        continue;
      }
      const fextl::string Path = Dir + Entry;
      if (std::find(Seen.begin(), Seen.end(), Path) != Seen.end()) {
        continue;
      }
      Seen.emplace_back(Path);

      const auto Verdict = FEX::RootFSCheck::Check(Path);
      const bool Active = !ConfiguredPath.empty() && (ConfiguredPath == Path || ConfiguredPath == Entry);
      const auto* Machine = FEX::RootFSCheck::MachineName(Verdict.Machine);
      const fextl::string MachineText = Machine     ? fextl::string {Machine} :
                                        Verdict.Machine ? fextl::fmt::format("e_machine {}", Verdict.Machine) :
                                                          fextl::string {"-"};
      fextl::fmt::print("{} {:<24} {:<18} {}{}\n", Active ? "*" : " ", Entry.c_str(), VerdictWord(Verdict.Verdict),
                        MachineText.c_str(), IsDirectory(Path + "-overlay") ? "  +overlay" : "");
      if (FEX::RootFSCheck::IsFatal(Verdict.Verdict) || Verdict.Verdict == FEX::RootFSCheck::Verdict::UNVERIFIED) {
        fextl::fmt::print("    {}\n", Verdict.Reason);
      }
      ++Count;
    }
  }

  if (Count == 0) {
    fextl::fmt::print("No rootfs found under:\n");
    for (const auto& Dir : RootFSSearchDirectories()) {
      fextl::fmt::print("  {}\n", Dir);
    }
    fextl::fmt::print("Build one with '{}RootFSFetcher build'.\n", POWERARM_EXE_PREFIX);
    return 1;
  }

  fextl::fmt::print("\n'*' is the one the current configuration resolves to");
  if (!ConfiguredPath.empty()) {
    fextl::fmt::print(" ({})", ConfiguredPath);
  }
  fextl::fmt::print(".\n");
  return 0;
}

// Resolves a name the way the emulator does, so `check` answers for the tree that would
// actually be used and not for a lookalike.
fextl::string ResolveNameOrPath(const fextl::string& NameOrPath) {
  if (NameOrPath.find('/') != fextl::string::npos) {
    return NameOrPath;
  }
  for (const auto& Dir : RootFSSearchDirectories()) {
    const fextl::string Candidate = Dir + NameOrPath;
    if (Exists(Candidate)) {
      return Candidate;
    }
  }
  return NameOrPath;
}

int CommandCheck(const fextl::string& NameArgument) {
  fextl::string Target = NameArgument;
  if (Target.empty()) {
    const auto Configured = FEXCore::Config::Get(FEXCore::Config::CONFIG_ROOTFS);
    if (!Configured || !*Configured || (*Configured)->empty()) {
      fextl::fmt::print(stderr, "POWERarmRootFSFetcher: no RootFS is configured and none was named.\n");
      fextl::fmt::print(stderr, "Build one with '{}RootFSFetcher build'.\n", POWERARM_EXE_PREFIX);
      return 1;
    }
    Target = **Configured;
  } else {
    Target = ResolveNameOrPath(Target);
  }

  const auto Verdict = FEX::RootFSCheck::Check(Target);
  fextl::fmt::print("{}\n", Target);
  fextl::fmt::print("  verdict  {}\n", VerdictWord(Verdict.Verdict));
  if (Verdict.Machine) {
    const auto* Machine = FEX::RootFSCheck::MachineName(Verdict.Machine);
    fextl::fmt::print("  machine  {} (from {})\n", Machine ? Machine : "unknown", Verdict.Probe);
  }
  if (IsDirectory(Target + "-overlay")) {
    fextl::fmt::print("  overlay  {}-overlay\n", Target);
  }
  if (!Verdict.Reason.empty()) {
    fextl::fmt::print("  {}\n", Verdict.Reason);
  }
  if (FEX::RootFSCheck::IsFatal(Verdict.Verdict)) {
    return 1;
  }
  return 0;
}

// Adding the overlay to a base that already exists: the second half of `build`, on its own.
// Useful when a base was built by build-alarm-sysroot.sh, when the pacman steps were skipped,
// and when the M2 pin is deliberately given an overlay after its gates have run.
int CommandOverlay(const fextl::string& NameArgument) {
  const auto ScriptsDir = FindScriptsDirectory();
  if (!ScriptsDir) {
    return 1;
  }

  fextl::string Base = NameArgument;
  if (Base.empty()) {
    const auto Configured = FEXCore::Config::Get(FEXCore::Config::CONFIG_ROOTFS);
    if (!Configured || !*Configured || (*Configured)->empty()) {
      fextl::fmt::print(stderr, "POWERarmRootFSFetcher: no RootFS is configured and none was named.\n");
      return 1;
    }
    Base = **Configured;
  } else {
    Base = ResolveNameOrPath(Base);
  }

  const auto Verdict = FEX::RootFSCheck::Check(Base);
  if (FEX::RootFSCheck::IsFatal(Verdict.Verdict)) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: {}.\n", Verdict.Reason);
    return 1;
  }

  const auto Picked = PickManifest(*ScriptsDir);
  if (!Picked) {
    return 1;
  }
  fextl::string Cache = Opts.Cache;
  if (Cache.empty()) {
    Cache = EnvOr("XDG_CACHE_HOME", HomeDirectory() + "/.cache") + "/powerarm/alarm-pkgs";
  }

  fextl::fmt::print("base     {}\n", Base);
  fextl::fmt::print("overlay  {}-overlay\n", Base);
  fextl::fmt::print("manifest {} (must be the one that built the base)\n", Picked->Path);
  if (!Opts.DryRun && !AskYesNo("\nAn overlay changes what every guest using this rootfs sees. Create it?")) {
    fextl::fmt::print("Nothing was touched.\n");
    return 1;
  }
  if (Opts.DryRun) {
    fextl::fmt::print("\n(--dry-run: stopping here)\n");
    return 0;
  }

  if (!BuildOverlay(*ScriptsDir, Picked->Path, Base, Cache)) {
    return 1;
  }
  if (Opts.PacmanInit && !InitGuestPacman(Base)) {
    return 1;
  }
  fextl::fmt::print("\nDone.\n");
  return 0;
}

int CommandDefault(const fextl::string& NameArgument) {
  if (NameArgument.empty()) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: 'default' needs a rootfs name or path.\n");
    return 1;
  }

  const auto Resolved = ResolveNameOrPath(NameArgument);
  const auto Verdict = FEX::RootFSCheck::Check(Resolved);
  if (FEX::RootFSCheck::IsFatal(Verdict.Verdict)) {
    // Refusing here is the whole point: writing an unusable RootFS into the config is
    // what makes every later failure look like an emulator bug.
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: refusing to set an unusable RootFS: {}.\n", Verdict.Reason);
    if (!Opts.Force) {
      fextl::fmt::print(stderr, "Pass --force to write it anyway.\n");
      return 1;
    }
    fextl::fmt::print(stderr, "--force given: writing it anyway.\n");
  } else if (Verdict.Verdict == FEX::RootFSCheck::Verdict::UNVERIFIED) {
    fextl::fmt::print("warning: {}.\n", Verdict.Reason);
  }

  // Keep a bare name as a name: it stays valid if the data directory moves.
  const fextl::string Value = NameArgument.find('/') == fextl::string::npos ? NameArgument : Resolved;
  return SetUserDefaultRootFS(Value) ? 0 : 1;
}

// ---------------------------------------------------------------- argument parsing

const char* Usage = R"(POWERarmRootFSFetcher [options] [command]

Builds and selects the AArch64 Arch Linux ARM rootfs POWERarm runs guests against.

Commands:
  build [NAME]        build a rootfs, its per-user overlay and guest pacman, and select it
                      (this is the default command)
  list                list every rootfs POWERarm can find, with its verified architecture
  check [NAME|PATH]   check one rootfs (default: the configured one); non-zero if unusable
  default NAME|PATH   point the user's config at an existing rootfs
  overlay [NAME|PATH] add the per-user overlay and guest pacman to an existing base
                      (--manifest must name the one that built it)

Manifests (--manifest):
  vk     the toolchain roots plus Vulkan/RADV and the desktop libraries -- the default,
         and the shape every app on this project is tested against
  m2     the M2 GCC toolchain sysroot; no overlay by default, because its byte-compare
         gates are keyed to the bare base
  PATH   any alarm_sysroot.py manifest file
)";

struct Command {
  fextl::string Name {"build"};
  fextl::string Argument {};
};

Command ParseArguments(int argc, char** argv) {
  optparse::OptionParser Parser =
    optparse::OptionParser().description("Build and select POWERarm's AArch64 guest rootfs").usage(Usage).add_help_option(true);

  Parser.add_option("--manifest").help("base (default), m2, vk, or a path to a manifest file");
  Parser.add_option("--dest").help("build into this directory instead of <datadir>/RootFS/<NAME>");
  Parser.add_option("--cache").help("package cache directory");
  Parser.add_option("--scripts").help("directory holding alarm_sysroot.py and the manifests");
  Parser.add_option("--mirror").action("append").help("repo base URL ending in /aarch64 (repeatable; tried in order)");
  Parser.add_option("--no-verify-signatures").action("store_true").help("trust the sha256 pins alone (signatures are checked by default)");
  Parser.add_option("--force").action("store_true").help("replace a non-empty destination");
  Parser.add_option("--no-set-default").action("store_true").help("do not write the user's Config.json");
  Parser.add_option("--no-align").action("store_true").help("skip the PT_LOAD p_align report");
  Parser.add_option("--overlay").action("store_true").help("create the per-user overlay even when the manifest defaults against it");
  Parser.add_option("--no-overlay").action("store_true").help("build the bare base only: no overlay, no guest pacman");
  Parser.add_option("--no-pacman-init").action("store_true").help("create the overlay but skip pacman-key/pacman -Sy");
  Parser.add_option("--emulator").help("the POWERarm binary guest pacman runs under (default: next to this tool)");
  Parser.add_option("--dry-run").action("store_true").help("print what would be built and stop");
  Parser.add_option("-y", "--assume-yes").action("store_true").help("do not prompt");

  optparse::Values Options = Parser.parse_args(argc, argv);

  if (Options.is_set_by_user("manifest")) {
    Opts.Manifest = Options["manifest"].c_str();
  }
  if (Options.is_set_by_user("dest")) {
    Opts.Dest = Options["dest"].c_str();
  }
  if (Options.is_set_by_user("cache")) {
    Opts.Cache = Options["cache"].c_str();
  }
  if (Options.is_set_by_user("scripts")) {
    Opts.Scripts = Options["scripts"].c_str();
  }
  for (const auto& Mirror : Options.all("mirror")) {
    Opts.Mirrors.emplace_back(Mirror);
  }
  Opts.VerifySignatures = !Options.is_set_by_user("no_verify_signatures");
  Opts.Force = Options.is_set_by_user("force");
  Opts.SetDefault = !Options.is_set_by_user("no_set_default");
  Opts.Align = !Options.is_set_by_user("no_align");
  Opts.DryRun = Options.is_set_by_user("dry_run");
  Opts.AssumeYes = Options.is_set_by_user("assume_yes");
  Opts.PacmanInit = !Options.is_set_by_user("no_pacman_init");
  if (Options.is_set_by_user("emulator")) {
    Opts.Emulator = Options["emulator"].c_str();
  }
  if (Options.is_set_by_user("no_overlay") && Options.is_set_by_user("overlay")) {
    fextl::fmt::print(stderr, "POWERarmRootFSFetcher: --overlay and --no-overlay are mutually exclusive.\n");
    std::exit(2);
  }
  if (Options.is_set_by_user("no_overlay")) {
    Opts.Overlay = false;
  } else if (Options.is_set_by_user("overlay")) {
    Opts.Overlay = true;
  }

  if (Opts.Scripts.empty()) {
    Opts.Scripts = EnvOr("POWERARM_ROOTFS_SCRIPTS", {});
  }

  Command Result;
  const auto Leftover = Parser.args();
  if (!Leftover.empty()) {
    Result.Name = Leftover[0];
    if (Leftover.size() > 1) {
      Result.Argument = Leftover[1];
    }
    if (Leftover.size() > 2) {
      fextl::fmt::print(stderr, "POWERarmRootFSFetcher: too many arguments.\n");
      std::exit(2);
    }
  }
  return Result;
}

} // namespace

int main(int argc, char** argv, char** const envp) {
  FEX::Config::LoadConfig({}, envp);
  FEXCore::Config::ReloadMetaLayer();

  const auto Cmd = ParseArguments(argc, argv);

  if (Cmd.Name == "build") {
    return CommandBuild(Cmd.Argument);
  }
  if (Cmd.Name == "list") {
    return CommandList();
  }
  if (Cmd.Name == "check") {
    return CommandCheck(Cmd.Argument);
  }
  if (Cmd.Name == "default") {
    return CommandDefault(Cmd.Argument);
  }
  if (Cmd.Name == "overlay") {
    return CommandOverlay(Cmd.Argument);
  }

  fextl::fmt::print(stderr, "POWERarmRootFSFetcher: unknown command '{}'.\n\n{}", Cmd.Name, Usage);
  return 2;
}
