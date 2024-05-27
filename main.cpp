#include <iostream>
#include <string>
#include "uci.cpp"

using namespace std;

int main() {
    Uci uci;
    while(true) {
        string msg;
        getline(cin, msg);
        uci.handle(msg);
    }
}
