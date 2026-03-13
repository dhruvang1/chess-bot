#include <iostream>
#include <string>
#include "uci.cpp"

using namespace std;

int main(int argc, char* argv[]) {
    bool datagen = false;
    string nnuePath;
    for (int i = 1; i < argc; i++) {
        string arg = argv[i];
        if (arg == "datagen") {
            datagen = true;
        } else if (arg == "--nnue" && i + 1 < argc) {
            nnuePath = argv[++i];
        }
    }
    Uci uci(datagen, nnuePath);
    while(true) {
        string msg;
        getline(cin, msg);
        uci.handle(msg);
    }
}
