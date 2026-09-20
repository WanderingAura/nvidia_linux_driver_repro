// VK_EXT_descriptor_heap repro driver. See README.md.
//
// Runs two suites in order and prints a single combined verdict:
//
//   Part 1, controls (compute_controls.cpp) - expected to pass everywhere.
//           They rule out innocent explanations: constant heap indices,
//           buffer-sourced dynamic indices, and push-data indices all work.
//
//   Part 2, the repro (graphics_repro.cpp)  - a fragment shader reading a
//           descriptor heap at a constant index of 1. Fails on NVIDIA
//           610.57.04 and 615.71.09, passes on Mesa RADV.
//
// Exit code 0 means no bug was observed; 1 means the repro reproduced.

#include <iostream>

int runComputeControls();
int runGraphicsRepro();

int main() {
    std::cout << "================================================================\n"
                 "  Part 1 of 2: CONTROL CASES (compute)\n"
                 "  These are expected to PASS on every driver.\n"
                 "================================================================\n\n";
    int controls = runComputeControls();

    std::cout << "\n================================================================\n"
                 "  Part 2 of 2: THE REPRO (fragment stage)\n"
                 "  Expected to FAIL on affected NVIDIA drivers, pass elsewhere.\n"
                 "================================================================\n\n";
    int repro = runGraphicsRepro();

    std::cout << "\n================================================================\n"
                 "  SUMMARY\n"
                 "    control cases (compute) : "
              << (controls == 0 ? "PASS" : "FAIL") << "\n"
              << "    repro (fragment)        : " << (repro == 0 ? "PASS" : "FAIL")
              << (repro == 0 ? "" : "   <-- the bug") << "\n"
                 "================================================================\n";

    if (controls == 0 && repro != 0) {
        std::cout << "\nThe controls all pass, so descriptor heaps work fine in compute at\n"
                     "every index. Only the fragment stage is affected, and only for heap\n"
                     "indices other than 0 -- even a literal constant 1. This is the bug.\n";
    } else if (controls == 0 && repro == 0) {
        std::cout << "\nNo failure observed on this driver.\n";
    }

    return repro == 0 ? 0 : 1;
}
