#ifndef USO_SYMBOL_H
#define USO_SYMBOL_H

//USO_EXPORT_SYMBOL is used to make symbols visible to other USOs and uso_sym
//Name mangling will still be applied if compiling a C++ file and can be disabled with extern "C"
#define USO_EXPORT_SYMBOL __attribute__ ((visibility ("default")))

#endif