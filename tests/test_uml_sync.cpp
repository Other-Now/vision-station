// Keeps docs/uml/state_station.puml honest: the transitions drawn in the
// diagram must be exactly the transitions in the C++ table.
//
// Supported PlantUML subset: `A --> B : Ev1 / Ev2`, `[*] --> Idle`, and
// composite states `state Active { ... }`, where an edge from a composite
// applies to every state nested inside it.
#include <gtest/gtest.h>

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>

#include "core/station_fsm.hpp"

using namespace vs;

namespace {
using Edge = std::tuple<std::string, std::string, std::string>;  // from, event, to

std::set<Edge> parse_puml(const std::string& path, std::string* err) {
  std::ifstream in(path);
  if (!in) {
    *err = "cannot open " + path;
    return {};
  }
  std::map<std::string, std::vector<std::string>> members;  // composite -> leaf states
  std::vector<std::string> stack;
  std::vector<std::tuple<std::string, std::string, std::string>> raw;
  const std::regex open(R"(^\s*state\s+(\w+)\s*\{)"), close(R"(^\s*\}\s*$)");
  const std::regex leaf(R"(^\s*state\s+(\w+)\s*$)");
  const std::regex edge(R"(^\s*(\w+)\s*-+>\s*(\w+)\s*:\s*(.+?)\s*$)");
  std::string line;
  std::smatch m;
  while (std::getline(in, line)) {
    if (std::regex_search(line, m, open)) {
      stack.push_back(m[1]);
      continue;
    }
    if (std::regex_search(line, m, close)) {
      if (!stack.empty()) stack.pop_back();
      continue;
    }
    auto add_member = [&](const std::string& s) {
      for (const auto& c : stack) members[c].push_back(s);
    };
    if (std::regex_search(line, m, leaf)) add_member(m[1]);
    if (std::regex_search(line, m, edge)) {
      add_member(m[1]);
      add_member(m[2]);
      raw.emplace_back(m[1], m[3], m[2]);
    }
  }
  std::set<Edge> out;
  for (const auto& [from, label, to] : raw) {
    std::vector<std::string> froms = members.count(from) ? members[from] : std::vector<std::string>{from};
    std::stringstream ss(label);
    std::string ev;
    while (std::getline(ss, ev, '/')) {
      ev = std::regex_replace(ev, std::regex(R"(^\s+|\s+$)"), "");
      for (const auto& f : froms)
        if (!members.count(f)) out.insert({f, ev, to});
    }
  }
  return out;
}
}  // namespace

TEST(UmlSync, StateDiagramMatchesTransitionTable) {
  std::string err;
  const auto drawn = parse_puml(std::string(VS_SOURCE_DIR) + "/docs/uml/state_station.puml", &err);
  ASSERT_TRUE(err.empty()) << err;
  std::set<Edge> code;
  for (const auto& t : transition_table())
    code.insert({std::string(to_string(t.from)), std::string(to_string(t.event)), std::string(to_string(t.to))});
  for (const auto& e : code)
    EXPECT_TRUE(drawn.count(e)) << "missing from diagram: " << std::get<0>(e) << " --" << std::get<1>(e) << "--> "
                                << std::get<2>(e);
  for (const auto& e : drawn)
    EXPECT_TRUE(code.count(e)) << "diagram edge not in code: " << std::get<0>(e) << " --" << std::get<1>(e)
                               << "--> " << std::get<2>(e);
}
