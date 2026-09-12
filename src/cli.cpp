#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.h"
#include "json.hpp"
#include <iostream>
#include <string>
#include <vector>
#include <iomanip>

using json = nlohmann::json;

void printTable(const std::vector<std::string>& cols, const std::vector<std::vector<std::string>>& rows) {
    if (cols.empty()) return;

    // Determine column widths
    std::vector<size_t> widths(cols.size(), 0);
    for (size_t i = 0; i < cols.size(); ++i) {
        widths[i] = cols[i].length();
    }
    for (const auto& row : rows) {
        for (size_t i = 0; i < row.size(); ++i) {
            if (i < widths.size() && row[i].length() > widths[i]) {
                widths[i] = row[i].length();
            }
        }
    }

    // Print separator
    auto print_separator = [&]() {
        std::cout << "+";
        for (size_t w : widths) std::cout << std::string(w + 2, '-') << "+";
        std::cout << "\n";
    };

    print_separator();
    
    // Print headers
    std::cout << "|";
    for (size_t i = 0; i < cols.size(); ++i) {
        std::cout << " " << std::left << std::setw(widths[i]) << cols[i] << " |";
    }
    std::cout << "\n";
    
    print_separator();

    // Print rows
    for (const auto& row : rows) {
        std::cout << "|";
        for (size_t i = 0; i < row.size(); ++i) {
            std::cout << " " << std::left << std::setw(widths[i]) << (i < row.size() ? row[i] : "") << " |";
        }
        std::cout << "\n";
    }

    print_separator();
    std::cout << "(" << rows.size() << " rows)\n\n";
}

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    int port = 8081;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--host" && i + 1 < argc) {
            host = argv[++i];
        } else if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        }
    }

    std::cout << "Teal / DuckFlow CLI Client\n";
    std::cout << "Connected to http://" << host << ":" << port << "\n";
    std::cout << "Type 'exit;' to quit.\n\n";

    httplib::Client cli(host, port);

    std::string current_query = "";
    
    while (true) {
        if (current_query.empty()) {
            std::cout << "teal> ";
        } else {
            std::cout << "   -> ";
        }
        
        std::string line;
        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line == "exit;") {
            break;
        }

        current_query += line + " ";
        
        // If it ends with semicolon, execute it
        if (line.find(';') != std::string::npos) {
            json payload;
            payload["query"] = current_query;
            
            auto res = cli.Post("/api/query", payload.dump(), "application/json");
            
            if (res) {
                if (res->status == 200) {
                    try {
                        auto data = json::parse(res->body);
                        if (data["status"] == "success") {
                            printTable(data["columns"], data["rows"]);
                        } else {
                            std::cerr << "Error: " << data["error"] << "\n\n";
                        }
                    } catch (const std::exception& e) {
                        std::cerr << "JSON Parsing Error: " << e.what() << "\n\n";
                    }
                } else {
                    std::cerr << "HTTP Error: " << res->status << "\n";
                    std::cerr << res->body << "\n\n";
                }
            } else {
                auto err = res.error();
                std::cerr << "Network Error: Failed to connect to server (" << httplib::to_string(err) << ")\n\n";
            }
            
            current_query = "";
        }
    }

    std::cout << "Disconnected.\n";
    return 0;
}
