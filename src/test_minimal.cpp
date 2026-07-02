// Minimal test to locate crash point
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    std::cout << "Step 1: Program started\n";
    
    std::cout << "Step 2: Checking args...\n";
    if (argc < 2) {
        std::cout << "Usage: test.exe -wallet=<address>\n";
        return 0;
    }
    
    std::string wallet = argv[1];
    std::cout << "Step 3: Wallet arg = " << wallet << "\n";
    
    std::cout << "Step 4: About to include key.h...\n";
    
    return 0;
}
