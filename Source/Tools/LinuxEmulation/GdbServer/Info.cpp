// SPDX-License-Identifier: MIT
/*
$info$
tags: glue|gdbserver
desc: Provides a gdb interface to the guest state
$end_info$
*/

#include "GdbServer/Info.h"

#include <FEXCore/Core/CoreState.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/sstream.h>
#include <FEXCore/Utils/CompilerDefs.h>
#include <FEXCore/Utils/FileLoading.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/StringUtils.h>

#include <array>
#include <string_view>

namespace FEX::GDB::Info {
fextl::string GetThreadName(uint32_t PID, uint32_t ThreadID) {
  const auto ThreadFile = fextl::fmt::format("/proc/{}/task/{}/comm", PID, ThreadID);
  fextl::string ThreadName;
  FEXCore::FileLoading::LoadFile(ThreadName, ThreadFile);
  // Trim out the potential newline, breaks GDB if it exists.
  return FEXCore::StringUtils::Trim(ThreadName);
}

fextl::string BuildOSXML() {
  fextl::ostringstream xml;

  xml << "<?xml version='1.0'?>\n";

  xml << "<!DOCTYPE target SYSTEM \"osdata.dtd\">\n";
  xml << "<osdata type=\"processes\">";
  // XXX
  xml << "</osdata>";

  xml << std::flush;

  return xml.str();
}

fextl::string BuildTargetXML() {
  fextl::ostringstream xml;

  xml << "<?xml version='1.0'?>\n";
  xml << "<!DOCTYPE target SYSTEM 'gdb-target.dtd'>\n";
  xml << "<target>\n";
  xml << "<architecture>aarch64</architecture>\n";
  xml << "<osabi>GNU/Linux</osabi>\n";

  auto reg = [&](std::string_view name, std::string_view type, int size) {
    xml << "<reg name='" << name << "' bitsize='" << size << "' type='" << type << "' />" << std::endl;
  };

  // Register numbering must match GdbServer::CommandReadRegisters / CommandReadReg.
  xml << "<feature name='org.gnu.gdb.aarch64.core'>\n";
  xml << R"(<flags id="cpsr_flags" size="4">
          <field name="V" start="28" end="28"/>
          <field name="C" start="29" end="29"/>
          <field name="Z" start="30" end="30"/>
          <field name="N" start="31" end="31"/>
        </flags>
        )";
  for (uint32_t i = 0; i < FEXCore::Core::CPUState::NUM_XREGS; i++) {
    reg(fextl::fmt::format("x{}", i), "int", 64);
  }
  reg("sp", "data_ptr", 64);
  reg("pc", "code_ptr", 64);
  reg("cpsr", "cpsr_flags", 32);
  xml << "</feature>\n";

  xml << "<feature name='org.gnu.gdb.aarch64.fpu'>\n";
  xml <<
    R"(<vector id="v2d" type="ieee_double" count="2"/>
        <vector id="v2u" type="uint64" count="2"/>
        <vector id="v2i" type="int64" count="2"/>
        <vector id="v4f" type="ieee_single" count="4"/>
        <vector id="v4u" type="uint32" count="4"/>
        <vector id="v4i" type="int32" count="4"/>
        <vector id="v8u" type="uint16" count="8"/>
        <vector id="v8i" type="int16" count="8"/>
        <vector id="v16u" type="uint8" count="16"/>
        <vector id="v16i" type="int8" count="16"/>
        <union id="vnd"><field name="f" type="v2d"/><field name="u" type="v2u"/><field name="s" type="v2i"/></union>
        <union id="vns"><field name="f" type="v4f"/><field name="u" type="v4u"/><field name="s" type="v4i"/></union>
        <union id="vnh"><field name="u" type="v8u"/><field name="s" type="v8i"/></union>
        <union id="vnb"><field name="u" type="v16u"/><field name="s" type="v16i"/></union>
        <union id="vnq"><field name="u" type="uint128"/><field name="s" type="int128"/></union>
        <union id="aarch64v"><field name="d" type="vnd"/><field name="s" type="vns"/><field name="h" type="vnh"/><field name="b" type="vnb"/><field name="q" type="vnq"/></union>
        )";
  for (size_t i = 0; i < FEXCore::Core::CPUState::NUM_VREGS; i++) {
    reg(fextl::fmt::format("v{}", i), "aarch64v", 128);
  }
  reg("fpsr", "int", 32);
  reg("fpcr", "int", 32);
  xml << "</feature>\n";

  xml << "</target>";
  xml << std::flush;

  return xml.str();
}

} // namespace FEX::GDB::Info
