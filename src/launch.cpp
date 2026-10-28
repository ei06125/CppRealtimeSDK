// Parse exact integer timing values and attach useful source-line diagnostics.
// Validate the complete graph before handing any configuration back to an execution owner.
#include "rtx/launch.hpp"
#include <charconv>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
namespace rtx {
namespace {
template<class T> T number(const std::string& text) {
 T result{};
 auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
 if(parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size()) throw std::invalid_argument("invalid or out-of-range integer: "+text);
 return result;
}
}
std::vector<Node> load_launch(std::istream& input,const CallbackRegistry& callbacks) {
 std::vector<Node> nodes;
 std::string line;
 std::size_t line_number=0;
 bool header=false;
 while(std::getline(input,line)) {
  ++line_number;
  try {
   const auto comment=line.find('#');
   if(comment!=std::string::npos) line.resize(comment);
   std::istringstream row(line);
   std::vector<std::string> fields;
   for(std::string field;row>>field;) fields.push_back(std::move(field));
   if(fields.empty()) continue;
   if(!header) {
    if(fields!=std::vector<std::string>{"rtx-launch","1"}) throw std::invalid_argument("expected rtx-launch 1 header");
    header=true;continue;
   }
   if(fields.size()<7 || fields[0]!="node") throw std::invalid_argument("expected node NAME CALLBACK PRIORITY PERIOD_NS DEADLINE_NS WATCHDOG_NS [DEPENDENCY ...]");
   auto callback=callbacks.find(fields[2]);
   if(callback==callbacks.end() || !callback->second) throw std::invalid_argument("unregistered callback: "+fields[2]);
   Node node;
   node.name=fields[1]; node.execute=callback->second;
   node.priority=number<int>(fields[3]);
   node.period=Duration(number<Duration::rep>(fields[4]));
   node.deadline=Duration(number<Duration::rep>(fields[5]));
   node.watchdog=Duration(number<Duration::rep>(fields[6]));
   std::unordered_set<std::string> dependencies;
   for(std::size_t i=7;i<fields.size();++i) {
    if(!dependencies.insert(fields[i]).second) throw std::invalid_argument("duplicate dependency: "+fields[i]);
    node.after.push_back(fields[i]);
   }
   nodes.push_back(std::move(node));
  }catch(const std::invalid_argument& ex) {
   throw std::invalid_argument("launch line "+std::to_string(line_number)+": "+ex.what());
  }
 }
 if(input.bad() || (!input.eof() && input.fail())) throw std::runtime_error("launch input read failed");
 if(!header || nodes.empty()) throw std::invalid_argument("launch configuration must contain a header and at least one node");
 Scheduler validate(nodes); // Validate names, durations, missing dependencies, and cycles.
 return nodes;
}
}
