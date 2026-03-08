#include <iostream>
#include <string>
#include "uci.cpp"

using namespace std;

int main(int argc, char* argv[]) {
    bool datagen = argc > 1 && string(argv[1]) == "datagen";
    Uci uci(datagen);
    while(true) {
        string msg;
        getline(cin, msg);
        uci.handle(msg);
    }
}
