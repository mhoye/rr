/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "GdbCommandHandler"

#include "GdbCommand.h"
#include "GdbCommandHandler.h"
#include "log.h"

#include <sstream>
#include <vector>

// HashMap would be better here but the unordered_map API is annoying
// and linear search is fine.
static std::vector<GdbCommand*>* gdb_command_list;

static std::string gdb_macro_binding(const GdbCommand& cmd) {
  return "python RRCmd('" + cmd.name() + "')\n";
}

/* static */ std::string GdbCommandHandler::gdb_macros() {
  std::stringstream ss;
  ss << std::string(R"Delimiter(

set python print-stack full
python

import re

def gdb_unescape(string):
    result = ""
    pos = 0
    while pos < len(string):
        result += chr(int(string[pos:pos+2], 16))
        pos += 2
    return result

def gdb_escape(string):
    result = ""
    pos = 0
    for curr_char in string:
        result += format(ord(curr_char), 'x')
    return result

class RRCmd(gdb.Command):
    def __init__(self, name):
        gdb.Command.__init__(self, name,
                             gdb.COMMAND_USER, gdb.COMPLETE_NONE, True)
        self.cmd_name = name

    def invoke(self, arg, from_tty):
        args = gdb.string_to_argv(arg)
        self.rr_cmd(args)

    def rr_cmd(self, args):
        rv = gdb.execute("maint packet qRRCmd:" + gdb_escape(self.cmd_name), to_string=True);
        rv_match = re.search('received: "(.*)"', rv, re.MULTILINE);
        if not rv_match:
            gdb.write("Response error: " + rv)
            return
        response = gdb_unescape(rv_match.group(1))
        gdb.write(response)

end

)Delimiter");

  if (gdb_command_list) {
    for (auto& it : *gdb_command_list) {
      ss << gdb_macro_binding(*it);
    }
  }
  return ss.str();
}

static GdbCommand* command_for_name(const std::string& name) {
  if (!gdb_command_list) {
    return nullptr;
  }
  for (auto& it : *gdb_command_list) {
    if (it->name() == name) {
      return it;
    }
  }
  return nullptr;
}

void GdbCommandHandler::register_command(GdbCommand& cmd) {
  LOG(debug) << "registering command: " << cmd.name();
  if (!gdb_command_list) {
    gdb_command_list = new std::vector<GdbCommand*>();
  }
  gdb_command_list->push_back(&cmd);
}

// Use the simplest two hex character by byte encoding
static std::string gdb_escape(const std::string& str) {
  std::stringstream ss;
  ss << std::hex;
  for (size_t i = 0; i < str.size(); i++) {
    auto chr = str.at(i);
    ss << (int)chr;
  }

  return ss.str();
}
static std::string gdb_unescape(const std::string& str) {
  std::stringstream ss;
  for (size_t i = 0; i < str.size(); i += 2) {
    ss << (char)std::stoul(str.substr(i, 2), nullptr, 16);
  }

  return ss.str();
}
static std::vector<std::string> parse_cmd(std::string& str) {
  std::vector<std::string> args;
  size_t pos = 0;
  std::string delimiter = ":";
  while ((pos = str.find(delimiter)) != std::string::npos) {
    args.push_back(gdb_unescape(str.substr(0, pos)));
    str.erase(0, pos + delimiter.length());
  }
  args.push_back(gdb_unescape(str));
  return args;
}

/* static */ std::string GdbCommandHandler::process_command(
    GdbServer& gdb_server, Task* t, std::string payload) {
  const std::vector<std::string> args = parse_cmd(payload);
  GdbCommand* cmd = command_for_name(args[0]);
  if (!cmd) {
    return gdb_escape(std::string() + "Command '" + args[0] + "' not found.\n");
  }
  LOG(debug) << "invoking command: " << cmd->name();
  std::string resp = cmd->invoke(gdb_server, t, args);
  LOG(debug) << "cmd response: " << resp;
  return gdb_escape(resp);
}
