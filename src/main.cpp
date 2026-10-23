#include "rythmo/detx.hpp"

#include <iostream>
#include <string>

using rythmo::DetxDocument;
using rythmo::DetxFormatError;
using rythmo::Issue;

namespace {
void usage() {
    std::cerr << "Usage:\n"
              << "  rythmo-cli inspect <file.detx>\n"
              << "  rythmo-cli validate <file.detx>\n"
              << "  rythmo-cli roundtrip <file.detx> <output.detx>\n";
}
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    try {
        const std::string command = argv[1];
        auto doc = DetxDocument::load(argv[2]);
        if (command == "inspect" && argc == 3) {
            std::cout << "fps: " << doc.fps();
            if (doc.fps_is_assumed()) std::cout << " (fallback; not stored in file)";
            std::cout << "\nroles: " << doc.roles().size()
                      << "\nlines: " << doc.lines().size()
                      << "\nmarkers: " << doc.markers().size() << '\n';
            for (const auto& role : doc.roles()) {
                std::cout << "  role '" << role.id << "': " << role.name << " (" << role.gender << ", " << role.color << ")\n";
            }
            return 0;
        }
        if (command == "validate" && argc == 3) {
            const auto issues = doc.validate();
            std::size_t errors = 0, warnings = 0;
            for (const auto& issue : issues) {
                std::cout << issue.to_string() << '\n';
                if (issue.severity == Issue::Severity::Error) ++errors; else ++warnings;
            }
            std::cout << errors << " error(s), " << warnings << " warning(s)\n";
            return errors ? 1 : 0;
        }
        if (command == "roundtrip" && argc == 4) {
            doc.save(argv[3]);
            std::cout << "wrote " << argv[3] << '\n';
            return 0;
        }
        usage(); return 2;
    } catch (const DetxFormatError& error) {
        std::cerr << "error: " << error.what() << '\n'; return 2;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n'; return 2;
    }
}
