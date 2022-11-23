#ifndef USO_H
#define USO_H

#ifdef __cplusplus
extern "C" {
#endif

#define USO_HANDLE_ANY ((uso_handle_t *)(-1))

typedef struct uso_handle_data uso_handle_t;

//Initializes USO library and load global symbol file
void uso_init(const char *global_sym_filename);
//Get handle to existing USO by filename
//Does not increment reference count
uso_handle_t *uso_get_handle(const char *filename);
//Get handle to USO from pointer inside any of its sections
//Does not increment reference count
uso_handle_t *uso_get_handle_ptr(void *ptr);
//Check if USO handle is valid
bool uso_is_handle_valid(uso_handle_t *handle);
//Open USO file
//Reference count will increment if already open
//Will return NULL if USO failed to load or open
//Error output will be reported to debug terminal
uso_handle_t *uso_open(const char *filename);
//Get pointer to exported symbol from USO handle
//USO_HANDLE_ANY can be passed in as the handle to check all loaded USOs
void *uso_sym(uso_handle_t *handle, const char *name);
//Close USO handle
//The USO will be unloaded when the reference count reaches zero and it is not being used by another loaded USO
void uso_close(uso_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif