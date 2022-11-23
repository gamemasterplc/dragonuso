#ifndef COUNTER_H
#define COUNTER_H

#include "uso_symbol.h"

class USO_EXPORT_SYMBOL Counter {
public:
	Counter();
	~Counter();
	
public:
	void increment();
	int get();
	
private:
	int m_value;
};

#endif