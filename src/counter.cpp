#include <libdragon.h>
#include "counter.h"

Counter::Counter()
{
	debugf("Running counter constructor.\n");
	m_value = 0;
}

Counter::~Counter()
{
	debugf("Running counter destructor.\n");
}


void Counter::increment()
{
	m_value++;
}

int Counter::get()
{
	return m_value;
}