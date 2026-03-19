//===--- Lilium.h - Lilium ToolChain Implementations --------------*- C++
//-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Lilium.h"
#include "Arch/ARM.h"
#include "Arch/LoongArch.h"
#include "Arch/Mips.h"
#include "Arch/PPC.h"
#include "Arch/RISCV.h"
#include "clang/Config/config.h"
#include "clang/Driver/CommonArgs.h"
#include "clang/Driver/Distro.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Options/Options.h"
#include "llvm/Option/ArgList.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <system_error>

using namespace clang::driver;
using namespace clang::driver::toolchains;
using namespace clang;
using namespace llvm::opt;

using tools::addPathIfExists;

/// Get our best guess at the multiarch triple for a target.
///
/// Debian-based systems are starting to use a multiarch setup where they use
/// a target-triple directory in the library and header search paths.
/// Unfortunately, this triple does not align with the vanilla target triple,
/// so we provide a rough mapping here.
std::string Lilium::getMultiarchTriple(const Driver &D,
                                       const llvm::Triple &TargetTriple,
                                       StringRef SysRoot) const {

  std::string name{};
  name += TargetTriple.getArchName();
  name += "-";
  name += TargetTriple.getOSName();
  name += "-";
  name += TargetTriple.getEnvironmentName();

  return name;
}

static StringRef getOSLibDir(const llvm::Triple &Triple, const ArgList &Args) {
  return "lib";
}

Lilium::Lilium(const Driver &D, const llvm::Triple &Triple, const ArgList &Args)
    : Generic_ELF(D, Triple, Args) {
  GCCInstallation.init(Triple, Args);
  SelectedMultilibs.assign({GCCInstallation.getMultilib()});
  llvm::Triple::ArchType Arch = Triple.getArch();
  std::string SysRoot = computeSysRoot();
  ToolChain::path_list &PPaths = getProgramPaths();

  Generic_GCC::PushPPaths(PPaths);

#ifdef ENABLE_LINKER_BUILD_ID
  ExtraOpts.push_back("--build-id");
#endif

  // The selection of paths to try here is designed to match the patterns which
  // the GCC driver itself uses, as this is part of the GCC-compatible driver.
  // This was determined by running GCC in a fake filesystem, creating all
  // possible permutations of these directories, and seeing which ones it added
  // to the link paths.
  path_list &Paths = getFilePaths();

  const std::string OSLibDir = std::string(getOSLibDir(Triple, Args));
  const std::string MultiarchTriple = getMultiarchTriple(D, Triple, SysRoot);

  Generic_GCC::AddMultilibPaths(D, SysRoot, OSLibDir, MultiarchTriple, Paths);

  addPathIfExists(D, concat(SysRoot, "/lib", MultiarchTriple), Paths);
  addPathIfExists(D, concat(SysRoot, "/lib/..", OSLibDir), Paths);

  addPathIfExists(D, concat(SysRoot, "/usr/lib", MultiarchTriple), Paths);
  addPathIfExists(D, concat(SysRoot, "/usr", OSLibDir), Paths);
  Generic_GCC::AddMultiarchPaths(D, SysRoot, OSLibDir, Paths);

  addPathIfExists(D, concat(SysRoot, "/lib"), Paths);
  addPathIfExists(D, concat(SysRoot, "/usr/lib"), Paths);
}

ToolChain::RuntimeLibType Lilium::GetDefaultRuntimeLibType() const {
  return ToolChain::RLT_CompilerRT;
}
ToolChain::CXXStdlibType Lilium::GetDefaultCXXStdlibType() const {
  return ToolChain::CST_Libcxx;
}

bool Lilium::HasNativeLLVMSupport() const { return true; }

Tool *Lilium::buildLinker() const { return new tools::gnutools::Linker(*this); }

Tool *Lilium::buildStaticLibTool() const {
  return new tools::gnutools::StaticLibTool(*this);
}

Tool *Lilium::buildAssembler() const {
  return new tools::gnutools::Assembler(*this);
}

std::string Lilium::computeSysRoot() const {
  if (!getDriver().SysRoot.empty())
    return getDriver().SysRoot;

  const char *wl_sysroot = getenv("WL_SYSROOT");

  if (wl_sysroot)
    return wl_sysroot;

  return std::string();
}

std::string Lilium::getDynamicLinker(const ArgList &Args) const {
  const llvm::Triple::ArchType Arch = getArch();
  const llvm::Triple &Triple = getTriple();

  StringRef ArchName{};
  switch (Arch) {
  case llvm::Triple::x86_64:
    ArchName = "x86_64";
    break;
  case llvm::Triple::x86:
    ArchName = "i686";
    break;
  default:
    break;
  }
  std::string linkerName{};
  switch (Triple.getEnvironment()) {
  case llvm::Triple::Kernel:
    linkerName += "lilium-loader-";
    linkerName += ArchName;
    linkerName += ".so";
    break;
  default:
    linkerName += "ld-lilium-";
    linkerName += ArchName;
    linkerName += ".so.1";
    break;
  }

  return linkerName;
}

void Lilium::AddClangSystemIncludeArgs(const ArgList &DriverArgs,
                                       ArgStringList &CC1Args) const {
  const Driver &D = getDriver();
  std::string SysRoot = computeSysRoot();

  if (DriverArgs.hasArg(clang::options::OPT_nostdinc))
    return;

  // Add 'include' in the resource directory, which is similar to
  // GCC_INCLUDE_DIR (private headers) in GCC. Note: the include directory
  // contains some files conflicting with system /usr/include. musl systems
  // prefer the /usr/include copies which are more relevant.
  SmallString<128> ResourceDirInclude(D.ResourceDir);
  llvm::sys::path::append(ResourceDirInclude, "include");
  if (!DriverArgs.hasArg(options::OPT_nobuiltininc) &&
      (!getTriple().isMusl() || DriverArgs.hasArg(options::OPT_nostdlibinc)))
    addSystemInclude(DriverArgs, CC1Args, ResourceDirInclude);

  if (DriverArgs.hasArg(options::OPT_nostdlibinc))
    return;

  // LOCAL_INCLUDE_DIR
  addSystemInclude(DriverArgs, CC1Args, concat(SysRoot, "/usr/local/include"));
  // TOOL_INCLUDE_DIR
  AddMultilibIncludeArgs(DriverArgs, CC1Args);

  // Check for configure-time C include directories.
  StringRef CIncludeDirs(C_INCLUDE_DIRS);
  if (CIncludeDirs != "") {
    SmallVector<StringRef, 5> dirs;
    CIncludeDirs.split(dirs, ":");
    for (StringRef dir : dirs) {
      StringRef Prefix =
          llvm::sys::path::is_absolute(dir) ? "" : StringRef(SysRoot);
      addExternCSystemInclude(DriverArgs, CC1Args, Prefix + dir);
    }
    return;
  }

  // On systems using multiarch and Android, add /usr/include/$triple before
  // /usr/include.
  std::string MultiarchIncludeDir = getMultiarchTriple(D, getTriple(), SysRoot);
  if (!MultiarchIncludeDir.empty() &&
      D.getVFS().exists(concat(SysRoot, "/usr/include", MultiarchIncludeDir)))
    addExternCSystemInclude(
        DriverArgs, CC1Args,
        concat(SysRoot, "/usr/include", MultiarchIncludeDir));

  if (getTriple().getOS() == llvm::Triple::RTEMS)
    return;

  // Add an include of '/include' directly. This isn't provided by default by
  // system GCCs, but is often used with cross-compiling GCCs, and harmless to
  // add even when Clang is acting as-if it were a system compiler.
  addExternCSystemInclude(DriverArgs, CC1Args, concat(SysRoot, "/include"));

  addExternCSystemInclude(DriverArgs, CC1Args, concat(SysRoot, "/usr/include"));

  if (!DriverArgs.hasArg(options::OPT_nobuiltininc) && getTriple().isMusl())
    addSystemInclude(DriverArgs, CC1Args, ResourceDirInclude);
}

void Lilium::addLibStdCxxIncludePaths(const llvm::opt::ArgList &DriverArgs,
                                      llvm::opt::ArgStringList &CC1Args) const {
  // We need a detected GCC installation on Lilium to provide libstdc++'s
  // headers in odd Liliumish places.
  if (!GCCInstallation.isValid())
    return;

  // Detect Debian g++-multiarch-incdir.diff.
  StringRef TripleStr = GCCInstallation.getTriple().str();

  StringRef LibDir = GCCInstallation.getParentLibPath();
  const Multilib &Multilib = GCCInstallation.getMultilib();
  const GCCVersion &Version = GCCInstallation.getVersion();

  const std::string LibStdCXXIncludePathCandidates[] = {
      // Android standalone toolchain has C++ headers in yet another place.
      LibDir.str() + "/../" + TripleStr.str() + "/include/c++/" + Version.Text,
      // Freescale SDK C++ headers are directly in <sysroot>/usr/include/c++,
      // without a subdirectory corresponding to the gcc version.
      LibDir.str() + "/../include/c++",
      // Cray's gcc installation puts headers under "g++" without a
      // version suffix.
      LibDir.str() + "/../include/g++",
  };

  for (const auto &IncludePath : LibStdCXXIncludePathCandidates) {
    if (addLibStdCXXIncludePaths(IncludePath, TripleStr,
                                 Multilib.includeSuffix(), DriverArgs, CC1Args))
      break;
  }
}

void Lilium::AddCudaIncludeArgs(const ArgList &DriverArgs,
                                ArgStringList &CC1Args) const {
  CudaInstallation->AddCudaIncludeArgs(DriverArgs, CC1Args);
}

void Lilium::AddHIPIncludeArgs(const ArgList &DriverArgs,
                               ArgStringList &CC1Args) const {
  RocmInstallation->AddHIPIncludeArgs(DriverArgs, CC1Args);
}

void Lilium::AddIAMCUIncludeArgs(const ArgList &DriverArgs,
                                 ArgStringList &CC1Args) const {
  if (GCCInstallation.isValid()) {
    CC1Args.push_back("-isystem");
    CC1Args.push_back(DriverArgs.MakeArgString(
        GCCInstallation.getParentLibPath() + "/../" +
        GCCInstallation.getTriple().str() + "/include"));
  }
}

void Lilium::addSYCLIncludeArgs(const ArgList &DriverArgs,
                                ArgStringList &CC1Args) const {
  SYCLInstallation->addSYCLIncludeArgs(DriverArgs, CC1Args);
}

bool Lilium::isPIEDefault(const llvm::opt::ArgList &Args) const { return true; }

bool Lilium::IsMathErrnoDefault() const { return false; }

SanitizerMask Lilium::getSupportedSanitizers() const {
  SanitizerMask Res = ToolChain::getSupportedSanitizers();

  return Res;
}

void Lilium::addExtraOpts(llvm::opt::ArgStringList &CmdArgs) const {
  for (const auto &Opt : ExtraOpts)
    CmdArgs.push_back(Opt.c_str());
}

const char *Lilium::getDefaultLinker() const {
  return Generic_ELF::getDefaultLinker();
}
