#include <stdlib.h>
#include <time.h>
#include "simulator.h"

float generate_price(float base) {

    float change = ((rand() % 100) - 50) / 100.0;

    return base + change;
}