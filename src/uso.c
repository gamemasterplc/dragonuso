#include <libdragon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include "uso.h"
#include "uso_internal.h"

typedef void (*func_ptr)(); //Generic function pointer
typedef uint32_t u_uint32_t __attribute__((aligned(1))); //Unaligned uint32_t

extern void __register_frame_info(void *ptr, void *object);
extern void __deregister_frame_info(void *ptr);
extern void __cxa_finalize(void *dso);
extern char *__cxa_demangle(const char *mangled_name, char *output_buffer, size_t *length, int *status);

//Increments the value of ptr by base
#define PTR_FIXUP(ptr, base) ((ptr) = (typeof(ptr))((uint8_t *)(base)+(uintptr_t)(ptr)))

uso_symbol_table_t *__uso_global_symbol_table;
struct uso_handle_data *__uso_list_head;
struct uso_handle_data *__uso_list_tail;
void (*__uso_notify_add_func)();
void (*__uso_notify_remove_func)();
bool __uso_initted;

//to should be a power of 2
static inline uint32_t roundup_value(uint32_t value, uint32_t to)
{
	return (value+to-1)&~(to-1);
}

//to should be a power of 2
static inline void *roundup_ptr(void *ptr, uint32_t to)
{
	return (void *)(((uintptr_t)ptr+to-1)&~(to-1));
}

static uint32_t get_uso_noload_start_ofs(uso_load_info_t *uso_load)
{
	//Noload starts immediately after USO with sufficient alignment
	return roundup_value(uso_load->uso_size, uso_load->noload_align);
}

static void *get_uso_noload_start(uso_load_info_t *uso_load, uso_header_t *uso_base)
{
	//Cast is to apply offset in 1-byte units
	return (uint8_t *)uso_base+get_uso_noload_start_ofs(uso_load);
}

static uint32_t get_uso_ram_size(uso_load_info_t *uso_load)
{
	//USO in RAM ends immediately after noload part
	return get_uso_noload_start_ofs(uso_load)+uso_load->noload_size;
}

static uint32_t get_uso_ram_align(uso_load_info_t *uso_load)
{
	//Return higher of uso_align and noload_align
	if(uso_load->uso_align > uso_load->noload_align) {
		return uso_load->uso_align;
	} else {
		return uso_load->noload_align;
	}
}

static void insert_uso(struct uso_handle_data *handle)
{
	struct uso_handle_data *prev = __uso_list_tail;
	//Make last handle next link to this handle
	if(!prev) {
		__uso_list_head = handle;
	} else {
		prev->next = handle;
	}
	//Set up new handle links
	handle->prev = prev;
	handle->next = NULL;
	__uso_list_tail = handle; //Append handle to end of list
}

static void remove_uso(struct uso_handle_data *handle)
{
	struct uso_handle_data *next = handle->next;
	struct uso_handle_data *prev = handle->prev;
	//Relink next handle to link to previous handle
	if(!next) {
		__uso_list_tail = prev;
	} else {
		next->prev = prev;
	}
	//Relink previous handle to link to next handle
	if(!prev) {
		__uso_list_head = next;
	} else {
		prev->next = next;
	}
}

static int symbol_compare(const void *arg1, const void *arg2)
{
	const uso_symbol_t *sym1 = arg1;
	const uso_symbol_t *sym2 = arg2;
	//Compare symbol names
	return strcmp(sym1->name, sym2->name);
}

static void *search_symbol_table(uso_symbol_table_t *table, const char *name)
{
	if(!table || table->length == 0) {
		//Return NULL for empty symbol tables
		return NULL;
	}
	//Do symbol table search
	uso_symbol_t cmp_symbol = { name, NULL, 0, 0 };
	uso_symbol_t *result = bsearch(&cmp_symbol, table->data, table->length, sizeof(uso_symbol_t), symbol_compare);
	if(result) {
		//Return pointer if symbol search succeeded
		return result->ptr;
	}
	//Return NULL for not found
	return NULL;
}

static void *search_loaded_symbols(const char *name, bool search_global)
{
	//Search in every loaded USO symbol table
	struct uso_handle_data *curr = __uso_list_head;
	while(curr) {
		void *ptr = search_symbol_table(curr->uso->export_syms, name);
		if(ptr) {
			//Found symbol in a symbol table
			return ptr;
		}
		curr = curr->next;
	}
	//Try global search if possible
	if(search_global) {
		return search_symbol_table(__uso_global_symbol_table, name);
	}
	//Return NULL
	return NULL;
}

static void fixup_symbol_table_names(uso_symbol_table_t *table)
{
	for(uint32_t i=0; i<table->length; i++) {
		PTR_FIXUP(table->data[i].name, table);
	}
}

static void fixup_section_table(uso_section_t *sections, void *noload_base, uint16_t num_sections)
{
	uint8_t *noload = noload_base;
	//Skip section 0 as that is the dummy section
	for(uint16_t i=1; i<num_sections; i++) {
		if(sections[i].data) {
			//Fixup section data pointer
			PTR_FIXUP(sections[i].data, sections);
			//Fixup section relocation pointer when not NULL
			if(sections[i].internal_relocs) {
				PTR_FIXUP(sections[i].internal_relocs, sections);
			}
			if(sections[i].external_relocs) {
				PTR_FIXUP(sections[i].external_relocs, sections);
			}
		} else {
			//Align noload pointer (do not change when alignment is 0)
			if(sections[i].data_align > 0) {
				noload = roundup_ptr(noload, sections[i].data_align);
			}
			sections[i].data = noload;
			//Move to next noload section
			noload += sections[i].data_size;
		}
	}
	//Make sure section 0's data pointer is NULL
	sections[0].data = NULL;
}

static void fixup_export_syms(uso_symbol_table_t *sym_table, uso_section_t *sections)
{
	//Symbol names must be fixed up before symbol search
	fixup_symbol_table_names(sym_table);
	//Fixup export symbol pointers
	for(uint32_t i=0; i<sym_table->length; i++) {
		PTR_FIXUP(sym_table->data[i].ptr, sections[sym_table->data[i].section].data);
	}
}

static bool fixup_import_syms(uso_symbol_table_t *sym_table)
{
	//Symbol resolution starts succeessful
	bool result = true;
	//Symbol names must be fixed up before symbol search
	fixup_symbol_table_names(sym_table);
	for(uint32_t i=0; i<sym_table->length; i++) {
		//Try to resolve symbol names in symbol tables
		void *ptr = search_loaded_symbols(sym_table->data[i].name, true);
		if(!__uso_is_symbol_weak(&sym_table->data[i]) && !ptr) {
			//Output error if symbol is not resolved and not weak
			//Also mark symbol resolution as failed
			debugf("Unresolved external symbol %s (%s).\n", sym_table->data[i].name, 
				__cxa_demangle(sym_table->data[i].name, NULL, NULL, NULL));
			result = false;
		}
		//Write pointer to symbol
		sym_table->data[i].ptr = ptr;
	}
	//Return symbol resolution result
	return result;
}

static void apply_uso_relocs(uso_header_t *uso, uint16_t target_section, bool internal)
{
	uint8_t *section_base = uso->sections[target_section].data;
	uso_reloc_table_t *table;
	//Get relocation table
	if(internal) {
		table = uso->sections[target_section].internal_relocs;
	} else {
		table = uso->sections[target_section].external_relocs;
	}
	//Skip invalid sections
	if(!section_base || !table) {
		return;
	}
	//Process relocations
	for(uint32_t i=0; i<table->length; i++) {
		uso_reloc_t *reloc = &table->data[i];
		//Target can be not aligned to 4 bytes and so uses the u_uint32_t type
		u_uint32_t *target = (u_uint32_t *)(section_base+reloc->offset);
		//Get relocation parameters
		uint8_t type = __uso_get_reloc_type(reloc);
		uint32_t target_index = __uso_get_reloc_target_index(reloc);
		uint32_t sym_addr;
		//Resolve symbol address of relocation
		if(internal) {
			sym_addr = (uint32_t)uso->sections[target_index].data+reloc->sym_offset;
		} else {
			sym_addr = (uint32_t)uso->import_syms->data[target_index].ptr;
		}
		//Apply relocations
		switch(type) {
			case R_MIPS_32:
			//Relocate pointers
				*target += sym_addr;
				break;
				
			case R_MIPS_26:
			//Relocate call instructions
			{
				uint32_t target_addr = ((*target & 0x3FFFFFF) << 2)+sym_addr;
				*target = (*target & 0xFC000000)|((target_addr & 0xFFFFFFC) >> 2);
			}
			break;
			
			case R_MIPS_HI16:
			//Relocate hi part of hi/lo pair
			{
				//Calculate original hi and address
				uint16_t hi = *target & 0xFFFF;
				uint32_t addr = hi << 16;
				//Look for lo in later relocs
				for(uint32_t j=i+1; j<table->length; j++) {
					uso_reloc_t *new_reloc = &table->data[j];
					if(__uso_get_reloc_type(new_reloc) == R_MIPS_LO16) {
						//Found lo relocation
						//Read lo from relocation target
						u_uint32_t *lo_target = (u_uint32_t *)(section_base+new_reloc->offset);
						uint16_t lo = *lo_target & 0xFFFF;
						//Calculate target address from hi and lo
						addr += lo;
						if(lo > 0x8000) {
							//Apply sign extension to lo
							addr -= 0x10000;
						}
						//Calculate new address
						addr += sym_addr;
						//Calculate hi
						hi = addr >> 16;
						if(addr & 0x8000) {
							//Change HI so LO works correctly with sign extension
							hi++;
						}
						break;
					}
				}
				//Apply relocation
				*target = (*target & 0xFFFF0000)|hi;
			}
			break;
			
			case R_MIPS_LO16:
			//Relocate lo part of hi/lo pair pair
			//Just increments lo of the target instruction by the symbol address
			{
				
				uint16_t lo = *target & 0xFFFF;
				lo += sym_addr;
				*target = (*target & 0xFFFF0000)|lo;
			}
			break;
			
			default:
			//Throw up an error if invalid relocation types are hit
				assertf(0, "Invalid relocation type %d.\n", type);
				break;
		}
	}
}

static void link_uso(uso_header_t *uso)
{
	//Apply relocations for each non-dummy section
	for(uint16_t i=1; i<uso->num_sections; i++) {
		apply_uso_relocs(uso, i, true);
		apply_uso_relocs(uso, i, false);
	}
}

static void flush_uso(uso_header_t *uso)
{
	//Invalidate cache for each non-dummy section
	for(uint16_t i=1; i<uso->num_sections; i++) {
		data_cache_hit_writeback_invalidate(uso->sections[i].data, uso->sections[i].data_size);
		inst_cache_hit_invalidate(uso->sections[i].data, uso->sections[i].data_size);
	}
}

static bool fixup_uso(uso_header_t *uso, void *noload_base)
{
	//Do section fixups
	PTR_FIXUP(uso->sections, uso);
	fixup_section_table(uso->sections, noload_base, uso->num_sections);
	//Do symbol fixups
	if(uso->export_syms) {
		PTR_FIXUP(uso->export_syms, uso);
		fixup_export_syms(uso->export_syms, uso->sections);
	}
	if(uso->import_syms) {
		PTR_FIXUP(uso->import_syms, uso);
		if(!fixup_import_syms(uso->import_syms)) {
			return false;
		}
	}
	//Do final link
	link_uso(uso);
	return true;
}

static void run_ctors(uso_header_t *uso)
{
	uso_section_t *ctor_section = &uso->sections[uso->ctors_section];
	func_ptr *ctor_start = ctor_section->data;
	//Check if any constructors exist
	if(ctor_start && ctor_section->data_size > 0) {
		//Run constructors in reverse of stored order
		func_ptr *ctor_end = (func_ptr *)((uint8_t *)ctor_start+ctor_section->data_size);
		func_ptr *ctor_curr = ctor_end-1;
		while(ctor_curr >= ctor_start) {
			(*ctor_curr)();
			ctor_curr--;
		}
	}
}

static void run_dtors(uso_header_t *uso)
{
	//Run atexit destructors before running dtors
	void *dso_handle = search_symbol_table(uso->export_syms, "__dso_handle");
	if(dso_handle) {
		__cxa_finalize(dso_handle);
	}
	uso_section_t *dtor_section = &uso->sections[uso->dtors_section];
	func_ptr *dtor_start = dtor_section->data;
	//Check if any destructors exist
	if(dtor_start && dtor_section->data_size > 0) {
		//Run destructors in stored order
		func_ptr *dtor_end = (func_ptr *)((uint8_t *)dtor_start+dtor_section->data_size);
		func_ptr *dtor_curr = dtor_start;
		while(dtor_curr < dtor_end) {
			(*dtor_curr)();
			dtor_curr++;
		}
	}
	
}

static void start_uso(uso_header_t *uso, uint32_t *frameobj_data)
{
	//Register exception frames first
	uso_section_t *ehframe_section = &uso->sections[uso->eh_frame_section];
	if(ehframe_section->data && ehframe_section->data_size > 0) {
		__register_frame_info(ehframe_section->data, frameobj_data);
	}
	run_ctors(uso);
	//Run _prolog after constructors if it exists
	func_ptr prolog_func = search_symbol_table(uso->export_syms, "_prolog");
	if(prolog_func) {
		prolog_func();
	}
}

static void end_uso(uso_header_t *uso)
{
	uso_section_t *ehframe_section = &uso->sections[uso->eh_frame_section];
	//Run epilog function before anything else
	func_ptr epilog_func = search_symbol_table(uso->export_syms, "_epilog");
	if(epilog_func) {
		epilog_func();
	}
	run_dtors(uso);
	//Deregister exception frames last
	if(ehframe_section->data && ehframe_section->data_size > 0) {
		__deregister_frame_info(ehframe_section->data);
	}
}

static bool is_ptr_inside_uso(uso_header_t *header, void *ptr)
{
	for(uint16_t i=0; i<header->num_sections; i++) {
		uso_section_t *section = &header->sections[i];
		if(section->data) {
			void *ptr_min = section->data;
			void *ptr_max = (uint8_t *)section->data+section->data_size;
			if(ptr >= ptr_min && ptr < ptr_max) {
				return true;
			}
		}
	}
	return false;
}

static bool is_uso_closeable(struct uso_handle_data *handle)
{
	uso_symbol_table_t *export_syms = handle->uso->export_syms; //Save handle to uso export symbols
	//Iterate through loaded USOs
	struct uso_handle_data *curr = __uso_list_head;
	while(curr) {
		if(curr == handle) {
			//Skip searching this symbol table and go to next one
			curr = curr->next;
			continue;
		}
		//The USO cannot be closed if another loaded USO imports a symbol from its symbol table
		for(uint32_t i=0; i<export_syms->length; i++) {
			if(search_symbol_table(curr->uso->import_syms, export_syms->data[i].name)) {
				return false;
			}
		}
		//Iterate to next USO
		curr = curr->next;
	}
	return true;
}

void uso_init(const char *global_sym_filename)
{
	//Open global symbol file
	FILE *file = fopen(global_sym_filename, "rb");
	assertf(file, "File not found: %s\n", global_sym_filename);
	//Calculate size of global symbols
	fseek(file, 0, SEEK_END);
	size_t size = ftell(file);
	__uso_global_symbol_table = malloc(size);
	//Read global symbols
	fseek(file, 0, SEEK_SET);
	fread(__uso_global_symbol_table, size, 1, file);
	fclose(file);
	fixup_symbol_table_names(__uso_global_symbol_table);
	//Initialize globals
	__uso_list_head = __uso_list_tail = NULL;
	__uso_initted = true;
	//Clean up resources
}

uso_handle_t *uso_get_handle(const char *filename)
{
	//Iterate over USOs
	struct uso_handle_data *curr = __uso_list_head;
	while(curr) {
		if(strcmp(curr->name, filename) == 0) {
			//Found USO with matching name 
			return curr;
		}
		//Iterate to next USO
		curr = curr->next;
	}
	//Did not find USO with matching name
	return NULL;
}

uso_handle_t *uso_get_handle_ptr(void *ptr)
{
	struct uso_handle_data *curr = __uso_list_head;
	while(curr) {
		if(is_ptr_inside_uso(curr->uso, ptr)) {
			return curr;
		}
		curr = curr->next;
	}
	return NULL;
}

bool uso_is_handle_valid(uso_handle_t *handle)
{
	//Iterate through loaded USOs
	struct uso_handle_data *curr = __uso_list_head;
	while(curr) {
		if(curr == handle) {
			//Found matching handle
			return true;
		}
		//Iterate to next USO
		curr = curr->next;
	}
	//Did not find matching handle
	return false;
}

uso_handle_t *uso_open(const char *filename)
{
	//Check if uso_init has been called
	assertf(__uso_initted, "Call uso_init before opening any USOs.\n");
	//Try opening existing handle
	uso_handle_t *handle = uso_get_handle(filename);
	if(handle) {
		//Increment reference count if existing handle is found
		handle->ref_count++;
		return handle;
	}
	//Try to open USO
	FILE *file = fopen(filename, "rb");
	if(!file) {
		//Output open error
		debugf("Failed to open USO %s.\n", filename);
		return NULL;
	}
	//Allocate new handle and copy name
	handle = malloc(sizeof(struct uso_handle_data)+strlen(filename)+1);
	strcpy(handle->name, filename);
	//Read USO load info
	uso_load_info_t load_info;
	fseek(file, -sizeof(uso_load_info_t), SEEK_END);
	fread(&load_info, sizeof(uso_load_info_t), 1, file);
	//Allocate USO
	uint32_t uso_size = get_uso_ram_size(&load_info);
	handle->uso = memalign(get_uso_ram_align(&load_info), uso_size);
	//Erase USO
	memset(handle->uso, 0, uso_size);
	//Read USO file
	fseek(file, 0, SEEK_SET);
	fread(handle->uso, load_info.uso_size, 1, file);
	fclose(file);
	//Do loading work to USO
	if(!fixup_uso(handle->uso, get_uso_noload_start(&load_info, handle->uso))) {
		//Output load error
		debugf("Failed to load USO %s.\n", filename);
		//Get rid of USO if it failed to load
		free(handle->uso);
		free(handle);
		return NULL;
	}
	//Invalidate cache of USO to make sure new code/data is seen
	flush_uso(handle->uso);
	//Add handle to USO list
	handle->ref_count = 1;
	insert_uso(handle);
	if(__uso_notify_add_func) {
		__uso_notify_add_func();
	}
	start_uso(handle->uso, handle->frameobj_data);
	return handle;
}

void *uso_sym(uso_handle_t *handle, const char *name)
{
	if(handle == USO_HANDLE_ANY) {
		//Search through all USOs if special handle is passed
		return search_loaded_symbols(name, false);
	}
	//Check if passed USO handle is valid
	assertf(uso_is_handle_valid(handle), "Can't get symbols from invalid USO handle %p.\n", handle);
	//Do search in this USO's symbol table
	return search_symbol_table(handle->uso->export_syms, name);
}

void uso_close(uso_handle_t *handle)
{
	assertf(uso_is_handle_valid(handle), "Can't close invalid USO handle %p.\n", handle);
	//Decrement reference count
	if(handle->ref_count != 0) {
		handle->ref_count--;
	}
	//Close USO if closable and no references remain
	if(is_uso_closeable(handle) && handle->ref_count == 0) {
		end_uso(handle->uso);
		//Do removal work of USO
		remove_uso(handle);
		if(__uso_notify_remove_func) {
			__uso_notify_remove_func();
		}
		free(handle->uso);
		free(handle);
	}
}