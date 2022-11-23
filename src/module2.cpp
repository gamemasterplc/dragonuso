#include <libdragon.h>
#include <stdio.h>
#include <stdexcept>
#include "uso_symbol.h"
#include "counter.h"

static Counter counter;

extern "C" USO_EXPORT_SYMBOL void update_counter()
{
	counter.increment();
	//Throw exception if counter is too high
	if(counter.get() > 500) {
		throw std::runtime_error("Counter too high");
	}
}

extern "C" USO_EXPORT_SYMBOL void print_counter()
{
	printf("counter = %d\n", counter.get());
}