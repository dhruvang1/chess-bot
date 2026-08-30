#include <iostream>
#include <string>
#include "uci.cpp"
#include "bench.cpp"

using namespace std;

int main(int argc, char* argv[]) {
    bool datagen = false;
    bool bench = false;
    int benchDepth = 10;
    string nnuePath;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "datagen") {
            datagen = true;
        } else if (arg == "bench") {
            bench = true;
            // optional depth: `bench 12`
            if (i + 1 < argc && isdigit((unsigned char)argv[i + 1][0])) benchDepth = stoi(argv[++i]);
        } else if (arg == "--nnue" && i + 1 < argc) {
            nnuePath = argv[++i];
        }
    }
    initLMR();  // must run once, single-threaded, before any Search worker threads spawn

    if (bench) {
        if (!nnuePath.empty() && !loadNNUE(nnuePath)) {
            cerr << "Error: NNUE file not found or invalid: " << nnuePath << endl;
            return 1;
        }
        return runBench(benchDepth);
    }
    auto* uci = new Uci(datagen, nnuePath);
    string msg;
    while (getline(cin, msg)) {
        uci->handle(msg);
    }
}
