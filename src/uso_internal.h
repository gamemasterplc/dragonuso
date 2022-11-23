#ifndef USO_INTERNAL_H
#define USO_INTERNAL_H

#include <stdint.h>
#include <stdbool.h>

//USO relocation types
#define R_MIPS_32 2
#define R_MIPS_26 4
#define R_MIPS_HI16 5
#define R_MIPS_LO16 6

typedef struct uso_symbol {
    const char *name; //Relative to symbol table
    void *ptr;
    uint16_t section;
    uint16_t name_len; //Top bit used to tell if symbol is weak
} uso_symbol_t;

_Static_assert(sizeof(uso_symbol_t) == 12, "Invalid uso_symbol_t size.");

//Symbols should appear sorted by name in ASCII order
typedef struct uso_symbol_table {
	uint32_t length;
	uso_symbol_t data[0]; //Real size is num_symbols
} uso_symbol_table_t;

_Static_assert(sizeof(uso_symbol_table_t) == 4, "Invalid uso_symbol_table_t size.");

typedef struct uso_reloc {
    uint32_t offset;
    uint32_t info; //Upper 6 bits are relocation type, lower 26 bits are either symbol or section index
    uint32_t sym_offset; //Section-relative symbol offset, zero for external relocations
} uso_reloc_t;

_Static_assert(sizeof(uso_reloc_t) == 12, "Invalid uso_reloc_t size.");

typedef struct uso_reloc_table {
	uint32_t length;
	uso_reloc_t data[0]; //Real size is length
} uso_reloc_table_t;

_Static_assert(sizeof(uso_reloc_table_t) == 4, "Invalid uso_reloc_table_t size.");

//Section 0 is treated as dummy section
//Every SHF_ALLOC section is included in file
//Is NOLOAD section when data is NULL in file
typedef struct uso_section {
    void *data;
    uint32_t data_size;
    uint32_t data_align;
    uso_reloc_table_t *internal_relocs;
    uso_reloc_table_t *external_relocs;
} uso_section_t;

_Static_assert(sizeof(uso_section_t) == 20, "Invalid uso_section_t size.");

typedef struct uso_header {
	uint16_t num_sections;
    uint16_t eh_frame_section;
    uso_section_t *sections;
    uso_symbol_table_t *import_syms;
    uso_symbol_table_t *export_syms;
    uint16_t ctors_section;
    uint16_t dtors_section;
	char src_elf_name[0]; //Treated as const char * string
} uso_header_t;

_Static_assert(sizeof(uso_header_t) == 20, "Invalid uso_header_t size.");

typedef struct uso_load_info {
    uint32_t uso_size;
    uint32_t uso_align;
    uint32_t noload_size;
    uint32_t noload_align;
} uso_load_info_t;

_Static_assert(sizeof(uso_load_info_t) == 16, "Invalid uso_load_info_t size.");

struct uso_handle_data {
	struct uso_handle_data *next;
	struct uso_handle_data *prev;
	uso_header_t *uso;
	size_t ref_count;
	uint32_t frameobj_data[6];
	char name[0];
};

//External global variables
extern uso_symbol_table_t *__uso_global_symbol_table;
//USO List variables
extern struct uso_handle_data *__uso_list_head;
extern struct uso_handle_data *__uso_list_tail;
//USO Debugger Notify Function Pointers
extern void (*__uso_notify_add_func)();
extern void (*__uso_notify_remove_func)();
extern bool __uso_initted;

//USO inline functions
static inline bool __uso_is_symbol_weak(uso_symbol_t *symbol)
{
	if(symbol->name_len & 0x8000) {
		return true;
	}
	return false;
}

static inline uint16_t __uso_symbol_get_name_length(uso_symbol_t *symbol)
{
	return symbol->name_len & 0x7FFF;
}

static inline uint8_t __uso_get_reloc_type(uso_reloc_t *reloc)
{
	return reloc->info >> 26;
}

static inline uint32_t __uso_get_reloc_target_index(uso_reloc_t *reloc)
{
	return reloc->info & 0x3FFFFFF;
}

#endif