#include <libdragon.h>
#include <math.h>
#include <stdexcept>
#include "uso.h"

typedef void (*func_ptr)();

int main()
{
	//Initialize libdragon
	debug_init(DEBUG_FEATURE_ALL);
	dfs_init(DFS_DEFAULT_LOCATION);
	//Initialize console
	console_init();
	console_set_render_mode(RENDER_MANUAL);
	console_set_debug(false);
	//Load global symbols
	debugf("Loading global symbols\n");
	uso_init("rom:/global_syms.sym");
	//Load modules
	debugf("Loading module 1\n");
	uso_handle_t *uso_handle_1 = uso_open("rom:/module1.uso");
	debugf("Loading module 2\n");
	uso_handle_t *uso_handle_2 = uso_open("rom:/module2.uso");
	//Grab function pointers from module 2
	debugf("Grabbing function pointers from module 2\n");
	func_ptr update_counter = (func_ptr)uso_sym(uso_handle_2, "update_counter");
	func_ptr print_counter = (func_ptr)uso_sym(uso_handle_2, "print_counter");
	while(1) {
		//Erase console
		console_clear();
		//Write no modules text if no modules are loaded
		if(!uso_is_handle_valid(uso_handle_1) && !uso_is_handle_valid(uso_handle_2)) {
			printf("No modules loaded\n");
		}
		if(uso_is_handle_valid(uso_handle_1)) {
			//Print module 1 loaded text if loaded
			printf("Module 1 loaded\n");
			//Unload module 1 after 10 seconds
			if(TICKS_READ() > TICKS_PER_SECOND*10) {
				uso_close(uso_handle_1);
			}
		}
		if(uso_is_handle_valid(uso_handle_2)) {
			//Print module 2 loaded text
			printf("Module 2 loaded\n");
			//Print function addresses
			printf("update_counter = %p\n", update_counter);
			printf("print_counter = %p\n", print_counter);
			try {
				//Do counter work
				update_counter();
				print_counter();
			} catch(std::runtime_error &error) {
				//Use exception from update_counter to notify module 2 unload
				uso_close(uso_handle_2);
			}
		}
		//Write console to screen
		console_render();
	}
}