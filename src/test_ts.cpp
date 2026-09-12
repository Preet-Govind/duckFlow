#include "duckdb_engine.h"
#include <iostream>

int main() {
    DuckDbEngine engine;
    if (!engine.connect("test.db")) {
        std::cerr << "failed to connect" << std::endl;
        return 1;
    }

    std::vector<std::string> cols;
    std::vector<std::vector<std::string>> rows;
    std::string err;

    engine.executeQuery("SELECT current_timestamp;", cols, rows, err);
    std::cout << "err: " << err << "\n";
    for(auto c : cols) std::cout << c << "\t";
    std::cout << "\n";
    for(auto r : rows) {
        for(auto v : r) std::cout << v << "\t";
        std::cout << "\n";
    }

    engine.executeQuery("SELECT now();", cols, rows, err);
    std::cout << "err: " << err << "\n";
    for(auto c : cols) std::cout << c << "\t";
    std::cout << "\n";
    for(auto r : rows) {
        for(auto v : r) std::cout << v << "\t";
        std::cout << "\n";
    }

    return 0;
}
