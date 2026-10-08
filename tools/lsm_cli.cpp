// Tiny interactive shell: put <k> <v> | get <k> | del <k> | flush | compact | stats | quit
#include <iostream>
#include <sstream>

#include "db.hpp"

int main(int argc, char** argv) {
    std::string dir = argc > 1 ? argv[1] : "./lsm_data";
    lsm::Options o;
    o.memtable_bytes = 4096;  // small, so you can watch flushes and compactions happen
    auto db = lsm::DB::open(dir, o);
    std::cout << "opened " << dir << "  (commands: put get del flush compact stats quit)\n";
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream is(line);
        std::string cmd, k, v;
        is >> cmd >> k;
        std::getline(is >> std::ws, v);
        if (cmd == "put") { db->put(k, v); std::cout << "ok\n"; }
        else if (cmd == "get") { auto r = db->get(k); std::cout << (r ? *r : "(not found)") << "\n"; }
        else if (cmd == "del") { db->del(k); std::cout << "ok\n"; }
        else if (cmd == "flush") { db->flush(); std::cout << "flushed\n"; }
        else if (cmd == "compact") { db->compact(); std::cout << "compacted\n"; }
        else if (cmd == "stats") {
            auto s = db->stats();
            std::cout << "sstables=" << s.sstables << " entries=" << s.sstable_entries << " flushes=" << s.flushes
                      << " compactions=" << s.compactions << " bloom_skips=" << s.bloom_skips
                      << " table_probes=" << s.table_probes << "\n";
        }
        else if (cmd == "quit" || cmd == "exit") break;
        else if (!cmd.empty()) std::cout << "unknown command\n";
    }
}
