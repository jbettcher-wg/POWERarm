// SPDX-License-Identifier: MIT
/*
$info$
tags: glue|gdbserver
desc: Provides a gdb interface to the guest state
$end_info$
*/
#pragma once

#include <FEXCore/fextl/string.h>

#include <cstdint>
#include <string_view>

namespace FEX::GDB::Info {
/**
 * @brief Fetches the thread's name
 *
 * @param PID The program id of the application
 * @param ThreadID The thread id of the program
 */
fextl::string GetThreadName(uint32_t PID, uint32_t ThreadID);

/**
 * @brief Returns the GDB specific construct of OS describing XML.
 */
fextl::string BuildOSXML();

/**
 * @brief Returns the GDB specific construct of target describing XML.
 */
fextl::string BuildTargetXML();
} // namespace FEX::GDB::Info
