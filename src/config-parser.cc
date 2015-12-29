/* -*- mode: c++; c-basic-offset: 2; indent-tabs-mode: nil; -*-
 * (c) 2015 Henner Zeller <h.zeller@acm.org>
 *
 * This file is part of BeagleG. http://github.com/hzeller/beagleg
 *
 * BeagleG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * BeagleG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with BeagleG.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config-parser.h"

#include <algorithm>
#include <fstream>
#include <iostream>

#include <assert.h>
#include <ctype.h>

#include "logging.h"
#include "string-util.h"

void ConfigParser::EventReceiver::ReportError(int line_no,
                                              const std::string &msg) {
  std::cerr << line_no << ":" << msg << std::endl;
}

ConfigParser::ConfigParser() : parse_success_(true) {}

bool ConfigParser::SetContentFromFile(const char *filename) {
  parse_success_ = true;
  std::ifstream file_stream(filename, std::ios::binary);
  content_.assign(std::istreambuf_iterator<char>(file_stream),
                  std::istreambuf_iterator<char>());
  return file_stream.good();
}

void ConfigParser::SetContent(const std::string &content) {
  parse_success_ = true;
  content_ = content;
}

// Extract next line out of source. Takes 
// Modifies source.
static StringPiece NextLine(StringPiece *source) {
  StringPiece result;
  if (source->length() == 0)
    return result;
  const StringPiece::iterator start = source->begin();
  StringPiece::iterator endline = start;
  for (/**/; endline != source->end(); ++endline) {
    // Whatever comes first terminates our line.
    if (!result.data() &&
        (*endline == '#' || *endline == '\r' || *endline == '\n')) {
      result.assign(start, endline - start);
    }
    if (*endline == '\n') {
      source->assign(endline + 1, source->length() - (endline - start) - 1);
      return result;
    }
  }
  // Encountered last line without final newline.
  result.assign(start, endline - start);
  source->assign(source->end(), 0);
  return result;
}

static std::string CanonicalizeName(const StringPiece &s) {
  return ToLower(TrimWhitespace(s));
}

bool ConfigParser::EmitConfigValues(EventReceiver *event_receiver) {
  // The first pass collects all the parse errors and emits them. Later on,
  // we refuse to run another time.
  if (!parse_success_)
    return false;
  bool success = true;
  bool current_section_interested = false;
  std::string current_section;
  int line_no = 0;
  StringPiece content_data(content_.data(), content_.length());
  StringPiece line = NextLine(&content_data);
  for (/**/; line.data() != NULL; line = NextLine(&content_data)) {
    ++line_no;
    line = TrimWhitespace(line);
    if (line.empty())
      continue;

    // Sections start with '['
    if (line[0] == '[') {
      if (line[line.length() - 1] != ']') {
        event_receiver->ReportError(line_no, "Section line does not end in ']'");
        parse_success_ = false;
        current_section_interested = false;  // rest is probably bogus.
        continue;
      }

      StringPiece section = line.substr(1, line.length() - 2);
      current_section = CanonicalizeName(section);
      current_section_interested = event_receiver
        ->SeenSection(line_no, current_section);
    }
    else {
      StringPiece::iterator eq_pos = std::find(line.begin(), line.end(), '=');
      if (eq_pos == line.end()) {
        event_receiver->ReportError(line_no, "name=value pair expected.");
        parse_success_ = false;
        continue;
      }
      if (current_section_interested) {
        std::string name = CanonicalizeName(StringPiece(line.begin(),
                                                        eq_pos - line.begin()));
        StringPiece value_piece
          = TrimWhitespace(StringPiece(eq_pos + 1, line.end() - eq_pos - 1));
        std::string value = value_piece.ToString();
        bool could_parse = event_receiver->SeenNameValue(line_no, name, value);
        if (!could_parse) {
          event_receiver
            ->ReportError(line_no,
                          StringPrintf("In section [%s]: Problem handling '%s = %s'",
                                       current_section.c_str(),
                                       name.c_str(), value.c_str()));
        }
        success &= could_parse;
      }
    }
  }
  return parse_success_ && success;
}
