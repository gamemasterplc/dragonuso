#include <libdragon.h>
#include <math.h>

//Show off executing functions when starting/ending USO along with a class

//The function when starting a USO must be named _prolog and defined as extern "C" in C++
extern "C" void _prolog()
{
	debugf("Starting module1.\n");
}

//The epilog function must be named _prolog and defined as extern "C" in C++
extern "C" void _epilog()
{
	debugf("Ending module1.\n");
}