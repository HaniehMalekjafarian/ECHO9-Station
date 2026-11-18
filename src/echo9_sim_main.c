#include "agentos_sync.h"
#include <stdio.h>

int main(void) {
    printf("=======================================================\n");
    printf("ECHO-9 Deep-Space Research Station — Phase 2 Crisis Sim\n");
    printf("=======================================================\n");
    if (echo9_run_crisis_simulation(true) != 0) {
        fprintf(stderr, "ECHO-9 Crisis Simulation FAILED!\n");
        return 1;
    }
    printf("=======================================================\n");
    printf("ECHO-9 CORE STABILIZED — ALL INVARIANTS VERIFIED!\n");
    printf("=======================================================\n");
    return 0;
}
